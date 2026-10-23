// NeuralVC.h
// Optional neural real-time voice conversion engine (no JUCE dependency).
//
// Pipeline (streaming, chunked with overlap-crossfade):
//
//   host SR mono -> resample 16k -> [content encoder .onnx] -> features [1,T,768]
//                                          |
//                             NSDF F0 (per frame, 50 fps) -> transpose/intonate
//                                          |
//                       [RVC generator .onnx] -> audio @ model SR -> resample host
//                                          |
//                       1-frame overlap crossfade -> delay line -> out
//
// Compatible with the common RVC / w-okada ONNX export convention:
//   content model:  float [1, N] @16k            -> float [1, T, 768]  (T = N/320)
//   generator:      phone float [1,T,768] (or [T,768])
//                   phone_lengths int64 [1]
//                   pitch int64 [1,T]  (coarse 1..255)
//                   pitchf float [1,T] (Hz)
//                   ds/sid int64 [1] (optional)  -> audio float [1,1,M] @ model SR
// Input tensors are matched adaptively by name/type, so most RVC exports load.
//
// Frame math (exact, keeps delay line balanced):
//   hop = 320 @16k (20 ms). Window = ctx(1) + chunk(2) frames = 960 samples.
//   Consumed per step: chunk frames. Emitted: (chunk+1) frames of audio; the
//   first emitted frame crossfades with the tail of the previous emission.
#pragma once
#if DYV_NEURAL
// Do NOT let the ORT C++ header initialise its API table in a static
// initialiser: that runs inside DllMain (loader lock) when the host loads the
// plugin, and if onnxruntime.dll is missing / wrong version the plugin dll
// fails to load at all ("problem opening the plugin ... unknown reason").
// We call Ort::InitApi() lazily, only when the neural engine is first used.
#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>
 #if defined (_WIN32) && defined (_MSC_VER)
bool dyvLoadOrt(); // OrtDelayLoad.cpp: LoadLibrary onnxruntime.dll from the plugin's own folder
 #endif
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace nvf
{
constexpr float kPiF = 3.14159265f;
constexpr int kHop16 = 320;          // ContentVec hop @16 kHz (20 ms)
constexpr int kCtxFrames = 1;        // context frames before the chunk
constexpr int kChunkFrames = 2;      // new frames per inference step
constexpr int kFeatDim = 768;        // ContentVec feature width

// ------------------------------------------------------------- resampler
// Streaming cubic-Hermite resampler (1-sample internal delay).
struct Resamp
{
    double ratio = 1.0;             // dst/src
    double ph = 1.0;
    float h0 = 0, h1 = 0, h2 = 0, h3 = 0;

    void reset (float fill = 0.f) { h0 = h1 = h2 = h3 = fill; ph = 1.0; }

    void push (const float* x, int n, std::vector<float>& out)
    {
        if (std::fabs (ratio - 1.0) < 1e-9)
        {
            out.insert (out.end(), x, x + n);
            return;
        }
        for (int i = 0; i < n; ++i)
        {
            h0 = h1; h1 = h2; h2 = h3; h3 = x[i];
            while (ph <= 2.0)
            {
                const float fr = (float) (ph - 1.0);   // between h1 and h2
                const float a = h0, b = h1, c = h2, d = h3;
                const float c1 = 0.5f * (c - a);
                const float c2 = a - 2.5f * b + 2.f * c - 0.5f * d;
                const float c3 = 0.5f * (d - a) + 1.5f * (b - c);
                out.push_back (((c3 * fr + c2) * fr + c1) * fr + b);
                ph += 1.0 / ratio;
            }
            ph -= 1.0;
        }
    }
};

// ------------------------------------------------- compact NSDF F0 (16 kHz)
struct F0Detector
{
    int W = 400;                       // ~25 ms window
    float lastF0 = 0.f, confS = 0.f;
    std::vector<float> buf;
    int64_t head = 0;                  // absolute 16k sample count
    std::vector<float> nsdf, seg;

    void reset() { buf.assign (1 << 14, 0.f); head = 0; lastF0 = 0; confS = 0; }
    void push (const float* x, int n)
    {
        for (int i = 0; i < n; ++i) buf[(size_t) ((head + i) & 16383)] = x[i];
        head += n;
    }

    // analyse the frame ending at sample `endAbs` (exclusive); f0<=1 if unvoiced
    float analyse (int64_t endAbs, float thresh)
    {
        if (endAbs < W + 2) return 0.f;
        const int64_t start = endAbs - W;
        if (start < 0) return 0.f;
        if (seg.size() < (size_t) (W + 2 * 300)) seg.resize (W + 600);
        for (int j = 0; j < W + 300; ++j) seg[(size_t) j] = buf[(size_t) ((start + j) & 16383)];

        const int minLag = 16000 / 450, maxLag = 16000 / 60; // 60..450 Hz
        double e0 = 0;
        for (int j = 0; j < W; ++j) e0 += (double) seg[(size_t) j] * seg[(size_t) j];
        if (e0 / W < 1e-7) return 0.f;

        double es = 0;
        for (int j = 0; j < W; ++j) es += (double) seg[(size_t) (1 + j)] * seg[(size_t) (1 + j)];
        float bestC = 0.f; float bestLag = 0.f;
        for (int tau = minLag; tau <= maxLag; ++tau)
        {
            double acf = 0;
            for (int j = 0; j < W; ++j) acf += (double) seg[(size_t) j] * seg[(size_t) (j + tau)];
            const float c = (float) (2.0 * acf / (e0 + es + 1e-20));
            if (c > bestC) { bestC = c; bestLag = (float) tau; }
            es += (double) seg[(size_t) (W + tau)] * seg[(size_t) (W + tau)]
                - (double) seg[(size_t) tau] * seg[(size_t) tau];
        }
        confS += 0.4f * (bestC - confS);
        if (bestC < thresh) return 0.f;
        float f0 = 16000.f / bestLag;
        if (lastF0 > 1.f)
        {
            const float r = f0 / lastF0;
            if (r > 1.3f || r < 0.77f) f0 = lastF0;     // continuity guard
            else f0 = 0.5f * (f0 + lastF0);
        }
        lastF0 = f0;
        return f0;
    }
};

// RVC coarse-pitch buckets (1..255)
inline int coarseOf (float f0)
{
    if (f0 <= 1.f) return 1;
    const float melMin = 1127.f * std::log (1.f + 50.f / 700.f);
    const float melMax = 1127.f * std::log (1.f + 1100.f / 700.f);
    const float mel = 1127.f * std::log (1.f + f0 / 700.f);
    int c = (int) std::lround ((mel - melMin) / (melMax - melMin) * 255.f);
    return std::min (255, std::max (1, c));
}

#if DYV_NEURAL

// ------------------------------------------------------------- ONNX model
struct ModelIO
{
    std::vector<std::string> inNames, outNames;
    std::vector<const char*> inPtrs, outPtrs;
    std::vector<std::vector<int64_t>> inShapes;
    std::vector<ONNXTensorElementDataType> inTypes;
    std::vector<std::vector<int64_t>> outShapes;
};

struct SessionHolder
{
    std::unique_ptr<Ort::Env> env;
    std::unique_ptr<Ort::Session> ses;
    ModelIO io;
};

// UTF-8 std::string -> native std::filesystem::path.
// On Windows Ort::Session and ifstream expect wchar_t paths (ORTCHAR_T ==
// wchar_t), and %APPDATA% folders are routinely non-ASCII, so go through
// u8path() for a correct UTF-8 -> UTF-16 conversion. On POSIX char paths are
// native and pass straight through.
static std::filesystem::path nativePath (const std::string& p)
{
#if defined(_WIN32)
    return std::filesystem::u8path (p);
#else
    return std::filesystem::path (p);
#endif
}

static bool readFileOk (const std::string& p)
{
    std::ifstream f (nativePath (p), std::ios::binary);
    return f.good() && f.seekg (0, std::ios::end).tellg() > 0;
}

static void fillModelIO (Ort::Session& s, ModelIO& io)
{
    Ort::AllocatorWithDefaultOptions al;
    const auto grab = [&] (auto count, auto get)
    {
        std::vector<std::string> v;
        for (size_t i = 0; i < count; ++i) v.emplace_back (get (i, al).get());
        return v;
    };
    io.inNames = grab (s.GetInputCount(), [&] (size_t i, Ort::AllocatorWithDefaultOptions& a) { return s.GetInputNameAllocated (i, a); });
    io.outNames = grab (s.GetOutputCount(), [&] (size_t i, Ort::AllocatorWithDefaultOptions& a) { return s.GetOutputNameAllocated (i, a); });
    for (auto& n : io.inNames) io.inPtrs.push_back (n.c_str());
    for (auto& n : io.outNames) io.outPtrs.push_back (n.c_str());
    for (size_t i = 0; i < io.inNames.size(); ++i)
    {
        auto ti = s.GetInputTypeInfo (i);             // keep alive: Unowned view below
        auto info = ti.GetTensorTypeAndShapeInfo();
        io.inShapes.push_back (info.GetShape());
        io.inTypes.push_back (info.GetElementType());
    }
    for (size_t i = 0; i < io.outNames.size(); ++i)
    {
        auto ti = s.GetOutputTypeInfo (i);
        io.outShapes.push_back (ti.GetTensorTypeAndShapeInfo().GetShape());
    }
}

// One-time, lazy, failure-tolerant ORT bring-up (never throws).
static bool ensureOrtApi()
{
    static const bool ok = []() -> bool
    {
       #if defined (_WIN32) && defined (_MSC_VER)
        if (! dyvLoadOrt()) return false;
       #endif
        const OrtApiBase* base = OrtGetApiBase();
        if (base == nullptr || base->GetApi (ORT_API_VERSION) == nullptr) return false; // runtime older than headers
        Ort::InitApi();
        return true;
    }();
    return ok;
}

static std::unique_ptr<SessionHolder> openSession (const std::string& path, int threads)
{
    if (! ensureOrtApi()) return nullptr;
    if (! readFileOk (path)) return nullptr;
    auto h = std::make_unique<SessionHolder>();
    h->env = std::make_unique<Ort::Env> (ORT_LOGGING_LEVEL_WARNING, "dyvocalizer-nn");
    Ort::SessionOptions so;
    so.SetGraphOptimizationLevel (GraphOptimizationLevel::ORT_ENABLE_ALL);
    so.SetExecutionMode (ORT_SEQUENTIAL);
    so.SetIntraOpNumThreads (std::max (1, threads));
    so.SetInterOpNumThreads (1);
    try { h->ses = std::make_unique<Ort::Session> (*h->env, nativePath (path).c_str(), so); }
    catch (...) { return nullptr; }
    fillModelIO (*h->ses, h->io);
    return h;
}

static std::string lower (std::string s)
{
    std::transform (s.begin(), s.end(), s.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
    return s;
}

// --------------------------------------------------------- neural engine
class NeuralEngine
{
public:
    ~NeuralEngine() { delete gen_.exchange (nullptr); }

    struct VoiceInfo { std::string name, path; };

    // scan a folder: content encoder must be content.onnx; voices = voice_*.onnx / *.onnx
    static std::vector<VoiceInfo> scanVoices (const std::string& dir)
    {
        std::vector<VoiceInfo> out;
        // tiny dirent-free listing via std::filesystem
        namespace fs = std::filesystem;
        std::error_code ec;
        if (! fs::is_directory (dir, ec)) return out;
        for (auto& e : fs::directory_iterator (dir, ec))
        {
            if (! e.is_regular_file()) continue;
            auto p = e.path();
            if (lower (p.extension().string()) != ".onnx") continue;
            auto stem = lower (p.stem().string());
            if (stem == "content") continue;
            out.push_back ({ p.stem().string(), p.string() });
        }
        std::sort (out.begin(), out.end(), [] (auto& a, auto& b) { return a.name < b.name; });
        return out;
    }

    bool loadContent (const std::string& path, int threads)
    {
        auto s = openSession (path, threads);
        if (! s) return false;
        content_ = std::move (s);
        return true;
    }

    bool contentReady() const { return content_ != nullptr; }

    // returns model sample rate, or 0 on failure
    double loadVoice (const std::string& path)
    {
        auto s = openSession (path, threads_);
        if (! s) return 0.0;

        // probe: silent 3-frame inference reveals outSamplesPerFrame -> model SR
        double sr = 48000.0;
        try
        {
            Ort::MemoryInfo mi = Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault);
            const int T = 3;
            std::vector<float> feat ((size_t) T * kFeatDim, 0.f);
            std::vector<float> pf ((size_t) T, 110.f);
            std::vector<int64_t> pc ((size_t) T, coarseOf (110.f));
            std::vector<int64_t> len { T };
            std::vector<int64_t> dsv { 0 }, ds2 { 0, 0 };
            std::vector<Ort::Value> feeds;
            std::vector<const char*> names;
            const std::array<int64_t, 1> dLen { 1 };
            const std::array<int64_t, 1> dSpk1 { 1 };
            const std::array<int64_t, 2> dSpk2 { 1, 1 };
            for (size_t i = 0; i < s->io.inNames.size(); ++i)
            {
                const std::string n = lower (s->io.inNames[i]);
                const auto ty = s->io.inTypes[i];
                if (n.find ("length") != std::string::npos && ty == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64)
                {
                    names.push_back (s->io.inNames[i].c_str());
                    feeds.push_back (Ort::Value::CreateTensor<int64_t> (mi, len.data(), 1, dLen.data(), 1));
                }
                else if (n.find ("pitch") != std::string::npos && ty == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
                {
                    names.push_back (s->io.inNames[i].c_str());
                    std::array<int64_t, 2> d { 1, T };
                    feeds.push_back (Ort::Value::CreateTensor<float> (mi, pf.data(), pf.size(), d.data(), 2));
                }
                else if (n.find ("pitch") != std::string::npos && ty == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64)
                {
                    names.push_back (s->io.inNames[i].c_str());
                    std::array<int64_t, 2> d { 1, T };
                    feeds.push_back (Ort::Value::CreateTensor<int64_t> (mi, pc.data(), pc.size(), d.data(), 2));
                }
                else if (ty == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT && s->io.inShapes[i].size() >= 2)
                {
                    names.push_back (s->io.inNames[i].c_str());
                    std::array<int64_t, 3> d { 1, T, kFeatDim };
                    feeds.push_back (Ort::Value::CreateTensor<float> (mi, feat.data(), feat.size(), d.data(), 3));
                }
                else if (ty == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64
                         && (n.find ("ds") != std::string::npos || n.find ("sid") != std::string::npos || n.find ("spk") != std::string::npos))
                {
                    names.push_back (s->io.inNames[i].c_str());
                    if (s->io.inShapes[i].size() >= 2)
                        feeds.push_back (Ort::Value::CreateTensor<int64_t> (mi, ds2.data(), 2, dSpk2.data(), 2));
                    else
                        feeds.push_back (Ort::Value::CreateTensor<int64_t> (mi, dsv.data(), 1, dSpk1.data(), 1));
                }
            }
            auto outs = s->ses->Run (Ort::RunOptions { nullptr }, names.data(), feeds.data(), feeds.size(),
                                     s->io.outPtrs.data(), (size_t) std::min<size_t> (1, s->io.outPtrs.size()));
            if (! outs.empty())
            {
                const auto sh = outs[0].GetTensorTypeAndShapeInfo().GetShape();
                int64_t M = 0;
                for (auto d : sh) if (d > M) M = d;
                if (M > 0) sr = (double) M * 50.0 / (double) T;
                // snap to common model rates
                for (double c : { 32000.0, 40000.0, 44100.0, 48000.0 })
                    if (std::fabs (sr - c) < 200.0) { sr = c; break; }
            }
        }
        catch (const Ort::Exception&) { sr = 48000.0; }

        modelSr = sr;
        swapVoice (std::move (s));
        return sr;
    }

    void setModelSampleRate (double sr) { modelSr = sr; }
    double getModelSampleRate() const { return modelSr; }

    void swapVoice (std::unique_ptr<SessionHolder> s)
    {
        auto* old = gen_.exchange (s.release());
        if (old)
        {
            // retire after a grace period >> chunk duration (audio thread safety)
            std::thread ([old]() { std::this_thread::sleep_for (std::chrono::milliseconds (400)); delete old; }).detach();
        }
        voiceLoaded_ = true;
    }

    bool voiceReady() const { return voiceLoaded_ && content_ != nullptr; }

    void prepare (double hostSampleRate)
    {
        hostSr = hostSampleRate;
        chunk16 = kCtxFrames * kHop16 + kChunkFrames * kHop16;    // 960
        consume16 = kChunkFrames * kHop16;                        // 640
        opfHost = (int) std::lround (hostSr / 50.0);              // audio samples per frame @host
        tailLen = opfHost;                                        // 1-frame crossfade
        L = (kCtxFrames + kChunkFrames + 1) * (int) std::lround (0.02 * hostSr);
        latency = L - tailLen;                                    // true input->out delay
        delayCap = (size_t) L + (size_t) (kChunkFrames + 3) * opfHost + 64;
        reset();
    }

    int getLatency() const { return latency; }

    void reset()
    {
        rsIn.reset(); rsOut.reset();
        acc16.clear(); f0.reset();
        delay.assign (delayCap, 0.f);
        dWrite = (size_t) tailLen;   // virtual zero tail pre-written
        tail.assign ((size_t) std::max (1, tailLen), 0.f);
        lastF0Hold = 0.f; medianLog = std::log (130.f);
        nIn = 0; nOut = 0;
    }

    void setParams (float pitchSemis, bool targetMode, float targetHz, float intonation)
    {
        semi = pitchSemis; tMode = targetMode; tHz = targetHz; into = intonation;
    }

    // sample-by-sample, mirrors dyv::Engine::process
    float process (float x)
    {
        if (! voiceReady()) return 0.f;

        // host -> 16k
        std::vector<float> tmp;
        rsIn.push (&x, 1, tmp);
        if (! tmp.empty())
        {
            f0.push (tmp.data(), (int) tmp.size());
            acc16.insert (acc16.end(), tmp.begin(), tmp.end());
        }
        nIn++;

        // run steps while enough 16k audio is buffered (keeps latency bounded)
        while ((int) acc16.size() >= chunk16)
        {
            if ((int) acc16.size() > 6 * chunk16) // CPU-starved: shed old audio, recover
                acc16.erase (acc16.begin(), acc16.end() - 4 * chunk16);
            runStep();
        }

        // read from delay line
        const float y = delay[(dWrite + delayCap - (size_t) L) % delayCap];
        nOut++;
        return y;
    }

    int64_t processedIn() const { return nIn; }

private:
    void runStep()
    {
        auto* gen = gen_.load();
        if (gen == nullptr || content_ == nullptr) { acc16.erase (acc16.begin(), acc16.begin() + consume16); return; }

        // ---- window (aligned with the absolute 16k timeline even if buffered)
        const int T = chunk16 / kHop16; // total frames in window (ctx + chunk)
        const int64_t surplus = (int64_t) acc16.size() - chunk16;
        const int64_t winEndAbs = f0.head - surplus;   // absolute 16k index of window end
        std::vector<float> win (acc16.begin(), acc16.begin() + chunk16);

        // ---- content features
        try
        {
            Ort::MemoryInfo mi = Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault);

            // find content input (float, rank 2 or 3)
            int ci = -1;
            for (size_t i = 0; i < content_->io.inNames.size(); ++i)
                if (content_->io.inTypes[i] == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT
                    && (content_->io.inShapes[i].size() == 2 || content_->io.inShapes[i].size() == 3))
                    { ci = (int) i; break; }
            if (ci < 0) { acc16.erase (acc16.begin(), acc16.begin() + consume16); return; }

            // feed matching the declared rank: [1,N] or [1,1,N]
            Ort::Value wT = content_->io.inShapes[(size_t) ci].size() == 3
                ? [&] { std::array<int64_t, 3> wd { 1, 1, (int64_t) win.size() };
                        return Ort::Value::CreateTensor<float> (mi, win.data(), win.size(), wd.data(), 3); } ()
                : [&] { std::array<int64_t, 2> wd { 1, (int64_t) win.size() };
                        return Ort::Value::CreateTensor<float> (mi, win.data(), win.size(), wd.data(), 2); } ();

            const char* inNames[] = { content_->io.inNames[(size_t) ci].c_str() };
            auto out = content_->ses->Run (Ort::RunOptions { nullptr }, inNames, &wT, 1,
                                           content_->io.outPtrs.data(), (size_t) std::min<size_t> (1, content_->io.outPtrs.size()));
            if (out.empty()) { acc16.erase (acc16.begin(), acc16.begin() + consume16); return; }

            auto oi = out[0].GetTensorTypeAndShapeInfo();
            auto osh = oi.GetShape();
            const float* feat = out[0].GetTensorData<float>();
            // normalize to [T, 768]
            int64_t featT = 0, featD = 768;
            if (osh.size() == 3) { featT = osh[1]; featD = osh[2]; }
            else if (osh.size() == 2) { featT = osh[0]; featD = osh[1]; }
            if (featT < T || featD != kFeatDim) { acc16.erase (acc16.begin(), acc16.begin() + consume16); return; }
            const int64_t off = featT - T; // use the LAST T frames

            // ---- per-frame F0 on the 16k window (frames end at hop boundaries)
            std::vector<float> pitchf ((size_t) T, 0.f);
            const float f0thr = 0.42f;
            for (int t = 0; t < T; ++t)
            {
                const int64_t endAbs = winEndAbs - chunk16 + (int64_t) (t + 1) * kHop16;
                pitchf[(size_t) t] = f0.analyse (endAbs, f0thr);
            }
            // hold last voiced value over unvoiced frames (matches RVC training fill)
            for (int t = 0; t < T; ++t)
            {
                if (pitchf[(size_t) t] > 1.f) lastF0Hold = pitchf[(size_t) t];
                else pitchf[(size_t) t] = lastF0Hold;
            }
            // running median (log EWMA) for target alignment
            for (int t = 0; t < T; ++t)
                if (pitchf[(size_t) t] > 1.f)
                    medianLog += 0.004f * (std::log (pitchf[(size_t) t]) - medianLog);

            const float base = tMode ? (float) (tHz / std::exp (medianLog)) : std::pow (2.f, semi / 12.f);
            std::vector<float> f0out ((size_t) T);
            std::vector<int64_t> coarse ((size_t) T);
            for (int t = 0; t < T; ++t)
            {
                const float f = pitchf[(size_t) t];
                float fo = 0.f;
                if (f > 1.f)
                {
                    fo = f * base * std::exp (std::log (f / std::exp (medianLog)) * (into - 1.f));
                    fo = std::min (600.f, std::max (40.f, fo));
                }
                f0out[(size_t) t] = fo;
                coarse[(size_t) t] = coarseOf (fo);
            }

            // ---- generator inputs (adaptive binding)
            std::vector<Ort::Value> feeds;
            std::vector<const char*> feedNames;
            std::vector<int64_t> lengths { T };
            std::vector<int64_t> dsv { 0 };
            std::vector<int64_t> ds2 { 0, 0 };
            for (size_t i = 0; i < gen->io.inNames.size(); ++i)
            {
                const std::string n = lower (gen->io.inNames[i]);
                const auto& sh = gen->io.inShapes[i];
                const auto ty = gen->io.inTypes[i];
                if (n.find ("length") != std::string::npos && ty == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64)
                {
                    feedNames.push_back (gen->io.inNames[i].c_str());
                    const std::array<int64_t, 1> d { 1 };
                    feeds.push_back (Ort::Value::CreateTensor<int64_t> (mi, lengths.data(), 1, d.data(), 1));
                }
                else if (n.find ("pitch") != std::string::npos && n.find ("coarse") == std::string::npos
                         && ty == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
                {
                    feedNames.push_back (gen->io.inNames[i].c_str());
                    feeds.push_back (mk2D (mi, f0out, sh));
                }
                else if (n.find ("pitch") != std::string::npos && ty == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64)
                {
                    feedNames.push_back (gen->io.inNames[i].c_str());
                    feeds.push_back (mk2D (mi, coarse, sh));
                }
                else if ((n.find ("phone") != std::string::npos || n.find ("hidden") != std::string::npos || n.find ("feat") != std::string::npos)
                         && ty == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
                {
                    // slice features to the last T frames, contiguous [T,768]
                    featBuf.assign (feat + off * kFeatDim, feat + (off + T) * kFeatDim);
                    feedNames.push_back (gen->io.inNames[i].c_str());
                    feeds.push_back (mkFeat (mi, featBuf, sh));
                }
                else if ((n.find ("ds") != std::string::npos || n.find ("sid") != std::string::npos || n.find ("spk") != std::string::npos)
                         && ty == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64)
                {
                    feedNames.push_back (gen->io.inNames[i].c_str());
                    if (sh.size() >= 2)
                    {
                        const std::array<int64_t, 2> d { 1, 1 };
                        feeds.push_back (Ort::Value::CreateTensor<int64_t> (mi, ds2.data(), 2, d.data(), 2));
                    }
                    else
                    {
                        const std::array<int64_t, 1> d { 1 };
                        feeds.push_back (Ort::Value::CreateTensor<int64_t> (mi, dsv.data(), 1, d.data(), 1));
                    }
                }
                // unknown inputs ignored -> ORT fills named-only feeds
            }
            (void) ds2; (void) dsv;

            // pad pitchf for unvoiced-with-zero-hold frames (0 Hz) as 0.0 stays 0.0
            auto outs = gen->ses->Run (Ort::RunOptions { nullptr }, feedNames.data(), feeds.data(),
                                       feeds.size(), gen->io.outPtrs.data(), (size_t) std::min<size_t> (1, gen->io.outPtrs.size()));
            if (outs.empty()) { acc16.erase (acc16.begin(), acc16.begin() + consume16); return; }

            // ---- output audio -> host SR
            auto ai = outs[0].GetTensorTypeAndShapeInfo();
            const auto ash = ai.GetShape();
            int64_t M = 1;
            for (auto d : ash) if (d > 1) M = d; // last big dim
            const float* au = outs[0].GetTensorData<float>();
            std::vector<float> emitted;
            rsOut.ratio = hostSr / modelSr;
            rsOut.push (au, (int) M, emitted);

            // ---- keep (chunkFrames+1) frames, crossfade head with previous tail
            const int keep = (kChunkFrames + 1) * opfHost;
            if ((int) emitted.size() >= keep)
            {
                const int startOff = (int) emitted.size() - keep;
                for (int i = 0; i < tailLen && startOff + i < (int) emitted.size(); ++i)
                {
                    const float w = 0.5f - 0.5f * std::cos (kPiF * (float) i / (float) tailLen);
                    emitted[(size_t) (startOff + i)] = tail[(size_t) i] * (1.f - w) + emitted[(size_t) (startOff + i)] * w;
                }
                for (int i = 0; i < tailLen; ++i)
                    tail[(size_t) i] = emitted[(size_t) (startOff + keep - tailLen + i)];

                // write into delay ring: head frame fuses with the previous tail
                // slot (crossfade above), so rewind and keep the ring balanced
                dWrite = dWrite >= (size_t) tailLen ? dWrite - (size_t) tailLen : dWrite;
                for (int i = 0; i < keep; ++i)
                {
                    delay[dWrite % delayCap] = emitted[(size_t) (startOff + i)];
                    ++dWrite;
                }
            }
        }
        catch (const Ort::Exception&) { /* keep silent, pass zeros */ }

        acc16.erase (acc16.begin(), acc16.begin() + consume16);
    }

    template <typename T>
    Ort::Value mk2D (Ort::MemoryInfo& mi, std::vector<T>& v, const std::vector<int64_t>& sh)
    {
        // feed [1,T] (fall back to declared rank-1 if the export wants that)
        if (sh.size() == 1)
        {
            std::array<int64_t, 1> d { (int64_t) v.size() };
            return Ort::Value::CreateTensor<T> (mi, v.data(), v.size(), d.data(), 1);
        }
        std::array<int64_t, 2> d { 1, (int64_t) v.size() };
        return Ort::Value::CreateTensor<T> (mi, v.data(), v.size(), d.data(), 2);
    }

    Ort::Value mkFeat (Ort::MemoryInfo& mi, std::vector<float>& v, const std::vector<int64_t>& sh)
    {
        if (sh.size() == 2)
        {
            std::array<int64_t, 2> d { (int64_t) v.size() / kFeatDim, kFeatDim };
            return Ort::Value::CreateTensor<float> (mi, v.data(), v.size(), d.data(), 2);
        }
        std::array<int64_t, 3> d { 1, (int64_t) v.size() / kFeatDim, kFeatDim };
        return Ort::Value::CreateTensor<float> (mi, v.data(), v.size(), d.data(), 3);
    }

private:
    std::unique_ptr<SessionHolder> content_;
    std::atomic<SessionHolder*> gen_ { nullptr };
    std::atomic<bool> voiceLoaded_ { false };
    int threads_ = 2;

    double hostSr = 48000.0, modelSr = 48000.0;
    int chunk16 = 960, consume16 = 640, opfHost = 960, tailLen = 960, L = 3840, latency = 2880;
    size_t delayCap = 7744;

    Resamp rsIn, rsOut;
    std::vector<float> acc16, delay, tail;
    size_t dWrite = 0;
    F0Detector f0;
    float lastF0Hold = 0.f, medianLog = std::log (130.f);
    float semi = 4.f, tHz = 200.f, into = 1.f;
    bool tMode = false;
    int64_t nIn = 0, nOut = 0;
    std::vector<float> featBuf;
};

#else // !DYV_NEURAL

// stub so the plugin still compiles without ONNX Runtime
class NeuralEngine
{
public:
    struct VoiceInfo { std::string name, path; };
    static std::vector<VoiceInfo> scanVoices (const std::string&) { return {}; }
    bool loadContent (const std::string&, int) { return false; }
    bool contentReady() const { return false; }
    double loadVoice (const std::string&) { return 0.0; }
    void setModelSampleRate (double) {}
    double getModelSampleRate() const { return 0; }
    void swapVoice (void*) {}
    bool voiceReady() const { return false; }
    void prepare (double) {}
    int getLatency() const { return 0; }
    void reset() {}
    void setParams (float, bool, float, float) {}
    float process (float x) { return x; }
};

#endif // DYV_NEURAL
} // namespace nvf

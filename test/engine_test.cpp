// engine_test.cpp — offline validation suite for VoiceFemEngine (no JUCE needed).
// Build: g++ -O2 -std=c++17 test/engine_test.cpp -o t && ./t   (exit code 0 = all pass)
#include "../Source/VoiceFemEngine.h"
#include <cstdio>
#include <chrono>
#include <random>
#include <cstring>
using namespace vfem;

static int failures = 0;

static void check (bool ok, const char* name, const char* info = "")
{
    printf ("[%s] %s%s%s\n", ok ? "PASS" : "FAIL", name, info[0] ? "  " : "", info);
    if (! ok) ++failures;
}

// ---------------------------------------------------------- synthetic voice
struct Res
{
    double y1 = 0, y2 = 0;
    double process (double x, double f, double sr, double bw)
    {
        const double r = std::exp (-kPi * bw / sr), th = 2 * kPi * f / sr;
        const double A1 = -2 * r * std::cos (th), A2 = r * r;
        const double y = (1 - r) * x - A1 * y1 - A2 * y2;
        y2 = y1; y1 = y;
        return y;
    }
};

// male-ish vowel: 110 Hz + vibrato, formants 650/1250/2600; ampEnvelope for on/off
struct VoiceGen
{
    Res r1, r2, r3;
    double ph = 0, sr = 48000;
    double f0base = 110, vib = 0.03;
    bool  amplitudeMod = false;

    float step (int i)
    {
        const double t = i / sr;
        double f0 = f0base * (1 + vib * std::sin (2 * kPi * 5 * t));
        ph += f0 / sr;
        double imp = 0;
        if (ph >= 1) { ph -= 1; imp = 1; }
        const double src = imp * sr / f0 * 0.02;
        double y = r1.process (src, 650, sr, 80) + 0.7 * r2.process (src, 1250, sr, 100) + 0.4 * r3.process (src, 2600, sr, 150);
        if (amplitudeMod)
        {
            const double gate = 0.5 * (1 + std::tanh (std::sin (2 * kPi * 0.8 * t) * 6)); // speech-like on/off
            y *= gate;
        }
        return (float) (y * 0.5);
    }
};

static double estF0 (const std::vector<float>& x, int s, int len, double sr)
{
    double best = 0; int bl = 0;
    double e0 = 0;
    for (int i = 0; i < len; i++) e0 += x[s + i] * x[s + i];
    for (int lag = (int) (sr / 400); lag < (int) (sr / 70); lag++)
    {
        double a = 0, e1 = 0;
        for (int i = 0; i < len - lag; i++) { a += x[s + i] * x[s + i + lag]; e1 += x[s + i + lag] * x[s + i + lag]; }
        const double sc = a / std::sqrt (e0 * e1 + 1e-12);
        if (sc > best) { best = sc; bl = lag; }
    }
    return bl ? sr / bl : 0.0;
}

static double centroid (const std::vector<float>& x, int s, int len, double sr)
{
    double num = 0, den = 0;
    for (double f = 100; f < 5000; f += 25)
    {
        double re = 0, im = 0;
        for (int i = 0; i < len; i++)
        {
            const double w = 0.5 - 0.5 * std::cos (2 * kPi * i / len);
            re += w * x[s + i] * std::cos (2 * kPi * f * i / sr);
            im += w * x[s + i] * std::sin (2 * kPi * f * i / sr);
        }
        const double p = re * re + im * im;
        num += f * p; den += p;
    }
    return num / (den + 1e-12);
}

static double bandRms (const std::vector<float>& x, int s, int len, double sr, double f1, double f2)
{
    double num = 0, den = 0;
    for (double f = f1; f < f2; f += 20)
    {
        double re = 0, im = 0;
        for (int i = 0; i < len; i++)
        {
            const double w = 0.5 - 0.5 * std::cos (2 * kPi * i / len);
            re += w * x[s + i] * std::cos (2 * kPi * f * i / sr);
            im += w * x[s + i] * std::sin (2 * kPi * f * i / sr);
        }
        const double p = re * re + im * im;
        num += p; den += 1;
    }
    return std::sqrt (num / (den * len));
}

static bool allFinite (const std::vector<float>& x) { for (float v : x) if (! std::isfinite (v)) return false; return true; }
static double peakOf (const std::vector<float>& x) { double m = 0; for (float v : x) m = std::max (m, (double) std::fabs (v)); return m; }

static std::vector<float> run (const std::vector<float>& inx, const Params& P, double sr, float minHz, int* lat = nullptr)
{
    Engine e; e.prepare (sr, minHz); e.setParams (P, true);
    if (lat) *lat = e.getLatency();
    std::vector<float> y (inx.size());
    for (size_t i = 0; i < inx.size(); i++) y[i] = e.process (inx[i]);
    return y;
}

// ------------------------------------------------------------------ tests
int main()
{
    const double sr = 48000;
    const int N = (int) (sr * 3);

    VoiceGen gen;
    std::vector<float> x (N);
    for (int i = 0; i < N; i++) x[i] = gen.step (i);
    double mx = 0;
    for (float v : x) mx = std::max (mx, (double) std::fabs (v));
    for (auto& v : x) v = (float) (v / mx * 0.5);

    const int s = (int) (sr * 1.5), L = 4096;
    const double f0in = estF0 (x, s, L, sr);
    const double cin = centroid (x, s, L, sr);

    Params base;
    base.natural = 0.5f; base.deEss = 0.3f; base.breath = 0.08f; base.bodyDb = -2.f; base.airDb = 2.5f;

    // ---------------------------------------------------------- basic configs
    {
        Params P = base; P.pitchSemis = 4; P.formantSemis = 0;
        int lat = 0;
        auto y = run (x, P, sr, 80, &lat);
        check (allFinite (y), "cfg pitch+4: finite");
        const double f0out = estF0 (y, s + lat, L, sr);
        char buf[128];
        snprintf (buf, sizeof (buf), "F0 in=%.1f out=%.1f (want ~%.1f)", f0in, f0out, f0in * std::pow (2, 4.0 / 12));
        check (std::fabs (f0out - f0in * std::pow (2, 4.0 / 12)) < f0in * std::pow (2, 4.0 / 12) * 0.04, "cfg pitch+4: F0 tracked", buf);

        Params P2 = base; P2.pitchSemis = 4; P2.formantSemis = 4;
        auto y2 = run (x, P2, sr, 80);
        const double cout = centroid (y2, s, L, sr);
        snprintf (buf, sizeof (buf), "centroid in=%.0f out=%.0f", cin, cout);
        check (cout > cin * 1.12, "cfg pitch+4 form+4: centroid up", buf);
        check (peakOf (y2) < 2.0, "cfg pitch+4 form+4: bounded");
    }

    // ------------------------------------------------------ target mode F0
    {
        Params P = base; P.targetMode = true; P.targetHz = 200; P.formantSemis = 2;
        int lat = 0;
        auto y = run (x, P, sr, 80, &lat);
        const double f0out = estF0 (y, s + lat, L, sr);
        char buf[96];
        snprintf (buf, sizeof (buf), "F0 out=%.1f (want ~200)", f0out);
        check (std::fabs (f0out - 200.0) < 10.0, "target 200 Hz: converged", buf);
    }

    // ------------------------------------------------------------ silence
    {
        std::vector<float> z (48000 * 2, 0.f);
        auto y = run (z, base, sr, 80);
        double p = 0;
        for (float v : y) p = std::max (p, (double) std::fabs (v));
        check (p == 0.0, "silence -> exact silence", "");
    }

    // ------------------------------------------ unvoiced passthrough (dry)
    {
        std::mt19937 g (7);
        std::normal_distribution<double> nd (0, 1);
        const int M = (int) (sr * 1);
        std::vector<float> nz (M);
        // sibilant-like: HP noise ~4-10 kHz via simple 1st-order diff cascade
        double p1 = 0, p2 = 0;
        for (int i = 0; i < M; i++)
        {
            const double w = nd (g);
            const double d1 = w - p1, d2 = d1 - p2;
            p1 = w; p2 = d1;
            nz[i] = (float) (d2 * 0.12);
        }
        Params P = base;
        P.pitchSemis = 8; P.formantSemis = 6; P.bodyDb = 0; P.airDb = 0; P.breath = 0;
        P.natural = 0; P.deEss = 0; P.mix = 1; P.outDb = 0;   // pure passthrough conditions
        int lat = 0;
        auto y = run (nz, P, sr, 80, &lat);
        const int st = (int) (sr * 0.5), len = (int) (sr * 0.3);
        double dot = 0, e1 = 0, e2 = 0;
        for (int i = 0; i < len; i++)
        {
            const double a = nz[st + i], b = y[st + i + lat];
            dot += a * b; e1 += a * a; e2 += b * b;
        }
        const double corr = dot / std::sqrt (e1 * e2 + 1e-30);
        char buf[64];
        snprintf (buf, sizeof (buf), "corr=%.4f", corr);
        check (corr > 0.97, "unvoiced passthrough delay-matched", buf);
    }

    // ------------------------------------------------------- de-esser action
    {
        VoiceGen g2; g2.vib = 0.0;
        const int M = (int) (sr * 2);
        std::vector<float> v (M);
        for (int i = 0; i < M; i++)
        {
            float a = g2.step (i);
            const double t = i / sr;
            if (t > 0.8 && t < 1.4) a += (float) (0.30 * std::sin (2 * kPi * 6400 * t)); // fake /s/
            v[i] = a;
        }
        Params P = base; P.breath = 0; P.bodyDb = 0; P.airDb = 0;
        P.deEss = 0.f;
        auto y0 = run (v, P, sr, 80);
        P.deEss = 1.f;
        auto y1 = run (v, P, sr, 80);
        const int sb = (int) (sr * 1.0), sl = (int) (sr * 0.3);
        const double r0 = bandRms (y0, sb + 0, sl, sr, 5600, 7400);
        const double r1 = bandRms (y1, sb + 0, sl, sr, 5600, 7400);
        char buf[96];
        snprintf (buf, sizeof (buf), "sibilant band rms: deEss0=%.3f deEss1=%.3f (-%.0f%%)", r0, r1, 100 * (1 - r1 / r0));
        check (r1 < r0 * 0.85, "de-esser reduces sibilance band", buf);
        const double low0 = bandRms (y0, sb, sl, sr, 300, 1500);
        const double low1 = bandRms (y1, sb, sl, sr, 300, 1500);
        check (std::fabs (low1 - low0) < low0 * 0.06, "de-esser keeps low band intact");
    }

    // ---------------------------------------------------------- breath action
    {
        VoiceGen g3; g3.vib = 0.0;
        const int M = (int) (sr * 2);
        std::vector<float> v (M);
        for (int i = 0; i < M; i++) v[i] = g3.step (i);
        Params P = base; P.bodyDb = 0; P.airDb = 0; P.deEss = 0; P.breath = 0.f;
        auto y0 = run (v, P, sr, 80);
        P.breath = 0.35f;
        auto y1 = run (v, P, sr, 80);
        const int sb = (int) (sr * 1.0), sl = (int) (sr * 0.5);
        const double r0 = bandRms (y0, sb, sl, sr, 2000, 7000);
        const double r1 = bandRms (y1, sb, sl, sr, 2000, 7000);
        char buf[96];
        snprintf (buf, sizeof (buf), "HF rms: breath0=%.4f breath1=%.4f", r0, r1);
        check (r1 > r0 * 1.15, "breath adds airy HF noise", buf);
    }

    // ------------------------------------------- param automation stress test
    {
        VoiceGen g4; g4.amplitudeMod = true;
        const int M = (int) (sr * 8);
        std::vector<float> v (M);
        for (int i = 0; i < M; i++) v[i] = g4.step (i);

        Engine e; e.prepare (sr, 65); // lowest range = biggest buffers
        std::mt19937 g (42);
        std::uniform_real_distribution<float> U (0.f, 1.f);
        bool finite = true;
        double pmax = 0;
        Params P = base;
        for (int i = 0; i < M; i++)
        {
            if (i % 480 == 0) // random parameter jumps ~ every 10 ms
            {
                P.pitchSemis = -12 + 24 * U (g);
                P.formantSemis = -6 + 14 * U (g);
                P.targetMode = (U (g) < 0.5f);
                P.targetHz = 100 + 300 * U (g);
                P.intonation = 1.5f * U (g);
                P.breath = U (g);
                P.bodyDb = -12 + 18 * U (g);
                P.airDb = -6 + 18 * U (g);
                P.mix = U (g);
                P.outDb = -12 + 24 * U (g);
                P.voicingThresh = 0.3f + 0.55f * U (g);
                P.natural = U (g);
                P.deEss = U (g);
                e.setParams (P);
            }
            const float out = e.process (v[i]);
            if (! std::isfinite (out)) { finite = false; break; }
            pmax = std::max (pmax, (double) std::fabs (out));
        }
        char buf[64];
        snprintf (buf, sizeof (buf), "peak=%.2f", pmax);
        check (finite && pmax < 10.0, "automation stress: finite & bounded", buf);
    }

    // ------------------------------------------------- multiple sample rates
    for (double sr2 : { 44100.0, 96000.0 })
    {
        VoiceGen g5; g5.sr = sr2;
        const int M = (int) (sr2 * 1.5);
        std::vector<float> v (M);
        for (int i = 0; i < M; i++) v[i] = g5.step (i);
        auto y = run (v, base, sr2, 80);
        char buf[64];
        snprintf (buf, sizeof (buf), "sr=%.0f peak=%.2f", sr2, peakOf (y));
        check (allFinite (y) && peakOf (y) < 2.5, "sample rate robustness", buf);
    }

    // ------------------------------------------------------------ CPU budget
    {
        Engine e; e.prepare (sr, 65);
        Params P = base; P.pitchSemis = 6; P.formantSemis = 3; P.targetMode = true;
        e.setParams (P, true);
        const int M = (int) (sr * 10);
        std::vector<float> v (M);
        VoiceGen g6; g6.amplitudeMod = true;
        for (int i = 0; i < M; i++) v[i] = g6.step (i);
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < M; i++) e.process (v[i]);
        const double ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();
        char buf[64];
        snprintf (buf, sizeof (buf), "cpu=%.1f%% of realtime (single core)", ms / 100.0);
        check (ms < 5000, "cpu budget < 50% realtime", buf);
    }

    // ------------------------------------------------------- latency report
    {
        Engine e; e.prepare (sr, 80);
        check (e.getLatency() > 1400 && e.getLatency() < 1800, "latency ~32 ms @48k/80Hz", "");
    }

    printf ("\n%s (%d failures)\n", failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", failures);
    return failures == 0 ? 0 : 1;
}

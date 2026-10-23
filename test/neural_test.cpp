// neural_test.cpp — streaming plumbing validation for NeuralVC (no JUCE).
// Uses stub ONNX models produced by tools/make_stubs.py.
// Build (see run command in README or CI): needs onnxruntime headers + lib.
#include "../Source/NeuralVC.h"
#include <cstdio>
#include <random>

static int failures = 0;
static void check (bool ok, const char* name, const char* info = "")
{
    printf ("[%s] %s%s%s\n", ok ? "PASS" : "FAIL", name, info[0] ? "  " : "", info);
    if (! ok) ++failures;
}

static bool finite (const std::vector<float>& x) { for (float v : x) if (! std::isfinite (v)) return false; return true; }
static double peak (const std::vector<float>& x) { double m = 0; for (float v : x) m = std::max (m, (double) std::fabs (v)); return m; }

int main (int argc, char** argv)
{
    std::string dir = argc > 1 ? argv[1] : ".";
    const std::string content = dir + "/content.onnx";
    const std::string alpha = dir + "/voice_alpha.onnx";
    const std::string beta = dir + "/voice_beta.onnx";

    nvf::NeuralEngine e;
    check (e.loadContent (content, 2), "content model loads");
    check (! e.loadContent (dir + "/missing.onnx", 2), "missing model rejected");

    const double sr = e.loadVoice (alpha);
    char buf[64];
    snprintf (buf, sizeof (buf), "probed SR=%.0f", sr);
    check (sr == 48000.0, "generator probe detects 48k", buf);
    check (e.voiceReady(), "engine ready");

    e.prepare (48000.0);
    const int lat = e.getLatency();
    snprintf (buf, sizeof (buf), "latency=%d samples (%.1f ms)", lat, lat / 48.0);
    check (lat > 2000 && lat < 4500, "latency in sane 40-95 ms range", buf);

    // feed a synthetic voiced-ish signal for 6 s, sample by sample
    const int N = 48000 * 6;
    std::vector<float> x (N);
    for (int i = 0; i < N; i++)
    {
        const double t = i / 48000.0;
        const double f0 = 110.0 * (1 + 0.02 * std::sin (2 * 3.14159265 * 3 * t));
        x[i] = (float) (0.4 * std::sin (2 * 3.14159265 * f0 * t) + 0.1 * std::sin (2 * 3.14159265 * f0 * 2 * t));
    }

    e.setParams (4.f, false, 200.f, 1.f);
    std::vector<float> y (N);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; i++) y[i] = e.process (x[i]);
    const double ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();

    snprintf (buf, sizeof (buf), "cpu=%.1f%% realtime", ms / 120.0);
    check (finite (y), "output finite", buf);
    check (peak (y) > 1e-4, "output non-silent");

    // balance: input and output advance at the same rate (no drift)
    // verify by feeding 4 more seconds and checking tail energy is present
    std::vector<float> y2 (48000 * 2);
    for (int i = 0; i < 48000 * 2; i++) y2[i] = e.process (x[i % N]);
    check (finite (y2) && peak (y2) > 1e-4, "long-run balance (no drift/underrun)");

    // voice switch: different generator weights -> different output, still stable
    const double sr2 = e.loadVoice (beta);
    check (sr2 == 48000.0, "second voice loads (switch works)");
    e.reset();
    std::vector<float> y3 (48000);
    for (int i = 0; i < 48000; i++) y3[i] = e.process (x[i]);
    double diff = 0, e1 = 0, e2 = 0;
    const int off = lat + 4800;
    for (int i = 0; i < 48000 - off; i++)
    {
        const double a = y[(size_t) (off + i)], b = y3[(size_t) (off + i)];
        diff += (a - b) * (a - b); e1 += a * a; e2 += b * b;
    }
    snprintf (buf, sizeof (buf), "rel diff=%.3f", std::sqrt (diff / (e1 + 1e-30)));
    check (std::sqrt (diff) / std::sqrt (e1 + 1e-30) > 0.05, "voice switch changes timbre", buf);
    check (finite (y3), "post-switch finite");

    printf ("\n%s (%d failures)\n", failures == 0 ? "ALL NEURAL TESTS PASSED" : "NEURAL TESTS FAILED", failures);
    return failures == 0 ? 0 : 1;
}

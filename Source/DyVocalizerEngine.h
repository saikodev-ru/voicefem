// DyVocalizerEngine.h
// Real-time pitch + formant voice processor (no JUCE dependency).
//
// Approach: pitch-synchronous overlap-add (PSOLA).
//  * Pitch is tracked with a decimated NSDF (McLeod-style) detector hardened
//    with a median filter and octave-jump protection -> rock-solid, no bubbling.
//  * Analysis "marks" are placed one period apart and refined by waveform
//    cross-correlation, so grains always start at the same phase of the cycle.
//  * Output grains are placed at spacing T0/pitchRatio -> pitch changes
//    WITHOUT touching the spectral envelope (formants stay put = no "chipmunk").
//  * Each grain is additionally resampled by the formant ratio -> formants move
//    independently of pitch; the ratio glides smoothly (no timbre steps).
//  * Correlated jitter/shimmer (Natural) and dual-band turbulence breath add
//    organic realism instead of a sterile, robotic tone.
//  * Unvoiced sounds (s, f, sh, breaths) bypass the grain engine and pass
//    through delay-matched, so consonants stay crisp and natural.
//  * Clean-up chain: presence bell tied to Air, split-band de-esser and a soft
//    ceiling limiter keep the output smooth and radio-ready.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace dyv
{
constexpr double kPi = 3.14159265358979323846;

struct Biquad
{
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;

    void reset() { z1 = z2 = 0; }

    float process (float x)
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    void set (double nb0, double nb1, double nb2, double na0, double na1, double na2)
    {
        b0 = (float) (nb0 / na0);
        b1 = (float) (nb1 / na0);
        b2 = (float) (nb2 / na0);
        a1 = (float) (na1 / na0);
        a2 = (float) (na2 / na0);
    }

    void lowShelf (double sr, double f0, double dB)
    {
        const double A = std::pow (10.0, dB / 40.0), w = 2 * kPi * f0 / sr;
        const double c = std::cos (w), s = std::sin (w);
        const double al = s / 2.0 * std::sqrt (2.0); // shelf slope S = 1
        const double sA = 2.0 * std::sqrt (A) * al;
        set (A * ((A + 1) - (A - 1) * c + sA), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - sA),
             (A + 1) + (A - 1) * c + sA, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sA);
    }

    void highShelf (double sr, double f0, double dB)
    {
        const double A = std::pow (10.0, dB / 40.0), w = 2 * kPi * f0 / sr;
        const double c = std::cos (w), s = std::sin (w);
        const double al = s / 2.0 * std::sqrt (2.0);
        const double sA = 2.0 * std::sqrt (A) * al;
        set (A * ((A + 1) + (A - 1) * c + sA), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - sA),
             (A + 1) - (A - 1) * c + sA, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - sA);
    }

    void peaking (double sr, double f0, double dB, double q)
    {
        const double A = std::pow (10.0, dB / 40.0), w = 2 * kPi * f0 / sr;
        const double c = std::cos (w), s = std::sin (w), al = s / (2 * q);
        set (1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
    }

    void highPass (double sr, double f0, double q = 0.707)
    {
        const double w = 2 * kPi * f0 / sr, c = std::cos (w), al = std::sin (w) / (2 * q);
        set ((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
    }

    void lowPass (double sr, double f0, double q = 0.707)
    {
        const double w = 2 * kPi * f0 / sr, c = std::cos (w), al = std::sin (w) / (2 * q);
        set ((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
    }

    void bandPass (double sr, double f0, double q)
    {
        const double w = 2 * kPi * f0 / sr, c = std::cos (w), al = std::sin (w) / (2 * q);
        set (al, 0, -al, 1 + al, -2 * c, 1 - al);
    }
};

// one-pole DC blocker (removes mic offset before analysis & output)
struct DCBlock
{
    float r = 0.998f, x1 = 0.f, y1 = 0.f;
    void reset() { x1 = y1 = 0.f; }
    float process (float x)
    {
        const float y = x - x1 + r * y1;
        x1 = x;
        y1 = y;
        return y;
    }
};

// soft-knee ceiling: transparent below 0.72, asymptote at 0.985 -> no harsh clipping
inline float softClip (float x)
{
    const float t = 0.72f, c = 0.985f;
    const float a = std::fabs (x);
    if (a <= t) return x;
    const float s = (a - t) / (c - t);
    return std::copysign (t + (c - t) * (1.f - std::exp (-s * 2.f)), x);
}

struct Params
{
    float pitchSemis = 4.f;     // pitch shift in semitones (Shift mode)
    float formantSemis = 2.f;   // formant shift in semitones
    bool  targetMode = false;   // true: aim at targetHz instead of fixed shift
    float targetHz = 200.f;     // average target pitch (Target mode)
    float intonation = 1.f;     // 0 = monotone, 1 = natural, >1 = more expressive
    float breath = 0.08f;       // 0..1 airy noise on voiced parts
    float bodyDb = -2.f;        // low shelf @250 Hz (negative = less chesty)
    float airDb = 2.5f;         // high shelf @5 kHz (+ presence bell @3 kHz)
    float mix = 1.f;            // 0 = dry (delay-matched), 1 = wet
    float outDb = 0.f;          // output gain
    float voicingThresh = 0.55f;// pitch-detector confidence threshold
    float natural = 0.5f;       // 0..1 organic jitter/shimmer amount
    float deEss = 0.3f;         // 0..1 sibilance taming (~6.4 kHz)
};

class Engine
{
public:
    Engine()
        : in (BUF, 0.f), outAcc (OBUF, 0.f), wAcc (OBUF, 0.f), dec (DBUF, 0.f),
          tmp (2048, 0.f), nsdf (2048, 0.f), ref (4096, 0.f)
    {
    }

    void prepare (double sampleRate, float minPitchHz)
    {
        sr = sampleRate;
        minHz = std::max (40.f, minPitchHz);
        D = (int) std::lround (2.6 * sr / minHz);
        dsf = std::max (1, (int) std::lround (sr / 12000.0));
        dsr = sr / dsf;
        hop = std::max (32, (int) std::lround (sr / 400.0));
        defaultT0 = (float) (sr * 0.008);
        dcIn.r = 0.998f;    // ~15 Hz @48k: mic offset out, voice intact
        dcOut.r = 0.9995f;  // ~4 Hz: only infra-bass removed
        bpBreath.bandPass (sr, 2500.0, 0.55);
        lpBreath.lowPass (sr, 900.0, 0.707);
        deBp.bandPass (sr, 6400.0, 2.2);
        envAtt = (float) std::exp (-1.0 / (0.002 * sr));
        envRel = (float) std::exp (-1.0 / (0.030 * sr));
        deAtt = (float) std::exp (-1.0 / (0.001 * sr));
        deRel = (float) std::exp (-1.0 / (0.050 * sr));
        reset();
        setParams (P, true);
    }

    int getLatency() const { return D; }

    void reset()
    {
        std::fill (in.begin(), in.end(), 0.f);
        std::fill (outAcc.begin(), outAcc.end(), 0.f);
        std::fill (wAcc.begin(), wAcc.end(), 0.f);
        std::fill (dec.begin(), dec.end(), 0.f);
        n = 0; decN = 0; decAcc = 0; decCnt = 0; hopCnt = 0; hopStamp = 0;
        nm = 0; lastMark = 0;
        T0s = (float) (sr / 150.0);
        vS = 0.f; havePitch = false;
        avgLog = std::log (130.f); avgCount = 0;
        rawCnt = 0; rawIdx = 0; octCnt = 0; lastRawStamp = 0;
        nextSynth = D; prevRh = (int) defaultT0;
        env = 0.f; tw = 0.f;
        deEnv = 0.f; deS = 1.f;
        jw = 0.f; shw = 0.f;
        formRatioS = formRatio;
        mixS = P.mix; gainS = outGain;
        dcIn.reset(); dcOut.reset();
        lowS.reset(); highS.reset(); presence.reset();
        bpBreath.reset(); lpBreath.reset(); deBp.reset();
        rng = 22222u;
    }

    void setParams (const Params& np, bool force = false)
    {
        if (force || np.bodyDb != P.bodyDb) lowS.lowShelf (sr, 250.0, np.bodyDb);
        if (force || np.airDb != P.airDb)
        {
            highS.highShelf (sr, 5000.0, np.airDb);
            // presence bell rides with Air: feminine clarity around 3 kHz
            const double bell = std::min (4.0, std::max (-3.0, 0.4 * (double) np.airDb));
            presence.peaking (sr, 3000.0, bell, 1.0);
        }
        P = np;
        semiRatio = std::pow (2.f, P.pitchSemis / 12.f);
        formRatio = std::pow (2.f, P.formantSemis / 12.f);
        if (force) formRatioS = formRatio;
        outGain = std::pow (10.f, P.outDb / 20.f);
    }

    float process (float x)
    {
        x = dcIn.process (x);
        in[n & MASK] = x;

        decAcc += x;
        if (++decCnt >= dsf)
        {
            dec[decN & DMASK] = decAcc / (float) dsf;
            ++decN; decAcc = 0; decCnt = 0;
        }
        if (++hopCnt >= hop)
        {
            hopCnt = 0;
            ++hopStamp;
            estimatePitch();
            generateMarks();
        }

        scheduleGrains();

        const int64_t o = n & OM;
        const float wet = outAcc[o];
        const float w = std::min (1.f, std::max (0.f, wAcc[o]));
        outAcc[o] = 0.f; wAcc[o] = 0.f;
        const float dry = in[(n - D) & MASK];

        float y = wet + dry * (1.f - w); // unvoiced parts pass through

        // formant ratio glide (~40 ms) -> no timbre steps when turning the knob
        formRatioS += 0.0005f * (formRatio - formRatioS);

        // breath: dual-band turbulence noise, gated by the grain weight
        const float a = std::fabs (y);
        env = a > env ? envAtt * env + (1.f - envAtt) * a : envRel * env + (1.f - envRel) * a;
        if (env < 1e-12f) env = 0.f;
        tw += 0.00025f * (((rand01() - 0.5f) * 0.5f) - tw); // slow turbulence sway
        {
            const float nz = rand01() * 2.f - 1.f;
            const float nAir = 0.75f * bpBreath.process (nz) + 0.30f * lpBreath.process (nz);
            y += P.breath * 2.0f * env * w * nAir * (1.f + tw);
        }

        y = presence.process (highS.process (lowS.process (y)));

        mixS += 0.002f * (P.mix - mixS);
        float v = mixS * y + (1.f - mixS) * dry;

        // split-band de-esser: dynamically subtract sibilance energy (~6.4 kHz)
        {
            const float band = deBp.process (v);
            const float rect = std::fabs (band);
            deEnv = rect > deEnv ? deAtt * deEnv + (1.f - deAtt) * rect
                                 : deRel * deEnv + (1.f - deRel) * rect;
            if (deEnv < 1e-12f) deEnv = 0.f;
            float red = 1.f;
            if (deEnv > 0.05f && P.deEss > 0.001f)
                red = 1.f - P.deEss * std::min (1.f, (deEnv - 0.05f) / deEnv * 1.6f) * 0.85f;
            deS += (red < deS ? 0.012f : 0.0008f) * (red - deS); // fast att / slow rel
            v -= (1.f - deS) * band;
        }

        v = dcOut.process (v);

        gainS += 0.002f * (outGain - gainS);
        const float out = softClip (v * gainS);

        ++n;
        return out;
    }

private:
    static constexpr int BUF = 1 << 16, MASK = BUF - 1;
    static constexpr int OBUF = 1 << 14, OM = OBUF - 1;
    static constexpr int DBUF = 1 << 13, DMASK = DBUF - 1;
    static constexpr int MN = 512, MM = MN - 1;

    float rand01()
    {
        rng = rng * 1664525u + 1013904223u;
        return (float) (rng >> 8) / 16777216.f;
    }

    // ---------------------------------------------------------------- pitch
    void estimatePitch()
    {
        const int minLag = std::max (2, (int) std::floor (dsr / 600.0));
        const int maxLag = std::min (700, (int) std::ceil (dsr / minHz) + 2);
        const int W = std::min (512, std::max (256, (int) (2.2 * dsr / minHz)));
        const int span = W + maxLag + 2;
        if (decN < span) return;

        const int64_t e = decN - span;
        for (int j = 0; j < span; ++j) tmp[(size_t) j] = dec[(size_t) ((e + j) & DMASK)];

        double e0 = 0;
        for (int j = 0; j < W; ++j) e0 += (double) tmp[(size_t) j] * tmp[(size_t) j];

        float conf = 0.f, lag = 0.f;
        if (e0 / W > 1e-8)
        {
            double es = 0;
            for (int j = 0; j < W; ++j) es += (double) tmp[(size_t) (1 + j)] * tmp[(size_t) (1 + j)];
            for (int tau = 1; tau <= maxLag + 1; ++tau)
            {
                double acf = 0;
                for (int j = 0; j < W; ++j) acf += (double) tmp[(size_t) j] * tmp[(size_t) (j + tau)];
                nsdf[(size_t) tau] = (float) (2.0 * acf / (e0 + es + 1e-20));
                es += (double) tmp[(size_t) (W + tau)] * tmp[(size_t) (W + tau)]
                    - (double) tmp[(size_t) tau] * tmp[(size_t) tau];
            }

            // McLeod peak picking
            struct Peak { int tau; float v; };
            Peak peaks[32]; int np = 0; float gmax = 0.f;
            int tau = 1;
            while (tau <= maxLag && nsdf[(size_t) tau] > 0.f) ++tau; // skip first lobe
            while (tau <= maxLag && np < 32)
            {
                while (tau <= maxLag && nsdf[(size_t) tau] <= 0.f) ++tau;
                if (tau > maxLag) break;
                int best = tau;
                while (tau <= maxLag && nsdf[(size_t) tau] > 0.f)
                {
                    if (nsdf[(size_t) tau] > nsdf[(size_t) best]) best = tau;
                    ++tau;
                }
                if (best >= minLag) { peaks[np++] = { best, nsdf[(size_t) best] }; gmax = std::max (gmax, nsdf[(size_t) best]); }
            }
            for (int i = 0; i < np; ++i)
            {
                if (peaks[i].v >= 0.88f * gmax)
                {
                    const int t = peaks[i].tau;
                    const float y0 = nsdf[(size_t) (t - 1)], y1 = nsdf[(size_t) t], y2 = nsdf[(size_t) (t + 1)];
                    const float den = y0 - 2 * y1 + y2;
                    const float d = std::fabs (den) > 1e-9f ? 0.5f * (y0 - y2) / den : 0.f;
                    lag = (float) t + std::max (-1.f, std::min (1.f, d));
                    conf = y1 - 0.25f * (y0 - y2) * d;
                    break;
                }
            }
        }

        // hysteresis voicing band (state-dependent -> no flutter at the threshold)
        const float thrHi = P.voicingThresh;
        const float thrLo = std::max (0.15f, P.voicingThresh - 0.08f);
        const float band0 = vS > 0.45f ? thrLo : thrHi;
        const float vTarget = std::min (1.f, std::max (0.f, (conf - (band0 - 0.08f)) / 0.16f));

        if (vTarget > 0.f && lag > 0.f)
        {
            const float newT0 = lag * (float) dsf;
            if (hopStamp - lastRawStamp > 16) rawCnt = 0; // history stale -> restart
            lastRawStamp = hopStamp;
            raw[rawIdx % 3] = newT0;
            if (rawCnt < 3) ++rawCnt;
            ++rawIdx;

            float med = newT0;
            if (rawCnt >= 3)
            {
                const float a = raw[0], b = raw[1], c = raw[2];
                med = a + b + c - std::max (a, std::max (b, c)) - std::min (a, std::min (b, c));
            }

            // octave-jump guard: a >2/3-octave step must persist 3 frames (~7.5 ms)
            bool accept = true;
            if (havePitch && rawCnt >= 3)
            {
                const float r = std::fabs (std::log2 (med / T0s));
                if (r > 0.65f) { if (++octCnt < 3) accept = false; else octCnt = 0; }
                else octCnt = 0;
            }
            if (accept)
            {
                if (! havePitch) { T0s = med; havePitch = true; }
                else T0s += 0.3f * (med - T0s);
            }
        }
        vS += (vTarget > vS ? 0.6f : 0.3f) * (vTarget - vS);

        if (vS > 0.5f && havePitch)
        {
            const float lf = std::log ((float) sr / T0s);
            const float base = (float) hop / (1.5f * (float) sr);
            const float al = std::max (base, 1.f / (float) (avgCount + 1));
            avgLog += al * (lf - avgLog);
            if (avgCount < 100000) ++avgCount;
        }
    }

    // ---------------------------------------------------------------- marks
    void generateMarks()
    {
        for (;;)
        {
            const bool voiced = vS > 0.35f && havePitch;
            const int Ti = (int) std::lround (voiced ? T0s : defaultT0);
            const int range = voiced ? Ti / 4 : 0;
            const int h = Ti / 2;
            const int64_t cand = lastMark + Ti;
            if (cand + range + h + 2 > n || Ti > 4000) break;

            int64_t best = cand;
            if (voiced)
            {
                double eRef = 0;
                for (int j = 0; j < Ti; ++j)
                {
                    const float v = in[(size_t) ((lastMark - h + j) & MASK)];
                    ref[(size_t) j] = v; eRef += (double) v * v;
                }
                double bestScore = -1e30;
                for (int d = -range; d <= range; ++d)
                {
                    double dot = 0, eSeg = 0;
                    const int64_t st = cand + d - h;
                    for (int j = 0; j < Ti; ++j)
                    {
                        const float v = in[(size_t) ((st + j) & MASK)];
                        dot += (double) ref[(size_t) j] * v; eSeg += (double) v * v;
                    }
                    const double sc = dot / std::sqrt (eRef * eSeg + 1e-12);
                    if (sc > bestScore) { bestScore = sc; best = cand + d; }
                }
            }
            const int64_t k = nm++;
            mPos[k & MM] = best;
            mT0[k & MM] = voiced ? T0s : defaultT0;
            mV[k & MM] = voiced ? vS : 0.f;
            lastMark = best;
        }
    }

    int64_t nearestMark (int64_t c) const
    {
        int64_t i = nm - 1;
        const int64_t lo = std::max<int64_t> (0, nm - MN + 1);
        while (i > lo && mPos[i & MM] > c) --i;
        if (i + 1 < nm && std::llabs (mPos[(i + 1) & MM] - c) < std::llabs (mPos[i & MM] - c)) ++i;
        return i;
    }

    // ------------------------------------------------------------ synthesis
    float at (int64_t i) const { return in[(size_t) ((i > n ? n : i) & MASK)]; }

    float readIn (double pos) const
    {
        const int64_t i = (int64_t) std::floor (pos);
        const float fr = (float) (pos - (double) i);
        const float a = at (i - 1), b = at (i), c = at (i + 1), d = at (i + 2);
        const float c1 = 0.5f * (c - a);
        const float c2 = a - 2.5f * b + 2.f * c - 0.5f * d;
        const float c3 = 0.5f * (d - a) + 1.5f * (b - c);
        return ((c3 * fr + c2) * fr + c1) * fr + b;
    }

    void scheduleGrains()
    {
        while (nextSynth - prevRh <= n + 1)
        {
            const int64_t s = nextSynth;
            if (nm == 0) { nextSynth += (int64_t) defaultT0; prevRh = (int) defaultT0; continue; }

            const int64_t idx = nearestMark (s - D);
            const int64_t m = mPos[idx & MM];
            const float T0m = mT0[idx & MM];
            const float v = mV[idx & MM];

            float ratio = 1.f;
            if (v > 0.001f)
            {
                const float f0 = (float) sr / T0m;
                const float avg = std::exp (avgLog);
                const float base = P.targetMode ? P.targetHz / avg : semiRatio;
                ratio = base * std::exp (std::log (f0 / avg) * (P.intonation - 1.f));
                ratio = std::min (3.f, std::max (0.5f, ratio));
            }

            // organic micro-variation: correlated random walks (jitter + shimmer)
            const float nat = std::min (1.f, std::max (0.f, P.natural));
            jw += 0.15f * ((rand01() - 0.5f) * 0.018f * nat - jw);   // ~±0.9 % period
            shw += 0.22f * ((rand01() - 0.5f) * 0.12f * nat - shw);  // ~±6 % amplitude

            int Rh = (int) std::lround ((float) T0m / ratio * (1.f + jw));
            Rh = std::max ((int) (sr / 1000.0), std::min ((int) (sr / 30.0), Rh));
            const int Lh = prevRh;

            if (v > 0.001f)
            {
                const float f = formRatioS;
                const float gg = v * (1.f + shw);
                const int64_t t0 = std::max<int64_t> (s - Lh, n);
                for (int64_t t = t0; t < s + Rh; ++t)
                {
                    const float u = (float) (t - s);
                    const float wn = u < 0.f ? 0.5f * (1.f + std::cos ((float) kPi * u / (float) Lh))
                                             : 0.5f * (1.f + std::cos ((float) kPi * u / (float) Rh));
                    const float g = readIn ((double) m + (double) u * f);
                    const size_t ti = (size_t) (t & OM);
                    outAcc[ti] += wn * gg * g;
                    wAcc[ti] += wn * v;
                }
            }
            prevRh = Rh;
            nextSynth = s + Rh;
        }
    }

    // --------------------------------------------------------------- state
    double sr = 48000.0, dsr = 12000.0;
    float minHz = 80.f, defaultT0 = 384.f;
    int D = 1560, dsf = 4, hop = 120;

    std::vector<float> in, outAcc, wAcc, dec, tmp, nsdf, ref;
    int64_t n = 0, decN = 0;
    float decAcc = 0.f; int decCnt = 0, hopCnt = 0, hopStamp = 0;

    int64_t mPos[MN] {};
    float mT0[MN] {}, mV[MN] {};
    int64_t nm = 0, lastMark = 0;

    float T0s = 320.f, vS = 0.f, avgLog = 4.87f;
    float raw[3] {};
    int rawCnt = 0, rawIdx = 0, octCnt = 0, lastRawStamp = 0;
    int avgCount = 0; bool havePitch = false;

    int64_t nextSynth = 0; int prevRh = 384;

    Params P;
    float semiRatio = 1.26f, formRatio = 1.12f, formRatioS = 1.12f, outGain = 1.f, mixS = 1.f, gainS = 1.f;
    float env = 0.f, tw = 0.f, envAtt = 0.9f, envRel = 0.99f;
    float deEnv = 0.f, deS = 1.f, deAtt = 0.95f, deRel = 0.99f;
    float jw = 0.f, shw = 0.f;
    Biquad lowS, highS, presence, bpBreath, lpBreath, deBp;
    DCBlock dcIn, dcOut;
    uint32_t rng = 1;
};
} // namespace dyv

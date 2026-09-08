// Measures how *constant* the output is with the Compression knob at maximum.
//
// dsp_bench measures the DSP classes in isolation; this drives the whole
// SmartCompProcessor — trim, oversampled compressor, makeup, limiter, safety
// clamp, DC blocker — because the complaint ("at full compression the signal
// should stand still at the wall, it still pumps and wobbles") is about what
// comes out of the plugin, not about any single stage.
//
// The numbers that matter are the short-term loudness statistics: a wall means
// a small spread between the quiet and loud windows and a level that sits just
// under the ceiling. Peak level alone says nothing — a signal can touch 0 dBFS
// on every transient and still swing 15 dB in perceived density.
//
// Build:  cmake --build <builddir> --target density_probe
// Run:    <builddir>/density_probe_artefacts/density_probe

#include "../Source/PluginProcessor.h"
#include "../Source/DSP/LookaheadLimiter.h"

#include <cmath>
#include <cstdio>
#include <vector>
#include <algorithm>
#include <string>

namespace {

constexpr double SR = 44100.0;
constexpr int    BLOCK = 512;

double dB (double linv) { return 20.0 * std::log10 (std::max (linv, 1.0e-12)); }
double lin (double db)  { return std::pow (10.0, db / 20.0); }

// Deterministic white noise — std::rand would make runs incomparable.
struct Noise
{
    unsigned s = 22222u;
    float next() { s = s * 1664525u + 1013904223u; return (float) ((int) (s >> 9) % 20001 - 10000) / 10000.0f; }
};

//==============================================================================
// Test material. Both generators are deliberately dynamic: a signal that is
// already flat cannot show whether the compressor flattens anything.

// Drum-loop-like: kick, snare and hats on a 120 BPM grid, plus an 8-bar
// loud/quiet section change. The section change is what exposes anything in the
// chain that tracks the programme level over seconds.
void makeBreakbeat (std::vector<float>& L, std::vector<float>& R, double seconds)
{
    const int n = (int) (seconds * SR);
    L.assign ((size_t) n, 0.0f); R.assign ((size_t) n, 0.0f);
    Noise noise;

    const double beat = 0.5;              // 120 BPM
    for (int i = 0; i < n; ++i)
    {
        const double t = (double) i / SR;
        const double posInBar = std::fmod (t, beat * 4.0);
        // 8 s loud / 8 s quiet, so a section-level tracker has time to follow
        const double sectionDB = (std::fmod (t, 16.0) < 8.0) ? 0.0 : -9.0;

        double s = 0.0;
        auto hit = [&] (double at, double decay) {
            double d = posInBar - at;
            return (d >= 0.0 && d < 0.5) ? std::exp (-d / decay) : 0.0;
        };

        // kick on 1 and 3
        for (double at : { 0.0, 1.0 }) {
            double e = hit (at, 0.055);
            if (e > 0.0) s += 0.9 * e * std::sin (2.0 * M_PI * 62.0 * (posInBar - at) + 1.2);
        }
        // snare on 2 and 4
        for (double at : { 0.5, 1.5 }) {
            double e = hit (at, 0.075);
            if (e > 0.0) s += 0.55 * e * (0.6 * noise.next() + 0.4 * std::sin (2.0 * M_PI * 190.0 * (posInBar - at)));
        }
        // hats on every 8th
        for (int k = 0; k < 8; ++k) {
            double e = hit (k * 0.25, 0.012);
            if (e > 0.0) s += 0.18 * e * noise.next();
        }
        // bass sustain underneath, so it is not pure transients
        s += 0.18 * std::sin (2.0 * M_PI * 82.0 * t);

        s *= lin (-9.0 + sectionDB);
        L[(size_t) i] = R[(size_t) i] = (float) s;
    }
}

// Vocal-like: word/pause cadence, word-to-word level variation and a quiet
// verse against a loud chorus.
void makeVocal (std::vector<float>& L, std::vector<float>& R, double seconds)
{
    const int n = (int) (seconds * SR);
    L.assign ((size_t) n, 0.0f); R.assign ((size_t) n, 0.0f);

    for (int i = 0; i < n; ++i)
    {
        const double t = (double) i / SR;
        const double wordPeriod = 0.55;
        const double wordT = std::fmod (t, wordPeriod);
        const int    wordIndex = (int) (t / wordPeriod);

        double env = (wordT < 0.32)
                   ? std::max (0.5 - 0.5 * std::cos (wordT / 0.32 * 2.0 * M_PI), 0.06)
                   : 0.06;
        // 10 dB word-to-word swing, plus an 8 s verse/chorus change
        double wordDB = (wordIndex % 3 == 0) ? 0.0 : ((wordIndex % 3 == 1) ? -6.0 : -10.0);
        double sectionDB = (std::fmod (t, 16.0) < 8.0) ? 0.0 : -8.0;

        const double f0 = 145.0;
        double s = 0.60 * std::sin (2.0 * M_PI * f0 * t)
                 + 0.25 * std::sin (2.0 * M_PI * f0 * 2.0 * t)
                 + 0.15 * std::sin (2.0 * M_PI * f0 * 3.3 * t);

        s *= env * lin (-10.0 + wordDB + sectionDB);
        L[(size_t) i] = R[(size_t) i] = (float) s;
    }
}

//==============================================================================
struct Stats
{
    double meanDB = 0, sdDB = 0, p05 = 0, p95 = 0, minDB = 0, maxDB = 0, peakDB = 0;
    double spread() const { return p95 - p05; }
};

// Short-term loudness statistics over 50 ms windows, skipping the settling time
// at the start. Only windows with actual signal are counted: gaps between words
// would otherwise dominate the spread and say nothing about density.
Stats analyse (const std::vector<float>& x, double skipSeconds, double floorRelDB = 25.0)
{
    const int win = (int) (0.050 * SR);
    const int start = (int) (skipSeconds * SR);

    std::vector<double> windows;
    double truePeak = 0.0;
    for (int p = start; p + win <= (int) x.size(); p += win)
    {
        double sum = 0.0;
        for (int i = 0; i < win; ++i) {
            double v = x[(size_t) (p + i)];
            sum += v * v;
            truePeak = std::max (truePeak, std::abs (v));
        }
        windows.push_back (dB (std::sqrt (sum / win)));
    }
    if (windows.empty()) return {};

    // Drop windows more than floorRelDB below the loudest one — pauses.
    double loudest = *std::max_element (windows.begin(), windows.end());
    std::vector<double> active;
    for (double w : windows) if (w > loudest - floorRelDB) active.push_back (w);
    if (active.size() < 4) active = windows;

    Stats st;
    st.peakDB = dB (truePeak);
    for (double w : active) st.meanDB += w;
    st.meanDB /= (double) active.size();
    for (double w : active) st.sdDB += (w - st.meanDB) * (w - st.meanDB);
    st.sdDB = std::sqrt (st.sdDB / (double) active.size());

    std::sort (active.begin(), active.end());
    st.minDB = active.front();
    st.maxDB = active.back();
    st.p05 = active[(size_t) (0.05 * (active.size() - 1))];
    st.p95 = active[(size_t) (0.95 * (active.size() - 1))];
    return st;
}

//==============================================================================
// What the chain did internally, alongside what came out. A compressor that
// reports a large but *constant* gain reduction is not compressing — it is
// applying a fixed attenuation that makeup then cancels, which looks like
// heavy processing on the meter and is inaudible in the result.
// gainSwing is the one that matches the complaint word for word. Density is
// gain that is large and STEADY; pumping is gain that is large and MOVING. A
// setting can look heavily compressed on the GR meter and still breathe,
// which is exactly what "es pumpt noch" describes.
struct Internals { double grMean = 0, grSD = 0, limMean = 0, gainSwing = 0; };

Internals lastInternals;

// Runs the real plugin end to end at a given knob position.
Stats runChain (const std::vector<float>& srcL, const std::vector<float>& srcR,
                float compKnob, std::vector<float>* outCapture = nullptr)
{
    SmartCompProcessor p;
    p.setPlayConfigDetails (2, 2, SR, BLOCK);
    p.prepareToPlay (SR, BLOCK);
    p.apvts.getParameter ("comp")->setValueNotifyingHost (compKnob / 36.0f);
    p.apvts.getParameter ("mix")->setValueNotifyingHost (1.0f);
    p.apvts.getParameter ("gain")->setValueNotifyingHost (
        p.apvts.getParameter ("gain")->convertTo0to1 (0.0f));
    p.apvts.getParameter ("gate")->setValueNotifyingHost (0.0f);   // -80 = off
    p.apvts.getParameter ("inTrim")->setValueNotifyingHost (
        p.apvts.getParameter ("inTrim")->convertTo0to1 (0.0f));
    p.rideMode.store (false);   // manual: we are testing the knob at max

    std::vector<float> out;
    out.reserve (srcL.size());

    juce::AudioBuffer<float> buf (2, BLOCK);
    juce::MidiBuffer midi;
    const int n = (int) srcL.size();
    const int skipBlocks = (int) (3.0 * SR / BLOCK);
    std::vector<double> grTrace, limTrace;
    int blockIndex = 0;
    for (int off = 0; off + BLOCK <= n; off += BLOCK, ++blockIndex)
    {
        std::copy (srcL.begin() + off, srcL.begin() + off + BLOCK, buf.getWritePointer (0));
        std::copy (srcR.begin() + off, srcR.begin() + off + BLOCK, buf.getWritePointer (1));
        p.processBlock (buf, midi);
        const float* o = buf.getReadPointer (0);
        out.insert (out.end(), o, o + BLOCK);
        if (blockIndex > skipBlocks) {
            grTrace.push_back (p.compGainReductionDB.load());
            limTrace.push_back (p.limiterGainReductionDB.load());
        }
    }

    lastInternals = {};
    if (! grTrace.empty()) {
        for (double v : grTrace)  lastInternals.grMean  += v;
        for (double v : limTrace) lastInternals.limMean += v;
        lastInternals.grMean  /= (double) grTrace.size();
        lastInternals.limMean /= (double) limTrace.size();
        for (double v : grTrace)
            lastInternals.grSD += (v - lastInternals.grMean) * (v - lastInternals.grMean);
        lastInternals.grSD = std::sqrt (lastInternals.grSD / (double) grTrace.size());
    }

    // Applied gain per 10 ms window = output envelope minus input envelope. Its
    // p95-p05 is the pump depth. Measured on the stock code this is 11.3 dB on
    // the breakbeat and 23.1 dB on the vocal at the knob's maximum — the gain
    // is swinging by more than 20 dB, which is what "it still pumps" means.
    {
        const int w = (int) (0.010 * SR);
        const int start = (int) (3.0 * SR);
        std::vector<double> g;
        for (int q = start; q + w <= (int) out.size() && q + w <= (int) srcL.size(); q += w) {
            double si = 0.0, so = 0.0;
            for (int i = 0; i < w; ++i) {
                si += (double) srcL[(size_t) (q + i)] * srcL[(size_t) (q + i)];
                so += (double) out[(size_t) (q + i)] * out[(size_t) (q + i)];
            }
            si = std::sqrt (si / w); so = std::sqrt (so / w);
            if (si > 1.0e-5) g.push_back (dB (so) - dB (si));
        }
        if (g.size() > 8) {
            std::sort (g.begin(), g.end());
            lastInternals.gainSwing = g[(size_t) (0.95 * (g.size() - 1))]
                                    - g[(size_t) (0.05 * (g.size() - 1))];
        }
    }

    if (outCapture != nullptr) *outCapture = out;
    return analyse (out, 3.0);
}

// Reference bounds, so the spread numbers have a scale rather than being
// compared only against each other. "limiter only" drives the material hard
// into the existing brickwall: peaks pinned, short-term loudness barely
// touched. "ideal leveler" normalises every 20 ms window to a constant RMS
// before limiting — a physically unachievable perfect AGC, i.e. the floor of
// what any amount of compression could reach on this material.
Stats refLimiterOnly (const std::vector<float>& srcL, const std::vector<float>& srcR, double driveDB)
{
    std::vector<float> L (srcL), R (srcR);
    const double g = lin (driveDB);
    for (size_t i = 0; i < L.size(); ++i) { L[i] = (float) (L[i] * g); R[i] = (float) (R[i] * g); }
    LookaheadLimiter limr;
    limr.prepare (SR, BLOCK);
    for (size_t off = 0; off + BLOCK <= L.size(); off += BLOCK)
        limr.process (L.data() + off, R.data() + off, BLOCK, -0.3f);
    return analyse (L, 3.0);
}

Stats refIdealLeveler (const std::vector<float>& srcL, const std::vector<float>& srcR)
{
    std::vector<float> L (srcL), R (srcR);
    const int w = (int) (0.020 * SR);
    const double targetRMS = lin (-12.0);
    for (size_t off = 0; off + w <= L.size(); off += (size_t) w) {
        double sum = 0.0;
        for (int i = 0; i < w; ++i) sum += (double) L[off + i] * L[off + i];
        const double rms = std::sqrt (sum / w);
        double g = (rms > 1.0e-7) ? targetRMS / rms : 0.0;
        g = std::min (g, lin (60.0));
        for (int i = 0; i < w; ++i) { L[off + i] = (float) (L[off + i] * g); R[off + i] = (float) (R[off + i] * g); }
    }
    LookaheadLimiter limr;
    limr.prepare (SR, BLOCK);
    for (size_t off = 0; off + BLOCK <= L.size(); off += BLOCK)
        limr.process (L.data() + off, R.data() + off, BLOCK, -0.3f);
    return analyse (L, 3.0);
}

void report (const char* label, const Stats& s, const Internals* in = nullptr)
{
    std::printf ("  %-18s  %7.2f  %6.2f  %7.2f  %7.2f",
                 label, s.meanDB, s.sdDB, s.spread(), s.peakDB);
    if (in != nullptr)
        std::printf ("   %6.2f %6.2f %6.2f %6.2f",
                     in->grMean, in->grSD, in->limMean, in->gainSwing);
    std::printf ("\n");
}

void runMaterial (const char* name,
                  void (*gen) (std::vector<float>&, std::vector<float>&, double))
{
    std::vector<float> sL, sR;
    gen (sL, sR, 24.0);
    Stats in = analyse (sL, 3.0);

    std::printf ("%s\n", name);
    std::printf ("  %-18s  %7s  %6s  %7s  %7s   %6s %6s %6s %6s\n",
                 "", "meanDB", "sd", "p95-p05", "peak", "compGR", "grSD", "limGR", "pump");
    report ("input (untouched)", in);
    for (float knob : { 12.0f, 24.0f, 36.0f })
    {
        Stats s = runChain (sL, sR, knob);
        report (("comp " + std::to_string ((int) knob)).c_str(), s, &lastInternals);
    }
    report ("ref: limiter +20dB", refLimiterOnly (sL, sR, 20.0));
    report ("ref: ideal leveler", refIdealLeveler (sL, sR));
    std::printf ("\n");
}

} // namespace

int main()
{
    std::printf ("\n=== SmartComp density probe @ %.0f Hz ===\n", SR);
    std::printf ("Short-term (50ms) loudness of the plugin output. At the knob's maximum\n");
    std::printf ("the spread should collapse and the mean should sit close to the ceiling.\n\n");

    // A wall that depends on how hot the source was is not a wall. On the stock
    // code this row moves 1:1 with the source, because the chain is unity gain
    // by construction: 18 dB of source range came out as 18.5 dB of output range.
    {
        std::vector<float> sL, sR;
        makeVocal (sL, sR, 24.0);
        std::printf ("SOURCE LEVEL INDEPENDENCE at comp 36 (vocal, re-gained)\n");
        for (double g : { -12.0, -6.0, 0.0, +6.0 })
        {
            std::vector<float> aL (sL), aR (sR);
            const double m = lin (g);
            for (size_t i = 0; i < aL.size(); ++i) { aL[i] = (float) (aL[i] * m); aR[i] = (float) (aR[i] * m); }
            Stats st = runChain (aL, aR, 36.0f);
            std::printf ("  source %+5.0f dB -> out mean %7.2f  peak %7.2f  pump %6.2f\n",
                         g, st.meanDB, st.peakDB, lastInternals.gainSwing);
        }
        std::printf ("\n");
    }

    runMaterial ("BREAKBEAT  (drum hits + 8s loud/quiet sections)", makeBreakbeat);
    runMaterial ("VOCAL      (word cadence + 8s verse/chorus)",     makeVocal);

    std::printf ("Reading: 'p95-p05' is the spread of short-term loudness — how much the\n");
    std::printf ("level still moves. 'peak' is the true peak; the ceiling is -0.3 dBFS.\n");
    std::printf ("'pump' is how far the plugin's own gain swings; density wants a large\n");
    std::printf ("gain that barely moves, so a high compGR next to a high pump is exactly\n");
    std::printf ("the failure the ear reports as breathing. The two 'ref:' rows bracket\n");
    std::printf ("what is achievable on this material at all.\n\n");
    return 0;
}

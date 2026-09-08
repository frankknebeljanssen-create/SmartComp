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


// A drum train with identical hits, constant level, no section changes. On this
// source any gain movement at all is pumping by definition — there is nothing
// for a leveler to level. That distinction matters: on real material the gain
// SHOULD move to remove a level difference, and a swing metric alone cannot
// tell that apart from breathing.
void makeConstantDrums (std::vector<float>& L, std::vector<float>& R, double seconds)
{
    const int n = (int) (seconds * SR);
    L.assign ((size_t) n, 0.0f); R.assign ((size_t) n, 0.0f);
    Noise noise;
    for (int i = 0; i < n; ++i)
    {
        const double t = (double) i / SR;
        const double d = std::fmod (t, 0.5);           // one identical hit every 500 ms
        double s = 0.0;
        s += 0.9 * std::exp (-d / 0.055) * std::sin (2.0 * M_PI * 62.0 * d + 1.2);
        s += 0.5 * std::exp (-d / 0.070) * noise.next();
        s += 0.18 * std::sin (2.0 * M_PI * 82.0 * t);
        L[(size_t) i] = R[(size_t) i] = (float) (s * lin (-9.0));
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

// Applied gain per 10 ms window = output envelope minus input envelope; the
// answer is the p95-p05 of that.
//
// Not all gain movement is pumping. A brickwall holding one transient down
// moves the gain hard for a few milliseconds and that is the limiter working
// as intended; breathing is the gain riding up and down at syllable and beat
// rate. A 30 ms smoother separates them: it averages a single transient's
// limiting away while leaving 2 Hz beat movement essentially intact (0.6 dB
// down). 300 ms was tried first and was wrong the other way — its 0.5 Hz
// corner removed the beat-rate breathing itself, so it scored a chain that
// audibly pumped every beat as perfectly steady.
double gainSwing (const std::vector<float>& src, const std::vector<float>& out)
{
    const int w = (int) (0.010 * SR);
    const int start = (int) (3.0 * SR);
    std::vector<double> g;
    for (int q = start; q + w <= (int) out.size() && q + w <= (int) src.size(); q += w) {
        double si = 0.0, so = 0.0;
        for (int i = 0; i < w; ++i) {
            si += (double) src[(size_t) (q + i)] * src[(size_t) (q + i)];
            so += (double) out[(size_t) (q + i)] * out[(size_t) (q + i)];
        }
        si = std::sqrt (si / w); so = std::sqrt (so / w);
        if (si > 1.0e-5) g.push_back (dB (so) - dB (si));
    }
    if (g.size() <= 8) return 0.0;

    const double k = std::exp (-0.010 / 0.030);
    double s = g.front();
    std::vector<double> sm;
    sm.reserve (g.size());
    for (double v : g) { s = s * k + v * (1.0 - k); sm.push_back (s); }
    sm.erase (sm.begin(), sm.begin() + (long) std::min<size_t> (sm.size() / 4, 200));
    std::sort (sm.begin(), sm.end());
    return sm[(size_t) (0.95 * (sm.size() - 1))] - sm[(size_t) (0.05 * (sm.size() - 1))];
}

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

    lastInternals.gainSwing = gainSwing (srcL, out);

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
    lastInternals = {};
    const double g = lin (driveDB);
    for (size_t i = 0; i < L.size(); ++i) { L[i] = (float) (L[i] * g); R[i] = (float) (R[i] * g); }
    LookaheadLimiter limr;
    limr.prepare (SR, BLOCK);
    for (size_t off = 0; off + BLOCK <= L.size(); off += BLOCK)
        limr.process (L.data() + off, R.data() + off, BLOCK, -0.3f);
    lastInternals.gainSwing = gainSwing (srcL, L);
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
    { Stats r = refLimiterOnly (sL, sR, 20.0); report ("ref: limiter +20dB", r, &lastInternals); }
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

    // What the drive does to what is NOT the music. density_probe's other rows
    // cannot see this: analyse() drops every window more than 25 dB below the
    // loudest, which is exactly where room tone lives. The phrase/silence
    // pattern here is deliberate — makeVocal never goes quiet between words, so
    // it cannot show what happens in a real pause.
    {
        std::printf ("NOISE FLOOR IN PAUSES at comp 36 (4s phrase / 3s silence, over a room bed)\n");
        Noise nz;
        for (double bedDB : { -70.0, -60.0, -50.0 }) {
            std::vector<float> sL, sR; makeVocal (sL, sR, 28.0);
            const double bed = lin (bedDB);
            for (size_t i = 0; i < sL.size(); ++i) {
                const double t = (double) i / SR;
                const double g = (std::fmod (t, 7.0) < 4.0) ? 1.0 : 0.0;   // 4s on, 3s silent
                const float n = (float) (nz.next() * bed);
                sL[i] = (float) (sL[i] * g) + n;
                sR[i] = (float) (sR[i] * g) + n;
            }
            std::vector<float> out;
            runChain (sL, sR, 36.0f, &out);
            auto rmsAt = [&] (double t0, double t1) {
                double sum = 0.0; int n = 0;
                for (int i = (int)(t0*SR); i < (int)(t1*SR) && i < (int) out.size(); ++i) { sum += (double) out[i]*out[i]; ++n; }
                return dB (std::sqrt (sum / std::max (n, 1)));
            };
            // Late in a silent stretch (t=19.5-20.5s is inside the third pause)
            const double pause = rmsAt (19.5, 20.5);
            const double words = rmsAt (15.0, 18.0);
            std::printf ("  bed %+5.0f dB -> pause %7.2f dB (lift %+6.2f), words %7.2f dB, gap %5.2f dB\n",
                         bedDB, pause, pause - bedDB, words, words - pause);
        }
        std::printf ("\n");
    }

    // The cleanest reading of the complaint: identical drum hits, constant level.
    {
        std::vector<float> sL, sR; makeConstantDrums (sL, sR, 20.0);
        std::printf ("PUMP ON A CONSTANT-LEVEL SOURCE (identical hits — any swing is pumping)\n");
        for (float knob : { 12.0f, 24.0f, 36.0f }) {
            runChain (sL, sR, knob);
            std::printf ("  comp %2d -> gain swing %6.2f dB   (compGR %5.2f, limGR %4.2f)\n",
                         (int) knob, lastInternals.gainSwing, lastInternals.grMean, lastInternals.limMean);
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

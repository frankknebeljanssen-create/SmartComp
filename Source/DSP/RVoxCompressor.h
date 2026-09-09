#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "SlidingExtremum.h"
#include <cmath>
#include <vector>
#include <array>
#include <atomic>

class RVoxCompressor
{
public:
    // Lookahead in seconds rather than samples, so the attack character does not
    // change with the session sample rate. It used to be a fixed 64 samples,
    // which meant 0.73 ms at 44.1 kHz but only 0.17 ms at 192 kHz.
    static constexpr float LOOKAHEAD_SECONDS = 0.0015f;
    static constexpr float MAX_RATIO         = 15.0f;
    static constexpr int   GR_HISTORY_SIZE   = 2048;
    static constexpr float KNEE_WIDTH_MIN    = 6.0f;
    static constexpr float KNEE_WIDTH_MAX    = 16.0f;
    // Where the knob stops being a compressor and starts being a wall. Below
    // knob 24 every expression scaled by slam collapses to the value it always
    // had, so the gentle range the plugin is built around — and the sweet spot
    // AUTO aims at, near 11 — is bit-identical.
    static constexpr float SLAM_START        = 24.0f / 36.0f;
    static constexpr float SLAM_DEPTH_DB     = 6.0f;    // extra threshold depth at the top
    static constexpr float SLAM_RMS_MS       = 15.0f;   // detector window at the top
    static constexpr float RMS_MS_NORMAL     = 50.0f;   // what prepare() already uses
    // How far below its own programme average the signal may fall before the
    // wall's drive is withdrawn. Without this the drive is applied to whatever
    // is there in a pause: measured, a -70 dBFS room bed came out at -19.1 dBFS,
    // a 50.9 dB lift that left it 11 dB under the words. The Gate cannot cover
    // this — it acts inside the compressor, upstream of the makeup, so even
    // fully closed its 30 dB still loses to a 36 dB lift.
    static constexpr float DRIVE_HOLD_DB     = 16.0f;   // full drive down to here
    static constexpr float DRIVE_FADE_DB     = 10.0f;   // and none this much further down

    // ATTACK, as one number driving two things. The knob's own travel is 0..100
    // with an AUTO flat at the bottom that is bit-identical to what shipped
    // before it existed.
    //
    // The second thing is the lookahead, and it is the stronger half. This
    // compressor targets the lowest gain found anywhere in its 1.5 ms window, so
    // reduction is complete before a transient lands — which is precisely what a
    // slow attack is supposed NOT to do. Measured on a drum loop at knob 24, the
    // attack coefficient alone moves the output crest by 3.9 dB and retiring the
    // lookahead alone by 4.4 dB; the two say almost the same thing, so driving
    // them separately would give two overlapping half-controls. Driven together
    // the travel is monotone end to end and worth 4.5 dB.
    //
    // Reported latency does not move: both the pre-emptive and the aligned gain
    // come out of the same delay line, so this changes which gain is read, not
    // how far the audio is delayed.
    static constexpr float ATK_AUTO_END = 6.0f;    // travel below this means AUTO
    static constexpr float ATK_MIN_MS   = 0.10f;   // what AUTO has always used
    static constexpr float ATK_MAX_MS   = 20.0f;

    // The wall takes back what a slower attack lets through: measured, the
    // control's crest range collapses from 4.2 dB at knob 24 to 0.6 dB at knob
    // 36, while its loudness drift grows to 1.1 dB. Trading a dB of level for
    // half a dB of punch is a bad deal, so the control is faded out over the
    // same span instead — and the knob dims in step with this, because it dims
    // for a reason rather than as decoration. One copy of the law, read by the
    // processor and the editor both.
    static constexpr float ATK_WALL_START = 28.0f;   // knob units
    static constexpr float ATK_WALL_END   = 34.0f;

    static float attackAuthorityForKnob (float knob)
    {
        return 1.0f - juce::jlimit (0.0f, 1.0f,
                                    (knob - ATK_WALL_START) / (ATK_WALL_END - ATK_WALL_START));
    }

    // RELEASE, locked to the session grid. Measured on a 120 BPM drum train,
    // the applied gain swells 9 dB between hits at the shipping release and
    // 2.9 dB at sixteen times that — the difference between a loop that
    // breathes hard and one that sits still, which is the character axis the
    // plugin had no control over. The useful span lands almost exactly on
    // 1/32 to 1/2 at 120 BPM, so the setting is named in notes rather than in
    // milliseconds: at a different tempo the same note keeps the same
    // relationship to the groove, which a time in ms does not.
    //
    // Stepped, because the values ARE the musical grid — there is nothing
    // between 1/8 and 1/16 worth reaching, and stepping removes any chance of
    // a stretch of travel that does nothing.
    enum ReleaseNote { RelAuto = 0, Rel32, Rel16, Rel8, Rel4, Rel2, RelCount };

    static float releaseNoteFraction (int note)
    {
        switch (note) {
            case Rel32: return 1.0f / 8.0f;    // of a beat
            case Rel16: return 1.0f / 4.0f;
            case Rel8:  return 1.0f / 2.0f;
            case Rel4:  return 1.0f;
            case Rel2:  return 2.0f;
            default:    return 0.0f;
        }
    }

    static const char* releaseNoteName (int note)
    {
        switch (note) {
            case Rel32: return "1/32";
            case Rel16: return "1/16";
            case Rel8:  return "1/8";
            case Rel4:  return "1/4";
            case Rel2:  return "1/2";
            default:    return "AUTO";
        }
    }

    static bool  attackIsAuto (float travel) { return travel <= ATK_AUTO_END; }

    static float attackTravelNorm (float travel)
    {
        return juce::jlimit (0.0f, 1.0f, (travel - ATK_AUTO_END) / (100.0f - ATK_AUTO_END));
    }

    static float attackMsForTravel (float travel)
    {
        if (attackIsAuto (travel)) return ATK_MIN_MS;
        // Exponential, because the measured crest is linear in log(attack): a
        // linear taper would spend most of the knob on nothing.
        return ATK_MIN_MS * std::pow (ATK_MAX_MS / ATK_MIN_MS, attackTravelNorm (travel));
    }

    static float lookaheadBlendForTravel (float travel)
    {
        if (attackIsAuto (travel)) return 1.0f;
        return std::pow (1.0f - attackTravelNorm (travel), 0.7f);
    }

    // Smoothstep rather than a straight ramp, so the knob has no gradient
    // discontinuity where the wall starts. Both files call this one function
    // instead of keeping two copies of the law.
    static float slamForAmount (float a)
    {
        const float w = juce::jlimit (0.0f, 1.0f, (a - SLAM_START) / (1.0f - SLAM_START));
        return w * w * (3.0f - 2.0f * w);
    }

    // What the compression law delivers in steady state, for a detector sitting
    // crestDB above the programme average it is referenced to.
    //
    // The sweet-spot search in PluginProcessor calls this rather than keeping a
    // second copy of the curve. The copy had drifted: it used a threshold depth
    // of 30 dB where the compressor uses 18, ignored the slam term, and held the
    // knee flat instead of adapting it — while the comment beside it claimed all
    // three matched. AUTO therefore aimed with a bent sight and parked lower
    // than the reduction it advertised.
    static float predictGainReductionDB (float compAmount, float crestDB, float kneeWidth)
    {
        const float depthDB = -6.0f + compAmount * 18.0f
                            + slamForAmount (compAmount) * SLAM_DEPTH_DB;
        const float ratio = 1.0f + compAmount * compAmount * (MAX_RATIO - 1.0f);
        const float aboveThresh = depthDB + crestDB;
        const float kneeScale = juce::jlimit (0.7f, 1.3f, 1.3f - std::abs (aboveThresh) / 24.0f);
        return computeGainReduction (aboveThresh, 0.0f, ratio, kneeWidth * kneeScale);
    }

    // Smooth attack mode: interpolates gain across the lookahead window
    bool smoothAttack = true;
    // 1 = full pre-emption (AUTO), 0 = the gain aligned with the audio it is
    // applied to. Set from the ATTACK travel; see lookaheadBlendForTravel.
    float lookaheadBlend = 1.0f;

    RVoxCompressor() = default;

    void prepare(double sampleRate, int /*maxBlockSize*/)
    {
        sr = sampleRate;
        invSr = 1.0 / sampleRate;
        lookahead = std::max(8, (int)std::lround(sr * (double)LOOKAHEAD_SECONDS));

        releaseCoeffFast = std::exp(-1.0f / (float(sr) * 0.060f));
        releaseCoeffSlow = std::exp(-1.0f / (float(sr) * 0.700f));
        rmsCoeff         = std::exp(-1.0f / (float(sr) * 0.050f));  // 50ms RMS — consonants (2-10ms) barely affect it
        attackCoeff      = std::exp(-1.0f / (float(sr) * 0.0015f)); // default 1.5ms
        peakAttackCoeff  = std::exp(-1.0f / (float(sr) * 0.001f));  // 1ms peak attack (was 0.2ms, tracked individual bass cycles)
        peakReleaseCoeff = std::exp(-1.0f / (float(sr) * 0.005f));  // 5ms peak release (was 2ms)
        expanderReleaseCoeff = std::exp(-1.0f / (float(sr) * 0.5f));
        gateOpenCoeff = std::exp(-1.0f / (float(sr) * 0.002f));   // 2ms open
        gateCloseCoeff = std::exp(-1.0f / (float(sr) * 0.030f));  // 30ms close
        gateDetAttackCoeff = 1.0f - std::exp(-1.0f / (float(sr) * 0.002f)); // 2ms gate detector attack (smoother open)

        // Detector low shelf: 1-pole LP at 200Hz, -3dB cut
        detLowCoeff = 1.0f - std::exp(-2.0f * juce::MathConstants<float>::pi * 200.0f / (float)sr);
        detLowCutGain = 0.29f;

        // SC HPF state
        scHpfSampleRate = (float)sampleRate;
        scHpfX1 = scHpfX2 = scHpfY1 = scHpfY2 = 0.0f;
        scHpfLastFreq = -1.0f;




        // Gain smoothing in dB domain: ~3ms time constant
        gainSmoothCoeff = std::exp(-1.0f / (float(sr) * 0.003f));

        // Lookahead gain smoothing. Retuned to settle inside the 1.5 ms window:
        // the old 0.27/0.74 ms pair only fitted because the coefficients were
        // being computed for half the rate the loop actually ran at.
        smoothCoeff1 = std::exp(-1.0f / (float(sr) * 0.00020f));
        smoothCoeff2 = std::exp(-1.0f / (float(sr) * 0.00045f));
        punchySmoothCoeff = std::exp(-1.0f / (float(sr) * 0.00035f));
        // GR metering: ~8ms smoothing for display
        grMeterCoeff = std::exp(-1.0f / (float(sr) * 0.008f));

        // Auto-threshold programme tracker. Asymmetric: adapts to a louder
        // section reasonably quickly, lets go slowly so short pauses and
        // sustained quiet passages do not move the threshold much.
        autoLevelRiseCoeff = std::exp(-1.0f / (float(sr) * 0.800f));
        autoLevelFallCoeff = std::exp(-1.0f / (float(sr) * 3.000f));
        // Fast to restore, slow to withdraw: a word must not be ducked on its
        // way in, and a decaying tail must not be chopped on its way out.
        driveRiseCoeff = std::exp(-1.0f / (float(sr) * 0.020f));
        driveFallCoeff = std::exp(-1.0f / (float(sr) * 0.250f));
        // The drive's own level reference. It follows the programme up in about
        // a second and lets go over twenty, so a pause cannot drag it down.
        // autoLevelDB alone will not do: it keeps updating on anything above
        // -60 dBFS, so a -50 dBFS room bed pulled it down during a pause and the
        // drive then normalised the room tone to speaking level — measured, the
        // bed came out 0.94 dB below the words.
        // A slow level for the gate to compare against. The compressor's own
        // envelope is far too fast for this job: at 25 ms it drops between drum
        // hits, so a gate driven from it closed on every gap in the music and
        // put the level variation straight back — measured, the breakbeat spread
        // stalled at 8.82 dB instead of 3.33.
        driveLevelCoeff   = std::exp(-1.0f / (float(sr) * 0.400f));
        driveRefRiseCoeff = std::exp(-1.0f / (float(sr) * 1.000f));
        driveRefFallCoeff = std::exp(-1.0f / (float(sr) * 20.000f));

        delayBufferL.assign(lookahead + 1, 0.0f);
        delayBufferR.assign(lookahead + 1, 0.0f);
        gainDBBuffer.assign(lookahead + 1, 0.0f);
        gainMin.prepare(lookahead);

        grHistorySamplesPerSlot = std::max(1, (int)(sr / 86.0));

        // Clear runtime state through reset() rather than repeating the list
        // here — the two copies had already drifted apart, leaving stale gain
        // and filter state to click on the first sample after a re-prepare.
        reset();
    }

    void reset()
    {
        std::fill(delayBufferL.begin(), delayBufferL.end(), 0.0f);
        std::fill(delayBufferR.begin(), delayBufferR.end(), 0.0f);
        std::fill(gainDBBuffer.begin(), gainDBBuffer.end(), 0.0f);
        gainMin.reset(0.0f);
        delayWritePos = 0;
        envDB = -100.0f;
        rmsSquaredSum = 0.0f;
        peakEnv = 0.0f;
        expanderEnvDB = -100.0f;
        gainReductionDB = 0.0f;
        smoothGR = 0.0f;
        smoothExpanderGainDB = 0.0f;
        smoothedGainDB = 0.0f;
        smoothedGainDB2 = 0.0f;
        prevAppliedGainLin = 1.0f;
        detLowLP = 0.0f;
        autoLevelDB = -18.0f;
        staticMakeupDB = 0.0f;
        driveScale = 0.0f;
        driveRefDB = -60.0f;
        driveLevelDB = -60.0f;
        autoLevelPrimed = false;
        gateOpen = true;
        scHpfX1 = scHpfX2 = scHpfY1 = scHpfY2 = 0.0f;
        prevGrDB = 0.0f;
        grHistory.fill(0.0f);
        inputHistory.fill(-100.0f);
        outputHistory.fill(-100.0f);
        grHistoryWritePos.store(0, std::memory_order_release);
        grHistorySampleCounter = 0;
        grHistoryBlockAccum = 0.0f;
        inputHistoryBlockAccum = -100.0f;
        outputHistoryBlockAccum = -100.0f;
    }

    void setReleaseTimes(float fastSec, float slowSec)
    {
        releaseCoeffFast = std::exp(-1.0f / (float(sr) * fastSec));
        releaseCoeffSlow = std::exp(-1.0f / (float(sr) * slowSec));
    }

    void setAttackTime(float attackSec)
    {
        attackCoeff = std::exp(-1.0f / (float(sr) * attackSec));
    }

    void setScHpfFreq(float freq)
    {
        scHpfFreq = freq;
        if (freq < 1.0f) return;  // OFF
        if (std::abs(freq - scHpfLastFreq) < 0.5f) return;  // no change
        scHpfLastFreq = freq;

        // 2nd order Butterworth HPF biquad coefficients
        float w0 = 2.0f * juce::MathConstants<float>::pi * freq / scHpfSampleRate;
        float cosW0 = std::cos(w0);
        float sinW0 = std::sin(w0);
        float alpha = sinW0 / (2.0f * 0.7071f);  // Q = 0.7071 (Butterworth)
        float a0 = 1.0f + alpha;
        scHpfB0 = ((1.0f + cosW0) / 2.0f) / a0;
        scHpfB1 = (-(1.0f + cosW0)) / a0;
        scHpfB2 = scHpfB0;
        scHpfA1 = (-2.0f * cosW0) / a0;
        scHpfA2 = (1.0f - alpha) / a0;
    }

    // Set by the processor each block so the before/after timeline can show the
    // real output level. Display only — nothing in the audio path reads it.
    float displayMakeupDB = 0.0f;
    float userKneeWidth = 10.0f;
    float maxGainReductionDB = 36.0f;
    float ratioMultiplier = 1.0f; // Adjustable via ADV display dot (Y axis)

    void process(float* bufferL, float* bufferR, int numSamples, float compDB,
                 float gateThreshDB = -80.0f,
                 const float* scL = nullptr, const float* scR = nullptr)
    {
        float compAmount = -compDB / 36.0f;
        if (compAmount < 0.001f)
        {
            // Smooth transition to unity gain per-sample to avoid clicks
            static constexpr float dB2LinFactor = 0.11512925464970229f;
            for (int i = 0; i < numSamples; ++i)
            {
                delayBufferL[delayWritePos] = bufferL[i];
                delayBufferR[delayWritePos] = bufferR[i];
                gainDBBuffer[delayWritePos] = 0.0f;
                // Keep the window fed while the knob sits at zero, otherwise
                // turning it up would look back at stale gain values.
                gainMin.pushAndGet(0.0f);
                // Keep the programme level current as well. Skipping it left
                // autoLevelDB unprimed while the knob sat at zero, so the first
                // sample after the knob moved seeded the level from whatever
                // instant that happened to be. Start turning during a gap
                // between words and it was seeded some 20 dB low, the threshold
                // went with it, and the compressor over-compressed by that much
                // until the 0.8 s tracker recovered — measured as a 20 dB hole
                // in the level during a one-second sweep from 0 to 36, worse
                // the faster the knob was turned.
                trackProgrammeLevel(runDetector(
                    (scL != nullptr) ? scL[i] : bufferL[i],
                    (scR != nullptr) ? scR[i] : bufferR[i], 0.08f, rmsCoeff));
                int readPos = (delayWritePos - lookahead + (int)delayBufferL.size()) % (int)delayBufferL.size();
                float delayedL = delayBufferL[readPos];
                float delayedR = delayBufferR[readPos];

                // Fade smoothed gain toward 0dB (unity) using SR-dependent coefficients
                smoothedGainDB = smoothCoeff1 * smoothedGainDB;  // → 0dB
                smoothedGainDB2 = smoothCoeff2 * smoothedGainDB2;
                float targetGain = std::exp(smoothedGainDB2 * dB2LinFactor);
                float appliedGain = prevAppliedGainLin + (targetGain - prevAppliedGainLin) * 0.5f;
                prevAppliedGainLin = targetGain;

                bufferL[i] = delayedL * appliedGain;
                bufferR[i] = delayedR * appliedGain;
                delayWritePos = (delayWritePos + 1) % (int)delayBufferL.size();
                float bypassInDB = 20.0f * std::log10(std::max(1e-10f, std::abs(bufferL[i]) + std::abs(bufferR[i])) * 0.5f);
                pushGRHistory(0.0f, bypassInDB);
                smoothGR = grMeterCoeff * smoothGR; // per-sample fade to zero
            }
            gainReductionDB = 0.0f;
            prevGrDB = 0.0f;
            staticMakeupDB = 0.0f;
            return;
        }

        const float slam = slamForAmount(compAmount);

        // ===== AUTO THRESHOLD =====
        // The threshold sits a knob-controlled distance below a slow average of
        // the programme level, rather than at a fixed dBFS value. With a fixed
        // threshold the knob only did anything on hot tracks: on a -20 dBFS
        // source the threshold was still above the signal at knob 12, so the
        // first third of the range produced no gain reduction at all.
        //
        // depth goes from 6 dB *above* the average (no compression) to 12 dB
        // below it, so knob position means the same thing regardless of how hot
        // the incoming track is. Calibrated against measurement: at the top of
        // the range this lands around 17 dB of reduction, which is already a lot
        // for a vocal. A steeper curve was tried first and produced 27 dB, which
        // drove makeup into its ceiling and left nothing but squash.
        //
        // autoLevelDB is measured from the detector, i.e. pre-compression, so it
        // cannot feed back from the gain being applied.
        float depthDB = -6.0f + compAmount * 18.0f + slam * SLAM_DEPTH_DB;
        float thresholdDB = autoLevelDB - depthDB;

        float ratio = 1.0f + compAmount * compAmount * (MAX_RATIO - 1.0f);
        ratio = 1.0f + (ratio - 1.0f) * ratioMultiplier; // Scale ratio around 1:1
        float baseKneeDB = userKneeWidth;
        // RMS-dominant detection
        float peakBlend = 0.08f + compAmount * 0.12f;

        // What the compression law implies in steady state. The makeup reads
        // this above knob 24 instead of averaging the reduction it just
        // delivered — a servo which, being the inverse of the compressor's own
        // gain, made the whole chain unity gain by construction and left the
        // limiter's ceiling permanently out of reach.
        // What the law delivers given how far this material's envelope actually
        // rides above the programme level it is referenced to. Assuming the
        // envelope sits ON that level — which is what using the depth alone
        // does — under-read the reduction by 20 dB during a fast knob sweep,
        // and the makeup that follows this then left a hole that size in the
        // level on the way up. detCrestDB is from the previous block; it moves
        // over seconds, so a block of lag is immaterial.
        staticMakeupDB = std::max(0.0f, depthDB) * (1.0f - 1.0f / ratio);
        if (! std::isfinite(staticMakeupDB)) staticMakeupDB = 0.0f;

        // The 50 ms RMS window is what caps level control at about 1.6 Hz: it
        // simply cannot see a syllable. That is the right character for gentle
        // compression and the wrong one for a wall, so the window shortens with
        // slam. Measured, this single change is what fixes the vocal — with the
        // window left at 50 ms its spread lands at 19.69 dB instead of 10.69.
        // At slam 0 the branch below takes the prepare()-time coefficient
        // verbatim, so nothing below knob 24 moves by a bit.
        const float rmsMsNow = RMS_MS_NORMAL + slam * (SLAM_RMS_MS - RMS_MS_NORMAL);
        const float rmsC = (slam > 0.0f)
                         ? std::exp(-1.0f / (float(sr) * rmsMsNow * 0.001f))
                         : rmsCoeff;

        for (int i = 0; i < numSamples; ++i)
        {
            float inL = bufferL[i];
            float inR = bufferR[i];
            float detL = (scL != nullptr) ? scL[i] : inL;
            float detR = (scR != nullptr) ? scR[i] : inR;

            const float detDB = runDetector(detL, detR, peakBlend, rmsC);

            // ===== SLOW PROGRAMME LEVEL (drives the auto threshold) =====
            // Deliberately far slower than the compressor's own release, so the
            // threshold adapts between sections without chasing the gain
            // reduction and flattening the compression out. Only tracked while
            // there is signal, otherwise a pause would drag it down and the
            // vocal would get slammed on the way back in.
            trackProgrammeLevel(detDB);

            // ===== PROGRAM-DEPENDENT ENVELOPE =====
            if (detDB > envDB)
            {
                // Attack: user-configurable time constant
                envDB = attackCoeff * envDB + (1.0f - attackCoeff) * detDB;
            }
            else
            {
                // More GR = slower release (anti-pump)
                // But cap the slowdown so extreme settings still breathe
                float grBlendLin = juce::jlimit(0.0f, 1.0f, prevGrDB / 20.0f);  // was /12 — now needs 20dB to go fully slow
                float grBlend = grBlendLin * grBlendLin;  // quadratic
                // Blend: high grBlend (heavy compression) → slow release
                float rc = releaseCoeffFast * (1.0f - grBlend) + releaseCoeffSlow * grBlend;
                // Less slowdown at extreme comp — let it breathe
                float compSlowdown = 1.0f + compAmount * 0.15f; // was 0.3 — now max 15% slower
                rc = 1.0f - (1.0f - rc) / compSlowdown;
                envDB = rc * envDB + (1.0f - rc) * detDB;
            }

            // How much of the wall's drive is currently earned. In a pause the
            // envelope sits far below the programme average and there is nothing
            // there worth lifting; during a word it is at or above it.
            {
                // The reference follows the programme while there IS programme,
                // and freezes when there is not. Letting it follow unconditionally
                // meant a pause dragged it down and the drive then normalised the
                // room tone to speaking level; freezing it unconditionally meant a
                // quiet section never got lifted and the 9 dB section step
                // survived intact. Gating it on driveScale — which is itself a
                // level test — separates the two: a section is a few dB down and
                // keeps the drive, a pause is tens of dB down and loses it.
                const float rc = (driveScale > 0.5f) ? driveRefRiseCoeff : 1.0f;
                driveRefDB = rc * driveRefDB + (1.0f - rc) * autoLevelDB;
                driveLevelDB = driveLevelCoeff * driveLevelDB + (1.0f - driveLevelCoeff) * detDB;
                const float below = driveRefDB - driveLevelDB;
                const float target = juce::jlimit(0.0f, 1.0f,
                                                  (DRIVE_HOLD_DB - below) / DRIVE_FADE_DB + 1.0f);
                const float c = (target < driveScale) ? driveFallCoeff : driveRiseCoeff;
                driveScale = c * driveScale + (1.0f - c) * target;
            }

            // ===== ADAPTIVE KNEE =====
            // Near threshold: wider knee (gentle transition)
            // Far above threshold: narrower knee (precise control)
            float distFromThresh = std::abs(envDB - thresholdDB);
            float kneeScale = juce::jlimit(0.7f, 1.3f, 1.3f - distFromThresh / 24.0f);
            float kneeDB = baseKneeDB * kneeScale;

            float grDB = computeGainReduction(envDB, thresholdDB, ratio, kneeDB);
            // Apply GR Range limit
            grDB = juce::jmin(grDB, maxGainReductionDB);

            // ===== NOISE GATE with hysteresis =====
            // gateThreshDB: -80 = off, -60..-20 = active range
            // Fast attack (opens quickly), medium release (closes smoothly)
            float gateRelCoeff = expanderReleaseCoeff; // ~500ms release

            if (detDB > expanderEnvDB)
                expanderEnvDB = expanderEnvDB + gateDetAttackCoeff * (detDB - expanderEnvDB);
            else
                expanderEnvDB = gateRelCoeff * expanderEnvDB + (1.0f - gateRelCoeff) * detDB;
            if (std::abs(expanderEnvDB) < 1e-10f) expanderEnvDB = -100.0f;

            float expanderTargetDB = 0.0f;
            if (gateThreshDB > -79.0f)  // gate is active
            {
                // Hysteresis: opens at thresh, closes at thresh - 6dB
                float openThresh = gateThreshDB;
                float closeThresh = gateThreshDB - 6.0f;
                float gateRange = 30.0f; // max attenuation in dB (softer than 40)

                if (expanderEnvDB < closeThresh)
                {
                    float below = closeThresh - expanderEnvDB;
                    float ea = juce::jlimit(0.0f, 1.0f, below / 15.0f);
                    expanderTargetDB = ea * -gateRange;
                    gateOpen = false;
                }
                else if (expanderEnvDB > openThresh)
                {
                    expanderTargetDB = 0.0f;
                    gateOpen = true;
                }
                else
                {
                    // In hysteresis zone — hold previous state
                    if (!gateOpen)
                    {
                        float below = closeThresh - expanderEnvDB;
                        float ea = juce::jlimit(0.0f, 1.0f, below / 15.0f);
                        expanderTargetDB = ea * -gateRange;
                    }
                }
            }
            // Asymmetric smoothing: fast open (2ms), slow close (30ms)
            float gateSmooth = (expanderTargetDB > smoothExpanderGainDB) ? gateOpenCoeff : gateCloseCoeff;
            smoothExpanderGainDB = smoothExpanderGainDB * gateSmooth + expanderTargetDB * (1.0f - gateSmooth);
            if (std::abs(smoothExpanderGainDB) < 0.01f) smoothExpanderGainDB = 0.0f;

            // ===== STORE GAIN IN dB DOMAIN =====
            float totalGainDB = -grDB + smoothExpanderGainDB;

            delayBufferL[delayWritePos] = inL;
            delayBufferR[delayWritePos] = inR;
            gainDBBuffer[delayWritePos] = totalGainDB;

            int readPos = (delayWritePos - lookahead + (int)delayBufferL.size()) % (int)delayBufferL.size();
            float delayedL = delayBufferL[readPos];
            float delayedR = delayBufferR[readPos];

            float appliedGainDB;

            if (smoothAttack)
            {
                // ===== LOOKAHEAD =====
                // Target the lowest gain required anywhere in the window, so
                // reduction is already underway when the peak reaches the
                // output. The two one-pole stages below turn that step into the
                // actual ramp.
                //
                // This used to cosine-interpolate between the window minimum
                // and the gain at the read position, weighted by how far away
                // the minimum was — but the weighting ran backwards: the closer
                // the peak, the *less* reduction was applied. On an isolated
                // transient the target dropped to full depth immediately, drifted
                // back toward unity over the window, then snapped down again as
                // the peak landed, which is a gain ripple rather than a ramp.
                float windowMinDB = gainMin.pushAndGet(totalGainDB);
                // Withdrawing the pre-emption is what lets a transient through.
                // Both terms read the same delay line, so the latency is
                // untouched and only the choice of gain changes.
                if (lookaheadBlend < 0.999f)
                    windowMinDB = gainDBBuffer[readPos]
                                + lookaheadBlend * (windowMinDB - gainDBBuffer[readPos]);

                // Stage 1: primary envelope smoothing
                smoothedGainDB = smoothCoeff1 * smoothedGainDB + (1.0f - smoothCoeff1) * windowMinDB;
                // Stage 2: micro-jitter removal
                smoothedGainDB2 = smoothCoeff2 * smoothedGainDB2 + (1.0f - smoothCoeff2) * smoothedGainDB;
                appliedGainDB = smoothedGainDB2;
            }
            else
            {
                // ===== PUNCHY MODE =====
                float targetGainDB = gainDBBuffer[readPos];
                smoothedGainDB = punchySmoothCoeff * smoothedGainDB + (1.0f - punchySmoothCoeff) * targetGainDB;
                smoothedGainDB2 = smoothCoeff2 * smoothedGainDB2 + (1.0f - smoothCoeff2) * smoothedGainDB;
                appliedGainDB = smoothedGainDB2;
            }

            // ===== ANTI-ALIASED GAIN APPLICATION =====
            static constexpr float dB2LinFactor = 0.11512925464970229f; // ln(10)/20
            float targetGain = std::exp(appliedGainDB * dB2LinFactor);
            float appliedGain = prevAppliedGainLin + (targetGain - prevAppliedGainLin) * 0.5f;
            prevAppliedGainLin = targetGain;

            delayWritePos = (delayWritePos + 1) % (int)delayBufferL.size();

            bufferL[i] = delayedL * appliedGain;
            bufferR[i] = delayedR * appliedGain;

            gainReductionDB = grDB;
            prevGrDB = grDB;
            pushGRHistory(grDB, envDB);

            // Per-sample GR metering — must be inside loop for correct timing
            smoothGR = grMeterCoeff * smoothGR + (1.0f - grMeterCoeff) * grDB;
        }

        sanitizeState();
    }

    float getGainReductionDB() const { return smoothGR; }
    // The slow programme level the threshold is referenced to. The drive above
    // knob 24 reads the same number, so drive and threshold cannot drift apart.
    float getProgrammeLevelDB() const { return autoLevelPrimed ? driveRefDB : -60.0f; }
    float getStaticMakeupDB()   const { return staticMakeupDB; }
    // 0..1: how much of the drive the current material earns. See DRIVE_HOLD_DB.
    float getDriveScale()       const { return driveScale; }
    bool  isProgrammeLevelPrimed() const { return autoLevelPrimed; }
    // Gate state, already computed internally but not previously exposed —
    // needed to draw the threshold/attenuation on the IN meter.
    bool isGateOpen() const { return gateOpen; }
    float getGateReductionDB() const { return smoothExpanderGainDB; }
    int getLatencySamples() const { return lookahead; }
    const std::array<float, GR_HISTORY_SIZE>& getGRHistory() const { return grHistory; }
    int getGRHistoryWritePos() const { return grHistoryWritePos.load(std::memory_order_acquire); }

    // Public for OS mode switch state preservation
    float smoothGR = 0.0f;
    std::array<float, GR_HISTORY_SIZE> grHistory {};
    std::array<float, GR_HISTORY_SIZE> inputHistory {};
    std::array<float, GR_HISTORY_SIZE> outputHistory {};  // input - GR for before/after display
    // Read by the editor's 60 Hz timer while the audio thread writes it. Atomic
    // with release/acquire so the compiler cannot cache or reorder it — with LTO
    // enabled a plain int could be hoisted out of the paint loop entirely.
    std::atomic<int> grHistoryWritePos { 0 };

private:
    // Every follower and filter below is pure IIR state: once it holds a NaN or
    // Inf it can never return to a valid value on its own, and the -100 dB
    // fallback in the detector turns that into "no signal" rather than an
    // audible fault — the compressor would just silently stop compressing.
    // Checked once per block, so the cost is negligible.
    void sanitizeState()
    {
        bool bad = ! (std::isfinite(rmsSquaredSum) && std::isfinite(peakEnv)
                   && std::isfinite(envDB) && std::isfinite(expanderEnvDB)
                   && std::isfinite(smoothedGainDB) && std::isfinite(smoothedGainDB2)
                   && std::isfinite(prevAppliedGainLin) && std::isfinite(detLowLP)
                   && std::isfinite(smoothExpanderGainDB) && std::isfinite(smoothGR)
                   && std::isfinite(staticMakeupDB) && std::isfinite(driveScale)
                   && std::isfinite(driveRefDB)
                   && std::isfinite(driveLevelDB));
        if (! bad)
            bad = ! (std::isfinite(scHpfX1) && std::isfinite(scHpfX2)
                  && std::isfinite(scHpfY1) && std::isfinite(scHpfY2));
        if (bad)
            reset();
    }


    // The detector, split out so it can also be run while the knob sits at zero
    // — see the unity path below for why that matters.
    float runDetector(float detL, float detR, float peakBlend, float rmsC)
    {
        // Sidechain filtering happens on the WAVEFORM, before rectification.
        // It used to run after sqrt(L^2+R^2), i.e. on an already-rectified
        // signal — filtering an envelope rather than audio, so neither the
        // SC HPF knob nor the 200 Hz shelf did what its label said. A 50 Hz
        // tone rectifies to a ~100 Hz ripple riding on DC, so high-passing
        // afterwards removed the DC but left the ripple, and the knob's
        // frequency had no straightforward meaning at all.
        float mono = (detL + detR) * 0.5f;

        // Sidechain HPF: keeps bass out of the detector (audio untouched)
        if (scHpfFreq > 1.0f) {
            // 2nd order Butterworth HPF via biquad
            float scHpfOut = scHpfB0 * mono + scHpfB1 * scHpfX1 + scHpfB2 * scHpfX2
                       - scHpfA1 * scHpfY1 - scHpfA2 * scHpfY2;
            scHpfX2 = scHpfX1; scHpfX1 = mono;
            scHpfY2 = scHpfY1; scHpfY1 = scHpfOut;
            mono = scHpfOut;
        }

        // Fixed low shelf: -3 dB below 200 Hz, keeps rumble out of the detector
        detLowLP = detLowLP + detLowCoeff * (mono - detLowLP);
        mono = mono - detLowCutGain * detLowLP;

        // ===== HYBRID RMS/PEAK DETECTOR =====
        float monoAbs = std::abs(mono);

        if (monoAbs > peakEnv)
            peakEnv = peakAttackCoeff * peakEnv + (1.0f - peakAttackCoeff) * monoAbs;
        else
            peakEnv = peakReleaseCoeff * peakEnv + (1.0f - peakReleaseCoeff) * monoAbs;

        float squared = monoAbs * monoAbs;
        rmsSquaredSum = rmsC * rmsSquaredSum + (1.0f - rmsC) * squared;
        float rmsLevel = std::sqrt(rmsSquaredSum);

        float detLevel = rmsLevel * (1.0f - peakBlend) + peakEnv * peakBlend;
        if (detLevel < 1e-15f) detLevel = 0.0f;
        // 20*log10(x) = 8.6858896*ln(x) — std::log is ~2x faster than log10
        static constexpr float ln2dB = 8.685889638065037f; // 20/ln(10)
        return (detLevel > 1e-10f) ? ln2dB * std::log(detLevel) : -100.0f;
    }

    // The slow programme level the threshold is referenced to. Only tracked
    // while there is signal, otherwise a pause would drag it down and the vocal
    // would get slammed on the way back in.
    void trackProgrammeLevel(float detDB)
    {
        if (detDB > -60.0f) {
            if (! autoLevelPrimed) { autoLevelDB = detDB; autoLevelPrimed = true; }
            float c = (detDB > autoLevelDB) ? autoLevelRiseCoeff : autoLevelFallCoeff;
            autoLevelDB = c * autoLevelDB + (1.0f - c) * detDB;
        }
    }

    // Warm soft-knee with smoothstep S-curve
    static float computeGainReduction(float inputDB, float thresholdDB, float ratio, float kneeDB)
    {
        float halfKnee = kneeDB / 2.0f;
        float output;
        if (inputDB < (thresholdDB - halfKnee))
        {
            output = inputDB;
        }
        else if (inputDB > (thresholdDB + halfKnee))
        {
            output = thresholdDB + (inputDB - thresholdDB) / ratio;
        }
        else
        {
            float x = inputDB - thresholdDB + halfKnee;
            float t = x / kneeDB;
            float tSat = t * t * (3.0f - 2.0f * t); // smoothstep
            float fullGR = (inputDB - thresholdDB) * (1.0f - 1.0f / ratio);
            output = inputDB - tSat * fullGR;
        }
        return std::max(0.0f, inputDB - output);
    }

    void pushGRHistory(float grDB, float inputDB = -100.0f)
    {
        grHistoryBlockAccum = std::max(grHistoryBlockAccum, grDB);
        inputHistoryBlockAccum = std::max(inputHistoryBlockAccum, inputDB);
        // Output = input, minus the reduction, plus the makeup that follows it.
        // The makeup term used to be missing, which was harmless while makeup
        // simply undid the reduction, but the wall's drive can add 20 dB or more
        // on top: without it the "after" trace draws the signal collapsing to
        // nothing at precisely the setting where the output is pinned at the
        // ceiling. One block of lag, which is invisible on a timeline display.
        float outDB = inputDB - grDB + displayMakeupDB;
        outputHistoryBlockAccum = std::max(outputHistoryBlockAccum, outDB);
        grHistorySampleCounter++;
        if (grHistorySampleCounter >= grHistorySamplesPerSlot) {
            const int wp = grHistoryWritePos.load(std::memory_order_relaxed);
            grHistory[(size_t)wp] = grHistoryBlockAccum;
            inputHistory[(size_t)wp] = inputHistoryBlockAccum;
            outputHistory[(size_t)wp] = outputHistoryBlockAccum;
            grHistoryWritePos.store((wp + 1) % GR_HISTORY_SIZE, std::memory_order_release);
            grHistorySampleCounter = 0;
            grHistoryBlockAccum = 0.0f;
            inputHistoryBlockAccum = -100.0f;
            outputHistoryBlockAccum = -100.0f;
        }
    }

    double sr = 44100.0, invSr = 1.0 / 44100.0;
    int lookahead = 64;
    std::vector<float> delayBufferL, delayBufferR;
    std::vector<float> gainDBBuffer; // gain stored in dB, not linear
    SlidingMinimum gainMin;          // lowest gain across the lookahead window
    int delayWritePos = 0;

    float releaseCoeffFast = 0.0f, releaseCoeffSlow = 0.0f;
    float attackCoeff = 0.0f;
    float rmsCoeff = 0.0f;
    float peakAttackCoeff = 0.0f, peakReleaseCoeff = 0.0f;
    float gainSmoothCoeff = 0.0f;
    float smoothCoeff1 = 0.0f, smoothCoeff2 = 0.0f, punchySmoothCoeff = 0.0f;
    float grMeterCoeff = 0.0f;

    float rmsSquaredSum = 0.0f;
    float peakEnv = 0.0f;
    float envDB = -100.0f;
    // Slow programme level the auto threshold is referenced to
    float autoLevelDB = -18.0f;
    bool  autoLevelPrimed = false;
    float autoLevelRiseCoeff = 0.0f, autoLevelFallCoeff = 0.0f;
    float staticMakeupDB = 0.0f;
    float driveScale = 0.0f;
    float driveRiseCoeff = 0.0f, driveFallCoeff = 0.0f;
    float driveRefDB = -60.0f;
    float driveRefRiseCoeff = 0.0f, driveRefFallCoeff = 0.0f;
    float driveLevelDB = -60.0f;
    float driveLevelCoeff = 0.0f;
    float expanderEnvDB = -100.0f;
    float expanderReleaseCoeff = 0.0f;
    float gateOpenCoeff = 0.0f, gateCloseCoeff = 0.0f;
    float gateDetAttackCoeff = 0.0f;
    float smoothExpanderGainDB = 0.0f;
    bool gateOpen = true;
    float gainReductionDB = 0.0f;
    // smoothGR, grHistory, grHistoryWritePos are public (above private:)
    float smoothedGainDB = 0.0f; // stage 1 smoothing
    float smoothedGainDB2 = 0.0f; // stage 2 micro-jitter removal
    float prevAppliedGainLin = 1.0f; // for per-sample gain interpolation
    float prevGrDB = 0.0f;

    // Detector low shelf cut
    float detLowLP = 0.0f;
    float detLowCoeff = 0.0f;
    float detLowCutGain = 0.29f;




    // Sidechain HPF (detector only, 2nd order Butterworth biquad)
    float scHpfFreq = 0.0f;  // 0 = off
    float scHpfB0 = 1.0f, scHpfB1 = 0.0f, scHpfB2 = 0.0f;
    float scHpfA1 = 0.0f, scHpfA2 = 0.0f;
    float scHpfX1 = 0.0f, scHpfX2 = 0.0f;
    float scHpfY1 = 0.0f, scHpfY2 = 0.0f;
    float scHpfLastFreq = -1.0f;
    float scHpfSampleRate = 44100.0f;

    int grHistorySampleCounter = 0;
    int grHistorySamplesPerSlot = 1;
    float grHistoryBlockAccum = 0.0f;
    float inputHistoryBlockAccum = -100.0f;
    float outputHistoryBlockAccum = -100.0f;
};

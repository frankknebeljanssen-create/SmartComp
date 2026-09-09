# SmartComp

Free one-knob compressor for macOS (VST3/AU) that finds its own sweet spot —
and, turned all the way up, stops being polite about it.

![SmartComp screenshot](docs/screenshot.png)

## Why

Most compressors need gain staging before the knob does anything useful: set
the threshold to a fixed dBFS value, and a quiet take sits below it doing
nothing while a hot take slams. SmartComp's threshold tracks a slow average of
the incoming level instead, so a given knob position delivers roughly the same
amount of gain reduction whether the track came in at −30 dBFS or −15 dBFS —
measured, within about 1.5 dB across that range.

Makeup replaces the loudness the compressor actually removed, measured as
K-weighted energy either side of it. Turning the knob up therefore changes the
character without changing the level, which is what lets you judge it by ear
instead of by loudness.

## The knob

**Bottom two thirds — the compressor.** Loudness holds still while the
dynamics tighten. This is where AUTO lives and where a vocal wants to sit.

**AUTO** watches where the sweet spot currently is and drives the knob there,
following it as the performance changes. Drag the knob away and it holds while
your mouse is down, then pulls back when you let go — harder the further you
dragged it, so a haul out of the red snaps back in about a second while a small
nudge eases back gently.

The sweet spot is a claim about gain reduction, not about knob position: its
edges are where the compressor delivers 3 and 5 dB on peaks, and `auto_probe`
measures both every run.

**Top third — the wall.** From knob 24 upward the plugin stops restoring the
level and starts driving into the limiter: the threshold deepens, the detector
window shortens, the release lengthens so the gain holds instead of recovering
between hits, and the makeup crosses over to an absolute loudness target. On a
drum loop this takes the short-term loudness spread from 14.2 dB in to 5.6 dB
out with the peaks sitting on the ceiling. It is not subtle and is not meant to
be — turn it down from there.

Below knob 24 none of that is active, so the gentle range is untouched by it.

## Signal chain

```
In Trim → Compressor (auto-threshold, gate, 2x oversampled) → Makeup
   → TRUE LEVEL → Limiter → Dry/Wet Mix → Out Gain
```

The limiter's ceiling is fixed at −0.3 dBFS and it holds it to within 0.03 dB.
Through the lower two thirds of the knob it does nothing at all; the wall is
the only thing that drives into it. Reported latency is 158 samples at
44.1 kHz (3.58 ms), fully compensated for the dry/wet mix and for bypass.

## Controls

| Control | Range | What it does |
|---|---|---|
| **Compression** | 0–36 | The main knob. Auto-tracking threshold and ratio; a wall above 24. |
| **AUTO** | on/off | Drives the knob to the sweet spot and keeps following it. |
| **TRUE LEVEL** | on/off | Matches output loudness to input, so you hear character rather than volume. |
| **Gate** | −80 to −20 dB | Noise gate, off at minimum. Its threshold is drawn on the input meter, with the current attenuation next to it. |
| **SC HP Filter** | 0–400 Hz | High-pass on the detector only, so bass does not pump the gain reduction. The panel draws its actual response. |
| **Mix** | 0–100% | Parallel compression blend, latency-compensated. |
| **In Trim** | ±12 dB | Input gain, ahead of the detector. |
| **Out Gain** | −24 to +12 dB | True output level, applied after everything. Not the limiter ceiling. |
| **Bypass** | — | Latency-compensated, click-free. |

AUTO and TRUE LEVEL are buttons on the interface rather than automatable
parameters. Everything else is exposed to the host.

## Requirements

- macOS (Apple Silicon or Intel)
- A VST3 or Audio Unit host (Ableton Live, Logic Pro, etc.)

## Install

No signed release yet — see [INSTALL.md](INSTALL.md) for how to remove
macOS's quarantine flag from an unsigned build.

Prebuilt binaries will appear under
[Releases](https://github.com/frankknebeljanssen-create/SmartComp/releases)
once available. Until then, build from source below.

## Building from source

Requires CMake 3.15+ and Xcode command line tools.

```bash
git clone https://github.com/frankknebeljanssen-create/SmartComp.git
cd SmartComp
./build_and_install.sh
```

This clones [JUCE](https://github.com/juce-framework/JUCE) into
`~/Library/Caches/SmartComp-juce` (kept outside the repo and outside any
Dropbox-synced folder — a synced build directory causes CMake cache
conflicts), builds the plugin, and installs the VST3 to
`~/Library/Audio/Plug-Ins/VST3/`. The AU and standalone builds are produced but
not installed.

## Measuring instead of guessing

Claims about how this thing behaves are checked against the real code, because
on this project reasoning about the DSP and porting it to Python have both
produced wrong numbers. Three harnesses, all runnable:

`tests/dsp_bench.cpp` exercises the DSP classes directly — limiter ceiling,
transient response, delivered time constants, and whether a knob position
means the same thing at different source levels.

```bash
cmake --build ~/Library/Caches/SmartComp-build --target dsp_bench
~/Library/Caches/SmartComp-build/dsp_bench_artefacts/Release/dsp_bench
```

`tests/density_probe.cpp` drives the whole processor and asks what comes out
over time: short-term loudness spread, how far the plugin's own gain swings,
how close the output gets to the ceiling, whether the result depends on how
hot the source was, and what the drive does to a room bed in the pauses. Two
reference rows bracket what is reachable on the material at all — the limiter
driven hard, and a perfect 20 ms AGC.

```bash
cmake --build ~/Library/Caches/SmartComp-build --target density_probe
~/Library/Caches/SmartComp-build/density_probe_artefacts/Release/density_probe
```

`tests/auto_probe.cpp` is a behavioural test for AUTO: that the knob converges
on the sweet spot, that the band's edges deliver the 3 and 5 dB they promise,
that a drag holds while the mouse is down and pulls back when it is released,
and that parking the knob in the red with AUTO off recovers rather than
sticking.

```bash
cmake --build ~/Library/Caches/SmartComp-build --target auto_probe
~/Library/Caches/SmartComp-build/auto_probe_artefacts/Release/auto_probe
```

The screenshot above is generated the same way rather than mocked up:
`tests/ui_shot.cpp` runs the real processor and editor through a genuine JUCE
event loop against a synthetic vocal, then saves what actually renders.

```bash
cmake --build ~/Library/Caches/SmartComp-build --target ui_shot
open ~/Library/Caches/SmartComp-build/ui_shot_artefacts/Release/ui_shot.app --args /tmp/screenshot.png
```

## Known limits

Driven hard, the wall lifts whatever is in the pauses along with the music. A
room bed at −70 dBFS still sits 48 dB under the words and one at −60 dBFS
38 dB under, both comfortably out of the way. At −50 dBFS the protection
cannot keep up and the gap closes to 7 dB. Gate it, or back the knob off.

## License

Not yet decided. The repository is public for now; no license has been
granted for reuse. This section will be updated before the first release.

## Companion plugin

SmartLim, a matching limiter, is planned as the second half of a set.

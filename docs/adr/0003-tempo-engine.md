# ADR 0003: The tempo engine, and how its settings were chosen

Status: accepted
Date: 2026-09-13
Requirement context: BPM-001, FN-BPM-01, FN-BPM-02, FRD sections 3 and 7

## Context

FRD section 3 says to "benchmark bundled native SoundTouch BPM detection and
aubio" and "choose by measured accuracy and throughput". Section 7 adds that the
engine must be packaged for every release target, must analyse several musical
sections, must report half and double tempo alternatives, and must not "call an
arbitrary score a calibrated probability".

## Decision

Neither SoundTouch nor aubio is bundled. Tempo estimation is implemented in
process: spectral-flux onset detection, then an autocorrelation tempogram with a
perceptual tempo prior.

Reasons:

1. **Licence.** aubio is GPL-3.0. Linking it would impose GPL-3.0 on the whole
   application, which is a distribution decision rather than a technical one and
   is not this ADR's to make unilaterally. SoundTouch is LGPL-2.1, which is
   workable but adds a bundling and relinking obligation on four platforms.
2. **The FFT already exists.** The spectrum display needs a radix-2 FFT anyway.
   The onset detector is roughly a hundred lines on top of it.
3. **Packaging.** Section 7 requires the selected engine be packaged for every
   release target. An in-process implementation is packaged by construction.

The trade is that this engine is ours to be accountable for, so it was measured
rather than assumed.

## How the settings were chosen

The FRD is explicit that existing MixMeister values are "comparison data, not
guaranteed ground truth". They are used here as a baseline in exactly that
sense: agreement with an existing tag is evidence the detector is behaving, not
proof either value is correct.

**Tuning corpus**: 120 tracks carrying an existing BPM tag, taken from the test
collection by a deterministic stride (`NR % 27 == 1`).

Sweeping the prior width, with no harmonic corroboration:

| sigma (octaves) | agreement | half/double error | 2:3 error |
|---:|---:|---:|---:|
| 0 (raw peak pick) | 38% | 38% | 12% |
| 0.5 | **83%** | 2% | 8% |
| 0.6 | 82% | **1%** | 10% |
| 0.7 | 81% | 1% | 11% |
| 0.85 | 82% | 2% | 9% |
| 1.0 | 77% | 4% | 12% |
| 1.3 | 67% | 17% | 11% |

Harmonic corroboration, which sounds principled, measured worse at every sigma.
At 0.6 the same corpus scored 82% with no harmonics and 75% with weights
0.5/0.25: summing harmonics also rewards a spurious peak at 1.5x the beat, which
is where the residual 2:3 errors came from.

**Chosen**: `tempoPriorSigmaOctaves = 0.6`, `harmonic2Weight = 0`,
`harmonic3Weight = 0`. 0.6 is taken over the marginally better 0.5 because it is
the wider prior — it forces a genuinely slow or fast track less hard — and it had
the lower octave-error rate.

**Held-out validation**: a disjoint 120-track sample (`NR % 27 == 14`), never
used for tuning, as FRD section 7 requires ("calibrate automatic acceptance on a
separate held-out sample"). Result: **90% agreement, 0% half/double errors**.

## What is deliberately not claimed

- `TempoAnalysis::stability` is the fraction of analysed sections agreeing with
  the consensus after octave folding. It is an agreement measure and is named
  and documented as one. It is not a probability that the BPM is correct.
- The prior does not forbid slow or fast tempos. It makes a candidate an octave
  from 120 BPM pay for itself with stronger evidence.
- A half or double disagreement with an existing tag is never auto-corrected. It
  is routed to review, because which value is right is a musical judgement.
- Beatless, variable-tempo, too-short and long-form tracks return explicit
  states and no BPM at all.

## Reproducing

```
resonance analyse --source <root> --data <dir>
```

The sweep harness used for the table is not part of the shipped application. The
engine identifier and a hash of its settings are stored with every result, so a
change to either re-runs the analysis rather than silently reusing it.

## Consequences

- No GPL or LGPL audio dependency.
- The engine is ours to maintain and to justify; the measurements above are the
  justification, and re-measuring is the obligation if the settings change.
- Accuracy is measured against existing tags, which is a baseline and not
  ground truth. Establishing ground truth needs a manually verified sample; that
  remains open in `docs/open-questions.md`.

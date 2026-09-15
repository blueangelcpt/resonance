# Progress log

Project: Resonance (`blueangelcpt/resonance`)
Host: Ubuntu resolute, x86_64, GCC 15.2.0, CMake 4.2.3, Ninja 1.13.2, Qt 6.10.2,
TagLib 2.2.1, SQLite 3.46.1.
Last updated: 2026-09-13.

Status vocabulary and per-requirement detail are in `docs/traceability.md`.
Everything **not** done is in `docs/open-questions.md`.

## Build and test

```bash
cmake --preset linux-release
cmake --build --preset linux-release
ctest --preset linux-release
```

| | |
|---|---|
| Configure | exit 0 |
| Build | exit 0, **zero warnings in first-party code** under `-Wall -Wextra -Wpedantic -Wcast-qual -Wconversion -Wsign-conversion -Wnon-virtual-dtor -Woverloaded-virtual -Wdouble-promotion -Wformat=2` |
| Tests | **3/3 suites, 89 cases, 292 assertions, 0 failures** |
| CLI | `build/linux-release/src/MusicLibrary.Cli/resonance`, 1.77 MB |
| Desktop | `build/linux-release/src/MusicLibrary.Desktop/resonance-desktop`, 2.43 MB |

Third-party headers are included as system headers, so the strict warning set
applies to this project's code and not to vendored code.

## Measured against the supplied collection

3240 MP3 files, 33 GB, at `/home/ian/mp3-tool/music`.

### Scan

| | |
|---|---|
| Files seen | 3240 |
| Unreadable | **0** |
| Container parse warnings | **0** |
| Elapsed | 413 s (≈7 files/s, hashing whole file **and** audio range) |
| Tag versions | 3233 ID3v2.3, 7 ID3v2.4 |
| APEv2 present | 542 |
| ID3v1 present | 3229 |
| Bitrate mode | 2432 CBR, 808 VBR |
| Xing/LAME headers | 2489 both, 617 Xing only, 134 neither |

### Source protection — SAFE-001

Every source file was hashed before and after a full scan:

```
find music -iname '*.mp3' -print0 | xargs -0 sha256sum | sort -k2
```

**All 3240 files byte-identical afterwards.** The same check passes after an
export run.

### Albums — ID-001

1613 provisional groups, **626 needing review**, dominated by missing track
numbers (589 files), which genuinely block the naming template.

An earlier run reported 1572 of 1613 needing review. That was a defect, not a
finding: single-track groups and container-folder name mismatches were being
treated as review items. Advisory flags are now distinguished and the decision
is stored via schema migration 002 rather than re-derived in SQL.

### Gain and privacy — GAIN-001, PRIV-001

| | |
|---|---|
| Files with ReplayGain fields | 524 |
| ReplayGain fields to remove | 1048 |
| MP3Gain undo data | 0 (no exceptions raised) |
| Privacy findings | 0 |

An earlier survey produced 252 privacy findings, all on `CATALOG`, `DISCID` and
`CRC-32` fields. Those are public release identifiers, not personal data: the
numeric-identifier heuristic was too broad. A public-field allowlist removed
every false positive with no loss of genuine findings.

### BPM — BPM-001

Tuned on 120 tracks carrying an existing BPM tag (deterministic stride
`NR % 27 == 1`), validated on a **disjoint** 120-track sample (`NR % 27 == 14`)
never used for tuning.

| Configuration | Agreement | Octave errors |
|---|---:|---:|
| Raw peak pick (no prior) | 38% | 38% |
| Prior σ=0.5 | 83% | 2% |
| **Prior σ=0.6 (chosen)** | 82% | 1% |
| Prior σ=1.3 | 67% | 17% |
| σ=0.6 with harmonic corroboration | 75% | — |

**Held-out result: 90% agreement, 0% octave errors.**

Existing tags are a baseline, not ground truth — the FRD is explicit about this.
Reasoning in `docs/adr/0003-tempo-engine.md`.

### Artwork — ART-001

Verified end to end against a live iTunes lookup: a 3000×3000 asset was
requested, **measured by decoding** at 3000×3000, and reduced to a 600×600
baseline JPEG, 3 components, deterministic across runs.

`file` on the output:
```
JPEG image data, JFIF standard 1.01, baseline, precision 8, 600x600, components 3
```

### Providers

| | |
|---|---|
| iTunes | 12 deduplicated candidates for a real album, each with evidence |
| LRCLIB | matched a real recording with plain and synced lyrics |
| MusicBrainz | 1.1 s rate limit honoured |

## Defects found and fixed

Each was found by running the code against real data or real tests, not by
inspection.

| Defect | How it was found | Fix |
|---|---|---|
| Nested `BEGIN` inside a batched scan transaction | Full scan aborted at file 1 | Savepoints for nested transactions |
| Padding trim cut into a frame ending in `0x00` | Re-read showed padding off by one | Padding computed from parsed frame extents |
| Frame ordinals counted per frame id | Preservation check reported an untouched frame as lost | Ordinals counted per discriminated key |
| Picture frame ordinal assigned twice | Embedded artwork never displayed | Assigned once, at the picture push site |
| Default `string_view` bound as SQL NULL | `NOT NULL` violation registering an output root | Bind the empty string explicitly |
| Writer accepted a file with no MPEG frame | Test wrapped a text file in an ID3 tag | Require a confirmed frame sync |
| Privacy heuristic flagged public fields | 252 findings on the real collection | Public-release-field allowlist |
| Album review flags too aggressive | 1572 of 1613 groups flagged | Advisory flags distinguished |
| LRCLIB album name as exact-match requirement | Live lookup returned nothing | Album compared by the matching policy |
| Raw edit distance rejected correct releases | "Desire" vs "Desire - Single" scored 0.46 | Edition-aware title and artist comparison |
| Filter debounce discarded a completed analysis | Spectrum blanked after selecting a track | Signal blocked for programmatic filters |
| `MINIMP3_FLOAT_OUTPUT` in one translation unit only | float/int16 mismatch across decoder call sites | Define moved onto the interface target |
| Test macro bound a reference into a temporary | Assertion read freed memory | Compare by value |

## Current state by milestone

| Milestone | State |
|---|---|
| 0. Cross-platform proof | **Linux x64 only.** Recipes for all four targets; nothing built or installed on Windows or macOS. |
| 1. Read-only inventory | Done and measured at 3240 files. |
| 2. Durable jobs and copy safety | Copy safety done and verified. Durable job *queue* is schema-only; commands run inline. |
| 3. Album matching and artwork | Done; provider-confirmed release identity not yet applied as an accepted correction. |
| 4. Tag writer and organisation | Done and verified, including the preservation gate. |
| 5. BPM and lyrics | Done and measured. |
| 6. Optimisation | Metadata compaction done. **MP3packer repacking not started.** |
| 7. Desktop interface | Done, including playback and the supplied Resonance design. |
| 8. Scale and release qualification | **Not started.** No signing, no installer lifecycle tests, no 70,000-entry exercise. |

## Next executable step

1. Run the CI workflow to produce Windows and macOS artifacts, and record what
   actually installed and launched on each.
2. Confirm audible playback on a machine with a working audio output device.
3. Decide the FFmpeg-via-Qt-Multimedia licensing question before any binary
   redistribution (`native/DEPENDENCIES.md`).
4. Run forced-termination and disk-full recovery tests.

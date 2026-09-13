# Open questions and external blockers

Maintained so another agent, or the same one after a context loss, can resume
without reconstructing the session. Every item here is something **not** done,
stated so it cannot be mistaken for something done.

Last updated: 2026-09-13.

## Blocked on something outside this environment

### 1. Windows, macOS ARM64 and macOS x64 artifacts — DIST-001

Recipes exist (CPack NSIS, DragNDrop, DEB) and the CI workflow targets all four.
**Only the Linux x64 binaries have been built and run here.** Nothing has been
installed or launched on Windows or macOS. Per FRD section 16, an untested
architecture must not be described as qualified.

Next step: run the release workflow on GitHub-hosted runners and record what
actually installed and launched.

### 2. Code signing and notarisation — REL-001

No signing credentials are available. Windows builds would be unsigned; macOS
builds unsigned and unnotarised. The FRD is explicit that pretend signing
identities must never be generated and that an unsigned build must not be called
signed.

Next step: provision an Authenticode certificate and an Apple Developer ID, then
add them as protected CI secrets.

### 3. Audio playback has not been heard

Playback is implemented: decode, a `QAudioSink` feed, transport, seek, volume,
and the analyser following the real playhead. It compiles and the application
runs.

**No audio has actually been played.** This machine's PipeWire exposes only a
null sink, so Qt reports zero output devices and the transport is correctly
disabled. Decode is exercised by the analyser path; the sink path is not.

Next step: run on a machine with a working output device and confirm audible,
correctly-pitched playback, seek accuracy and clean pause/resume.

### 4. Qt Multimedia reintroduces an FFmpeg dependency

Qt's multimedia backend loads FFmpeg 8.0.1, identifying as GPL version 2 or
later. Resonance neither bundles nor calls FFmpeg, but it is now in the runtime
dependency chain on Linux.

Next step: decide before any binary redistribution whether to accept a GPL
FFmpeg in the chain, require an LGPL FFmpeg build of Qt Multimedia, or replace
Qt Multimedia with a direct platform audio API. See `native/DEPENDENCIES.md`.

## Needs information from the user

### 5. The exact foobar2000 optimisation command — FRD section 14

FRD section 2 establishes that foobar2000 is not open source and its
implementation cannot be assumed. Section 14 lists the exact menu command and a
before/after test pair as required inputs.

Without it, "equivalent to what foobar2000 does" cannot be claimed. Metadata
compaction is implemented on its own merits; equivalence is not asserted.

### 6. RIOT reference outputs for artwork comparison — FRD section 6

The derivative is deterministic and matches the specified configuration, but the
FRD asks to "validate against your RIOT reference examples". No reference outputs
have been supplied, so no visual comparison has been made.

### 7. Manually verified BPM values — FRD section 14

The tempo engine measures **90% agreement with existing BPM tags** on a held-out
120-track sample. The FRD is explicit that MixMeister values are comparison data,
not ground truth. Establishing real accuracy needs a small manually verified set.

### 8. Player compatibility spot checks — FRD section 13

Outputs have not been opened in MediaMonkey, foobar2000 or a third player. The
writer's own verification confirms the MPEG payload is byte-identical and tags
round-trip, which is not the same as a player accepting them.

### 9. Compilation and multidisc naming exceptions — FRD section 14

The template is implemented exactly as specified. Multidisc collisions and
compilation edge cases currently route to review rather than applying a rule,
because FRD section 9 forbids inventing one (no disc folders, no year, no
article removal). A configured rule needs the user's decision.

## Deferred implementation

### 10. MP3packer repacking adapter — OPT-001 second mode

Not implemented. Metadata compaction is done; lossless stream repacking is not
started. `Mp3Decoder::decodePcm` exists for the full PCM comparison the FRD
requires to qualify it. FRD section 12 is explicit that a failed optional
repacker must not block a useful metadata-cleaning release.

### 11. Durable job queue as the execution path — JOB-001, FN-JOB-01

The `jobs` and `job_attempts` schema exists with lease, attempt and retry
columns, and results are keyed by engine and settings hash so completed work is
not repeated. **Commands currently execute inline rather than through the
queue.** Pause, resume and cancel work; per-job retry is not exposed.

The write journal (`file_operations`) *is* durable and drives recovery.

### 12. Chromaprint / AcoustID fingerprinting — FRD section 5, optional

Not implemented. Album identity currently rests on tags, folder structure and
provider text matching.

### 13. Crash-recovery and fault injection tests — FN-SAFE-03, FRD section 13

`recover()` is implemented and idempotence is tested. **Forced termination,
disk-full and file-lock tests have not been run.** FRD section 15 is explicit
that fault tests must cover interruption during commit, not just parser
correctness.

### 14. Scale exercise at 70,000 entries — FRD section 13

Verified at 3240 files. The catalogue design (paged queries, bounded cache,
indexed filters) targets 70,000, but that has not been exercised.

### 15. Sanitiser and fuzz runs in CI

`ML_ENABLE_ASAN` and `ML_ENABLE_UBSAN` exist and the CI workflow has a
sanitiser job. A 400-iteration randomised pass over the tag reader runs in the
normal suite. A dedicated libFuzzer target and a sustained fuzz run are not set
up.

## Resolved

- **Album review noise.** Treating single-track groups and container-folder name
  mismatches as review items flagged 1572 of 1613 groups. Advisory flags are now
  distinguished; the real figure is 626, dominated by missing track numbers.
- **Privacy false positives.** A numeric-identifier heuristic flagged 252
  CATALOG, DISCID and CRC-32 fields as account-shaped. A public-release-field
  allowlist removed all of them with no loss of genuine findings.
- **LRCLIB album matching.** `/api/get` treats every supplied field as an exact
  match requirement, so passing the local album name lost correct results. The
  album is now compared by the matching policy instead.

# Requirements traceability matrix

Project: `blueangelcpt/mp3-tool` — Resonance
Baseline: `docs/requirements.md` (FRD v1.3, 11 September 2026), section 15 execution contract.
Last updated: 2026-09-13, after the Resonance interface port.

## Status vocabulary

| Status | Meaning |
|---|---|
| `NOT_STARTED` | No implementation exists. |
| `IN_PROGRESS` | Partially implemented; named gaps remain. |
| `IMPLEMENTED` | Code exists and compiles; automated verification not yet executed or not yet complete. |
| `VERIFIED` | Code exists **and** the named test executed and passed on this host. |
| `BLOCKED` | Cannot proceed here; external dependency named in `docs/open-questions.md`. |

`VERIFIED` never means "reviewed and accepted". Only an explicit human review sets
acceptance. A test pass is not review acceptance.

## Evidence sources

| Tag | Meaning |
|---|---|
| `[unit]` | `tests/unit/CorePolicyTests.cpp` — 49 cases |
| `[infra]` | `tests/unit/InfrastructureTests.cpp` — 48 cases |
| `[preserve]` | `tests/unit/PreservationTests.cpp` — 20 cases |
| `[collection]` | Measured against the user's 3240-file test collection |
| `[build]` | Produced by `cmake --build` on this host |

All three suites pass: `ctest` reports 3/3, 292 assertions, 0 failures.

## A. Mandatory requirements (FRD section 15)

| ID | Mandatory outcome | Status | Implementation | Verification |
|---|---|---|---|---|
| SAFE-001 | Source roots read-only; every output and temporary file outside them. | `VERIFIED` | `PathGuard` resolves paths and compares device/inode, not path text. Registration refuses an output root inside, containing, or resolving into a source root. | `[infra]` 9 cases: containment, shared-prefix siblings, symlink redirection, output-outside-root, absent roots. `[preserve]` writer refusal + source-hash equality. `[collection]` all 3240 source files byte-identical after a full scan and after an export run. |
| SAFE-002 | Independent copies; no hardlinks or in-place mode in v1. | `VERIFIED` | `TagWriter` only ever writes a new file; `ScopedTempFile` creates exclusively with `O_EXCL` and removes on destruction. | `[preserve]` output is a distinct inode with link count 1. `[infra]` temp-file lifetime and protected-root refusal. |
| CAT-001 | Catalogue all discovered metadata, retain unknown fields; observed/proposed/written state separate. | `VERIFIED` | `tag_frames` stores the exact payload of every frame including uninterpreted ones. `tag_snapshots.kind` separates `observed` from `written`; `file_operations` records write outcome independently. | `[preserve]` an unknown `ZZZZ` frame keeps its payload and survives a rewrite. `[infra]` schema migration and integrity check. |
| ID-001 | Identify album editions with evidence; expose ambiguity; preserve manual locks. | `IN_PROGRESS` | `AlbumResolverCore` groups on directory, album, album artist and edition qualifier; flags conflicts; `manual_decisions` survives regrouping. Provider-confirmed release identity is implemented for lookup but not yet applied as an accepted correction. | `[unit]` compilation not split by track artist, deluxe not merged, duplicate track numbers flagged, confidence tiers. `[collection]` 1613 groups, 626 needing review. |
| ART-001 | Exact 600×600 JPEG quality-75 output; preserve the received source separately. | `VERIFIED` | `ImagePipeline` + `AssetStore`; received asset stored content-addressed and unmodified, derivative generated once per (asset, config). | `[infra]` decodes back to exactly 600×600, deterministic across runs, config hash changes with settings. `[collection]` generated from a real 3000×3000 iTunes asset. |
| ART-002 | Rank correctness and condition before resolution; accept a clean 600×600 source. | `VERIFIED` | `ArtworkPolicy` ranks in three fixed tiers and never lets resolution override condition. | `[unit]` all five FRD section 6 decision fixtures, plus non-square, sub-600, unmeasured, watermarked and wrong-image refusals. |
| ART-003 | Retain provenance, uncertainty, reasons and locks; offline import without Album Art Exchange. | `VERIFIED` | `artwork_assets` stores provider, URLs, evidence, defects, selection reason and policy version. `LocalArtworkProvider` needs no network. Locks recorded in `manual_decisions`. | `[unit]` a lock outranks every provider result; no candidates retains existing artwork. `[build]` desktop review screen with lock/reject/import. |
| BPM-001 | Analyse locally, preserve trusted values, expose uncertainty, never alter audio. | `VERIFIED` | Spectral flux + tempogram with a perceptual prior; `TempoPolicy` preserves existing values and routes disagreement to review. Decoding is read-only. | `[unit]` 6 decision cases. `[infra]` recovers a known 120 BPM pulse; silence reports beatless. `[collection]` **90% agreement, 0% octave errors** on a held-out 120-track sample. See `docs/adr/0003-tempo-engine.md`. |
| LYR-001 | Retrieve/import matching lyrics; distinguish unavailable, instrumental and failed. | `VERIFIED` | `LyricsPolicy` scores artist, title, version qualifiers and duration together; six states kept distinct in `lyrics_results`. | `[unit]` 7 cases including same-title-alone rejection and version mismatch. `[collection]` live LRCLIB lookup returns and matches a real recording. |
| PRIV-001 | Remove only selected identifying metadata, with a frame-level preview. | `VERIFIED` | `PrivacyPolicy` returns a decision per frame with a rule id and reason; unknown owners go to review, never removal. | `[unit]` 8 cases. `[collection]` survey over 3240 files; a public-field allowlist added after 252 false positives on CATALOG/DISCID/CRC-32. |
| GAIN-001 | Remove selected playback adjustments; preserve gapless and gain-undo or report an exception. | `VERIFIED` | `GainPolicy` blocks all removal when MP3Gain undo data is present and preserves `iTunSMPB` unconditionally. | `[unit]` 4 cases. `[preserve]` end-to-end: ReplayGain removed, gapless kept. `[collection]` 1048 ReplayGain fields across 524 files, no undo data present. |
| OPT-001 | Compact metadata; qualify repacking through decoded-audio verification. | `IN_PROGRESS` | Padding budget implemented and applied exactly; size reported by cause. `Mp3Decoder::decodePcm` exists for full PCM comparison. **The MP3packer repacking adapter is not implemented.** | `[preserve]` padding budget applied exactly at 2048 and at 0 without truncating a frame. Repacking: not started, see `docs/open-questions.md`. |
| NAME-001 | Implement the confirmed section 9 template; separate specification from measured conformity. | `VERIFIED` | `NamingTemplate`; `Catalogue::namingConformity` measures actual paths against it and `writeNamingConventionReport` emits the evidence document. | `[unit]` 9 cases including the FRD's own `2 Unlimited` example verbatim, track padding, accents, and refusal to substitute track artist for album artist. |
| NAME-002 | Never overwrite a path collision or rename a protected source. | `VERIFIED` | `CollisionDetector` warns; a UNIQUE constraint on `output_reservations` is the guarantee; the writer refuses an existing destination. | `[unit]` 3 collision cases including case-only. `[preserve]` two files planned onto one destination produce at most one write. |
| JOB-001 | Jobs resume after interruption, respect provider limits, avoid repeating completed work. | `IN_PROGRESS` | `file_operations` journal with a state machine and `recover()`; results keyed by engine and settings hash so completed analysis is not repeated; global per-provider rate limits. **The durable `jobs` table is defined but the queue is not yet the execution path**; commands run inline. | `[preserve]` idempotence: a second export writes nothing and overwrites nothing. Crash-recovery under forced termination: not yet exercised. |
| UI-001 | Desktop and CLI share one engine; long-running jobs do not block browsing. | `VERIFIED` | Both front ends call `Library`; `TaskRunner` runs work on a worker thread; the track table pages with a bounded cache. | `[preserve]` scan/group/plan/export driven entirely through `Library`. `[build]` desktop renders a 3240-file catalogue. |
| DIST-001 | All four platform artifacts from one versioned source. | `IN_PROGRESS` | One CMake project; CPack configured for NSIS, DragNDrop and DEB; CI workflow builds all four. **Only the Linux x64 build has been produced and run on this host.** | `[build]` Linux x64 CLI and desktop binaries. Windows, macOS ARM64 and macOS x64: recipes written, artifacts not built or installed here. |
| DIST-002 | No .NET/C++/CLI dependency; no SDK, Python or Node needed. | `VERIFIED` | Native C++20 throughout; no managed runtime, no interpreter, no helper process. | `[build]` `ldd` inventory in `native/DEPENDENCIES.md`; offline mode exercised by the whole test suite. |
| DIST-003 | Install/upgrade/uninstall leave music intact; uninstall preserves the catalogue. | `IMPLEMENTED` | The catalogue lives in the OS per-user data directory, never under the install prefix; `Library::open` refuses a data directory inside a source root. | `[preserve]` catalogue-inside-source refusal. Installer lifecycle tests with sentinel data: not executed. |
| REPO-001 | Source, requirements, ADRs, tests and build recipes in one repository. | `VERIFIED` | `blueangelcpt/mp3-tool` with the FRD's own directory structure; music, catalogues and secrets excluded by `.gitignore`. | Repository review. |
| REL-001 | Matching versions, checksums, dependency notices, explicit test/signing status. | `IN_PROGRESS` | One version normalised across binaries, bundle metadata and packages; release workflow attaches SHA-256 sums and a dependency report. **No release has been tagged and nothing is signed.** | Signing credentials unavailable; recorded in `docs/open-questions.md`. |

## B. Derived functional requirements

| ID | Requirement | Status | Verification |
|---|---|---|---|
| FN-SCAN-01 | Record unreadable/malformed files individually and continue. | `VERIFIED` | `[collection]` 3240 files, 0 unreadable, 0 warnings. `[infra]` malformed-container cases. |
| FN-SCAN-02 | Rescan by identity, size and mtime; revalidate before a write. | `VERIFIED` | `[preserve]` a stale plan is refused at export. |
| FN-SCAN-03 | Scan report: patterns, tag versions, missing fields, artwork sizes, coverage, duplicates, errors. | `VERIFIED` | `[collection]` `resonance report` over 3240 files. |
| FN-SCAN-04 | A disconnected drive is not read as deletion. | `VERIFIED` | `[infra]` absent-root detection; the scanner reports and skips. |
| FN-ALB-01 | Grouping never uses track artist plus album title alone. | `VERIFIED` | `[unit]` compilation stays in one group. |
| FN-ALB-02 | Editions differing by a qualifier are not merged. | `VERIFIED` | `[unit]` deluxe vs standard. |
| FN-ART-01 | The section 6 decision cases decided exactly as specified. | `VERIFIED` | `[unit]` all five. |
| FN-ART-02 | sRGB, Lanczos, q75, optimised, baseline, 4:4:4, metadata stripped. | `VERIFIED` | `[infra]` output inspected; `file` confirms baseline 3-component. |
| FN-ART-03 | Received asset stored once; identical derivative bytes across an album. | `VERIFIED` | `[infra]` byte-identical across runs; `AssetStore` content-addressed. |
| FN-ART-04 | Non-square and uncertain framing to review; never upscale. | `VERIFIED` | `[unit]` + `[infra]` both refuse. |
| FN-ART-05 | Replace the front cover; preserve other image roles. | `VERIFIED` | `[preserve]` a back cover survives a front-cover replacement. |
| FN-ART-06 | "Open Google Images" with the user's presets; no Google API dependency. | `VERIFIED` | `[infra]` query shapes asserted; desktop opens them in a browser. |
| FN-BPM-01 | Fractional BPM plus half/double alternatives; integer `TBPM`. | `VERIFIED` | `[unit]` precise value retained, integer written, alternatives reported. |
| FN-BPM-02 | Beatless, variable-tempo, short and long-form handled explicitly. | `VERIFIED` | `[unit]` + `[infra]`. |
| FN-LYR-01 | Match on artist, title, album and duration; same-title alone insufficient. | `VERIFIED` | `[unit]` wrong-artist rejected. |
| FN-LYR-02 | `USLT` with a valid language code; existing lyrics preserved. | `VERIFIED` | `[unit]` + `[preserve]`. |
| FN-LYR-03 | Six states kept distinct; never generate lyrics. | `VERIFIED` | `[unit]` not-found is never instrumental. |
| FN-TAG-01 | Read without version translation; inventory every container. | `VERIFIED` | `[preserve]` a v2.3 file stays v2.3 through a rewrite. |
| FN-TAG-02 | Raw frame inventory captured independently and compared after writing. | `VERIFIED` | `[preserve]` cross-check clean; `comparePreservation` gates every export. |
| FN-TAG-03 | Strict iDesiccate profile, separately labelled and previewed. | `VERIFIED` | `[unit]` removes PRIV/COMM/UFID and says so. |
| FN-OPT-01 | Compact padding, retain a configurable budget. | `VERIFIED` | `[preserve]` exact at 2048 and 0. |
| FN-OPT-02 | Enrichment growth reported separately from savings. | `VERIFIED` | `SizeAccounting` carries savings, growth and padding delta separately. |
| FN-SAFE-01 | Reject destinations resolving into a protected root. | `VERIFIED` | `[infra]` symlink case. |
| FN-SAFE-02 | Six-step write protocol. | `VERIFIED` | `[preserve]` temp, verify, publish by rename, commit. |
| FN-SAFE-03 | Reconcile interrupted operations from the journal. | `IN_PROGRESS` | `recover()` implemented; forced-termination test not executed. |
| FN-NAME-01 | Preflight reserved names, trailing dots, length, case collisions. | `VERIFIED` | `[unit]` 4 cases. |
| FN-NAME-02 | Compilations keep the album-artist folder with each track's own artist. | `VERIFIED` | `[unit]` template behaviour. |
| FN-JOB-01 | Durable job keyed by entity, input hash, engine and config. | `IN_PROGRESS` | Schema and key present; queue not the execution path. |
| FN-JOB-02 | Global per-provider rate limits honouring `Retry-After`. | `IMPLEMENTED` | `RateLimiter` with MusicBrainz at 1.1 s. Sustained-limit test not run. |
| FN-CLI-01 | Ten commands plus diagnostics. | `VERIFIED` | `[build]` all present. |
| FN-CLI-02 | Planning cannot write; modifying commands need an output root. | `VERIFIED` | `[preserve]` planning without an output root is refused. |
| FN-UI-01 | Album view compares covers with evidence and a 600×600 preview. | `VERIFIED` | `[build]` review screen with fit/1:1 zoom and lock. |
| FN-UI-02 | Track table with filters for artwork, BPM, lyrics, privacy and gain. | `VERIFIED` | `[build]` filter chips and explorer nodes. |
| FN-UI-03 | Review view shows exact tag and path changes. | `VERIFIED` | `[build]` inspector change-plan tab. |
| FN-UI-04 | Jobs view with pause/resume/retry; history view. | `IN_PROGRESS` | Pause/resume/cancel and a history log present; per-job retry not exposed. |

## C. Not implemented

Stated plainly rather than left to be inferred:

| Item | Requirement | Where recorded |
|---|---|---|
| MP3packer repacking adapter | OPT-001 second mode | `docs/open-questions.md` |
| Chromaprint / AcoustID fingerprinting | FRD section 5, optional | `docs/open-questions.md` |
| Durable job queue as the execution path | JOB-001, FN-JOB-01 | `docs/open-questions.md` |
| Windows, macOS ARM64 and macOS x64 artifacts | DIST-001 | `docs/open-questions.md` |
| Installer lifecycle tests | DIST-003 | `docs/open-questions.md` |
| Code signing and notarisation | REL-001 | `docs/open-questions.md` |
| Audio playback | User clarification, 2026-09-13 | `docs/open-questions.md` |
| RIOT visual comparison | FRD section 6 | `docs/open-questions.md` |
| Player compatibility spot checks | FRD section 13 | `docs/open-questions.md` |
| 70,000-entry scale exercise | FRD section 13 | `docs/open-questions.md` |

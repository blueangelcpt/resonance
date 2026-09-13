# Requirements traceability matrix

Project: `blueangelcpt/mp3-tool` — MP3 Library manager
Baseline: `docs/requirements.md` (FRD v1.3, 11 September 2026), section 15 execution contract.
Maintained by: the implementing agent. Updated after every coherent implementation step.

## Status vocabulary

| Status | Meaning |
|---|---|
| `NOT_STARTED` | No implementation exists. |
| `IN_PROGRESS` | Partially implemented; named gaps remain. |
| `IMPLEMENTED` | Code exists and compiles; automated verification not yet executed or not yet complete. |
| `VERIFIED` | Code exists **and** the named test executed and passed on this host. Evidence recorded. |
| `BLOCKED` | Cannot proceed here; external dependency named in `docs/open-questions.md`. |

`VERIFIED` never means "reviewed and accepted". Only an explicit human review sets acceptance,
per the handoff's review loop. A test pass is not review acceptance.

## A. Mandatory requirements (FRD section 15)

| ID | Mandatory outcome | Status | Implementation | Verification |
|---|---|---|---|---|
| SAFE-001 | Source roots read-only to application operations; every output and temporary file outside them. | `NOT_STARTED` | — | — |
| SAFE-002 | Process independent copies; no hardlinks or in-place mode in v1. | `NOT_STARTED` | — | — |
| CAT-001 | Catalogue all discovered MP3 metadata, retain unknown fields; observed/proposed/written state separate. | `NOT_STARTED` | — | — |
| ID-001 | Identify album editions with supporting evidence; expose ambiguity; preserve manual locks. | `NOT_STARTED` | — | — |
| ART-001 | Exact 600×600 JPEG quality-75 output; preserve the selected received source separately. | `NOT_STARTED` | — | — |
| ART-002 | Rank correctness and fidelity/condition before resolution; accept a clean 600×600 source. | `NOT_STARTED` | — | — |
| ART-003 | Retain provenance, uncertainty, reasons and locks; manual/offline import without Album Art Exchange. | `NOT_STARTED` | — | — |
| BPM-001 | Analyse locally, preserve trusted values, expose uncertainty, never alter audio. | `NOT_STARTED` | — | — |
| LYR-001 | Retrieve/import matching lyrics; distinguish unavailable, instrumental and failed. | `NOT_STARTED` | — | — |
| PRIV-001 | Remove only selected identifying metadata, with a frame-level preview. | `NOT_STARTED` | — | — |
| GAIN-001 | Remove selected playback adjustments; preserve gapless and gain-undo data or report an exception. | `NOT_STARTED` | — | — |
| OPT-001 | Compact metadata; qualify optional repacking through decoded-audio verification. | `NOT_STARTED` | — | — |
| NAME-001 | Implement the confirmed section 9 template; separate specification from measured conformity. | `NOT_STARTED` | — | — |
| NAME-002 | Never overwrite a path collision or rename a protected source. | `NOT_STARTED` | — | — |
| JOB-001 | Jobs resume after interruption, respect provider limits, avoid repeating completed work. | `NOT_STARTED` | — | — |
| UI-001 | Desktop and CLI share one engine; long-running jobs do not block browsing. | `NOT_STARTED` | — | — |
| DIST-001 | All four platform/architecture artifacts from one versioned source. | `NOT_STARTED` | — | — |
| DIST-002 | No .NET/C++/CLI dependency; end users need no SDK, Python or Node. | `NOT_STARTED` | — | — |
| DIST-003 | Install/upgrade/uninstall leave music intact; uninstall preserves catalogue by default. | `NOT_STARTED` | — | — |
| REPO-001 | Source, requirements, ADRs, tests and build recipes in one GitHub repository. | `IN_PROGRESS` | Repository `blueangelcpt/mp3-tool` created with the FRD structure. | Repository review pending. |
| REL-001 | Matching versions, checksums, dependency notices, explicit test/signing status. | `NOT_STARTED` | — | — |

## B. Derived functional requirements

Requirements taken from FRD sections 1–14 that the section 15 table does not itemise. These carry
`FN-` identifiers assigned by this implementation and are traced the same way.

| ID | Requirement | Source | Status | Implementation | Verification |
|---|---|---|---|---|---|
| FN-SCAN-01 | Stream directory entries; record unreadable/truncated/malformed files individually and continue. | §5 | `NOT_STARTED` | — | — |
| FN-SCAN-02 | Rescan uses filesystem identity, size and mtime to detect change; revalidate content before a write. | §5 | `NOT_STARTED` | — | — |
| FN-SCAN-03 | Scan report: naming patterns, tag-version distribution, missing fields, artwork sizes, coverage, gain, privacy frames, duplicates, errors. | §5 | `NOT_STARTED` | — | — |
| FN-SCAN-04 | A disconnected drive must not be read as deletion of the collection. | §5 | `NOT_STARTED` | — | — |
| FN-ALB-01 | Provisional grouping uses folder, album title, album artist, disc, track count, dates, identifiers — never track-artist+album alone. | §5 | `NOT_STARTED` | — | — |
| FN-ALB-02 | Do not merge different editions because titles match. | §5 | `NOT_STARTED` | — | — |
| FN-ART-01 | Selection acceptance cases in §6 table decided exactly as specified. | §6 | `NOT_STARTED` | — | — |
| FN-ART-02 | Derivative: sRGB, Lanczos, JPEG q75, optimised, baseline, 4:4:4, EXIF/XMP stripped. | §6 | `NOT_STARTED` | — | — |
| FN-ART-03 | Store received asset unmodified once; identical derivative bytes across an album. | §6 | `NOT_STARTED` | — | — |
| FN-ART-04 | Non-square and uncertain framing route to review; never silent crop or stretch; never upscale. | §6 | `NOT_STARTED` | — | — |
| FN-ART-05 | Replace front-cover `APIC`; inventory and preserve other image roles. | §6 | `NOT_STARTED` | — | — |
| FN-ART-06 | "Open Google Images" review action with the user's query presets; no Google API dependency. | §2,§6 | `NOT_STARTED` | — | — |
| FN-BPM-01 | Store fractional BPM plus half/double alternatives and stability evidence; write integer `TBPM`. | §7 | `NOT_STARTED` | — | — |
| FN-BPM-02 | Handle beatless, variable-tempo, very short and long-form tracks explicitly. | §7 | `NOT_STARTED` | — | — |
| FN-LYR-01 | LRCLIB match on artist, title, album and duration; same-title alone insufficient. | §7 | `NOT_STARTED` | — | — |
| FN-LYR-02 | Write `USLT` with a valid language code; preserve existing lyrics unless replacement selected. | §7 | `NOT_STARTED` | — | — |
| FN-LYR-03 | States `found`, `instrumental`, `not_found`, `needs_review`, `rate_limited`, `failed` kept distinct; never generate lyrics. | §7 | `NOT_STARTED` | — | — |
| FN-TAG-01 | Read original tags without automatic version translation; inventory ID3v1, v2.2/2.3/2.4, APEv2, duplicates, unsupported frames. | §8 | `NOT_STARTED` | — | — |
| FN-TAG-02 | Capture raw frame inventories independently of the tag library and compare after writing. | §8 | `NOT_STARTED` | — | — |
| FN-TAG-03 | Strict profile reproducing iDesiccate's broad `PRIV`/`COMM`/`UFID` removal, separately labelled, previewed. | §2,§8 | `NOT_STARTED` | — | — |
| FN-OPT-01 | Compact excess padding, retain a configurable padding budget (default 2 KiB). | §8 | `NOT_STARTED` | — | — |
| FN-OPT-02 | Report enrichment growth separately from optimisation savings. | §8 | `NOT_STARTED` | — | — |
| FN-SAFE-01 | Reject destinations inside a protected root, overlapping roots, or symlink/junction paths resolving into one. | §9 | `NOT_STARTED` | — | — |
| FN-SAFE-02 | Six-step write protocol: revalidate, copy to exclusive temp, apply, verify, publish by rename, commit. | §9 | `NOT_STARTED` | — | — |
| FN-SAFE-03 | Reconcile interrupted operations on restart from the journal and hashes. | §9 | `NOT_STARTED` | — | — |
| FN-NAME-01 | Preflight reserved names, trailing spaces/dots, path length, Unicode/case collisions, duplicate track numbers. | §9 | `NOT_STARTED` | — | — |
| FN-NAME-02 | Compilations keep the common album-artist folder with each track's own artist in the filename. | §9 | `NOT_STARTED` | — | — |
| FN-JOB-01 | Durable job keyed by entity, input hash, engine version and config hash; lease, heartbeat, bounded attempts. | §10 | `NOT_STARTED` | — | — |
| FN-JOB-02 | Global per-provider rate limits honouring MusicBrainz 1 req/s and `Retry-After`. | §10 | `NOT_STARTED` | — | — |
| FN-CLI-01 | Commands `scan`, `albums`, `sample`, `analyse`, `plan`, `review`, `export-copies`, `verify`, `resume`, `report`. | §11 | `NOT_STARTED` | — | — |
| FN-CLI-02 | Analysis and planning commands have no tag-writing capability; modifying commands require an output root. | §11 | `NOT_STARTED` | — | — |
| FN-UI-01 | Album view compares current and candidate covers with evidence, condition flags and 600×600 preview. | §11 | `NOT_STARTED` | — | — |
| FN-UI-02 | Track table with filters for artwork, BPM, lyrics, privacy and gain. | §11 | `NOT_STARTED` | — | — |
| FN-UI-03 | Review view shows exact tag and path changes; accept field/album, lock, reject, leave unresolved. | §11 | `NOT_STARTED` | — | — |
| FN-UI-04 | Jobs view with pause/resume/retry; history view with recovery status. | §11 | `NOT_STARTED` | — | — |

## C. Evidence index

Build, test and measurement evidence is recorded in `docs/progress.md` with exact commands and
exit codes. Unresolved external dependencies are recorded in `docs/open-questions.md`.

| Evidence | Location |
|---|---|
| Build and test commands, versions, exit codes | `docs/progress.md` |
| Architecture decisions and deviations from the FRD's suggested components | `docs/adr/` |
| External blockers | `docs/open-questions.md` |
| Measured collection conformity to the naming template | `docs/naming-convention.md` (generated) |

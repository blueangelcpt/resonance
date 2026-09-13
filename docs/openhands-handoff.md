# OpenHands developer handoff — MP3 Library

Prepared 12 September 2026. Status: PREPARED, NOT SUBMITTED. The supervisor could not reach the user's localhost:3000 instance. No OpenHands conversation, repository or implementation is claimed to exist.

## Start instruction

You are OpenHands, the developer, using the user's configured local LLM. ChatGPT/Codex is the supervisor and independent code reviewer. Ian is the product owner. Implement the appended functional requirements incrementally. This experiment measures local-agent development with external review; do not switch to a hosted model or incur provider charges without the user's instruction. Do not ask the supervisor to implement the code for you.

Read this handoff first, then the complete appended FRD. Store the appended FRD unchanged at docs/requirements.md. Its historical environment statements describe the supervisor's environment; inspect your own workspace instead of assuming those paths are present or absent. Keep the agreed C++20, Qt 6 Widgets, SQLite and CMake architecture. No .NET. Use tabs for source indentation.

## Absolute source protection

Do not modify, rename, delete, retag or write temporary files into E:\Music or any alias/mount of it. Never create hardlinked test copies. For this first assignment use synthetic or appropriately licensed fixtures only; do not scan or copy the real collection yet. Later sample work must use independent copies outside protected roots, following the FRD. Repository files, build outputs, databases and caches must live outside music roots. Do not claim source safety merely because a happy-path test passes.

## Assignment OH-001: native foundation and preservation proof

1. Inspect the supplied project workspace, existing instructions, OS/architecture, toolchain and git status. Preserve pre-existing work. Record the actual OpenHands version and local model identifier/settings if available, without exposing credentials. Do not change model settings.
2. Create an isolated project directory and feature branch if no project exists. Use a local Git repository initially; a missing GitHub remote does not block development. Never invent a remote owner, publish the project or push to an unrelated repository.
3. Establish CMake presets, a shared non-GUI core, a CLI and minimal Qt Widgets window. Add SQLite initialization with a versioned schema and a TagLib adapter spike using generated MP3 tag fixtures. CLI must run without a display. Keep dependency versions reproducible; use official package sources.
4. Prove selective tag editing on fixtures: enumerate original frames, change one intended field, preserve unrelated/unknown frames or explicitly report an unsupported case. For valid audio fixtures, verify the MPEG payload is unchanged. Include malformed/truncated inputs and unknown-frame payloads containing frame-like text. Do not implement a whole custom MP3 parser to make the adapter look complete.
5. Add meaningful CTest tests and supported sanitizer configuration. Build and execute what the actual host supports. Add Windows, macOS arm64/x64 and Linux build/package workflows from the FRD. Produce a local native artifact where possible; distinguish recipes from built artifacts and built artifacts from installed/tested ones. Missing target runners or signing credentials are recorded blockers, not fabricated passes.
6. Record dependency footprint, basic startup/idle-memory measurements where measurable, exact reproduction commands and limitations. Do not invent performance numbers or claim a full 70,000-track benchmark.
7. Commit the coherent result and return the review packet below. Stop at REVIEW_PENDING before proceeding to the scanner or later milestones. Continue through routine build failures autonomously; if an external blocker remains, finish all independent work within this assignment and report it precisely.

This is the first slice of FRD milestone 0. Its full four-target qualification remains open until actual evidence exists. Do not attempt all enrichment features in one unreviewed change.

## Durable progress and review loop

Maintain docs/progress.md, docs/open-questions.md and docs/reviews/OH-001.md. Save progress after each coherent step and before context rollover. Record task status as NOT_STARTED, IN_PROGRESS, BLOCKED, REVIEW_PENDING, CHANGES_REQUESTED or ACCEPTED. Only an explicit supervisor review can set ACCEPTED. A test pass is not review acceptance.

For every submission provide:

- Repository path and remote if any; branch; base and head commit SHA; clean/dirty status.
- Requirement IDs addressed, implementation summary and changed files.
- Exact build/test commands, tool versions, exit codes, test results and log locations.
- Native artifacts produced, their architecture and checksums; installation tests actually executed.
- Unresolved failures, skipped checks and external blockers, each with reason and next action.
- A git diff or git bundle/source archive sufficient for independent review; omit build caches, real music, secrets and personal databases.
- Elapsed time, iterations and model/tool failure categories where observable. Mark token counts or other unavailable metrics as unavailable.

Supervisor review checks requirements, actual diffs, source protection, metadata preservation, ownership/lifetimes, cancellation/recovery, dependency packaging and meaningful tests. The supervisor may run independent verification and send numbered findings. OpenHands implements fixes and resubmits with a finding-to-commit mapping. Never weaken a test or silently change a requirement to remove a failure. Do not merge or advance to the next assignment until the supervisor accepts the current slice. This checkpoint is part of the experiment, not a request for the product owner to approve every routine implementation choice.

If the supervisor is not connected directly, return this packet to Ian for forwarding in the existing conversation. Do not claim an automated review loop or background monitoring is active. GitHub can become the shared review channel once the actual repository and access are established.

## Complete requirements baseline (v1.3)

The text below is the complete existing FRD. The handoff above adds the developer/supervisor workflow and narrows the first assignment; it does not authorize changes to the source music.

---

# MP3 Library — functional requirements and build plan

Prepared for Ian • 11 September 2026 • Version 1.3

**Recommendation, revised for the user's exclusion of .NET.** Build a native C++20 application with Qt 6 Widgets, SQLite, and a CLI sharing the same engine. Use CMake/Ninja and pinned native dependencies. Ship Windows, macOS and Linux installers. No application, helper or installer component may require .NET, C++/CLI or a managed runtime. This supersedes the implementation choices in earlier versions. Performance and package-size advantages remain benchmark hypotheses; preservation and source protection are mandatory. See sections 15–17 for the coding-agent contract and release requirements.

**Current status.** The requirements baseline is prepared. The user has supplied the intended naming template; collection-wide compliance remains unverified because the drive is not exposed here. No music files have been accessed, copied, scanned, or modified. `E:\Music` is a path on your Windows computer and is not accessible from this workspace. Both available execution environments were checked and are Linux; neither exposes `E:\Music` nor `/mnt/e/Music`. Implementation and measurements against your music remain future work. No GitHub repository or release binaries have been created in this session. This plan authorizes no changes to the source collection: the first processing runs use independent copies.

## 1. Scope and desired results

| Your requirement | Planned behaviour |
|---|---|
| SQLite metadata catalogue | Store filesystem facts, technical audio properties, all discovered tag fields, album membership, enrichment results, and processing history. |
| Album inventory | Group provisional albums, distinguish releases and editions, and flag inconsistent or incomplete groups. |
| Best artwork | Prefer clean, faithful original digital assets or well-restored artwork from streaming catalogues and official labels/artists. Judge correctness and image condition before resolution; a pristine 600×600 asset can beat a larger damaged scan. Retain provenance and selection reasons. |
| Embedded artwork | Write a 600×600 JPEG at quality 75 as the front cover. Keep the best source separately. |
| BPM | Analyse locally, preserve existing trusted BPM, identify uncertainty and half/double-tempo alternatives. |
| Privacy cleaning | Remove configured identifying metadata using a frame-aware parser, with a complete preview. |
| MP3 size optimisation | Compact excess metadata; evaluate optional lossless stream repacking. Never introduce lossy audio transcoding. |
| Lyrics | Retrieve matching lyrics, preserve existing text, embed plain lyrics, and distinguish instrumental, unavailable, and failed lookups. |
| Remove level adjustments | Remove track/album playback-gain metadata; identify cases where the audio itself was previously gain-adjusted. |
| File organisation | Use the user-confirmed album-artist/album/track-artist-title template; detect collisions before copying or renaming outputs. |

Success means correct, traceable results and clearly identified exceptions. Filling every field with an uncertain guess would make the collection worse. Unavailable lyrics and ambiguous recordings remain explicit unresolved states.

Version 1 handles MP3 files. Other formats may appear in the inventory but their modification is outside the first release.

## 2. Research findings that affect the design

**iDesiccate source is available, and the linked project matches version 1.1.0.** I downloaded and inspected the author's source archive. The project declares `ApplicationVersion` as `1.1.0.0`. In `Main.cs`, selected options remove `PRIV`, `COMM`, and `UFID` frames. The comment option also attempts to clear a trailing ID3v1 comment during a rewrite. The author describes the source as public domain. [Author's release page](https://invertedsky.net/?p=611), [source archive](https://invertedsky.net/downloads/desiccate.zip).

The inspected implementation accepts ID3v2.3 headers, searches the tag buffer for four-byte frame identifiers, and deletes the original before renaming its temporary file. These are reasons to implement the cleaning policy with a mature parser and a safer writer. Removing every `UFID` is too broad: Mutagen documents MusicBrainz recording identifiers in that frame. Useful comments and gapless metadata also need deliberate treatment. [Mutagen ID3 documentation](https://mutagen.readthedocs.io/en/latest/user/id3.html).

Archive inspected: 18,590 bytes; SHA-256 `21c095bd7a68223443e7df245828897ecea147d0677cd99203e35f5556f0febd`. Static inspection only; the old application was not executed. This is metadata cleaning, not a claim to remove identifiers embedded within the audio signal.

**foobar2000 is not an open-source application.** Its application licence permits restricted binary redistribution; an SDK and individual components are separate. We cannot assume the implementation of your optimisation command is available. The exact menu command, installed component, and a before/after test pair remain necessary to establish equivalent behaviour. [foobar2000 licence](https://www.foobar2000.org/license).

MP3packer is an identifiable open-source candidate for lossless stream optimisation. Its author describes packing encoded data into smaller frames, retaining gapless information, and potentially changing CBR files to VBR. Savings vary and may be negligible. This is a candidate integration, not a finding that foobar2000 uses MP3packer. Its old code and platform support require a prototype. [MP3packer author's documentation and source links](https://hydrogenaudio.org/index.php/topic,32379.0.html).

**Google image search should be a review aid.** Google's Custom Search JSON API is closed to new customers, with existing customers required to transition by 1 January 2027. A new application should not depend on that service. Preserve your query as an “Open Google Images” action and accept a pasted image URL or local artwork file. [Google API status](https://developers.google.com/custom-search/v1/overview).

The review action opens an image-results page. Its default Apple query is `[artist name] [album name] site:music.apple.com`, without a size filter that would exclude a clean 600×600 source. Retain the user's original `imagesize:1200x1200` query and 1800, 2400 and 3000 presets as optional shortcuts. Automated provider discovery uses the catalogue adapters.

**Apple can be queried directly, with constraints.** The archived iTunes Search API supports album searches and documents approximately 20 calls per minute, subject to change. Apple Music's artwork object supplies a URL template and maximum width/height. The beets Apple adapter already demonstrates high-resolution URL handling, but undocumented CDN URL rewriting must be isolated and checked against the actual downloaded image. [iTunes search API](https://developer.apple.com/library/archive/documentation/AudioVideo/Conceptual/iTuneSearchAPI/Searching.html), [Apple artwork object](https://developer.apple.com/documentation/applemusicapi/artwork), [beets artwork adapter source](https://github.com/beetbox/beets/blob/master/beetsplug/fetchart.py).

Apple's published iTunes API artwork terms concern promotional use. They do not establish a general permission for bulk artwork embedding. Confirm the applicable access and usage terms during the provider prototype; keep the Apple adapter replaceable. Local artwork and Cover Art Archive are alternative technical sources, with their own provenance and applicable terms. [Apple API terms](https://developer.apple.com/library/archive/documentation/AudioVideo/Conceptual/iTuneSearchAPI/index.html), [Cover Art Archive](https://coverartarchive.org/).

**beets is the closest existing project.** It already offers artwork retrieval/embedding, metadata matching, lyrics, and automatic BPM via librosa. Its scrub plugin clears tags and reconstructs them from its known metadata, which is not the selective preservation policy proposed here. Use beets and its libraries as behavioural references and development-only comparison tools. The shipped C++ application must not depend on a Python environment or invoke the beets import workflow. [FetchArt](https://beets.readthedocs.io/en/stable/plugins/fetchart.html), [AutoBPM](https://beets.readthedocs.io/en/stable/plugins/autobpm.html), [Lyrics](https://beets.readthedocs.io/en/stable/plugins/lyrics.html), [Scrub](https://beets.readthedocs.io/en/stable/plugins/scrub.html).

**Replace manual applications by capability.** MediaHuman documents automatic lyric embedding, but its product documentation does not supply the headless integration contract this app needs. A direct lyrics-provider interface is a better foundation. Likewise, this research did not establish a supported MixMeister batch API/CLI; existing MixMeister values become comparison data, not guaranteed ground truth. [MediaHuman Lyrics Finder](https://www.mediahuman.com/lyrics-finder/).

**Removing gain tags does not undo applied gain.** MediaMonkey distinguishes playback coefficients from changes to the source audio. MP3Gain also stores undo information in APEv2 tags. Do not delete that undo information as ordinary clutter. [MediaMonkey volume levelling](https://www.mediamonkey.com/wiki/WebHelp:Volume_Leveling/5.0), [MP3Gain FAQ](https://mp3gain.sourceforge.net/faq.php).

## 3. Proposed technology and architecture

| Component | Choice and purpose |
|---|---|
| Core | C++20; typed domain models, RAII ownership and explicit service boundaries. |
| Catalogue | SQLite through its native C API and a small RAII adapter, explicit SQL migrations, indexed queries and full-text search where useful. |
| Tag handling | TagLib behind a preservation-tested adapter; explicit ID3 frame and ID3v1/APEv2 handling. Library selection must pass the early preservation gate. |
| Images | A bundled, pinned ImageMagick helper for colour-managed Lanczos resizing and JPEG quality 75; no system installation required. |
| Audio decoding/validation | A pinned FFmpeg build; audio analysis and verification run locally. |
| BPM | Benchmark bundled native SoundTouch BPM detection and aubio; use librosa only as an optional development comparison. Choose by measured accuracy and throughput. |
| File optimisation | Metadata compaction first; MP3packer adapter subject to validation. |
| Identification | MusicBrainz; optional local Chromaprint fingerprinting with AcoustID lookup. |
| Lyrics | LRCLIB first; replaceable additional providers and local text/LRC import. |
| CLI | Scriptable commands, progress output, JSON reports, meaningful exit codes. |
| Desktop | Qt 6 Widgets with model/view tables, paginated data, lazy thumbnails and background jobs. |
| Packaging | GitHub Actions builds Windows x64 NSIS installers, macOS ARM64/x64 DMGs and Linux x64 DEB packages from the same versioned source. |

TagLib provides native C++ metadata APIs, including format-specific access. It remains a candidate until preservation tests pass; general metadata support does not guarantee round-tripping every frame. Qt Widgets supplies the desktop model/view framework. Use `QAbstractTableModel` and `QTableView`, with bounded fetching and caching; never create a widget for every track. [TagLib](https://taglib.org/), [Qt model/view](https://doc.qt.io/qt-6/model-view-programming.html).

Use CMake presets and Ninja for repeatable builds, with a pinned vcpkg manifest/baseline for native dependencies. Keep platform packaging under the same build system. The CLI must run without a display server; only the desktop target links Qt Widgets. Qt Core/Network may be used in infrastructure. Avoid Qt WebEngine and unnecessary modules. Retain the pinned ImageMagick helper initially to preserve the specified colour/JPEG behaviour; evaluate a smaller native image pipeline only with output-quality evidence.

The CLI and GUI call the same services: `Scanner`, `AlbumResolver`, `ArtworkService`, `TempoService`, `LyricsService`, `TagPolicy`, `ChangePlanner`, `CopyWriter`, `Verifier`, and `Organizer`. Provider adapters return proposals; they cannot save tags or rename music. Only the writer can mutate an allowed output path.

A coordinator owns the durable job queue and SQLite writes. CPU workers analyse audio in separate processes. A small network pool performs provider requests under shared rate limits. The GUI reads paginated results and never loads every track's artwork or lyrics into memory.

## 4. SQLite data model

| Entity | Main contents |
|---|---|
| `library_roots`, `scan_runs` | Protected roots, volume identity, scan generation, completion/errors. |
| `files` | Relative path, filesystem identity, size, modification time, codec, duration, sample rate, channel count, bitrate mode, hashes. |
| `tag_snapshots`, `tag_frames` | Original container/version, frame ID, owner/description/language, value, unknown payload, and snapshot identity. |
| `albums`, `album_tracks` | Provisional group, release identity, edition, disc/track order, album artist, compilation flags. |
| `metadata_candidates` | Proposed values, provider IDs, evidence, score, rule version, and selection status. |
| `artwork_assets` | Received dimensions/format, content hash, provider/page/image URLs, retrieval time, release association, source-type evidence, condition flags, selection/rejection reasons, local asset path, derivative settings. Distinguish measured dimensions from evidence of native resolution. |
| `tempo_results` | Raw BPM, alternatives, stability evidence, engine/version/settings, decision. |
| `lyrics_results` | Text, language, timing if present, source, matching evidence, instrumental/unavailable/error state. |
| `jobs`, `job_attempts` | Stage, input/config hash, lease, retry time, attempts, outcome and timings. |
| `change_sets`, `file_operations` | Exact before/after plan, output reservation, temporary path, expected hashes, recovery state. |
| `manual_decisions` | Locked values, rejected candidates, chosen artwork and naming exceptions. |

Keep all descriptive metadata in SQLite. Store image bytes once in a content-addressed asset directory and reference them from artwork frames; artwork is a binary asset rather than a repeated database thumbnail. Retain opaque non-art frame data when it cannot be interpreted. Original test inputs and later backups provide byte-exact restoration, including original padding and tag layout.

Use foreign keys, unique path reservations, indexes for album membership and job status, and small transactions. SQLite WAL allows concurrent readers with one writer; put the database on a local disk, not an SMB/NFS share. Use a maintained SQLite build containing the WAL-reset fix, and SQLite's backup mechanism for live backups. [SQLite WAL documentation](https://sqlite.org/wal.html).

The database stores observed state separately from proposed and successfully written state. A failed write must never make the catalogue claim that the file was updated.

## 5. Inventory and album identification

The scanner streams directory entries and reads metadata incrementally. Record unreadable, truncated, malformed, or unsupported files individually and continue. On subsequent scans, use filesystem identity, size, and modification time to identify likely changes. Revalidate content before a write; matching timestamps alone are not proof that bytes are unchanged. Compute full hashes during copy/verification, with audio hashes available for duplicate analysis.

Group provisional albums using folder structure, album title, album artist, disc numbering, track count, dates, and available release identifiers. Do not use track artist plus album title as the only key: compilations and guest performers would split incorrectly. Do not merge different editions simply because their titles match.

Resolve confident existing identifiers first, then compare candidate release tracklists and durations. Fingerprint uncertain recordings where useful. An AcoustID recording match is supporting evidence, not proof of the album edition. MusicBrainz distinguishes recordings, releases, and release groups. [MusicBrainz API](https://musicbrainz.org/doc/MusicBrainz_API), [AcoustID API](https://acoustid.org/webservice).

Automatically propose high-confidence corrections; require review for conflicting editions, missing tracks, artist conflicts, and ambiguous mappings. Preserve accents, original-language titles, intentional capitalisation, and remix/live/remaster qualifiers. Manual locks survive rescans and provider refreshes.

The scan report includes naming-pattern examples, tag-version distribution, missing fields, artwork sizes, lyrics/BPM coverage, gain fields, suspicious privacy frames, likely duplicates, and errors. A disconnected drive must not be mistaken for deletion of its whole collection.

## 6. Artwork pipeline

**User-confirmed quality policy.** The goal is a faithful, clean version of the intended cover. Prefer an original digital asset or a careful restoration over a photograph or scan showing physical sleeve damage, even when the latter contains more pixels. A clean 600×600 digital cover is an acceptable final source. Higher resolution is valuable when it carries additional usable detail without compromising colour, condition or authenticity.

1. Resolve album identity and gather candidates, including existing embedded/local covers. Match the intended cover and edition before comparing quality. A streaming reissue with a different cover must not silently replace the intended artwork; a faithful restoration of the same design is desirable even if supplied for a later digital release. Record that relationship and review uncertain edition mappings.
2. Prefer discovery through Apple Music and other qualified streaming-catalogue adapters, plus official record-label and artist assets. Apple remains the first automated candidate where usable; start with the South African storefront and controlled fallback storefronts for misses. Support official-site discovery through manual search and URL/local-file import initially; add automated adapters only when the access route is established. Cover Art Archive and other qualified archives remain fallbacks. Existing user-selected artwork remains a candidate and manual locks survive refreshes.
3. Assess fidelity and condition before resolution: colour cast/fading, sleeve wear, ring wear, creases, scratches, stickers, unwanted borders, skew, scan moiré, compression blocks, blur and sharpening halos. Preserve intentional artistic texture; do not classify all grain or distressed design as damage. A source domain provides supporting evidence, not proof of quality, restoration or original-master status.
4. Store source type as evidenced digital asset, evidenced restoration, scan/photo, or unknown, with evidence and uncertainty. Provider descriptions are claims unless independently supported. Automated image metrics may flag candidates for review but must not claim to prove original provenance or colour fidelity. Show the selection rationale and allow a manual decision when evidence is insufficient.
5. Among equally faithful and clean candidates, prefer more genuine usable detail. Inspect decoded dimensions and any available native-resolution evidence; a requested CDN size or `3000x3000` filename is not proof. Accept a good 600×600 source and good non-multiple sizes such as 2000×2000. The familiar 1200/1800/2400/3000 sizes remain convenient search presets, not eligibility rules or quality bonuses. Never upscale to manufacture compliance.
6. Reject wrong images and watermarked promotional substitutes. Route non-square candidates and uncertain framing to review; do not silently crop or stretch them to square. If nothing adequate is found, retain current artwork and report the unresolved result. Provider failures must not cause an inferior automatic replacement.
7. Save the selected received asset unmodified once, then generate the 600×600 derivative once per encoder configuration. Embed identical derivative bytes throughout the album. Retain source URLs, hashes, evidence, defects, candidate decisions and policy version in SQLite so selection can be explained and repeated.

The user reports that Album Art Exchange became unusable because of login and geographic restrictions. It is not a required provider and must not block discovery or release. Previously obtained user-supplied files can enter through ordinary local import. The remembered name “Heart FM” is unconfirmed; do not silently substitute another service or make it a required integration before identifying it.

**Selection acceptance cases.** These are explicit decision fixtures for the coding agent; condition labels come from reviewed examples, not invented automatic certainty.

| Candidates for the intended cover | Required decision |
|---|---|
| Clean 600×600 digital asset; 1451×1367 scan with faded colours, damage and artifacts | Choose the clean 600×600 asset; its resolution alone is not an unresolved issue. |
| Equally faithful, clean 2000×2000 and 1200×1200 assets with verified usable detail | Choose 2000×2000; divisibility by 600 carries no quality advantage. |
| Damaged scan served by a streaming catalogue; clean faithful asset from another verified source | Prefer the clean asset; provider priority does not override observed quality. |
| Clean high-resolution alternate-edition cover; correct intended cover with adequate quality | Preserve the intended cover; show any proposed edition change for review. |
| Conflicting colours or uncertain damage versus intentional texture | Surface the uncertainty and seek a review decision; do not invent certainty from a sharpness score. |

Proposed derivative: colour-convert to sRGB, resize with Lanczos, save JPEG quality 75 with optimised encoding and baseline/non-progressive output. Start with 4:4:4 subsampling for fine lettering and validate against your RIOT reference examples. Remove EXIF/XMP and extraneous comments from the derivative. JPEG “75” is encoder-specific; the plan promises a defined reproducible output, not byte-for-byte equivalence to RIOT. [ImageMagick options](https://imagemagick.org/command-line-options/).

Replace the front-cover `APIC` frame. Inventory and preserve other image roles unless a separate reviewed policy removes them. Existing compliant JPEGs should not be repeatedly recompressed. Keep an explicit “source quality unresolved” state for inadequate or uncertain sources, including sources below the required output resolution; a clean, correctly matched 600×600 source is adequate.

## 7. BPM and lyrics

**BPM.** Start with local decoding into analysis audio; the MP3 audio remains untouched. Compare SoundTouch BPM detection and aubio on representative genres. Use detection only: never pass a target BPM or tempo-change option. Package the selected native engine for every release target. [SoundTouch/SoundStretch capabilities](https://www.surina.net/soundtouch/soundstretch.html). Analyse several musical sections and use a fuller pass where results disagree. Store fractional BPM in SQLite, alternatives at half/double tempo, and stability evidence. Do not call an arbitrary score a calibrated probability.

Use existing MixMeister results as a baseline, then manually check disagreements. Handle beatless, variable-tempo, very short, and long-form tracks explicitly. Preserve trusted existing values by default. Store an integer in standards-compatible `TBPM`; retain the precise result in SQLite and offer a tested decimal-compatibility profile if required by your players. Calibrate automatic acceptance on a separate held-out sample.

**Lyrics.** LRCLIB offers a machine-friendly service. Its current source supports artist/title, optional album/duration, plain lyrics, timed lyrics, and an instrumental flag. Match artist, title, version and duration; a same-title result alone is insufficient. [LRCLIB project](https://github.com/tranxuanthang/lrclib), [metadata lookup source](https://github.com/tranxuanthang/lrclib/blob/main/server/src/routes/get_lyrics_by_metadata.rs).

Preserve existing lyrics unless replacement is explicitly selected. Write unsynchronised text in `USLT`, with a valid language code. Preserve existing timed lyrics; optional timed-lyrics import can use `SYLT` and an LRC export after player testing. Keep provider attribution and retrieval time in SQLite.

Separate `found`, `instrumental`, `not_found`, `needs_review`, `rate_limited`, and `failed`. Missing lyrics are not proof of an instrumental. Cache misses for a configurable period and retry service errors separately. Allow pasted lyrics and local text files, including original material unlikely to exist in public catalogues. Do not generate or silently translate missing lyrics. Additional providers need suitable retrieval and storage terms; a software repository licence does not license the lyrics themselves.

## 8. Tag cleaning, gain removal, and optimisation

**Tag policy.** Read original tags without automatic version translation before building the plan. Inventory ID3v1, ID3v2.2/2.3/2.4, APEv2, duplicate fields and unsupported frames. Preserve the existing supported ID3 version by default; offer an explicit ID3v2.3/UTF-16 compatibility profile. Version conversion must expose fields that would lose information. Tag-library defaults must not decide this policy accidentally. Capture raw frame inventories independently and compare them after writing; a high-level save API is insufficient evidence of preservation. Use Mutagen in development if helpful for independent comparisons, but do not ship Python solely for this. [TagLib](https://taglib.org/).

Use selective privacy rules by frame and owner/description. Remove recognised purchase/account information; preserve known public recording IDs. Route unknown private frames to review. Offer a separately labelled strict profile matching iDesiccate's broad removal choices, with the affected comments and identifiers listed beforehand. Scan for personal data in custom text fields and other tag containers too.

Preserve titles, artists, album artists, composers, genre, dates, disc/track numbering, ISRCs, useful public IDs, ratings, play counts, and intentional custom fields unless the chosen plan changes them. Redact sensitive payloads from routine logs. The local catalogue may itself contain original private metadata, so exports must omit it by default.

**Gain policy.** Remove recognised track/album ReplayGain gain/peak/reference fields across ID3 custom frames and APEv2, applicable `RVA2`/legacy relative-volume frames, and Sound Check `iTunNORM`. Preserve `iTunSMPB` and other gapless information. Detect gain information in encoder headers separately; do not blindly zero header bytes or remove the whole LAME/Xing header.

Files with MP3Gain undo data, or evidence of applied gain, receive an exception. Preserve recovery data and keep them out of blanket APE-tag removal. Undoing prior audio gain would be a distinct requested transformation, not a side effect of clearing playback adjustments. Without recovery information, the original level cannot be reliably inferred.

**Size policy.** Compact excessive ID3 padding and remove only approved redundant metadata after enrichment is ready. Retain a small configurable padding budget, such as 2 KiB, to avoid unnecessary rewrites during future edits. Missing artwork and lyrics can legitimately make a file larger; report enrichment growth separately from optimisation savings.

Evaluate MP3packer as an optional second mode. Keep an output only when it is smaller than the equivalent tagged input and passes decoded-audio/sample-count verification, gapless checks, seek tests, and metadata checks. Reject or quarantine unsupported and malformed streams. Expose any CBR-to-VBR or checksum-layout change in the plan. Lossless repacking changes compressed bytes, so a compressed-audio hash is not the correct equality test for that mode.

## 9. File safety and organisation

Initial working layout: protected source `E:\Music`; independent sample inputs `E:\MusicLab\input`; processed outputs `E:\MusicLab\output`. Store the catalogue, caches and logs outside the protected source. Paths are proposals, not directories created in this session.

The writer must reject any destination inside a protected source root, overlapping roots, or a symlink/junction/reparse-point path that resolves into the source. Make ordinary byte copies, never hardlinks. Check file identities as well as path strings. Workers receive staging/output paths only. Do not create sidecars, thumbnails, temporary files, or logs under the source root.

For each output:

1. Revalidate the source against the planned identity and content; reject stale plans.
2. Copy to an exclusively created temporary file in the destination filesystem and verify the copy hash. Never overwrite an existing destination.
3. Apply the combined metadata plan to that copy, with optional repacking as a separately recorded operation.
4. Reopen and verify tags, image bytes/dimensions, lyrics, gain policy and audio integrity.
5. Flush and publish the validated file using the platform's appropriate same-filesystem rename operation; then commit its successful state in SQLite.
6. Reconcile interrupted operations on restart using the journal and hashes. A filesystem rename and SQLite transaction are not one atomic transaction.

Disk-full errors, power loss, file locks, crashes and disconnected drives leave original files intact. Recovery never silently overwrites a newer destination. During sampling, verify original source hashes before and after the entire run.

The user-confirmed naming convention is `{album_artist}/{album}/{track:02}. {artist} - {title}.mp3`, relative to the selected root. On Windows the full pattern is `E:\Music\<Album Artist>\<Album>\<Track #:2>. <Artist> - <Title>.mp3`. The user supplied this extensionless example: `E:\Music\2 Unlimited\Get Ready!\01. 2 Unlimited - Get Ready for This (Orchestral Mix)`. The application preserves the actual MP3 extension; the example may omit it because extensions are hidden, but that has not been verified.

Map Album Artist (`TPE2`) to the top folder, Album (`TALB`) to the album folder, numeric Track (`TRCK`, excluding any total) to a minimum two-digit prefix, Artist (`TPE1`) to the filename artist, and Title (`TIT2`) to the filename title. Preserve punctuation, accents and version qualifiers subject only to documented filesystem sanitisation. The separator after the track number is a period and one space; the artist/title separator is space-hyphen-space. Track 1 becomes `01`, while track 100 remains `100`.

For compilations, keep the common album-artist folder and each track's own artist in the filename. Missing album artist, missing/invalid track numbers, multiple tag values and multidisc collisions require review or a separately configured rule. Do not automatically substitute track artist for album artist, insert disc folders, add a year, remove leading articles, or alter the template to resolve a collision. Library-wide conformity to this intended convention has not been measured.

Preflight Windows reserved names/characters, trailing spaces/dots, path lengths, Unicode/case collisions and duplicate track numbers. Keep canonical tags independent of filename sanitisation. Store original-to-output path mappings; generate remapped playlist copies if needed. Report duplicate recordings without deleting them.

Version 1 always exports copies. A later original-library replacement mode requires a new explicit instruction, tested backup/restore behaviour and a reviewed change set. It is outside the initial implementation.

## 10. Scale, jobs, and expected running time

Seventy thousand tracks is a reasonable SQLite catalogue size with appropriate indexes and bounded memory. The expensive work is audio decoding, image transfer, provider requests and file I/O. Measure those separately before promising a completion time.

Each stage has a durable job keyed by entity, relevant input hash, engine/provider version and configuration hash. Results can be reused across exact duplicate audio or the same album while preserving release-specific decisions. Retry transient failures with backoff and respect `Retry-After`; a provider outage must not trigger deletion of existing fields.

Use a lease and heartbeat for running jobs, bounded attempts, pause/resume, and reclaim after a crash. Idempotence is required: an unchanged completed plan must not rewrite files or repeat successful lookups. Separate semantic changes from formatting changes so renaming a file does not trigger a new BPM scan.

Start with 2–4 analysis workers on a typical desktop and a single copy/write stream on spinning disks; tune from measured CPU, RAM and disk utilisation. Ensure worker processes do not each spawn an unrestricted internal thread pool. Network limits are global per provider, not per worker. MusicBrainz requires at most one request per second per client; AcoustID also publishes usage limits. [MusicBrainz limits](https://musicbrainz.org/doc/MusicBrainz_API), [AcoustID limits](https://acoustid.org/webservice).

Illustrations, not performance claims:

| Assumption | Calculated implication |
|---|---|
| 6,000 albums; one Apple request each at 20/minute | 5 hours of API pacing before extra lookups, retries and image transfer. |
| 70,000 tracks; 10 seconds per track; four effective workers | About 49 hours for that analysis stage. |
| Same library; 30 seconds per track; four effective workers | About 146 hours. |
| Same library; 60 seconds per track; four effective workers | About 292 hours. |
| Average file size 10 MB | About 700 GB for a complete extra copy, plus assets and temporary space. |

Measure rates on your machine using the sample and extrapolate with `remaining_tracks × measured_seconds_per_track ÷ effective_workers`. Do not assume linear worker scaling or that a GPU is necessary. Preserve the ability to run locally without recurring AI/API token costs; provider charges, if chosen, are separate.

## 11. Desktop workflow and CLI

The main screen shows the protected source, selected output, scan coverage and job progress. An album view displays current and candidate covers at comparable sizes, a preview of the 600×600 output, original-pixel zoom, source/edition evidence, condition flags and selection reasons. It must be easy to compare colours, lettering and damage, choose a smaller but cleaner cover, and lock that decision. A track table exposes missing or disputed metadata with filters for artwork, BPM, lyrics, privacy and gain.

A review view shows exact tag and path changes, source evidence and estimated size effects. You can accept individual fields, accept an album, lock a choice, reject a provider match, or leave it unresolved. A jobs view supports pause/resume and retry. A history view shows completed outputs and recovery status. Tables and thumbnails load on demand.

Planned CLI commands: `scan`, `albums`, `sample`, `analyse`, `plan`, `review`, `export-copies`, `verify`, `resume`, and `report`. Every modifying command requires an output root; analysis and planning commands have no tag-writing capability. Reports can be exported as JSON or CSV. These are proposed interfaces, not commands available to run yet.

## 12. Build milestones and effort

The following estimates are retained as provisional planning ranges from the previous architecture, not a validated C++ estimate. Re-estimate at milestone 0 after proving native packaging, metadata preservation and the selected toolchain. They are not a fixed quote or an estimate of automated-agent elapsed time.

| Milestone | Deliverable and exit gate | Developer days |
|---|---|---:|
| 0. Cross-platform proof | Minimal desktop/CLI, metadata round-trip fixtures and native helpers installed and launched on all four targets; native dependency closure and initial size/startup measurements recorded. | 3–5 |
| 1. Read-only inventory | Packaged scanner, SQLite catalogue, album list and coverage report; protected source unchanged. | 2–3 |
| 2. Durable jobs and copy safety | Independent sample creation, source guards, leases/recovery, hashes and change-plan format. | 3–5 |
| 3. Album matching and artwork | Release candidates, review decisions, source validation, cached originals and 600×600 derivatives. | 4–7 |
| 4. Tag writer and organisation | Combined selective edits on copies, gain/privacy rules, collision-safe destination planning. | 3–5 |
| 5. BPM and lyrics | Benchmarked tempo adapter, LRCLIB/local lyrics, provenance, exceptions and resumability. | 4–7 |
| 6. Optimisation | Metadata compaction, foobar comparison, validated optional lossless-repacking adapter. | 3–5 |
| 7. Desktop interface | Album/track review, filters, locks, jobs and history sharing the CLI engine. | 4–7 |
| 8. Scale and release qualification | Fault recovery, player compatibility, larger sample, installer upgrade/uninstall tests on all targets, signing and operating guide. | 5–8 |
| **Total** | **Tested Windows, macOS and Linux desktop v1 and CLI** | **31–52** |

That is approximately 6–11 full-time developer weeks, subject to the cross-platform prototype. A useful read-only scanner arrives early. A core CLI covering matching, copied-file editing, BPM and lyrics follows milestones 0–5, roughly 19–32 developer days. The four specified release targets are included; Windows/Linux ARM64 and any original-library replacement mode are additional scope. Signing-account provisioning and user review waiting time are excluded.

Re-estimate after the first sample. The main unknowns are provider coverage for your collection, the exact foobar operation, tag irregularities, BPM accuracy and native tool packaging. A failed optional repacker prototype must not block a useful metadata-cleaning release.

## 13. Test rollout and acceptance criteria

Select approximately 100–200 tracks across 15–25 albums, preserving complete albums where practical. Include compilations, multiple discs, older MP3s, different tag versions, Afrikaans/non-ASCII names, live/remix material, variable tempo, large/missing covers, lyrics, gain tags and purchase metadata. Keep part of the sample out of threshold tuning.

The sample is copied to independent inputs and all transformation outputs are written elsewhere. Retain reference outputs from RIOT, MixMeister, iDesiccate, MediaMonkey and the specific foobar command where useful. These tools operate only on copies during comparison.

| Area | Required evidence |
|---|---|
| Source protection | Source content hashes unchanged; no writes through normal paths, aliases, hardlinks or junctions. |
| Tag preservation | Intended changes appear; unrelated metadata survives; unsupported conversion losses are reported. |
| Artwork | Section 6 selection cases pass, including clean 600×600 over damaged larger scan; provenance and decisions persist. One selected front cover, actual 600×600 JPEG, expected derivative hash/config, consistent across an album. |
| Lyrics | Correct recording/version and language; instrumental/unavailable states remain distinct; existing text preserved. |
| BPM | Agreement measured on held-out examples; half/double errors and variable tempo reported; no unearned confidence claims. |
| Gain/privacy | Targeted fields removed; public IDs, useful custom fields and gapless data preserved by policy; recovery-data exceptions retained. |
| Audio | Tag-only operations retain MPEG stream bytes. Repacking passes full decoded PCM and sample-count comparisons using a pinned decoder configuration without gain/DSP, plus independent gapless/seek checks. |
| Recovery | Forced termination, disk-full, file-lock and stale-plan tests leave recoverable outputs and intact inputs. |
| Idempotence | A second unchanged run performs no redundant writes or completed enrichment work. |
| Organisation | No collisions/overwrites, correct album grouping, preserved Unicode, auditable path mapping. |
| Scale | Read-only catalogue exercise at 70,000 entries; bounded memory; responsive browsing; provider-limit compliance. |
| Player compatibility | Spot checks in MediaMonkey, foobar2000 and an actual additional target player. |

After the small sample passes, process 1,000–2,000 copied tracks, then run a full read-only inventory and change plan. Full-library export to a separate destination can follow after the results and required disk space are reviewed. The source collection stays protected throughout this rollout.

Fixtures should include synthetic tags and known audio, adversarial frame contents, malformed artwork, invalid Unicode, duplicate names and path aliases. Tests target preservation and failure recovery, not just whether API calls return success. Automatic matching quality must be measured separately from software correctness.

## 14. Information needed during implementation

The first scanner can establish most of the missing facts without modifying music. The naming template is supplied in section 9. Remaining workflow inputs are the exact foobar menu command/component, any intended exceptions for compilations and multidisc collisions, preferred rating/comment retention behaviour, and a small set of manually verified BPM/RIOT outputs. Storage capacity and the CPU/disk used for the initial run determine concurrency and batch size.

The immediate implementation target is milestone 0 followed by milestone 1: prove packaging and metadata preservation across platforms, then deliver the read-only scanner and naming report. Source-library mutation remains unavailable.

## 15. Coding-agent execution contract

This document is intended for implementation by a coding agent. The following requirements are mandatory; implementation suggestions elsewhere may be refined when evidence warrants it. Record changes to the recommended architecture in an ADR and preserve all functional and safety requirements. Do not silently substitute a simpler behaviour to make a test pass.

| Requirement | Mandatory outcome | Verification |
|---|---|---|
| SAFE-001 | Source roots are read-only to application operations; every output and temporary file is outside them. | Source-hash and filesystem-alias tests in section 13. |
| SAFE-002 | Process independent copies; no hardlinks or in-place mode in v1. | File-identity tests and end-to-end export fixture. |
| CAT-001 | Catalogue all discovered MP3 metadata and retain unknown fields; observed, proposed and written state remain separate. | Mixed-tag fixtures, failed-write tests and schema inspection. |
| ID-001 | Identify album editions using supporting evidence; expose ambiguity and preserve manual locks. | Compilation, multidisc and conflicting-release fixtures. |
| ART-001 | Apply the exact 600×600 JPEG/quality-75 output configuration and preserve the selected received source separately. | Image decoding, deterministic derivative tests and album consistency. |
| ART-002 | Rank cover correctness and fidelity/condition before resolution. Prefer clean digital or restored assets; accept a clean 600×600 source. | Section 6 decision fixtures and reviewed real examples; no dimension-only or provider-only winner. |
| ART-003 | Retain provenance, uncertainty, selection reasons and manual locks; offer qualified streaming/official-source discovery and manual imports without requiring Album Art Exchange. | Persisted decision/review tests, inaccessible-provider fallback and offline import tests. |
| BPM-001 | Analyse locally, preserve trusted values and expose uncertainty without altering audio. | Held-out benchmark plus unchanged-audio checks. |
| LYR-001 | Retrieve/import matching lyrics and distinguish unavailable, instrumental and failed lookups. | Provider fixtures, version mismatches and existing-lyrics tests. |
| PRIV-001 | Remove only selected identifying metadata, with a frame-level preview. | Targeted removal and unrelated-frame preservation tests. |
| GAIN-001 | Remove selected playback adjustments; preserve gapless and gain-undo data or report an exception. | ID3/APEv2/encoder-header fixtures. |
| OPT-001 | Compact metadata; qualify any optional repacking through decoded-audio verification. | Size accounting, full PCM comparison and gapless/seek checks. |
| NAME-001 | Implement the user-confirmed template in section 9; distinguish that specification from measured collection conformity. | Exact template fixtures plus a read-only evidence report when the source is accessible. |
| NAME-002 | Never overwrite a path collision or rename a protected source. | Cross-platform filename and collision tests. |
| JOB-001 | Jobs resume after interruption, respect provider limits and avoid repeating completed work. | Restart, retry, rate-limit and idempotence tests. |
| UI-001 | Desktop and CLI share one engine; long-running jobs do not block browsing. | Shared use-case tests and a 70,000-entry browsing exercise. |
| DIST-001 | Produce all four platform/architecture artifacts in section 16 from one versioned source. | Install and launch each packaged artifact on its target OS. |
| DIST-002 | No application/helper/installer component depends on .NET or C++/CLI. End users need no development SDK, Python, Node or separately downloaded helper. | Clean-machine installation and offline local-operation test. |
| DIST-003 | Install, upgrade and uninstall leave the user's music intact; uninstall preserves catalogue/settings by default. | Installer lifecycle tests with sentinel user data. |
| REPO-001 | Keep source, requirements, ADRs, tests and build recipes in one GitHub repository. | Repository and workflow review. |
| REL-001 | Releases have matching versions, checksums, dependency notices and explicit test/signing status. | Release manifest and artifact verification. |

**Execution order.** Begin with the repository structure and packaging/metadata proof in milestone 0. Build the remaining milestones incrementally. A successful `cmake --build --preset release` does not establish installer compatibility or safe tagging. Keep `docs/progress.md` updated with completed requirement IDs, exact validation performed, remaining failures and the next executable step. Keep unresolved external dependencies in `docs/open-questions.md` so another agent can resume without reconstructing the session.

**Naming evidence.** The intended convention is **confirmed by the user's example and explicit template** in section 9. Its coverage across the collection is unverified. Implement that exact default now. When the drive becomes accessible, enumerate paths read-only and generate `docs/naming-convention.md` containing the total MP3 count, relative path-depth distribution, the count and percentage matching the confirmed template, other observed grammars, representative matches, and exception classes. Cover compilations, multidisc albums, years in folder names, track/disc padding, separators, featuring credits and repeated album titles. Compare tags where needed to distinguish track artist from album artist. Paths alone cannot prove that semantic distinction. Preserve exceptions and show the proposed transformation before any output is organised.

If a drive is absent, implement NAME-001 template behaviour using fixtures and mark only its collection audit blocked. Do not invent example paths from the user's library or claim the library has been scanned.

**Repository plan.** Suggested working name: `mp3-library-manager`, private initially. The actual owner and final name must be grounded in the authenticated account or supplied project context. This is a planned repository, not one created during this research session. Use a monorepo with these responsibilities:

| Path | Responsibility |
|---|---|
| `src/MusicLibrary.Core` | Domain models, requirements policies, pure matching/planning logic. |
| `src/MusicLibrary.Application` | Use cases, orchestration, jobs and service interfaces. |
| `src/MusicLibrary.Infrastructure` | SQLite, filesystem protection, providers and native helper adapters. |
| `src/MusicLibrary.Desktop` | Qt Widgets views, table models and desktop composition. |
| `src/MusicLibrary.Cli` | CLI commands using the same application layer. |
| `tests/` | Unit, integration, preservation, recovery and installer tests. |
| `native/` | Helper build recipes, pinned source versions and dependency notices. |
| `packaging/` | Windows NSIS, macOS app/DMG and Debian package definitions. |
| `docs/` | FRD, architecture, ADRs, naming evidence, progress and operating guide. |
| `.github/workflows/` | Validation, platform packaging and release workflows. |

The domain/application projects must not depend on Qt Widgets or Windows-only APIs. Use parameterised SQL, bounded concurrency, cancellable operations and platform filesystem adapters. Use typed process arguments without shell interpolation. Resolve helpers from the installed application directory rather than a mutable system PATH. Keep the code's indentation configuration explicit in `.editorconfig`; use tabs for indentation.

No real music, personal catalogue database, downloaded lyric collection, signing key or provider credential belongs in Git. Test fixtures must be synthetic or suitably licensed. Pin compiler/toolchain versions, Qt, CMake, the vcpkg baseline, native dependencies, helpers and CI actions. The native dependency review must include redistribution obligations; bundling a GPL/LGPL executable does not remove its licence requirements.

**C++ implementation rules.** Use RAII for file handles, SQLite statements/transactions, locks and temporary-output cleanup. Express ownership with value types and smart pointers; document non-owning references and lifetimes. Follow Qt parent ownership consistently and never give two owners responsibility for the same object. Parse lengths with checked arithmetic and explicit bounds; do not reinterpret untrusted tag buffers as unchecked structs. Use stop tokens or equivalent cancellation with bounded queues. Convert exceptions/errors at job and UI boundaries into durable failure records. Run AddressSanitizer and UndefinedBehaviorSanitizer on supported CI targets, and fuzz the tag-reading adapter with truncated and malformed inputs. Fault tests must cover interruption during commit, not just parser correctness.

## 16. Executables, installers and GitHub releases

**Deployment model.** Compile C++20 to native machine code for each target. Use release builds and package required native libraries and helpers; no managed-runtime publishing step is permitted. Dynamically link the selected Qt modules initially. A single executable is not a requirement. Record dependency versions and rebuild releases when bundled dependencies need security fixes. [Qt deployment](https://doc.qt.io/qt-6/deployment.html).

Use `windeployqt` and `macdeployqt` to help stage Qt dependencies, then explicitly collect third-party helpers and validate the complete installation. Include any required official C++ runtime redistributable in the Windows installer. These deployment tools do not replace clean-machine tests. [Qt Windows deployment](https://doc.qt.io/qt-6/windows-deployment.html), [Qt macOS deployment](https://doc.qt.io/qt-6/macos-deployment.html).

| Target | Native build target | Required v1 artifact | Packaging |
|---|---|---|---|
| Windows x64 | MSVC-compatible x64 | Versioned setup `.exe` | CPack/NSIS; installed GUI, CLI and required helpers. |
| macOS Apple Silicon | Apple Clang arm64 | Versioned ARM64 `.dmg` | Correct `.app` bundle with drag-to-Applications installation. |
| macOS Intel | Apple Clang x86_64 | Versioned x64 `.dmg` | Separate Intel build including Intel native helpers. |
| Linux x64 | GCC or Clang x86_64 | Versioned `amd64.deb` | CPack DEB with desktop entry, icons, CLI launcher and declared OS dependencies. |

**Windows installer interpretation.** The user requested a Windows installer; the previous plan selected MSI. The default is now a native NSIS setup executable, removing the WiX build dependency. This is a concrete packaging change, not an MSI file renamed to EXE. If MSI specifically becomes mandatory, record that requirement and qualify an MSI packaging route consistent with the no-.NET constraint. [CPack NSIS](https://cmake.org/cmake/help/latest/cpack_gen/nsis.html), [NSIS](https://nsis.sourceforge.io/Main_Page).

A DMG is the distribution disk image containing the application bundle; it does not require a privileged installer. Separate Intel and Apple Silicon DMGs are the initial target. A universal macOS build is optional and requires universal-compatible copies of every bundled native dependency. A `.deb` targets Debian-family distributions; it is not a claim to support every Linux distribution. [Qt macOS deployment](https://doc.qt.io/qt-6/macos-deployment.html), [CPack DEB](https://cmake.org/cmake/help/latest/cpack_gen/deb.html).

Proposed initial qualification systems are Windows 11 x64, macOS 14+ on both architectures, and Ubuntu 24.04 LTS x64. Confirm these against the exact compiler/Qt/helper versions in milestone 0 and document any change. Additional Debian-family versions are supported only after install/runtime testing; declare resolved system-library dependencies in the DEB. Build Linux artifacts on the oldest supported baseline to avoid accidentally raising the glibc requirement. An ARM machine running only an emulated Intel application is not an ARM-native test.

Use OS-standard per-user data directories. Application updates must not move or reset the catalogue or source roots. Database migrations must be versioned and backed up; a downgrade must detect incompatible schema versions and refuse safely. The application runs as an ordinary user even if installation needs elevation. Installation and first launch must never start a music rewrite automatically.

**CI and releases.** Configure GitHub Actions jobs for Windows, macOS and Ubuntu, with explicit architecture targets. Each job builds and tests the same commit and packages platform-matched helper binaries. Use native target testing where available; record what was built but not executed if runner access is temporarily missing. Do not describe an untested architecture as qualified. [GitHub runner selection](https://docs.github.com/en/actions/how-tos/write-workflows/choose-where-workflows-run/choose-the-runner-for-a-job).

Pull requests run tests and build checks without release credentials. Version tags such as `v0.1.0` produce a draft release after all required jobs pass. Normalize that same version for the Windows installer, application metadata, macOS bundle metadata and Debian package ordering. Attach installers, a CLI archive where useful, SHA-256 checksums, a dependency/SBOM report, notices and release notes. Public publication is a separate repository policy decision; the workflow must not automatically expose a private project.

Production Windows releases should be Authenticode-signed and timestamped. Production macOS releases require a properly signed app and notarisation/stapling workflow for normal direct distribution. Keep certificates and credentials in protected CI secrets or a signing service; never generate pretend signing identities or claim an unsigned build is signed. Developer builds may be explicitly unsigned. If signing credentials are unavailable, produce the unsigned artifacts and list the blocked distribution gate while continuing other work. Signing does not guarantee an immediate clean SmartScreen reputation. [Apple notarisation](https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution).

For each release target, test fresh installation, application and CLI startup, a read-only scan, copied-file enrichment with bundled helpers, upgrade from the previous version, and uninstall. Verify missing-network behaviour separately from local functionality: remote enrichment requires connectivity, while catalogue browsing, naming review, local BPM, local artwork processing and copied-file tagging must work offline.

## 17. C++ trade-offs and measurable goals

C++20 is the selected language under the user's explicit no-.NET requirement. It supports native compilation and direct integration with established C/C++ audio and metadata libraries. Qt Widgets provides a practical shared desktop implementation for the required operating systems. The CLI and desktop share policies and processing code.

**Speed.** Lower application overhead and control over allocations can improve scanning and responsiveness. They do not establish a multiplier for this collection. Decoders and BPM engines often already execute native code; disk throughput and remote-provider limits can dominate elapsed time. Measure initial scan, unchanged rescan, BPM audio-hours per wall-clock hour, tagging throughput and UI latency independently. Record hardware, storage, build settings, concurrency and cache state. Never compare different BPM algorithms as though the result measured language overhead alone.

**Size.** Avoiding a bundled managed/interpreted runtime can reduce distribution size, but Qt, FFmpeg, image processing and codec dependencies may dominate. Measure compressed installer size, installed bytes, idle working set and startup time for every target. Keep debug symbols in separate artifacts and ship only required modules/plugins. Do not promise a tiny installer before dependency closure is measured; static linking alone is not proof of a smaller total package.

**Complexity.** C++ requires deliberate lifetime management, careful parsing and stronger memory-error testing. The rules in section 15 and metadata-preservation gates apply from the first prototype. Using established libraries reduces custom parser work but does not remove validation obligations.

**Dependency review.** Select appropriate Qt modules and record their actual licence terms, including third-party components; do not assume every optional Qt module has identical licensing. Document the chosen dynamic-link deployment and required notices in the release manifest. [Qt licensing](https://doc.qt.io/qt-6/licensing.html).

**Milestone 0 evidence.** Produce runnable native CLI/desktop packages on all four targets, an explicit dependency inventory with no .NET components, a tag-preservation report, and a baseline size/performance report. Set practical budgets from those measurements before building the remaining features. Retain all functional requirements, the confirmed naming template, and protection of the original music collection.

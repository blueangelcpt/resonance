# ADR 0001: Native dependency choices

Status: accepted
Date: 2026-09-13
Requirement context: DIST-002, REL-001, FRD sections 3 and 15

## Context

FRD section 3 proposes a component list: TagLib for tags, a bundled ImageMagick
helper for artwork, a pinned FFmpeg build for decoding, and SoundTouch or aubio
for BPM. Section 15 permits refining implementation suggestions "when evidence
warrants it", and requires the change be recorded in an ADR with all functional
and safety requirements preserved.

Section 15 also requires that end users need no separately downloaded helper,
and that the native dependency review covers redistribution obligations.

## Decision

| Concern | FRD suggestion | Chosen | Why |
|---|---|---|---|
| Tags | TagLib | **TagLib 2.2.1** | Adopted as suggested. Passed the preservation gate: see `tests/unit/PreservationTests.cpp`. |
| Catalogue | SQLite C API | **SQLite 3.46** | Adopted as suggested. |
| Images | bundled ImageMagick helper | **libjpeg-turbo + libpng, in process** | See below. |
| MP3 decoding | pinned FFmpeg | **minimp3, vendored** | See below. |
| BPM engine | SoundTouch or aubio | **in-process spectral flux + tempogram** | See `0003-tempo-engine.md`. |
| HTTP | Qt Network or libcurl | **libcurl** | Keeps the CLI free of any Qt dependency. |

### Images: libjpeg-turbo and libpng instead of an ImageMagick helper

ART-001 specifies an exact output: sRGB, Lanczos, 600x600, JPEG quality 75,
optimised, baseline, 4:4:4, EXIF and XMP stripped. Every one of those is
directly expressible through libjpeg-turbo's API, and the resampling is
implemented here in linear light.

Three reasons to prefer this over shipping an ImageMagick binary:

1. **No helper process to resolve, package or trust.** DIST-002 requires that
   end users need no separately downloaded helper. An in-process library removes
   the helper-resolution problem the FRD itself flags ("resolve helpers from the
   installed application directory rather than a mutable system PATH").
2. **Determinism is testable.** FN-ART-03 requires identical derivative bytes
   across an album. With the encoder in process, that is asserted directly;
   `tests/unit/InfrastructureTests.cpp` checks byte equality across two runs.
3. **Smaller redistribution surface.** libjpeg-turbo and libpng are permissive.
   ImageMagick is Apache-2.0 but drags in a large delegate tree whose individual
   licences would each need review.

The FRD's own condition for evaluating a smaller native pipeline was
"output-quality evidence". The evidence is in the tests: the derivative decodes
back to exactly 600x600, is byte-identical across runs, refuses to upscale, and
refuses to crop a non-square source. What is **not** yet done is a visual
comparison against the user's RIOT reference examples; that is recorded in
`docs/open-questions.md`.

### MP3 decoding: minimp3 instead of FFmpeg

Decoding is needed for two things only: analysis audio for BPM and the spectrum
display, and full PCM comparison for the optional repacking verification
(OPT-001). Both are MP3-only, because FRD section 1 scopes version 1 to MP3.

FFmpeg would add tens of megabytes, a large CVE surface, and an LGPL/GPL
obligation that the FRD explicitly warns about ("bundling a GPL/LGPL executable
does not remove its licence requirements"). minimp3 is a single header under
CC0, pinned at revision `ea99364f61c14656440e8d77e9c233ccf3124633`, and is
compiled into the application with no runtime dependency at all.

The trade: minimp3 decodes MP3 and nothing else. If a later version extends
beyond MP3, this decision must be revisited. That is the correct time to pay
FFmpeg's cost, not now.

## Consequences

- No bundled helper executables. The Debian package declares resolved shared
  library dependencies through `CPACK_DEBIAN_PACKAGE_SHLIBDEPS`.
- The CLI links no Qt module and runs without a display server.
- Licence obligations are recorded in `native/DEPENDENCIES.md`.
- Decoding support is MP3-only, matching the version 1 scope.

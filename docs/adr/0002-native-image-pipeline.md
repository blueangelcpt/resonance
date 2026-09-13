# ADR 0002: The artwork derivative pipeline

Status: accepted
Date: 2026-09-13
Requirement context: ART-001, ART-002, FN-ART-02, FN-ART-03, FN-ART-04

## Context

ART-001 fixes the output configuration exactly. FRD section 6 adds that JPEG
"75" is encoder-specific, and that the plan promises "a defined reproducible
output, not byte-for-byte equivalence to RIOT".

## Decision

The pipeline is: decode to 8-bit sRGB, convert to linear light, resample with a
separable Lanczos-3 kernel, convert back to sRGB, encode as baseline JPEG at
quality 75 with optimised Huffman tables and 4:4:4 subsampling.

Points worth stating explicitly:

- **Resampling happens in linear light.** Resampling sRGB values directly
  darkens edges. This is what "colour-managed" has to mean in practice, and it
  is asserted by a test that a flat mid-grey image keeps its mean brightness
  through a 1000 to 600 reduction.
- **The kernel widens when downscaling.** A fixed 3-tap radius aliases badly
  reducing 3000 px to 600 px; the support is scaled by the reduction factor so
  the filter averages over the whole source footprint.
- **The kernel is normalised per output sample.** An unnormalised Lanczos shifts
  overall brightness.
- **4:4:4, not the library default.** `jpeg_set_defaults` chooses 4:2:0, which
  blurs coloured lettering at 600 px. The FRD asks for 4:4:4 for exactly this
  reason.
- **No metadata is written.** The Adobe marker is disabled and no EXIF or XMP is
  emitted, so the derivative carries nothing from the source.
- **The encoder is part of the derivative's identity.** `DerivativeConfig::hash`
  includes `encoder=libjpeg-turbo` alongside the settings, because quality 75 is
  encoder-specific. Changing the encoder regenerates rather than silently
  reusing.

## Refusals

The pipeline refuses rather than guessing:

- A source smaller than 600 px on any side is refused. FRD section 6 forbids
  upscaling to manufacture compliance, so this is an error, not a resize.
- A non-square source is refused. FN-ART-04 requires framing to be a review
  decision, never an automatic crop or stretch.
- A buffer that is not JPEG or PNG is refused. A candidate whose bytes cannot be
  decoded cannot have its dimensions or condition measured, and ART-002 forbids
  selecting on a claimed size.

## Open

A visual comparison against the user's RIOT reference outputs has not been
performed. Recorded in `docs/open-questions.md`.

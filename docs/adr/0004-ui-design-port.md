# ADR 0004: Porting the Resonance interface design

Status: accepted
Date: 2026-09-13
Requirement context: UI-001, FN-UI-01..04

## Context

The user supplied an interface design produced in Google Stitch
(`design/resonance-vfd.html`): a dark neon "workbench" with a header diagnostic
strip, a library tree, a centre column of stacked panels, a right-hand
inspector, and a footer status bar.

The design is authored as Tailwind-configured HTML for the web. The application
is Qt 6 Widgets, so the markup cannot be used directly.

## Decision

Port the **visual language**, bind every panel to **real data**.

### What was taken verbatim

The design's token palette, in `src/MusicLibrary.Desktop/src/Theme.hpp`: the
surface ramp, the neon cyan/purple/pink accents, the overload red, the text
scale. The typography intent (JetBrains Mono for data, Space Grotesk for
headings, Inter for body) with explicit fallback chains, because none of those
families is guaranteed to be installed and Qt substitutes silently otherwise.

The layout: header strip, three columns at the design's proportions, bento
panels with accent top borders, dense tabular rows, the recessed dot-matrix
enclosure.

### What was rebound

The design depicts a media player with features this application does not have.
Panels were bound to the equivalent Resonance function rather than filled with
invented readings:

| Design panel | Bound to |
|---|---|
| Library Explorer tree | Real roots, albums and filter nodes with live counts |
| VFD dot-matrix spectrum | A real FFT of the decoded file, via the same minimp3 decoder and radix-2 FFT the BPM analyser uses |
| Columns-UI track list | The paginated track table |
| Inspector: metadata grid | The real ID3v2 frame inventory, including uninterpreted frames |
| Inspector: lyrics | The real lyrics state, evidence and embedded text |
| Inspector: 10-band EQ rack | Replaced by the change-plan preview |
| Header CPU/RAM/DAC telemetry | Real catalogue counts, and a permanent source-protection badge |

### What was not built

The design's CPU, RAM, WASAPI and DAC telemetry readouts were not reproduced.
Drawing a gauge that reports a number the application does not measure would be
a fabrication, and the FRD is repeatedly explicit that invented certainty is
worse than an honest gap.

The 10-band equaliser was replaced rather than drawn inert. A visible EQ that
does not affect anything is a worse outcome than the panel that carries the
information this application actually exists to produce.

## Subsequent clarification

The user has since confirmed that Resonance is intended to be **both** a player
and a library tool. Playback is therefore in scope and is tracked in
`docs/open-questions.md`; it needs an audio output backend (Qt Multimedia's
`QAudioSink`), which is the one component not already present. The decoder and
the FFT are already in place, so once audio output exists the spectrum panel
follows the real playhead instead of a swept analysis, and the deck's transport
controls become live.

## Consequences

- A token change in `Theme.hpp` lands across the whole interface.
- The desktop target is the only one linking Qt Widgets; the CLI is unaffected.
- `--screenshot` renders the window offscreen and exits, which doubles as a CI
  smoke test that the interface composes and paints.

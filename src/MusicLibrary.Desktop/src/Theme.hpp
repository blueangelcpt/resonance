// SPDX-License-Identifier: GPL-3.0-or-later
// The Resonance visual language, ported from the supplied Stitch design.
//
// The design was authored as Tailwind tokens for the web. The tokens themselves
// are the design; this file is the single place they live, so the Qt widgets
// never hard-code a colour.
//
// Fonts: the design specifies JetBrains Mono for data and telemetry, Space
// Grotesk for headings and Inter for body text. None is guaranteed to be
// installed, so each has an explicit fallback chain ending in a family Qt always
// resolves.
#pragma once

#include <QColor>
#include <QFont>
#include <QString>

namespace ml::desktop::theme {

// ---------------------------------------------------------------------------
// Palette. Names and values are taken verbatim from the design's token set.
// ---------------------------------------------------------------------------

// Surfaces, darkest to lightest.
inline const QColor kSurfaceDeepVoid{0x06, 0x07, 0x0a};
inline const QColor kSurface{0x0a, 0x0c, 0x13};
inline const QColor kSurfacePanel{0x10, 0x13, 0x1d};
inline const QColor kSurfaceContainer{0x14, 0x18, 0x26};
inline const QColor kSurfaceContainerHigh{0x19, 0x1f, 0x32};
inline const QColor kSurfaceVariant{0x1c, 0x21, 0x33};
inline const QColor kSurfaceBright{0x22, 0x29, 0x40};
inline const QColor kSurfaceContainerHighest{0x23, 0x29, 0x42};
inline const QColor kSurfaceOverlay{0x26, 0x2b, 0x3e};

// Lines and separators.
inline const QColor kOutline{0x84, 0x94, 0x95};
inline const QColor kOutlineVariant{0x23, 0x29, 0x42};
inline const QColor kSplitterMuted{0x1d, 0x23, 0x38};
inline const QColor kSplitterActive{0x35, 0x3d, 0x59};

// Text.
inline const QColor kTextPrimary{0xf0, 0xf4, 0xfc};
inline const QColor kTextSecondary{0x8d, 0x96, 0xb0};
inline const QColor kTextMuted{0x4b, 0x53, 0x6a};
inline const QColor kOnSurface{0xe3, 0xe1, 0xe9};
inline const QColor kOnSurfaceVariant{0x9b, 0xa9, 0xc7};

// Neon accents. These carry meaning in the design: cyan is nominal, purple is
// the mid band, pink is a peak or an active selection, red is an overload.
inline const QColor kNeonCyan{0x00, 0xf0, 0xff};
inline const QColor kNeonCyanDim{0x00, 0xdb, 0xe9};
inline const QColor kNeonPurple{0xb0, 0x26, 0xff};
inline const QColor kNeonPink{0xff, 0x2a, 0x85};
inline const QColor kNeonPinkHot{0xff, 0x00, 0x7f};
inline const QColor kOverloadRed{0xff, 0x17, 0x44};
inline const QColor kPrimary{0xdb, 0xfc, 0xff};

// Glows, as used behind active elements.
inline const QColor kGlowCyan{0x00, 0xf0, 0xff, 102};    // 0.40 alpha
inline const QColor kGlowPurple{0xb0, 0x26, 0xff, 102};  // 0.40 alpha
inline const QColor kGlowPink{0xff, 0x2a, 0x85, 115};    // 0.45 alpha

/// Semantic accent for a status word used throughout the interface.
QColor accentForStatus(const QString& status);

// ---------------------------------------------------------------------------
// Typography
// ---------------------------------------------------------------------------

/// Monospace, for data, telemetry, paths and anything that must align.
QFont monoFont(int pointSize = 9, QFont::Weight weight = QFont::Normal);

/// Display face, for panel headings.
QFont displayFont(int pointSize = 10, QFont::Weight weight = QFont::DemiBold);

/// Body face, for ordinary interface text.
QFont bodyFont(int pointSize = 9, QFont::Weight weight = QFont::Normal);

/// A small, wide-tracked uppercase label, as the design uses for panel titles.
QFont labelFont(int pointSize = 8);

// ---------------------------------------------------------------------------
// Stylesheet
// ---------------------------------------------------------------------------

/// The application-wide stylesheet. Applied once at startup.
QString applicationStyleSheet();

/// A panel frame in the design's bento style. `accent` tints the top border.
QString panelStyle(const QColor& accent = kOutlineVariant);

/// Converts a colour to the `#rrggbb` form a stylesheet needs.
QString hex(const QColor& colour);
/// Converts a colour to `rgba(r, g, b, a)` with a float alpha.
QString rgba(const QColor& colour, double alpha);

} // namespace ml::desktop::theme

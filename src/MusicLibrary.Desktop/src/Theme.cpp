// SPDX-License-Identifier: GPL-3.0-or-later
#include "Theme.hpp"

#include <QFontDatabase>
#include <QStringList>

namespace ml::desktop::theme {

namespace {

/// Picks the first family that is actually installed. Qt will happily accept a
/// family it cannot resolve and silently substitute something arbitrary, so the
/// choice is made explicitly instead.
QString firstAvailableFamily(const QStringList& candidates, const QString& fallback) {
	const QStringList installed = QFontDatabase::families();
	for (const QString& candidate : candidates) {
		if (installed.contains(candidate, Qt::CaseInsensitive)) return candidate;
	}
	return fallback;
}

QString monoFamily() {
	static const QString family = firstAvailableFamily(
		{QStringLiteral("JetBrains Mono"), QStringLiteral("JetBrainsMono Nerd Font"),
		 QStringLiteral("Cascadia Mono"), QStringLiteral("DejaVu Sans Mono"),
		 QStringLiteral("Liberation Mono"), QStringLiteral("Consolas"),
		 QStringLiteral("Menlo"), QStringLiteral("Courier New")},
		QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
	return family;
}

QString displayFamily() {
	static const QString family = firstAvailableFamily(
		{QStringLiteral("Space Grotesk"), QStringLiteral("Inter"),
		 QStringLiteral("Segoe UI Variable"), QStringLiteral("Segoe UI"),
		 QStringLiteral("Noto Sans"), QStringLiteral("DejaVu Sans"),
		 QStringLiteral("Helvetica Neue")},
		QFontDatabase::systemFont(QFontDatabase::GeneralFont).family());
	return family;
}

QString bodyFamily() {
	static const QString family = firstAvailableFamily(
		{QStringLiteral("Inter"), QStringLiteral("Segoe UI"), QStringLiteral("Noto Sans"),
		 QStringLiteral("DejaVu Sans"), QStringLiteral("Helvetica Neue")},
		QFontDatabase::systemFont(QFontDatabase::GeneralFont).family());
	return family;
}

} // namespace

QString hex(const QColor& colour) {
	return colour.name(QColor::HexRgb);
}

QString rgba(const QColor& colour, double alpha) {
	return QStringLiteral("rgba(%1, %2, %3, %4)")
		.arg(colour.red()).arg(colour.green()).arg(colour.blue()).arg(alpha);
}

QColor accentForStatus(const QString& status) {
	// Cyan means settled, purple means proposed, pink means needs a human, red
	// means a failure. This mirrors the meter colours in the design.
	const QString lower = status.toLower();
	if (lower.contains(QStringLiteral("fail")) || lower.contains(QStringLiteral("error"))
		|| lower.contains(QStringLiteral("unreadable")) || lower.contains(QStringLiteral("collision"))) {
		return kOverloadRed;
	}
	if (lower.contains(QStringLiteral("review")) || lower.contains(QStringLiteral("blocked"))
		|| lower.contains(QStringLiteral("conflict")) || lower.contains(QStringLiteral("weak"))) {
		return kNeonPink;
	}
	if (lower.contains(QStringLiteral("propose")) || lower.contains(QStringLiteral("candidate"))
		|| lower.contains(QStringLiteral("moderate")) || lower.contains(QStringLiteral("planned"))) {
		return kNeonPurple;
	}
	if (lower.contains(QStringLiteral("ok")) || lower.contains(QStringLiteral("found"))
		|| lower.contains(QStringLiteral("selected")) || lower.contains(QStringLiteral("locked"))
		|| lower.contains(QStringLiteral("strong")) || lower.contains(QStringLiteral("committed"))
		|| lower.contains(QStringLiteral("no_change")) || lower.contains(QStringLiteral("preserved"))) {
		return kNeonCyan;
	}
	return kTextSecondary;
}

QFont monoFont(int pointSize, QFont::Weight weight) {
	QFont font(monoFamily(), pointSize);
	font.setWeight(weight);
	font.setStyleHint(QFont::Monospace);
	font.setFixedPitch(true);
	return font;
}

QFont displayFont(int pointSize, QFont::Weight weight) {
	QFont font(displayFamily(), pointSize);
	font.setWeight(weight);
	return font;
}

QFont bodyFont(int pointSize, QFont::Weight weight) {
	QFont font(bodyFamily(), pointSize);
	font.setWeight(weight);
	return font;
}

QFont labelFont(int pointSize) {
	QFont font(displayFamily(), pointSize);
	font.setWeight(QFont::DemiBold);
	font.setCapitalization(QFont::AllUppercase);
	// The design tracks its panel labels out noticeably.
	font.setLetterSpacing(QFont::PercentageSpacing, 112);
	return font;
}

QString panelStyle(const QColor& accent) {
	return QStringLiteral(
		"QFrame[mlPanel=\"true\"] {"
		"  background: %1;"
		"  border: 1px solid %2;"
		"  border-top: 2px solid %3;"
		"  border-radius: 4px;"
		"}")
		.arg(hex(kSurfacePanel))
		.arg(hex(kOutlineVariant))
		.arg(hex(accent));
}

QString applicationStyleSheet() {
	// Written as one sheet rather than per-widget styling so the whole interface
	// stays consistent and a token change lands everywhere at once.
	return QStringLiteral(R"(
QWidget {
	background: %{surface};
	color: %{textPrimary};
	selection-background-color: %{neonPurple};
	selection-color: #ffffff;
}

QMainWindow, QDialog { background: %{surfaceDeepVoid}; }

/* ---- Bento panels ---------------------------------------------------- */
QFrame[mlPanel="true"] {
	background: %{surfacePanel};
	border: 1px solid %{outlineVariant};
	border-radius: 4px;
}
QFrame[mlPanel="true"][mlAccent="cyan"]   { border-top: 2px solid %{neonCyan}; }
QFrame[mlPanel="true"][mlAccent="purple"] { border-top: 2px solid %{neonPurple}; }
QFrame[mlPanel="true"][mlAccent="pink"]   { border-top: 2px solid %{neonPink}; }

QFrame[mlStrip="true"] {
	background: %{surfaceDeepVoid};
	border-bottom: 1px solid %{outlineVariant};
}

QLabel[mlPanelTitle="true"] { color: %{neonCyan}; background: transparent; }
QLabel[mlMuted="true"]      { color: %{textMuted}; background: transparent; }
QLabel[mlSecondary="true"]  { color: %{textSecondary}; background: transparent; }
QLabel { background: transparent; }

/* ---- Buttons --------------------------------------------------------- */
QPushButton {
	background: %{surfaceContainerHigh};
	border: 1px solid %{splitterMuted};
	border-radius: 3px;
	padding: 5px 12px;
	color: %{onSurface};
}
QPushButton:hover   { background: %{surfaceBright}; border-color: %{splitterActive}; }
QPushButton:pressed { background: %{surfaceContainer}; }
QPushButton:disabled { color: %{textMuted}; background: %{surfaceContainer}; border-color: %{surfaceContainer}; }
QPushButton:focus   { border: 1px solid %{neonCyan}; }

QPushButton[mlPrimary="true"] {
	background: %{surfaceContainerHigh};
	border: 1px solid %{neonCyan};
	color: %{neonCyan};
	font-weight: 600;
}
QPushButton[mlPrimary="true"]:hover  { background: %{glowCyanSoft}; }
QPushButton[mlDanger="true"]  { border-color: %{neonPink}; color: %{neonPink}; }

/* ---- Text inputs ----------------------------------------------------- */
QLineEdit, QPlainTextEdit, QTextBrowser, QSpinBox, QComboBox {
	background: %{surfaceDeepVoid};
	border: 1px solid %{outlineVariant};
	border-radius: 3px;
	padding: 4px 6px;
	color: %{textPrimary};
	selection-background-color: %{neonPurple};
}
QLineEdit:focus, QPlainTextEdit:focus, QSpinBox:focus, QComboBox:focus {
	border: 1px solid %{neonCyan};
}
QComboBox::drop-down { border: none; width: 18px; }
QComboBox QAbstractItemView {
	background: %{surfaceContainer};
	border: 1px solid %{splitterActive};
	selection-background-color: %{neonPurple};
}

/* ---- Tables ---------------------------------------------------------- */
QTableView, QTreeWidget, QListWidget {
	background: %{surfaceDeepVoid};
	alternate-background-color: %{surfacePanel};
	border: 1px solid %{outlineVariant};
	gridline-color: %{splitterMuted};
	color: %{onSurface};
	outline: none;
}
QTableView::item, QTreeWidget::item, QListWidget::item { padding: 2px 4px; border: none; }
QTableView::item:selected, QTreeWidget::item:selected, QListWidget::item:selected {
	background: %{glowPinkSoft};
	color: %{textPrimary};
	border-left: 2px solid %{neonPink};
}
QTableView::item:hover, QTreeWidget::item:hover, QListWidget::item:hover {
	background: %{surfaceContainer};
}

QHeaderView::section {
	background: %{surfaceContainer};
	color: %{textSecondary};
	border: none;
	border-right: 1px solid %{splitterMuted};
	border-bottom: 1px solid %{outlineVariant};
	padding: 5px 6px;
	font-weight: 600;
}
QHeaderView::section:hover { color: %{neonCyan}; }
QTableCornerButton::section { background: %{surfaceContainer}; border: none; }

/* ---- Tabs ------------------------------------------------------------ */
QTabWidget::pane { border: 1px solid %{outlineVariant}; background: %{surfacePanel}; top: -1px; }
QTabBar::tab {
	background: %{surfaceContainer};
	color: %{textSecondary};
	border: 1px solid %{outlineVariant};
	border-bottom: none;
	padding: 6px 16px;
	margin-right: 2px;
}
QTabBar::tab:selected {
	background: %{surfacePanel};
	color: %{neonCyan};
	border-top: 2px solid %{neonCyan};
}
QTabBar::tab:hover:!selected { color: %{textPrimary}; background: %{surfaceContainerHigh}; }

/* ---- Scrollbars ------------------------------------------------------ */
QScrollBar:vertical   { background: %{surfaceDeepVoid}; width: 11px; margin: 0; }
QScrollBar:horizontal { background: %{surfaceDeepVoid}; height: 11px; margin: 0; }
QScrollBar::handle:vertical, QScrollBar::handle:horizontal {
	background: %{splitterActive};
	border-radius: 5px;
	min-height: 24px;
	min-width: 24px;
}
QScrollBar::handle:hover { background: %{neonPurple}; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

/* ---- Progress -------------------------------------------------------- */
QProgressBar {
	background: %{surfaceDeepVoid};
	border: 1px solid %{outlineVariant};
	border-radius: 3px;
	text-align: center;
	color: %{textPrimary};
	height: 16px;
}
QProgressBar::chunk {
	background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
		stop:0 %{neonCyan}, stop:1 %{neonPurple});
	border-radius: 2px;
}

/* ---- Checks, menus, splitters, tooltips ------------------------------ */
QCheckBox, QRadioButton { color: %{onSurface}; background: transparent; spacing: 6px; }
QCheckBox::indicator, QRadioButton::indicator {
	width: 13px; height: 13px;
	border: 1px solid %{splitterActive};
	border-radius: 2px;
	background: %{surfaceDeepVoid};
}
QCheckBox::indicator:checked, QRadioButton::indicator:checked {
	background: %{neonCyan};
	border-color: %{neonCyan};
}

QMenuBar { background: %{surfaceDeepVoid}; border-bottom: 1px solid %{outlineVariant}; }
QMenuBar::item { padding: 5px 11px; background: transparent; color: %{textSecondary}; }
QMenuBar::item:selected { background: %{surfaceContainerHigh}; color: %{neonCyan}; }
QMenu { background: %{surfaceContainer}; border: 1px solid %{splitterActive}; padding: 4px; }
QMenu::item { padding: 5px 22px; }
QMenu::item:selected { background: %{neonPurple}; color: #ffffff; }
QMenu::separator { height: 1px; background: %{outlineVariant}; margin: 4px 8px; }

QSplitter::handle { background: %{outlineVariant}; }
QSplitter::handle:hover { background: %{neonCyan}; }
QSplitter::handle:horizontal { width: 3px; }
QSplitter::handle:vertical { height: 3px; }

QStatusBar { background: %{surfaceDeepVoid}; border-top: 1px solid %{outlineVariant}; color: %{textSecondary}; }
QStatusBar::item { border: none; }

QToolTip {
	background: %{surfaceOverlay};
	color: %{textPrimary};
	border: 1px solid %{neonCyan};
	padding: 5px;
}

QGroupBox {
	border: 1px solid %{outlineVariant};
	border-radius: 4px;
	margin-top: 14px;
	padding-top: 8px;
	background: %{surfacePanel};
}
QGroupBox::title {
	subcontrol-origin: margin;
	left: 10px;
	padding: 0 5px;
	color: %{neonCyan};
	font-weight: 600;
}

QSlider::groove:horizontal { height: 4px; background: %{surfaceDeepVoid}; border-radius: 2px; }
QSlider::sub-page:horizontal {
	background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 %{neonCyan}, stop:1 %{neonPurple});
	border-radius: 2px;
}
QSlider::handle:horizontal {
	background: %{neonCyan};
	width: 11px; margin: -5px 0;
	border-radius: 5px;
}
)")
		.replace(QStringLiteral("%{surface}"), hex(kSurface))
		.replace(QStringLiteral("%{surfaceDeepVoid}"), hex(kSurfaceDeepVoid))
		.replace(QStringLiteral("%{surfacePanel}"), hex(kSurfacePanel))
		.replace(QStringLiteral("%{surfaceContainerHigh}"), hex(kSurfaceContainerHigh))
		.replace(QStringLiteral("%{surfaceContainer}"), hex(kSurfaceContainer))
		.replace(QStringLiteral("%{surfaceBright}"), hex(kSurfaceBright))
		.replace(QStringLiteral("%{surfaceOverlay}"), hex(kSurfaceOverlay))
		.replace(QStringLiteral("%{outlineVariant}"), hex(kOutlineVariant))
		.replace(QStringLiteral("%{splitterMuted}"), hex(kSplitterMuted))
		.replace(QStringLiteral("%{splitterActive}"), hex(kSplitterActive))
		.replace(QStringLiteral("%{textPrimary}"), hex(kTextPrimary))
		.replace(QStringLiteral("%{textSecondary}"), hex(kTextSecondary))
		.replace(QStringLiteral("%{textMuted}"), hex(kTextMuted))
		.replace(QStringLiteral("%{onSurface}"), hex(kOnSurface))
		.replace(QStringLiteral("%{neonCyan}"), hex(kNeonCyan))
		.replace(QStringLiteral("%{neonPurple}"), hex(kNeonPurple))
		.replace(QStringLiteral("%{neonPink}"), hex(kNeonPink))
		.replace(QStringLiteral("%{glowCyanSoft}"), rgba(kNeonCyan, 0.14))
		.replace(QStringLiteral("%{glowPinkSoft}"), rgba(kNeonPink, 0.20));
}

} // namespace ml::desktop::theme

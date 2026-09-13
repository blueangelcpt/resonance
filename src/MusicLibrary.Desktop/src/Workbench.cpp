// SPDX-License-Identifier: GPL-3.0-or-later
#include "Workbench.hpp"
#include "Theme.hpp"
#include "mlcore/Text.hpp"
#include "mlinfra/ImagePipeline.hpp"
#include "mlinfra/TagReader.hpp"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QTreeWidget>
#include <QResizeEvent>
#include <QVBoxLayout>

#include <algorithm>

#include <cstring>
#include <functional>

namespace fs = std::filesystem;

namespace ml::desktop {

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

/// A coloured status chip, as the design uses throughout.
QString chip(const QString& text, const QColor& colour) {
	return QStringLiteral(
		"<span style='background:%1; color:%2; border:1px solid %2; border-radius:3px; "
		"padding:1px 6px; font-size:9px;'>%3</span>")
		.arg(theme::rgba(colour, 0.16))
		.arg(theme::hex(colour))
		.arg(text.toHtmlEscaped());
}

QString keyValueRow(const QString& key, const QString& value, const QColor& valueColour) {
	return QStringLiteral(
		"<tr>"
		"<td style='color:%1; padding:2px 10px 2px 0; white-space:nowrap;'>%2</td>"
		"<td style='color:%3; font-family:monospace;'>%4</td>"
		"</tr>")
		.arg(theme::hex(theme::kTextSecondary))
		.arg(key.toHtmlEscaped())
		.arg(theme::hex(valueColour))
		.arg(value.toHtmlEscaped());
}

/// Resolves a catalogued relative path back to an absolute source path.
fs::path resolveSource(const Library& library, const std::string& relativePath) {
	std::error_code ec;
	for (const auto& root : library.guard().protectedRoots()) {
		const fs::path candidate = root.resolvedPath / relativePath;
		if (fs::exists(candidate, ec) && !ec) return candidate;
	}
	return {};
}

} // namespace

// ---------------------------------------------------------------------------
// Panel
// ---------------------------------------------------------------------------

Panel::Panel(const QString& title, const QString& accent, QWidget* parent) : QFrame(parent) {
	setProperty("mlPanel", true);
	setProperty("mlAccent", accent);
	setFrameShape(QFrame::NoFrame);

	auto* layout = new QVBoxLayout(this);
	layout->setContentsMargins(1, 1, 1, 1);
	layout->setSpacing(0);

	m_headerRow = new QWidget(this);
	auto* headerLayout = new QHBoxLayout(m_headerRow);
	headerLayout->setContentsMargins(10, 6, 8, 6);
	headerLayout->setSpacing(8);

	m_titleLabel = new QLabel(title, m_headerRow);
	m_titleLabel->setFont(theme::labelFont(8));
	m_titleLabel->setProperty("mlPanelTitle", true);
	headerLayout->addWidget(m_titleLabel);

	m_subtitleLabel = new QLabel(m_headerRow);
	m_subtitleLabel->setFont(theme::monoFont(8));
	m_subtitleLabel->setProperty("mlMuted", true);
	headerLayout->addWidget(m_subtitleLabel);
	headerLayout->addStretch(1);

	layout->addWidget(m_headerRow);

	auto* separator = new QFrame(this);
	separator->setFrameShape(QFrame::HLine);
	separator->setStyleSheet(QStringLiteral("background:%1; max-height:1px; border:none;")
		.arg(theme::hex(theme::kOutlineVariant)));
	layout->addWidget(separator);

	m_body = new QWidget(this);
	layout->addWidget(m_body, 1);
}

void Panel::setSubtitle(const QString& subtitle) {
	m_subtitleLabel->setText(subtitle);
}

void Panel::addHeaderWidget(QWidget* widget) {
	widget->setParent(m_headerRow);
	qobject_cast<QHBoxLayout*>(m_headerRow->layout())->addWidget(widget);
}

// ---------------------------------------------------------------------------
// LibraryExplorer
// ---------------------------------------------------------------------------

LibraryExplorer::LibraryExplorer(QWidget* parent) : QWidget(parent) {
	auto* layout = new QVBoxLayout(this);
	layout->setContentsMargins(6, 6, 6, 6);
	layout->setSpacing(6);

	m_filterEdit = new QLineEdit(this);
	m_filterEdit->setPlaceholderText(QStringLiteral("Filter hierarchy…"));
	m_filterEdit->setClearButtonEnabled(true);
	m_filterEdit->setFont(theme::monoFont(9));
	layout->addWidget(m_filterEdit);

	m_tree = new QTreeWidget(this);
	m_tree->setHeaderHidden(true);
	m_tree->setColumnCount(2);
	m_tree->setRootIsDecorated(true);
	m_tree->setIndentation(14);
	m_tree->setFont(theme::monoFont(9));
	m_tree->header()->setStretchLastSection(false);
	m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
	m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
	layout->addWidget(m_tree, 1);

	m_footnote = new QLabel(this);
	m_footnote->setFont(theme::monoFont(7));
	m_footnote->setProperty("mlMuted", true);
	m_footnote->setWordWrap(true);
	m_footnote->setText(QStringLiteral("Source is read-only. Nothing here writes to your music."));
	layout->addWidget(m_footnote);

	connect(m_tree, &QTreeWidget::itemClicked, this, &LibraryExplorer::onItemActivated);
	connect(m_filterEdit, &QLineEdit::textChanged, this, &LibraryExplorer::onFilterTextChanged);
}

void LibraryExplorer::setLibrary(Library* library) {
	m_library = library;
	refresh();
}

QTreeWidgetItem* LibraryExplorer::addNode(QTreeWidgetItem* parent, const QString& label,
	const QString& count, const QColor& accent) {
	auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_tree);
	item->setText(0, label);
	item->setText(1, count);
	item->setForeground(0, accent);
	item->setForeground(1, theme::kTextMuted);
	return item;
}

void LibraryExplorer::refresh() {
	m_tree->clear();
	if (!m_library || !m_library->isOpen()) {
		auto* item = addNode(nullptr, QStringLiteral("No library open"), {}, theme::kTextMuted);
		item->setFlags(Qt::NoItemFlags);
		return;
	}

	TrackFilter all;
	const auto count = [&](TrackFilter filter) {
		auto result = m_library->catalogue().countFiles(filter);
		return result.ok() ? result.value() : 0;
	};

	// --- Root -------------------------------------------------------------
	auto* root = addNode(nullptr, QStringLiteral("Entire Library"),
		QString::number(count(all)), theme::kNeonCyan);
	root->setData(0, Qt::UserRole, QStringLiteral("all"));
	root->setExpanded(true);

	// --- Attention filters -------------------------------------------------
	auto* attention = addNode(root, QStringLiteral("Needs attention"), {}, theme::kNeonPink);
	attention->setExpanded(true);

	struct FilterNode { const char* label; TrackFilter filter; QColor colour; };
	std::vector<FilterNode> nodes;
	{
		TrackFilter f; f.missingArtwork = true;
		nodes.push_back({"No artwork", f, theme::kNeonPink});
	}
	{
		TrackFilter f; f.missingLyrics = true;
		nodes.push_back({"No lyrics", f, theme::kNeonPurple});
	}
	{
		TrackFilter f; f.missingBpm = true;
		nodes.push_back({"No BPM", f, theme::kNeonPurple});
	}
	{
		TrackFilter f; f.hasGainFields = true;
		nodes.push_back({"Gain fields present", f, theme::kNeonCyan});
	}
	{
		TrackFilter f; f.hasPrivacyFindings = true;
		nodes.push_back({"Privacy findings", f, theme::kOverloadRed});
	}
	{
		TrackFilter f; f.readStatus = "unreadable";
		nodes.push_back({"Unreadable", f, theme::kOverloadRed});
	}

	for (const auto& node : nodes) {
		const std::int64_t n = count(node.filter);
		auto* item = addNode(attention, QString::fromLatin1(node.label),
			QString::number(n), n > 0 ? node.colour : theme::kTextMuted);
		item->setData(0, Qt::UserRole, QStringLiteral("filter"));
		item->setData(0, Qt::UserRole + 1, QVariant::fromValue(static_cast<int>(&node - nodes.data())));
	}

	// --- Albums -------------------------------------------------------------
	auto albums = m_library->catalogue().listAlbums(false, 400, 0);
	auto albumCount = m_library->catalogue().countAlbums(false);
	auto reviewCount = m_library->catalogue().countAlbums(true);

	auto* albumsNode = addNode(root, QStringLiteral("Albums"),
		QString::number(albumCount.ok() ? albumCount.value() : 0), theme::kNeonCyan);

	if (reviewCount.ok() && reviewCount.value() > 0) {
		auto* review = addNode(albumsNode, QStringLiteral("Needing review"),
			QString::number(reviewCount.value()), theme::kNeonPink);
		review->setData(0, Qt::UserRole, QStringLiteral("albums_review"));
	}

	if (albums.ok()) {
		for (const auto& album : albums.value()) {
			const QString label = album.albumArtist.empty()
				? qs(album.album)
				: qs(album.albumArtist) + QStringLiteral(" — ") + qs(album.album);
			auto* item = addNode(albumsNode, label.isEmpty() ? QStringLiteral("(untitled)") : label,
				QString::number(album.observedTrackCount),
				album.needsReview() ? theme::kNeonPink : theme::kOnSurface);
			item->setData(0, Qt::UserRole, QStringLiteral("album"));
			item->setData(0, Qt::UserRole + 1, static_cast<qlonglong>(album.id.value));
		}
	}

	m_footnote->setText(QStringLiteral(
		"Source is read-only. %1 albums, %2 need review.")
		.arg(albumCount.ok() ? albumCount.value() : 0)
		.arg(reviewCount.ok() ? reviewCount.value() : 0));
}

void LibraryExplorer::onItemActivated(QTreeWidgetItem* item, int) {
	if (!item) return;
	const QString kind = item->data(0, Qt::UserRole).toString();

	if (kind == QStringLiteral("all")) {
		emit filterRequested(TrackFilter{});
		return;
	}
	if (kind == QStringLiteral("album")) {
		emit albumRequested(AlbumId(item->data(0, Qt::UserRole + 1).toLongLong()));
		TrackFilter filter;
		filter.albumId = AlbumId(item->data(0, Qt::UserRole + 1).toLongLong());
		emit filterRequested(filter);
		return;
	}
	if (kind == QStringLiteral("filter")) {
		// Rebuild the filter from the node's label so the tree stays the single
		// source of truth for what each node means.
		const QString label = item->text(0);
		TrackFilter filter;
		if (label == QStringLiteral("No artwork")) filter.missingArtwork = true;
		else if (label == QStringLiteral("No lyrics")) filter.missingLyrics = true;
		else if (label == QStringLiteral("No BPM")) filter.missingBpm = true;
		else if (label == QStringLiteral("Gain fields present")) filter.hasGainFields = true;
		else if (label == QStringLiteral("Privacy findings")) filter.hasPrivacyFindings = true;
		else if (label == QStringLiteral("Unreadable")) filter.readStatus = "unreadable";
		emit filterRequested(filter);
	}
}

void LibraryExplorer::onFilterTextChanged(const QString& text) {
	// Hide nodes that do not match, keeping ancestors of a match visible.
	std::function<bool(QTreeWidgetItem*)> apply = [&](QTreeWidgetItem* item) {
		bool anyVisible = item->text(0).contains(text, Qt::CaseInsensitive) || text.isEmpty();
		for (int i = 0; i < item->childCount(); ++i) {
			if (apply(item->child(i))) anyVisible = true;
		}
		item->setHidden(!anyVisible);
		return anyVisible;
	};
	for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
		apply(m_tree->topLevelItem(i));
	}
}

// ---------------------------------------------------------------------------
// DeckPanel
// ---------------------------------------------------------------------------

DeckPanel::DeckPanel(QWidget* parent) : QWidget(parent) {
	auto* layout = new QHBoxLayout(this);
	layout->setContentsMargins(10, 8, 10, 8);
	layout->setSpacing(12);

	// Cover.
	m_coverLabel = new QLabel(this);
	m_coverLabel->setFixedSize(96, 96);
	m_coverLabel->setAlignment(Qt::AlignCenter);
	m_coverLabel->setStyleSheet(QStringLiteral(
		"background:%1; border:1px solid %2; border-radius:3px; color:%3;")
		.arg(theme::hex(theme::kSurfaceDeepVoid))
		.arg(theme::hex(theme::kOutlineVariant))
		.arg(theme::hex(theme::kTextMuted)));
	m_coverLabel->setText(QStringLiteral("no\ncover"));
	m_coverLabel->setFont(theme::monoFont(8));
	layout->addWidget(m_coverLabel);

	// VFD readout column.
	auto* readout = new QVBoxLayout();
	readout->setSpacing(4);

	m_counterLabel = new QLabel(QStringLiteral("--:--"), this);
	QFont counter = theme::monoFont(26, QFont::Bold);
	m_counterLabel->setFont(counter);
	m_counterLabel->setStyleSheet(QStringLiteral("color:%1;").arg(theme::hex(theme::kNeonCyan)));
	readout->addWidget(m_counterLabel);

	m_specBadges = new QLabel(this);
	m_specBadges->setTextFormat(Qt::RichText);
	m_specBadges->setFont(theme::monoFont(8));
	readout->addWidget(m_specBadges);

	layout->addLayout(readout);

	// Marquee and state.
	auto* marqueeColumn = new QVBoxLayout();
	marqueeColumn->setSpacing(6);

	m_marqueeLabel = new QLabel(this);
	m_marqueeLabel->setFont(theme::monoFont(11, QFont::DemiBold));
	m_marqueeLabel->setStyleSheet(QStringLiteral(
		"color:%1; background:%2; border:1px solid %3; border-radius:3px; padding:6px 10px;")
		.arg(theme::hex(theme::kPrimary))
		.arg(theme::hex(theme::kSurfaceDeepVoid))
		.arg(theme::hex(theme::kOutlineVariant)));
	m_marqueeLabel->setText(QStringLiteral("  — select a track —"));
	marqueeColumn->addWidget(m_marqueeLabel);

	m_stateBadges = new QLabel(this);
	m_stateBadges->setTextFormat(Qt::RichText);
	m_stateBadges->setFont(theme::monoFont(8));
	m_stateBadges->setWordWrap(true);
	marqueeColumn->addWidget(m_stateBadges);
	marqueeColumn->addStretch(1);

	layout->addLayout(marqueeColumn, 1);

	// Controls.
	auto* controls = new QVBoxLayout();
	controls->setSpacing(6);
	m_sweepButton = new QPushButton(QStringLiteral("▶ Sweep"), this);
	m_sweepButton->setCheckable(true);
	m_sweepButton->setProperty("mlPrimary", true);
	m_sweepButton->setToolTip(QStringLiteral(
		"Sweep the analyser across the decoded track.\n\n"
		"Resonance has no playback engine: this moves a playhead through the "
		"file's analysed spectrum, it does not play audio."));
	connect(m_sweepButton, &QPushButton::toggled, this, &DeckPanel::sweepToggled);
	controls->addWidget(m_sweepButton);

	auto* analyseButton = new QPushButton(QStringLiteral("Analyse"), this);
	analyseButton->setToolTip(QStringLiteral("Decode this track and compute its spectrum."));
	connect(analyseButton, &QPushButton::clicked, this, [this]() {
		if (m_record) emit analyseRequested(m_record->id);
	});
	controls->addWidget(analyseButton);
	controls->addStretch(1);

	layout->addLayout(controls);

	// Marquee scroll.
	auto* timer = new QTimer(this);
	timer->setInterval(180);
	connect(timer, &QTimer::timeout, this, &DeckPanel::updateMarquee);
	timer->start();
}

void DeckPanel::setLibrary(Library* library) {
	m_library = library;
	clear();
}

void DeckPanel::clear() {
	m_record.reset();
	m_coverLabel->setPixmap(QPixmap());
	m_coverLabel->setText(QStringLiteral("no\ncover"));
	m_counterLabel->setText(QStringLiteral("--:--"));
	m_specBadges->clear();
	m_stateBadges->clear();
	m_marqueeText.clear();
	m_marqueeLabel->setText(QStringLiteral("  — select a track —"));
}

void DeckPanel::showFile(const FileRecord& record) {
	m_record = record;

	m_counterLabel->setText(qs(text::formatDuration(record.audio.durationMs)));

	// Technical spec badges: all measured from the stream, never from tags.
	QStringList spec;
	if (record.audio.bitrateKbps > 0) {
		spec << chip(QStringLiteral("%1 kbps %2").arg(record.audio.bitrateKbps)
			.arg(qs(std::string(toString(record.audio.bitrateMode))).toUpper()), theme::kNeonCyan);
	}
	if (record.audio.sampleRateHz > 0) {
		spec << chip(QStringLiteral("%1 Hz").arg(record.audio.sampleRateHz), theme::kNeonCyan);
	}
	if (record.audio.channels > 0) {
		spec << chip(record.audio.channels == 1 ? QStringLiteral("MONO") : QStringLiteral("STEREO"),
			theme::kNeonCyan);
	}
	if (!record.primaryContainer.empty()) {
		spec << chip(qs(record.primaryContainer).toUpper(), theme::kNeonPurple);
	}
	m_specBadges->setText(spec.join(QStringLiteral(" ")));

	// State badges: what Resonance knows about this file.
	QStringList state;
	state << chip(record.hasArtwork
		? QStringLiteral("COVER %1×%2").arg(record.artworkWidth).arg(record.artworkHeight)
		: QStringLiteral("NO COVER"),
		record.hasArtwork ? theme::kNeonCyan : theme::kNeonPink);
	state << chip(record.hasLyrics ? QStringLiteral("LYRICS") : QStringLiteral("NO LYRICS"),
		record.hasLyrics ? theme::kNeonCyan : theme::kTextMuted);
	state << chip(record.bpm ? QStringLiteral("BPM %1").arg(static_cast<int>(*record.bpm))
		: QStringLiteral("NO BPM"),
		record.bpm ? theme::kNeonCyan : theme::kTextMuted);
	if (record.hasGainFields) state << chip(QStringLiteral("GAIN FIELDS"), theme::kNeonPurple);
	if (record.hasPrivacyFindings) state << chip(QStringLiteral("PRIVACY"), theme::kOverloadRed);
	if (record.readStatus != "ok") {
		state << chip(qs(record.readStatus).toUpper(), theme::kOverloadRed);
	}
	m_stateBadges->setText(state.join(QStringLiteral(" ")));

	m_marqueeText = qs(record.artist) + QStringLiteral("  ·  ") + qs(record.title)
		+ QStringLiteral("  ·  ") + qs(record.album) + QStringLiteral("      ");
	m_marqueeOffset = 0;

	// Cover, read from the file on demand rather than cached for every row.
	m_coverLabel->setPixmap(QPixmap());
	m_coverLabel->setText(QStringLiteral("no\ncover"));
	if (m_library && record.hasArtwork) {
		const fs::path source = resolveSource(*m_library, record.relativePath);
		if (!source.empty()) {
			TagReadOptions options;
			options.retainFramePayloads = true;
			auto read = TagReader::read(source, options);
			if (read) {
				for (const auto& frame : read.value().snapshot.frames) {
					if (frame.id != "APIC" && frame.id != "PIC") continue;
					if (frame.binary.empty()) continue;
					QImage image;
					image.loadFromData(reinterpret_cast<const uchar*>(frame.binary.data()),
						static_cast<int>(frame.binary.size()));
					if (!image.isNull()) {
						m_coverLabel->setText(QString());
						m_coverLabel->setPixmap(QPixmap::fromImage(
							image.scaled(94, 94, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
					}
					break;
				}
			}
		}
	}
}

void DeckPanel::setPosition(double fraction) {
	m_position = fraction;
	if (!m_record) return;
	const std::int64_t atMs = static_cast<std::int64_t>(fraction * m_record->audio.durationMs);
	m_counterLabel->setText(qs(text::formatDuration(atMs)) + QStringLiteral(" / ")
		+ qs(text::formatDuration(m_record->audio.durationMs)));
}

void DeckPanel::updateMarquee() {
	if (m_marqueeText.isEmpty()) return;
	// A scrolling monospace ticker, as the design specifies.
	constexpr int kWidth = 58;
	if (m_marqueeText.size() <= kWidth) {
		m_marqueeLabel->setText(m_marqueeText);
		return;
	}
	m_marqueeOffset = (m_marqueeOffset + 1) % m_marqueeText.size();
	const QString doubled = m_marqueeText + m_marqueeText;
	m_marqueeLabel->setText(doubled.mid(m_marqueeOffset, kWidth));
}

// ---------------------------------------------------------------------------
// InspectorPanel
// ---------------------------------------------------------------------------

InspectorPanel::InspectorPanel(QWidget* parent) : QWidget(parent) {
	auto* layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);

	// --- Track information -------------------------------------------------
	auto* header = new QWidget(this);
	auto* headerLayout = new QVBoxLayout(header);
	headerLayout->setContentsMargins(10, 8, 10, 6);
	headerLayout->setSpacing(2);

	m_trackTitle = new QLabel(header);
	m_trackTitle->setFont(theme::displayFont(11, QFont::Bold));
	m_trackTitle->setStyleSheet(QStringLiteral("color:%1;").arg(theme::hex(theme::kTextPrimary)));
	m_trackTitle->setWordWrap(true);
	headerLayout->addWidget(m_trackTitle);

	m_trackSubtitle = new QLabel(header);
	m_trackSubtitle->setFont(theme::monoFont(8));
	m_trackSubtitle->setProperty("mlSecondary", true);
	m_trackSubtitle->setWordWrap(true);
	headerLayout->addWidget(m_trackSubtitle);

	layout->addWidget(header);

	// --- Album art, centred in the space beneath the track information ------
	auto* coverHolder = new QWidget(this);
	auto* coverLayout = new QVBoxLayout(coverHolder);
	coverLayout->setContentsMargins(10, 2, 10, 6);
	coverLayout->setSpacing(4);

	m_coverLabel = new QLabel(coverHolder);
	m_coverLabel->setAlignment(Qt::AlignCenter);
	m_coverLabel->setMinimumHeight(180);
	m_coverLabel->setStyleSheet(QStringLiteral(
		"background:%1; border:1px solid %2; border-radius:4px; color:%3;")
		.arg(theme::hex(theme::kSurfaceDeepVoid))
		.arg(theme::hex(theme::kOutlineVariant))
		.arg(theme::hex(theme::kTextMuted)));
	m_coverLabel->setFont(theme::monoFont(9));
	// Centred horizontally within the column, as requested.
	coverLayout->addWidget(m_coverLabel, 0, Qt::AlignHCenter);

	m_coverCaption = new QLabel(coverHolder);
	m_coverCaption->setAlignment(Qt::AlignCenter);
	m_coverCaption->setFont(theme::monoFont(7));
	m_coverCaption->setProperty("mlMuted", true);
	coverLayout->addWidget(m_coverCaption);

	layout->addWidget(coverHolder);

	m_tabs = new QTabWidget(this);

	m_changeBrowser = new QTextBrowser(m_tabs);
	m_changeBrowser->setOpenExternalLinks(true);
	m_tabs->addTab(m_changeBrowser, QStringLiteral("Change plan"));

	m_metadataBrowser = new QTextBrowser(m_tabs);
	m_metadataBrowser->setFont(theme::monoFont(8));
	m_tabs->addTab(m_metadataBrowser, QStringLiteral("ID3 inspector"));

	m_lyricsBrowser = new QTextBrowser(m_tabs);
	m_tabs->addTab(m_lyricsBrowser, QStringLiteral("Lyrics"));

	layout->addWidget(m_tabs, 1);

	m_footer = new QLabel(this);
	m_footer->setFont(theme::monoFont(7));
	m_footer->setProperty("mlMuted", true);
	m_footer->setContentsMargins(8, 4, 8, 4);
	m_footer->setWordWrap(true);
	layout->addWidget(m_footer);

	clear();
}

void InspectorPanel::setLibrary(Library* library) {
	m_library = library;
	clear();
}

void InspectorPanel::clear() {
	const QString empty = QStringLiteral(
		"<p style='color:%1'><i>Select a track.</i></p>").arg(theme::hex(theme::kTextMuted));
	m_changeBrowser->setHtml(empty);
	m_metadataBrowser->setHtml(empty);
	m_lyricsBrowser->setHtml(empty);
	m_footer->setText(QStringLiteral("No file selected."));

	m_trackTitle->setText(QStringLiteral("No track selected"));
	m_trackSubtitle->clear();
	m_coverImage = QImage();
	m_coverLabel->setPixmap(QPixmap());
	m_coverLabel->setText(QStringLiteral("no artwork"));
	m_coverLabel->setFixedSize(QSize(200, 200));
	m_coverCaption->clear();
}

void InspectorPanel::populateTrackHeader(const FileRecord& record) {
	m_trackTitle->setText(record.title.empty()
		? qs(record.fileName)
		: qs(record.title));

	QStringList parts;
	if (!record.artist.empty()) parts << qs(record.artist);
	if (!record.album.empty()) parts << qs(record.album);
	if (record.trackNumber) parts << QStringLiteral("track %1").arg(*record.trackNumber);
	if (record.audio.durationMs > 0) parts << qs(text::formatDuration(record.audio.durationMs));
	m_trackSubtitle->setText(parts.join(QStringLiteral("  ·  ")));
}

void InspectorPanel::loadCoverArt(const FileRecord& record) {
	m_coverImage = QImage();
	m_coverLabel->setPixmap(QPixmap());
	m_coverLabel->setText(QStringLiteral("no artwork"));
	m_coverCaption->clear();

	if (!m_library) return;

	const fs::path source = resolveSource(*m_library, record.relativePath);
	if (source.empty()) {
		m_coverCaption->setText(QStringLiteral("source not reachable"));
		rescaleCover();
		return;
	}

	// Read the picture frames on demand. The track table never holds artwork for
	// every row; only the selected file's cover is loaded.
	TagReadOptions options;
	options.retainFramePayloads = true;
	auto read = TagReader::read(source, options);
	if (!read) {
		m_coverCaption->setText(QStringLiteral("could not read tags"));
		rescaleCover();
		return;
	}

	// Prefer the declared front cover; fall back to the only picture present.
	const TagFrame* chosen = nullptr;
	int chosenOrdinal = -1;
	for (const auto& picture : read.value().snapshot.pictures) {
		if (picture.type == PictureType::FrontCover) { chosenOrdinal = picture.frameOrdinal; break; }
	}
	for (const auto& frame : read.value().snapshot.frames) {
		if (frame.id != "APIC" && frame.id != "PIC") continue;
		if (frame.binary.empty()) continue;
		if (chosenOrdinal < 0 || frame.ordinal == chosenOrdinal) { chosen = &frame; break; }
	}

	if (!chosen) {
		m_coverCaption->setText(QStringLiteral("no embedded artwork"));
		rescaleCover();
		return;
	}

	QImage image;
	image.loadFromData(reinterpret_cast<const uchar*>(chosen->binary.data()),
		static_cast<int>(chosen->binary.size()));
	if (image.isNull()) {
		m_coverCaption->setText(QStringLiteral("artwork could not be decoded"));
		rescaleCover();
		return;
	}

	m_coverImage = image;
	m_coverLabel->setText(QString());

	// Caption the measured size, and say plainly whether it meets the 600 px
	// output requirement rather than leaving the reader to work it out.
	const int shortest = std::min(image.width(), image.height());
	const QString verdict = (shortest >= 600)
		? QStringLiteral("meets the 600×600 output")
		: QStringLiteral("below the 600×600 output — will not be upscaled");
	m_coverCaption->setText(QStringLiteral("%1×%2 · %3 · %4")
		.arg(image.width()).arg(image.height())
		.arg(qs(chosen->value.empty() ? std::string("image") : chosen->value))
		.arg(verdict));
	m_coverCaption->setStyleSheet(QStringLiteral("color:%1;")
		.arg(theme::hex(shortest >= 600 ? theme::kNeonCyan : theme::kNeonPink)));

	rescaleCover();
}

void InspectorPanel::rescaleCover() {
	// Square box, as wide as the column allows, so the art stays centred and
	// keeps its aspect ratio at any splitter position.
	const int available = std::max(140, width() - 28);
	const int edge = std::min(available, 340);
	m_coverLabel->setFixedSize(QSize(edge, edge));

	if (m_coverImage.isNull()) {
		m_coverLabel->setPixmap(QPixmap());
		m_coverLabel->setText(m_coverLabel->text().isEmpty()
			? QStringLiteral("no artwork") : m_coverLabel->text());
		return;
	}
	m_coverLabel->setPixmap(QPixmap::fromImage(m_coverImage.scaled(
		edge - 4, edge - 4, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
}

void InspectorPanel::resizeEvent(QResizeEvent* event) {
	QWidget::resizeEvent(event);
	rescaleCover();
}

void InspectorPanel::showFile(FileId file) {
	if (!m_library || !m_library->isOpen()) return;

	auto record = m_library->catalogue().loadFile(file);
	if (!record || !record.value()) {
		clear();
		return;
	}

	populateTrackHeader(*record.value());
	loadCoverArt(*record.value());

	auto plan = m_library->previewFile(file);
	if (plan) {
		populateChangePlan(plan.value());
	} else {
		m_changeBrowser->setHtml(QStringLiteral("<p style='color:%1'>Could not preview: %2</p>")
			.arg(theme::hex(theme::kOverloadRed))
			.arg(qs(plan.error().describe()).toHtmlEscaped()));
	}

	auto snapshot = m_library->catalogue().latestSnapshot(file);
	if (snapshot && snapshot.value()) {
		populateMetadata(*snapshot.value(), *record.value());
		populateLyrics(file, *snapshot.value());
	}

	m_footer->setText(QStringLiteral("%1  ·  %2")
		.arg(qs(record.value()->relativePath))
		.arg(qs(text::formatBytes(record.value()->identity.sizeBytes))));
}

void InspectorPanel::populateChangePlan(const FilePlan& plan) {
	QString html;
	html += QStringLiteral("<div style='font-family:monospace; font-size:10px;'>");

	html += QStringLiteral("<p><b style='color:%1'>Destination</b><br>%2</p>")
		.arg(theme::hex(theme::kNeonCyan))
		.arg(plan.destinationRelativePath.empty()
			? QStringLiteral("<span style='color:%1'>review required — no destination proposed</span>")
				.arg(theme::hex(theme::kNeonPink))
			: qs(plan.destinationRelativePath).toHtmlEscaped());

	if (!plan.naming.exceptions.empty()) {
		html += QStringLiteral("<p><b style='color:%1'>Naming</b></p><ul style='margin-top:2px'>")
			.arg(theme::hex(theme::kNeonCyan));
		for (const auto& exception : plan.naming.exceptions) {
			html += QStringLiteral("<li>%1 %2</li>")
				.arg(chip(exception.advisory ? QStringLiteral("advisory") : QStringLiteral("review"),
					exception.advisory ? theme::kTextSecondary : theme::kNeonPink))
				.arg(qs(exception.detail).toHtmlEscaped());
		}
		html += QStringLiteral("</ul>");
	}

	const auto changing = plan.changingDecisions();
	html += QStringLiteral("<p><b style='color:%1'>Tag changes (%2)</b></p>")
		.arg(theme::hex(theme::kNeonCyan)).arg(changing.size());

	if (changing.empty()) {
		html += QStringLiteral("<p style='color:%1'><i>None. Every existing field is preserved.</i></p>")
			.arg(theme::hex(theme::kTextMuted));
	} else {
		html += QStringLiteral("<table width='100%' cellspacing='0' cellpadding='4'>");
		for (const auto* decision : changing) {
			const QColor colour = theme::accentForStatus(qs(std::string(toString(decision->action))));
			html += QStringLiteral(
				"<tr><td valign='top' width='70'>%1</td>"
				"<td><b>%2</b>%3<br>"
				"<span style='color:%4; font-size:9px'>%5</span><br>"
				"<span style='color:%6; font-size:9px'>%7</span></td></tr>")
				.arg(chip(qs(std::string(toString(decision->action))).toUpper(), colour))
				.arg(qs(decision->frameId).toHtmlEscaped())
				.arg(decision->description.empty() ? QString()
					: QStringLiteral(" [%1]").arg(qs(decision->description).toHtmlEscaped()))
				.arg(theme::hex(theme::kTextMuted))
				.arg(qs(decision->ruleId).toHtmlEscaped())
				.arg(theme::hex(theme::kTextSecondary))
				.arg(qs(decision->reason).toHtmlEscaped());
		}
		html += QStringLiteral("</table>");
	}

	html += QStringLiteral("<table width='100%' cellspacing='0' cellpadding='2'>");
	html += keyValueRow(QStringLiteral("Artwork"), qs(std::string(toString(plan.artwork.outcome))),
		theme::accentForStatus(qs(std::string(toString(plan.artwork.outcome)))));
	html += keyValueRow(QStringLiteral("BPM"), qs(std::string(toString(plan.tempo.kind))),
		theme::accentForStatus(qs(std::string(toString(plan.tempo.kind)))));
	html += keyValueRow(QStringLiteral("Lyrics"), qs(std::string(toString(plan.lyrics.state))),
		theme::accentForStatus(qs(std::string(toString(plan.lyrics.state)))));
	html += keyValueRow(QStringLiteral("Tag version out"), qs(std::string(toString(plan.outputContainer))),
		theme::kOnSurface);
	html += keyValueRow(QStringLiteral("Policy savings"),
		qs(text::formatBytes(plan.size.optimisationSavings)), theme::kNeonCyan);
	html += keyValueRow(QStringLiteral("Enrichment growth"),
		qs(text::formatBytes(plan.size.enrichmentGrowth)), theme::kNeonPurple);
	html += keyValueRow(QStringLiteral("Padding change"),
		qs(text::formatBytes(plan.size.paddingDelta)), theme::kTextSecondary);
	html += QStringLiteral("</table>");

	if (!plan.tempo.reason.empty()) {
		html += QStringLiteral("<p style='color:%1; font-size:9px'>BPM: %2</p>")
			.arg(theme::hex(theme::kTextSecondary)).arg(qs(plan.tempo.reason).toHtmlEscaped());
	}
	if (!plan.lyrics.reason.empty()) {
		html += QStringLiteral("<p style='color:%1; font-size:9px'>Lyrics: %2</p>")
			.arg(theme::hex(theme::kTextSecondary)).arg(qs(plan.lyrics.reason).toHtmlEscaped());
	}

	if (!plan.blockers.empty()) {
		html += QStringLiteral("<p><b style='color:%1'>Blocked</b></p><ul>")
			.arg(theme::hex(theme::kNeonPink));
		for (auto blocker : plan.blockers) {
			html += QStringLiteral("<li style='color:%1'>%2</li>")
				.arg(theme::hex(theme::kNeonPink))
				.arg(qs(std::string(toString(blocker))));
		}
		html += QStringLiteral("</ul>");
	}
	for (const auto& note : plan.notes) {
		html += QStringLiteral("<p style='color:%1; font-size:9px'><i>%2</i></p>")
			.arg(theme::hex(theme::kTextMuted)).arg(qs(note).toHtmlEscaped());
	}

	html += QStringLiteral("</div>");
	m_changeBrowser->setHtml(html);
}

void InspectorPanel::populateMetadata(const TagSnapshot& snapshot, const FileRecord& record) {
	QString html = QStringLiteral("<div style='font-family:monospace; font-size:10px;'>");

	html += QStringLiteral("<p><b style='color:%1'>Container</b></p><table cellpadding='2'>")
		.arg(theme::hex(theme::kNeonCyan));
	html += keyValueRow(QStringLiteral("Primary"), qs(std::string(toString(snapshot.primaryContainer))),
		theme::kNeonCyan);
	html += keyValueRow(QStringLiteral("ID3v2 bytes"),
		QString::number(snapshot.id3v2TagBytes), theme::kOnSurface);
	html += keyValueRow(QStringLiteral("Padding"),
		QString::number(snapshot.id3v2PaddingBytes), theme::kOnSurface);
	html += keyValueRow(QStringLiteral("APEv2 bytes"),
		QString::number(snapshot.apeTagBytes), theme::kOnSurface);
	html += keyValueRow(QStringLiteral("ID3v1"),
		snapshot.hasId3v1 ? QStringLiteral("present") : QStringLiteral("absent"), theme::kOnSurface);
	html += keyValueRow(QStringLiteral("Audio offset"),
		QString::number(record.audio.audioOffset), theme::kTextSecondary);
	html += keyValueRow(QStringLiteral("Audio length"),
		QString::number(record.audio.audioLength), theme::kTextSecondary);
	html += keyValueRow(QStringLiteral("Audio SHA-256"),
		qs(record.audioSha256.substr(0, 24)) + QStringLiteral("…"), theme::kTextSecondary);
	html += QStringLiteral("</table>");

	html += QStringLiteral("<p><b style='color:%1'>Frames (%2)</b></p>")
		.arg(theme::hex(theme::kNeonCyan)).arg(snapshot.frames.size());
	html += QStringLiteral("<table width='100%' cellspacing='0' cellpadding='3'>");
	for (const auto& frame : snapshot.frames) {
		QString value = qs(frame.value);
		if (value.size() > 90) value = value.left(90) + QStringLiteral("…");
		if (value.isEmpty() && !frame.binary.empty()) {
			value = QStringLiteral("<%1 bytes binary>").arg(frame.binary.size());
		}
		QString label = qs(frame.id);
		if (!frame.description.empty()) {
			label += QStringLiteral(":%1").arg(qs(frame.description));
		} else if (!frame.owner.empty()) {
			label += QStringLiteral(":%1").arg(qs(frame.owner));
		}
		const QColor colour = frame.interpreted ? theme::kOnSurface : theme::kNeonPurple;
		html += keyValueRow(label, value, colour);
	}
	html += QStringLiteral("</table>");

	if (!snapshot.uninterpretedFrameIds.empty()) {
		html += QStringLiteral(
			"<p style='color:%1; font-size:9px'>%2 frame(s) could not be interpreted. "
			"Their exact payloads are retained and written back unchanged.</p>")
			.arg(theme::hex(theme::kNeonPurple))
			.arg(snapshot.uninterpretedFrameIds.size());
	}
	for (const auto& warning : snapshot.readWarnings) {
		html += QStringLiteral("<p style='color:%1; font-size:9px'>⚠ %2</p>")
			.arg(theme::hex(theme::kNeonPink)).arg(qs(warning).toHtmlEscaped());
	}

	html += QStringLiteral("</div>");
	m_metadataBrowser->setHtml(html);
}

void InspectorPanel::populateLyrics(FileId file, const TagSnapshot& snapshot) {
	QString html = QStringLiteral("<div>");

	auto stored = m_library->catalogue().loadLyrics(file);
	if (stored && stored.value()) {
		const LyricsDecision& decision = *stored.value();
		html += QStringLiteral("<p>%1 &nbsp; %2</p>")
			.arg(chip(qs(std::string(toString(decision.state))).toUpper(),
				theme::accentForStatus(qs(std::string(toString(decision.state))))))
			.arg(chip(qs(std::string(toString(decision.confidence))).toUpper(),
				theme::accentForStatus(qs(std::string(toString(decision.confidence))))));
		html += QStringLiteral("<p style='color:%1; font-size:10px'>%2</p>")
			.arg(theme::hex(theme::kTextSecondary)).arg(qs(decision.reason).toHtmlEscaped());

		if (!decision.evidence.empty()) {
			html += QStringLiteral("<ul style='font-size:10px'>");
			for (const auto& evidence : decision.evidence) {
				html += QStringLiteral("<li style='color:%1'>%2 %3</li>")
					.arg(theme::hex(evidence.supporting ? theme::kNeonCyan : theme::kNeonPink))
					.arg(evidence.supporting ? QStringLiteral("✔") : QStringLiteral("✘"))
					.arg(qs(evidence.detail).toHtmlEscaped());
			}
			html += QStringLiteral("</ul>");
		}
	}

	const std::string embedded = snapshot.lyrics();
	if (!embedded.empty()) {
		html += QStringLiteral("<p><b style='color:%1'>Embedded in the file</b></p>")
			.arg(theme::hex(theme::kNeonCyan));
		html += QStringLiteral("<pre style='color:%1; white-space:pre-wrap; font-size:10px'>%2</pre>")
			.arg(theme::hex(theme::kOnSurface)).arg(qs(embedded).toHtmlEscaped());
	} else {
		html += QStringLiteral("<p style='color:%1'><i>No lyrics are embedded in this file.</i></p>")
			.arg(theme::hex(theme::kTextMuted));
	}

	html += QStringLiteral("</div>");
	m_lyricsBrowser->setHtml(html);
}

} // namespace ml::desktop

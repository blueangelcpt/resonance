// SPDX-License-Identifier: GPL-3.0-or-later
#include "TrackTableModel.hpp"
#include "mlcore/Text.hpp"

#include <QBrush>
#include <QColor>
#include <QFont>

namespace ml::desktop {

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

/// A tick, a cross, or a dash for "not applicable". Text rather than icons so
/// the table stays legible at any scale and copies as text.
QString mark(bool value) { return value ? QStringLiteral("yes") : QString(); }

} // namespace

// ---------------------------------------------------------------------------
// TrackTableModel
// ---------------------------------------------------------------------------

TrackTableModel::TrackTableModel(QObject* parent) : QAbstractTableModel(parent) {}

void TrackTableModel::setLibrary(Library* library) {
	beginResetModel();
	m_library = library;
	m_pages.clear();
	m_pageOrder.clear();
	m_rowCount = 0;
	endResetModel();
	refresh();
}

void TrackTableModel::setFilter(const TrackFilter& filter) {
	m_filter = filter;
	refresh();
}

void TrackTableModel::refresh() {
	beginResetModel();
	m_pages.clear();
	m_pageOrder.clear();
	m_rowCount = 0;

	if (m_library && m_library->isOpen()) {
		auto count = m_library->readCatalogue().countFiles(m_filter);
		if (count) m_rowCount = static_cast<int>(count.value());
	}
	endResetModel();
}

int TrackTableModel::rowCount(const QModelIndex& parent) const {
	return parent.isValid() ? 0 : m_rowCount;
}

int TrackTableModel::columnCount(const QModelIndex& parent) const {
	return parent.isValid() ? 0 : ColumnCount;
}

void TrackTableModel::ensureLoaded(int row) const {
	if (!m_library || !m_library->isOpen()) return;

	const int page = row / kPageSize;
	if (m_pages.contains(page)) return;

	auto records = m_library->readCatalogue().queryFiles(m_filter, kPageSize,
		static_cast<std::int64_t>(page) * kPageSize);
	if (!records) return;

	m_pages.insert(page, records.value());
	m_pageOrder.push_back(page);

	// Bounded cache: evict the oldest page rather than growing without limit.
	while (m_pageOrder.size() > kMaxCachedPages) {
		const int evicted = m_pageOrder.front();
		m_pageOrder.pop_front();
		m_pages.remove(evicted);
	}
}

std::optional<FileRecord> TrackTableModel::recordAt(int row) const {
	if (row < 0 || row >= m_rowCount) return std::nullopt;
	ensureLoaded(row);

	const int page = row / kPageSize;
	const int offset = row % kPageSize;
	auto it = m_pages.constFind(page);
	if (it == m_pages.constEnd()) return std::nullopt;
	if (offset >= static_cast<int>(it->size())) return std::nullopt;
	return (*it)[static_cast<std::size_t>(offset)];
}

QVariant TrackTableModel::data(const QModelIndex& index, int role) const {
	if (!index.isValid()) return {};

	const auto record = recordAt(index.row());
	if (!record) {
		return role == Qt::DisplayRole ? QVariant(QStringLiteral("…")) : QVariant();
	}

	if (role == Qt::TextAlignmentRole) {
		switch (index.column()) {
			case ColumnTrack:
			case ColumnDuration:
			case ColumnBitrate:
			case ColumnBpm:
				return QVariant(Qt::AlignRight | Qt::AlignVCenter);
			default:
				return QVariant(Qt::AlignLeft | Qt::AlignVCenter);
		}
	}

	if (role == Qt::ForegroundRole) {
		// An unreadable file is coloured so it cannot be mistaken for a normal
		// row with empty tags.
		if (record->readStatus != "ok") return QBrush(QColor(0xC0, 0x39, 0x2B));
		return {};
	}

	if (role == Qt::ToolTipRole) {
		QString tip = qs(record->relativePath);
		if (record->readStatus != "ok") {
			tip += QStringLiteral("\n\nRead status: ") + qs(record->readStatus);
			if (!record->readError.empty()) tip += QStringLiteral("\n") + qs(record->readError);
		}
		if (record->hasArtwork && record->artworkWidth > 0) {
			tip += QStringLiteral("\n\nEmbedded cover: %1 x %2")
				.arg(record->artworkWidth).arg(record->artworkHeight);
		}
		return tip;
	}

	if (role != Qt::DisplayRole) return {};

	switch (index.column()) {
		case ColumnTrack:
			return record->trackNumber ? QString::number(*record->trackNumber) : QString();
		case ColumnTitle: return qs(record->title);
		case ColumnArtist: return qs(record->artist);
		case ColumnAlbum: return qs(record->album);
		case ColumnAlbumArtist: return qs(record->albumArtist);
		case ColumnDuration: return qs(text::formatDuration(record->audio.durationMs));
		case ColumnBitrate:
			return record->audio.bitrateKbps > 0
				? QStringLiteral("%1 %2").arg(record->audio.bitrateKbps)
					.arg(qs(std::string(toString(record->audio.bitrateMode))))
				: QString();
		case ColumnArtwork:
			if (!record->hasArtwork) return QStringLiteral("none");
			if (record->artworkWidth <= 0) return QStringLiteral("yes");
			return QStringLiteral("%1x%2").arg(record->artworkWidth).arg(record->artworkHeight);
		case ColumnBpm:
			return record->bpm ? QString::number(static_cast<int>(*record->bpm)) : QString();
		case ColumnLyrics: return mark(record->hasLyrics);
		case ColumnGain: return mark(record->hasGainFields);
		case ColumnPrivacy: return mark(record->hasPrivacyFindings);
		case ColumnTagVersion: return qs(record->primaryContainer);
		case ColumnPath: return qs(record->relativePath);
		default: return {};
	}
}

QVariant TrackTableModel::headerData(int section, Qt::Orientation orientation, int role) const {
	if (role != Qt::DisplayRole || orientation != Qt::Horizontal) return {};

	switch (section) {
		case ColumnTrack: return QStringLiteral("#");
		case ColumnTitle: return QStringLiteral("Title");
		case ColumnArtist: return QStringLiteral("Artist");
		case ColumnAlbum: return QStringLiteral("Album");
		case ColumnAlbumArtist: return QStringLiteral("Album artist");
		case ColumnDuration: return QStringLiteral("Length");
		case ColumnBitrate: return QStringLiteral("Bitrate");
		case ColumnArtwork: return QStringLiteral("Cover");
		case ColumnBpm: return QStringLiteral("BPM");
		case ColumnLyrics: return QStringLiteral("Lyrics");
		case ColumnGain: return QStringLiteral("Gain");
		case ColumnPrivacy: return QStringLiteral("Privacy");
		case ColumnTagVersion: return QStringLiteral("Tag");
		case ColumnPath: return QStringLiteral("Relative path");
		default: return {};
	}
}

// ---------------------------------------------------------------------------
// AlbumListModel
// ---------------------------------------------------------------------------

AlbumListModel::AlbumListModel(QObject* parent) : QAbstractTableModel(parent) {}

void AlbumListModel::setLibrary(Library* library) {
	m_library = library;
	refresh();
}

void AlbumListModel::setOnlyNeedingReview(bool only) {
	m_onlyNeedingReview = only;
	refresh();
}

void AlbumListModel::refresh() {
	beginResetModel();
	m_albums.clear();
	if (m_library && m_library->isOpen()) {
		auto albums = m_library->readCatalogue().listAlbums(m_onlyNeedingReview, 0, 0);
		if (albums) m_albums = albums.value();
	}
	endResetModel();
}

int AlbumListModel::rowCount(const QModelIndex& parent) const {
	return parent.isValid() ? 0 : static_cast<int>(m_albums.size());
}

int AlbumListModel::columnCount(const QModelIndex& parent) const {
	return parent.isValid() ? 0 : ColumnCount;
}

std::optional<ProvisionalAlbum> AlbumListModel::albumAt(int row) const {
	if (row < 0 || row >= static_cast<int>(m_albums.size())) return std::nullopt;
	return m_albums[static_cast<std::size_t>(row)];
}

QVariant AlbumListModel::data(const QModelIndex& index, int role) const {
	if (!index.isValid() || index.row() >= static_cast<int>(m_albums.size())) return {};
	const ProvisionalAlbum& album = m_albums[static_cast<std::size_t>(index.row())];

	if (role == Qt::ForegroundRole) {
		if (!album.flags.empty()) return QBrush(QColor(0xB7, 0x79, 0x1F));
		return {};
	}
	if (role == Qt::ToolTipRole) {
		QString tip;
		for (const auto& evidence : album.evidence) {
			tip += QStringLiteral("[%1] %2\n")
				.arg(evidence.supporting ? QChar('+') : QChar('-'))
				.arg(qs(evidence.detail));
		}
		return tip.trimmed();
	}
	if (role != Qt::DisplayRole) return {};

	switch (index.column()) {
		case ColumnAlbumArtist: return qs(album.albumArtist);
		case ColumnAlbum:
			return album.editionQualifier.empty()
				? qs(album.album)
				: QStringLiteral("%1  [%2]").arg(qs(album.album)).arg(qs(album.editionQualifier));
		case ColumnTracks:
			if (album.declaredTrackTotal && *album.declaredTrackTotal != album.observedTrackCount) {
				return QStringLiteral("%1 of %2").arg(album.observedTrackCount)
					.arg(*album.declaredTrackTotal);
			}
			return QString::number(album.observedTrackCount);
		case ColumnDate: return qs(album.date);
		case ColumnConfidence: return qs(std::string(toString(album.identityConfidence)));
		case ColumnFlags: {
			QStringList flags;
			for (auto flag : album.flags) flags << qs(std::string(toString(flag)));
			return flags.join(QStringLiteral(", "));
		}
		default: return {};
	}
}

QVariant AlbumListModel::headerData(int section, Qt::Orientation orientation, int role) const {
	if (role != Qt::DisplayRole || orientation != Qt::Horizontal) return {};
	switch (section) {
		case ColumnAlbumArtist: return QStringLiteral("Album artist");
		case ColumnAlbum: return QStringLiteral("Album");
		case ColumnTracks: return QStringLiteral("Tracks");
		case ColumnDate: return QStringLiteral("Date");
		case ColumnConfidence: return QStringLiteral("Identity");
		case ColumnFlags: return QStringLiteral("Needs attention");
		default: return {};
	}
}

} // namespace ml::desktop

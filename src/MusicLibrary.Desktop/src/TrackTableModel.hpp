// SPDX-License-Identifier: GPL-3.0-or-later
// FN-UI-02 / UI-001: the track table.
//
// A QAbstractTableModel over the catalogue with bounded fetching. It never
// creates a widget per track and never loads every row into memory: rows are
// fetched a page at a time and cached, so a 70,000-entry catalogue browses in
// constant memory.
#pragma once

#include "mlapp/Library.hpp"

#include <QAbstractTableModel>
#include <QHash>

#include <deque>
#include <optional>

namespace ml::desktop {

class TrackTableModel : public QAbstractTableModel {
	Q_OBJECT

public:
	enum Column {
		ColumnTrack = 0,
		ColumnTitle,
		ColumnArtist,
		ColumnAlbum,
		ColumnAlbumArtist,
		ColumnDuration,
		ColumnBitrate,
		ColumnArtwork,
		ColumnBpm,
		ColumnLyrics,
		ColumnGain,
		ColumnPrivacy,
		ColumnTagVersion,
		ColumnPath,
		ColumnCount,
	};

	explicit TrackTableModel(QObject* parent = nullptr);

	void setLibrary(Library* library);
	void setFilter(const TrackFilter& filter);
	const TrackFilter& filter() const { return m_filter; }

	/// Re-reads the row count and drops the cache.
	void refresh();

	int rowCount(const QModelIndex& parent = {}) const override;
	int columnCount(const QModelIndex& parent = {}) const override;
	QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
	QVariant headerData(int section, Qt::Orientation orientation,
		int role = Qt::DisplayRole) const override;

	/// The record behind a row, or nullopt when it is not yet loaded.
	std::optional<FileRecord> recordAt(int row) const;

private:
	/// Ensures the page containing `row` is cached. Const because the cache is
	/// an implementation detail of a logically const read.
	void ensureLoaded(int row) const;

	static constexpr int kPageSize = 256;
	/// Pages retained. Bounded so scrolling a huge catalogue cannot grow without
	/// limit; the least recently loaded page is evicted.
	static constexpr std::size_t kMaxCachedPages = 24;

	Library* m_library = nullptr;
	TrackFilter m_filter;
	int m_rowCount = 0;

	mutable QHash<int, std::vector<FileRecord>> m_pages;
	mutable std::deque<int> m_pageOrder;
};

/// The album list, with its review flags.
class AlbumListModel : public QAbstractTableModel {
	Q_OBJECT

public:
	enum Column {
		ColumnAlbumArtist = 0,
		ColumnAlbum,
		ColumnTracks,
		ColumnDate,
		ColumnConfidence,
		ColumnFlags,
		ColumnCount,
	};

	explicit AlbumListModel(QObject* parent = nullptr);

	void setLibrary(Library* library);
	void setOnlyNeedingReview(bool only);
	void refresh();

	int rowCount(const QModelIndex& parent = {}) const override;
	int columnCount(const QModelIndex& parent = {}) const override;
	QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
	QVariant headerData(int section, Qt::Orientation orientation,
		int role = Qt::DisplayRole) const override;

	std::optional<ProvisionalAlbum> albumAt(int row) const;

private:
	Library* m_library = nullptr;
	bool m_onlyNeedingReview = false;
	std::vector<ProvisionalAlbum> m_albums;
};

} // namespace ml::desktop

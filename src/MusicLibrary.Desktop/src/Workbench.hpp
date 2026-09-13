// SPDX-License-Identifier: GPL-3.0-or-later
// The Resonance workbench: the three-column layout from the supplied design.
//
//   Library Explorer (tree)  |  Deck + VFD spectrum + track list  |  Inspector
//
// Every panel is bound to real catalogue data. Where the design showed a
// playback or DSP feature this application does not have, the panel carries the
// equivalent Resonance function instead of a decorative placeholder; the panel
// headers say which.
#pragma once

#include "TrackTableModel.hpp"
#include "VfdSpectrumWidget.hpp"
#include "mlapp/Library.hpp"

#include <QFrame>
#include <QWidget>

#include <optional>

class QLabel;
class QLineEdit;
class QPushButton;
class QTableView;
class QTabWidget;
class QTextBrowser;
class QTreeWidget;
class QTreeWidgetItem;
class QCheckBox;

namespace ml::desktop {

/// A bento panel: titled frame with an accent top border.
class Panel : public QFrame {
	Q_OBJECT

public:
	Panel(const QString& title, const QString& accent, QWidget* parent = nullptr);

	/// The area callers put their content into.
	QWidget* body() const { return m_body; }

	void setSubtitle(const QString& subtitle);
	void addHeaderWidget(QWidget* widget);

private:
	QWidget* m_body = nullptr;
	QLabel* m_titleLabel = nullptr;
	QLabel* m_subtitleLabel = nullptr;
	QWidget* m_headerRow = nullptr;
};

/// The left column: a hierarchical navigator over roots, albums and filters.
class LibraryExplorer : public QWidget {
	Q_OBJECT

public:
	explicit LibraryExplorer(QWidget* parent = nullptr);

	void setLibrary(Library* library);
	void refresh();

signals:
	/// A filter node was chosen. The workbench applies it to the track table.
	void filterRequested(const TrackFilter& filter);
	/// An album node was chosen.
	void albumRequested(AlbumId album);

private slots:
	void onItemActivated(QTreeWidgetItem* item, int column);
	void onFilterTextChanged(const QString& text);

private:
	QTreeWidgetItem* addNode(QTreeWidgetItem* parent, const QString& label, const QString& count,
		const QColor& accent);

	Library* m_library = nullptr;
	QTreeWidget* m_tree = nullptr;
	QLineEdit* m_filterEdit = nullptr;
	QLabel* m_footnote = nullptr;
};

/// The right column: metadata, lyrics and the change-plan preview.
class InspectorPanel : public QWidget {
	Q_OBJECT

public:
	explicit InspectorPanel(QWidget* parent = nullptr);

	void setLibrary(Library* library);
	void showFile(FileId file);
	void clear();

signals:
	void statusMessage(const QString& message);

protected:
	void resizeEvent(QResizeEvent* event) override;

private:
	void populateChangePlan(const FilePlan& plan);
	void populateMetadata(const TagSnapshot& snapshot, const FileRecord& record);
	void populateLyrics(FileId file, const TagSnapshot& snapshot);
	void populateTrackHeader(const FileRecord& record);
	void loadCoverArt(const FileRecord& record);
	void rescaleCover();

	Library* m_library = nullptr;

	// Track identity, then the cover centred beneath it.
	QLabel* m_trackTitle = nullptr;
	QLabel* m_trackSubtitle = nullptr;
	QLabel* m_coverLabel = nullptr;
	QLabel* m_coverCaption = nullptr;
	QImage m_coverImage;

	QTabWidget* m_tabs = nullptr;
	QTextBrowser* m_changeBrowser = nullptr;
	QTextBrowser* m_metadataBrowser = nullptr;
	QTextBrowser* m_lyricsBrowser = nullptr;
	QLabel* m_footer = nullptr;
};

/// The centre column's top panel: the selected track, its cover and its state.
class DeckPanel : public QWidget {
	Q_OBJECT

public:
	explicit DeckPanel(QWidget* parent = nullptr);

	void setLibrary(Library* library);
	void showFile(const FileRecord& record);
	void clear();

	/// Drives the marquee and the position readout from the spectrum sweep.
	void setPosition(double fraction);

signals:
	void analyseRequested(FileId file);
	void sweepToggled(bool sweeping);

private:
	void updateMarquee();

	Library* m_library = nullptr;
	std::optional<FileRecord> m_record;

	QLabel* m_coverLabel = nullptr;
	QLabel* m_counterLabel = nullptr;
	QLabel* m_marqueeLabel = nullptr;
	QLabel* m_specBadges = nullptr;
	QLabel* m_stateBadges = nullptr;
	QPushButton* m_sweepButton = nullptr;
	QString m_marqueeText;
	int m_marqueeOffset = 0;
	double m_position = 0.0;
};

} // namespace ml::desktop

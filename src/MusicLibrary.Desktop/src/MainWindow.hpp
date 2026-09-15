// SPDX-License-Identifier: GPL-3.0-or-later
// FN-UI-01..04: the desktop shell.
//
// Five views over one engine: Library (source, output, coverage), Tracks,
// Albums, Review and Jobs/History. Every action calls the same Library methods
// the CLI calls, so the desktop cannot do anything the CLI cannot.
#pragma once

#include "AlbumReviewWidget.hpp"
#include "AudioPlayer.hpp"
#include "TaskRunner.hpp"
#include "Theme.hpp"
#include "VfdSpectrumWidget.hpp"
#include "Workbench.hpp"
#include "TrackTableModel.hpp"
#include "mlapp/Library.hpp"

#include <QMainWindow>
#include <QTimer>

#include <memory>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QTabWidget;
class QTableView;
class QTextBrowser;
class QPlainTextEdit;

namespace ml::desktop {

class MainWindow : public QMainWindow {
	Q_OBJECT

public:
	explicit MainWindow(QWidget* parent = nullptr);
	~MainWindow() override;

	/// Opens a library directly, bypassing the saved settings. Used by the
	/// command line and by the screenshot smoke test.
	void openLibraryAt(const QString& source, const QString& output, const QString& data,
		bool offline);

	/// Selects the first track that matches the current filter, so a freshly
	/// opened window shows real content rather than empty panels.
	void selectFirstTrack();

	/// Filters the track table to `text` and selects the first match.
	void selectTrackMatching(const QString& text);

	/// Analyses the currently selected track's spectrum.
	void analyseSelectedTrack();

	/// Starts playback of the selected track.
	void playSelectedTrack();

	/// True when a track row is currently selected.
	bool hasSelection() const;

protected:
	void closeEvent(QCloseEvent* event) override;

private slots:
	void onChooseSource();
	void onChooseOutput();
	void onChooseData();
	void onOpenLibrary();

	void onScan();
	void onResolveAlbums();
	void onAnalyseTempo();
	void onFetchArtwork();
	void onFetchLyrics();
	void onPlan();
	void onExport();
	void onVerify();
	void onResume();
	void onCreateSample();
	void onWriteNamingReport();

	void onTaskStarted(const QString& title);
	void onTaskProgress(qint64 done, qint64 total, const QString& message);
	void onTaskFinished(bool success, const QString& title);
	void onPauseToggled();
	void onCancel();

	void onFilterChanged();
	void onTrackActivated(const QModelIndex& index);
	void onTrackDoubleClicked(const QModelIndex& index);
	void onExplorerFilter(const TrackFilter& filter);
	void onAnalyseSpectrum(FileId file);
	void onPlayPause();
	void onStop();
	void onSeek(double fraction);
	void onPlayerPosition(qint64 positionMs, qint64 durationMs);
	void onPlayerState(ml::desktop::PlaybackState state);
	void onAlbumActivated(const QModelIndex& index);
	void onRefreshCoverage();

private:
	QWidget* buildWorkbenchTab();
	QWidget* buildLibraryTab();
	QWidget* buildAlbumsTab();
	QWidget* buildJobsTab();
	QWidget* buildHeaderStrip();
	void refreshHeaderTelemetry();

	void setBusy(bool busy);
	void refreshAll();
	void appendLog(const QString& message);
	bool requireOpenLibrary();
	TrackFilter currentFilter() const;
	/// Loads the given track into the player if it is not already the loaded
	/// one, decoding on demand. Shared by play/pause (which then toggles) and
	/// double-click (which then always plays). Returns false, having already
	/// reported why, if the track could not be made ready to play.
	bool loadForPlayback(const FileRecord& record);
	/// Advances to and plays the row after the one currently selected in the
	/// track table, if there is one. Called when a track finishes on its own.
	void playNextTrack();

	std::unique_ptr<Library> m_library;
	TaskRunner* m_runner = nullptr;
	AudioPlayer* m_player = nullptr;
	/// Drives the live analyser from the audio actually being played.
	QTimer* m_visualiserTimer = nullptr;
	/// The file currently loaded into the player, so selecting a row does not
	/// re-decode a track that is already playing.
	FileId m_loadedForPlayback;

	QTabWidget* m_tabs = nullptr;

	// Library tab
	QLineEdit* m_sourceEdit = nullptr;
	QLineEdit* m_outputEdit = nullptr;
	QLineEdit* m_dataEdit = nullptr;
	QCheckBox* m_offlineCheck = nullptr;
	QPushButton* m_openButton = nullptr;
	QTextBrowser* m_coverageBrowser = nullptr;
	QLabel* m_safetyLabel = nullptr;
	/// Mirrors m_progress/m_progressLabel (Jobs tab) so progress is visible
	/// right where a command was started, not only on a different tab.
	QProgressBar* m_libraryProgress = nullptr;
	QLabel* m_libraryProgressLabel = nullptr;

	// Header strip
	QLabel* m_headerTitle = nullptr;
	QLabel* m_headerSource = nullptr;
	QLabel* m_headerMode = nullptr;
	QLabel* m_headerCounts = nullptr;

	// Workbench
	LibraryExplorer* m_explorer = nullptr;
	DeckPanel* m_deck = nullptr;
	VfdSpectrumWidget* m_spectrum = nullptr;
	InspectorPanel* m_inspector = nullptr;
	QLabel* m_spectrumTelemetry = nullptr;

	// Tracks tab
	TrackTableModel* m_trackModel = nullptr;
	QTableView* m_trackTable = nullptr;
	QLineEdit* m_searchEdit = nullptr;
	QCheckBox* m_filterNoArtwork = nullptr;
	QCheckBox* m_filterNoLyrics = nullptr;
	QCheckBox* m_filterNoBpm = nullptr;
	QCheckBox* m_filterGain = nullptr;
	QCheckBox* m_filterPrivacy = nullptr;
	QLabel* m_trackCountLabel = nullptr;
	QTextBrowser* m_planPreview = nullptr;

	// Albums tab
	AlbumListModel* m_albumModel = nullptr;
	QTableView* m_albumTable = nullptr;
	QCheckBox* m_albumsNeedingReview = nullptr;
	AlbumReviewWidget* m_reviewWidget = nullptr;

	// Jobs tab
	QProgressBar* m_progress = nullptr;
	QLabel* m_progressLabel = nullptr;
	QPushButton* m_pauseButton = nullptr;
	QPushButton* m_cancelButton = nullptr;
	QPlainTextEdit* m_log = nullptr;

	// Actions disabled while a task runs.
	std::vector<QPushButton*> m_commandButtons;

	std::int64_t m_lastChangeSet = 0;
};

} // namespace ml::desktop

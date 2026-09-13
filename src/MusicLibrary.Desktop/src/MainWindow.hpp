// SPDX-License-Identifier: GPL-3.0-or-later
// FN-UI-01..04: the desktop shell.
//
// Five views over one engine: Library (source, output, coverage), Tracks,
// Albums, Review and Jobs/History. Every action calls the same Library methods
// the CLI calls, so the desktop cannot do anything the CLI cannot.
#pragma once

#include "AlbumReviewWidget.hpp"
#include "TaskRunner.hpp"
#include "TrackTableModel.hpp"
#include "mlapp/Library.hpp"

#include <QMainWindow>

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
	void onAlbumActivated(const QModelIndex& index);
	void onRefreshCoverage();

private:
	QWidget* buildLibraryTab();
	QWidget* buildTracksTab();
	QWidget* buildAlbumsTab();
	QWidget* buildJobsTab();

	void setBusy(bool busy);
	void refreshAll();
	void appendLog(const QString& message);
	bool requireOpenLibrary();
	TrackFilter currentFilter() const;

	std::unique_ptr<Library> m_library;
	TaskRunner* m_runner = nullptr;

	QTabWidget* m_tabs = nullptr;

	// Library tab
	QLineEdit* m_sourceEdit = nullptr;
	QLineEdit* m_outputEdit = nullptr;
	QLineEdit* m_dataEdit = nullptr;
	QCheckBox* m_offlineCheck = nullptr;
	QPushButton* m_openButton = nullptr;
	QTextBrowser* m_coverageBrowser = nullptr;
	QLabel* m_safetyLabel = nullptr;

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

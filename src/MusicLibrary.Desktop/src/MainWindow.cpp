// SPDX-License-Identifier: GPL-3.0-or-later
#include "MainWindow.hpp"
#include "mlcore/Text.hpp"

#include <mlversion/Version.hpp>

#include <QAction>
#include <QCheckBox>
#include <QCloseEvent>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableView>
#include <QTextBrowser>
#include <QTimer>
#include <QVBoxLayout>

namespace fs = std::filesystem;

namespace ml::desktop {

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

QString htmlRow(const QString& label, const QString& value) {
	return QStringLiteral("<tr><td>%1</td><td align=\"right\"><b>%2</b></td></tr>")
		.arg(label.toHtmlEscaped()).arg(value.toHtmlEscaped());
}

} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
	setWindowTitle(QStringLiteral("Resonance %1").arg(qs(std::string(version::kVersion))));
	resize(1500, 950);

	m_library = std::make_unique<Library>();
	m_runner = new TaskRunner(this);   // Qt parent ownership.

	connect(m_runner, &TaskRunner::started, this, &MainWindow::onTaskStarted);
	connect(m_runner, &TaskRunner::progressed, this, &MainWindow::onTaskProgress);
	connect(m_runner, &TaskRunner::finished, this, &MainWindow::onTaskFinished);

	m_tabs = new QTabWidget(this);
	m_tabs->addTab(buildLibraryTab(), QStringLiteral("Library"));
	m_tabs->addTab(buildTracksTab(), QStringLiteral("Tracks"));
	m_tabs->addTab(buildAlbumsTab(), QStringLiteral("Albums && review"));
	m_tabs->addTab(buildJobsTab(), QStringLiteral("Jobs && history"));
	setCentralWidget(m_tabs);

	// --- Menu ----------------------------------------------------------------
	auto* fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
	fileMenu->addAction(QStringLiteral("Open library…"), this, &MainWindow::onOpenLibrary);
	fileMenu->addAction(QStringLiteral("Write naming evidence report…"), this,
		&MainWindow::onWriteNamingReport);
	fileMenu->addSeparator();
	fileMenu->addAction(QStringLiteral("Quit"), this, &QWidget::close);

	auto* helpMenu = menuBar()->addMenu(QStringLiteral("&Help"));
	helpMenu->addAction(QStringLiteral("About"), this, [this]() {
		QMessageBox::about(this, QStringLiteral("About Resonance"),
			QStringLiteral(
				"<h3>Resonance %1</h3>"
				"<p>%2</p>"
				"<p>Your source collection is read-only to every operation in this "
				"application. Version 1 always writes copies to a separate output root; "
				"it never modifies the original files.</p>"
				"<p><a href=\"%3\">%3</a></p>")
			.arg(qs(std::string(version::kVersion)))
			.arg(qs(std::string(version::kDescription)))
			.arg(qs(std::string(version::kHomepage))));
	});

	statusBar()->showMessage(QStringLiteral("Choose a source folder and open the library."));

	// Restore the previous session's paths.
	QSettings settings(QStringLiteral("blueangelcpt"), QStringLiteral("Resonance"));
	m_sourceEdit->setText(settings.value(QStringLiteral("sourceRoot")).toString());
	m_outputEdit->setText(settings.value(QStringLiteral("outputRoot")).toString());
	m_dataEdit->setText(settings.value(QStringLiteral("dataDirectory")).toString());
	if (m_dataEdit->text().isEmpty()) {
		m_dataEdit->setText(qs(LibraryConfig::defaultDataDirectory().string()));
	}
}

MainWindow::~MainWindow() = default;

void MainWindow::closeEvent(QCloseEvent* event) {
	if (m_runner->isRunning()) {
		const auto answer = QMessageBox::question(this, QStringLiteral("Work in progress"),
			QStringLiteral("A task is still running. Cancel it and quit?\n\n"
				"Any file part-way through being written is discarded rather than published, "
				"and your source collection is unaffected."),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
		if (answer != QMessageBox::Yes) {
			event->ignore();
			return;
		}
		m_runner->cancel();
		m_library->requestCancel();
	}

	QSettings settings(QStringLiteral("blueangelcpt"), QStringLiteral("Resonance"));
	settings.setValue(QStringLiteral("sourceRoot"), m_sourceEdit->text());
	settings.setValue(QStringLiteral("outputRoot"), m_outputEdit->text());
	settings.setValue(QStringLiteral("dataDirectory"), m_dataEdit->text());

	event->accept();
}

// ---------------------------------------------------------------------------
// Library tab
// ---------------------------------------------------------------------------

QWidget* MainWindow::buildLibraryTab() {
	auto* page = new QWidget(this);
	auto* layout = new QVBoxLayout(page);

	// --- Paths ---------------------------------------------------------------
	auto* pathsGroup = new QGroupBox(QStringLiteral("Folders"), page);
	auto* pathsLayout = new QVBoxLayout(pathsGroup);

	const auto addRow = [&](const QString& label, QLineEdit*& edit, const QString& tip,
		void (MainWindow::*slot)()) {
		auto* row = new QHBoxLayout();
		auto* labelWidget = new QLabel(label, pathsGroup);
		labelWidget->setMinimumWidth(150);
		row->addWidget(labelWidget);
		edit = new QLineEdit(pathsGroup);
		edit->setToolTip(tip);
		row->addWidget(edit, 1);
		auto* browse = new QPushButton(QStringLiteral("Browse…"), pathsGroup);
		connect(browse, &QPushButton::clicked, this, slot);
		row->addWidget(browse);
		pathsLayout->addLayout(row);
	};

	addRow(QStringLiteral("Source (read-only)"), m_sourceEdit,
		QStringLiteral("Your music collection. This application never writes to it."),
		&MainWindow::onChooseSource);
	addRow(QStringLiteral("Output (copies)"), m_outputEdit,
		QStringLiteral("Where organised copies are written. Must be outside the source folder."),
		&MainWindow::onChooseOutput);
	addRow(QStringLiteral("Catalogue and cache"), m_dataEdit,
		QStringLiteral("Where the catalogue, artwork assets and logs live. "
			"Never inside the source folder."),
		&MainWindow::onChooseData);

	auto* optionRow = new QHBoxLayout();
	m_offlineCheck = new QCheckBox(QStringLiteral("Offline (no network lookups)"), pathsGroup);
	m_offlineCheck->setToolTip(QStringLiteral(
		"Catalogue browsing, naming review, local BPM analysis, local artwork processing "
		"and copied-file tagging all work offline. Only remote enrichment needs a connection."));
	optionRow->addWidget(m_offlineCheck);
	optionRow->addStretch(1);
	m_openButton = new QPushButton(QStringLiteral("Open library"), pathsGroup);
	m_openButton->setDefault(true);
	connect(m_openButton, &QPushButton::clicked, this, &MainWindow::onOpenLibrary);
	optionRow->addWidget(m_openButton);
	pathsLayout->addLayout(optionRow);

	layout->addWidget(pathsGroup);

	// --- Safety notice -------------------------------------------------------
	m_safetyLabel = new QLabel(page);
	m_safetyLabel->setWordWrap(true);
	m_safetyLabel->setTextFormat(Qt::RichText);
	m_safetyLabel->setText(QStringLiteral(
		"<div style='background:#1e3a2f; border:1px solid #2f7d5b; padding:8px;'>"
		"<b>Source protection.</b> The source folder is read-only to every operation. "
		"The writer refuses any destination that resolves inside it, including through a "
		"symlink or junction, and every output is written to a temporary file, verified, "
		"and only then published. Opening the library or scanning never starts a rewrite."
		"</div>"));
	layout->addWidget(m_safetyLabel);

	// --- Commands ------------------------------------------------------------
	auto* commandsGroup = new QGroupBox(QStringLiteral("Commands"), page);
	auto* commandsLayout = new QVBoxLayout(commandsGroup);

	const auto addButton = [&](QHBoxLayout* row, const QString& label, const QString& tip,
		void (MainWindow::*slot)()) {
		auto* button = new QPushButton(label, commandsGroup);
		button->setToolTip(tip);
		connect(button, &QPushButton::clicked, this, slot);
		row->addWidget(button);
		m_commandButtons.push_back(button);
		return button;
	};

	auto* readRow = new QHBoxLayout();
	readRow->addWidget(new QLabel(QStringLiteral("Read-only:"), commandsGroup));
	addButton(readRow, QStringLiteral("1. Scan"),
		QStringLiteral("Inventory the source folder. Writes nothing to your music."),
		&MainWindow::onScan);
	addButton(readRow, QStringLiteral("2. Group albums"),
		QStringLiteral("Group files into provisional albums and flag inconsistencies."),
		&MainWindow::onResolveAlbums);
	addButton(readRow, QStringLiteral("Create test sample"),
		QStringLiteral("Copy an independent sample elsewhere for testing."),
		&MainWindow::onCreateSample);
	readRow->addStretch(1);
	commandsLayout->addLayout(readRow);

	auto* analyseRow = new QHBoxLayout();
	analyseRow->addWidget(new QLabel(QStringLiteral("Analyse:"), commandsGroup));
	addButton(analyseRow, QStringLiteral("3. BPM (local)"),
		QStringLiteral("Analyse tempo locally. Decodes audio for analysis; the MP3 is not altered."),
		&MainWindow::onAnalyseTempo);
	addButton(analyseRow, QStringLiteral("4. Artwork"),
		QStringLiteral("Gather and rank artwork candidates. Works offline using local sources."),
		&MainWindow::onFetchArtwork);
	addButton(analyseRow, QStringLiteral("5. Lyrics"),
		QStringLiteral("Look up lyrics. Unavailable, instrumental and failed stay distinct."),
		&MainWindow::onFetchLyrics);
	analyseRow->addStretch(1);
	commandsLayout->addLayout(analyseRow);

	auto* writeRow = new QHBoxLayout();
	writeRow->addWidget(new QLabel(QStringLiteral("Plan and write:"), commandsGroup));
	addButton(writeRow, QStringLiteral("6. Build change plan"),
		QStringLiteral("Compute the exact changes. This cannot modify any file."),
		&MainWindow::onPlan);
	addButton(writeRow, QStringLiteral("7. Export copies"),
		QStringLiteral("Write organised copies to the output folder. Originals are untouched."),
		&MainWindow::onExport);
	addButton(writeRow, QStringLiteral("8. Verify"),
		QStringLiteral("Re-read the written files and check them against their plans."),
		&MainWindow::onVerify);
	addButton(writeRow, QStringLiteral("Resume after interruption"),
		QStringLiteral("Reconcile operations left incomplete by a crash or power loss."),
		&MainWindow::onResume);
	writeRow->addStretch(1);
	commandsLayout->addLayout(writeRow);

	layout->addWidget(commandsGroup);

	// --- Coverage ------------------------------------------------------------
	auto* coverageGroup = new QGroupBox(QStringLiteral("Coverage"), page);
	auto* coverageLayout = new QVBoxLayout(coverageGroup);
	m_coverageBrowser = new QTextBrowser(coverageGroup);
	coverageLayout->addWidget(m_coverageBrowser);
	auto* refreshButton = new QPushButton(QStringLiteral("Refresh"), coverageGroup);
	connect(refreshButton, &QPushButton::clicked, this, &MainWindow::onRefreshCoverage);
	coverageLayout->addWidget(refreshButton, 0, Qt::AlignLeft);
	layout->addWidget(coverageGroup, 1);

	return page;
}

// ---------------------------------------------------------------------------
// Tracks tab
// ---------------------------------------------------------------------------

QWidget* MainWindow::buildTracksTab() {
	auto* page = new QWidget(this);
	auto* layout = new QVBoxLayout(page);

	auto* filterRow = new QHBoxLayout();
	filterRow->addWidget(new QLabel(QStringLiteral("Search"), page));
	m_searchEdit = new QLineEdit(page);
	m_searchEdit->setPlaceholderText(QStringLiteral("title, artist, album or path"));
	m_searchEdit->setClearButtonEnabled(true);
	filterRow->addWidget(m_searchEdit, 1);

	m_filterNoArtwork = new QCheckBox(QStringLiteral("No artwork"), page);
	m_filterNoLyrics = new QCheckBox(QStringLiteral("No lyrics"), page);
	m_filterNoBpm = new QCheckBox(QStringLiteral("No BPM"), page);
	m_filterGain = new QCheckBox(QStringLiteral("Has gain fields"), page);
	m_filterPrivacy = new QCheckBox(QStringLiteral("Privacy findings"), page);

	for (QCheckBox* box : {m_filterNoArtwork, m_filterNoLyrics, m_filterNoBpm, m_filterGain,
			m_filterPrivacy}) {
		filterRow->addWidget(box);
		connect(box, &QCheckBox::toggled, this, &MainWindow::onFilterChanged);
	}
	layout->addLayout(filterRow);

	connect(m_searchEdit, &QLineEdit::textChanged, this, [this]() {
		// Debounce so typing does not re-query on every keystroke.
		static QTimer* timer = nullptr;
		if (!timer) {
			timer = new QTimer(this);
			timer->setSingleShot(true);
			timer->setInterval(250);
			connect(timer, &QTimer::timeout, this, &MainWindow::onFilterChanged);
		}
		timer->start();
	});

	auto* splitter = new QSplitter(Qt::Vertical, page);

	m_trackModel = new TrackTableModel(this);
	m_trackTable = new QTableView(splitter);
	m_trackTable->setModel(m_trackModel);
	m_trackTable->setSelectionBehavior(QAbstractItemView::SelectRows);
	m_trackTable->setSelectionMode(QAbstractItemView::SingleSelection);
	m_trackTable->setAlternatingRowColors(true);
	m_trackTable->setSortingEnabled(false);
	m_trackTable->verticalHeader()->setVisible(false);
	m_trackTable->verticalHeader()->setDefaultSectionSize(22);
	m_trackTable->horizontalHeader()->setStretchLastSection(true);
	connect(m_trackTable, &QTableView::clicked, this, &MainWindow::onTrackActivated);
	splitter->addWidget(m_trackTable);

	m_planPreview = new QTextBrowser(splitter);
	m_planPreview->setHtml(QStringLiteral(
		"<i>Select a track to see the exact changes that would be made to its copy.</i>"));
	splitter->addWidget(m_planPreview);
	splitter->setStretchFactor(0, 3);
	splitter->setStretchFactor(1, 2);

	layout->addWidget(splitter, 1);

	m_trackCountLabel = new QLabel(QStringLiteral("No library open."), page);
	layout->addWidget(m_trackCountLabel);

	return page;
}

// ---------------------------------------------------------------------------
// Albums tab
// ---------------------------------------------------------------------------

QWidget* MainWindow::buildAlbumsTab() {
	auto* page = new QWidget(this);
	auto* layout = new QVBoxLayout(page);

	auto* topRow = new QHBoxLayout();
	m_albumsNeedingReview = new QCheckBox(QStringLiteral("Only albums needing review"), page);
	connect(m_albumsNeedingReview, &QCheckBox::toggled, this, [this](bool checked) {
		m_albumModel->setOnlyNeedingReview(checked);
	});
	topRow->addWidget(m_albumsNeedingReview);
	topRow->addStretch(1);
	layout->addLayout(topRow);

	auto* splitter = new QSplitter(Qt::Vertical, page);

	m_albumModel = new AlbumListModel(this);
	m_albumTable = new QTableView(splitter);
	m_albumTable->setModel(m_albumModel);
	m_albumTable->setSelectionBehavior(QAbstractItemView::SelectRows);
	m_albumTable->setSelectionMode(QAbstractItemView::SingleSelection);
	m_albumTable->setAlternatingRowColors(true);
	m_albumTable->verticalHeader()->setVisible(false);
	m_albumTable->horizontalHeader()->setStretchLastSection(true);
	m_albumTable->setMaximumHeight(260);
	connect(m_albumTable, &QTableView::clicked, this, &MainWindow::onAlbumActivated);
	splitter->addWidget(m_albumTable);

	m_reviewWidget = new AlbumReviewWidget(splitter);
	connect(m_reviewWidget, &AlbumReviewWidget::statusMessage, this, [this](const QString& message) {
		statusBar()->showMessage(message, 8000);
		appendLog(message);
	});
	connect(m_reviewWidget, &AlbumReviewWidget::decisionRecorded, this, [this]() {
		m_albumModel->refresh();
	});
	splitter->addWidget(m_reviewWidget);
	splitter->setStretchFactor(0, 1);
	splitter->setStretchFactor(1, 4);

	layout->addWidget(splitter, 1);
	return page;
}

// ---------------------------------------------------------------------------
// Jobs tab
// ---------------------------------------------------------------------------

QWidget* MainWindow::buildJobsTab() {
	auto* page = new QWidget(this);
	auto* layout = new QVBoxLayout(page);

	auto* progressGroup = new QGroupBox(QStringLiteral("Current task"), page);
	auto* progressLayout = new QVBoxLayout(progressGroup);

	m_progressLabel = new QLabel(QStringLiteral("Idle."), progressGroup);
	progressLayout->addWidget(m_progressLabel);

	m_progress = new QProgressBar(progressGroup);
	m_progress->setRange(0, 100);
	m_progress->setValue(0);
	progressLayout->addWidget(m_progress);

	auto* controlRow = new QHBoxLayout();
	m_pauseButton = new QPushButton(QStringLiteral("Pause"), progressGroup);
	m_pauseButton->setEnabled(false);
	m_pauseButton->setToolTip(QStringLiteral(
		"Pause between files. A file part-way through being written is always finished "
		"or discarded, never left half-written."));
	connect(m_pauseButton, &QPushButton::clicked, this, &MainWindow::onPauseToggled);
	controlRow->addWidget(m_pauseButton);

	m_cancelButton = new QPushButton(QStringLiteral("Cancel"), progressGroup);
	m_cancelButton->setEnabled(false);
	connect(m_cancelButton, &QPushButton::clicked, this, &MainWindow::onCancel);
	controlRow->addWidget(m_cancelButton);
	controlRow->addStretch(1);
	progressLayout->addLayout(controlRow);

	layout->addWidget(progressGroup);

	auto* historyGroup = new QGroupBox(QStringLiteral("History"), page);
	auto* historyLayout = new QVBoxLayout(historyGroup);
	m_log = new QPlainTextEdit(historyGroup);
	m_log->setReadOnly(true);
	m_log->setMaximumBlockCount(5000);
	historyLayout->addWidget(m_log);
	layout->addWidget(historyGroup, 1);

	return page;
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

void MainWindow::onChooseSource() {
	const QString path = QFileDialog::getExistingDirectory(this,
		QStringLiteral("Choose your music folder (it will never be written to)"),
		m_sourceEdit->text());
	if (!path.isEmpty()) m_sourceEdit->setText(path);
}

void MainWindow::onChooseOutput() {
	const QString path = QFileDialog::getExistingDirectory(this,
		QStringLiteral("Choose where organised copies are written"), m_outputEdit->text());
	if (!path.isEmpty()) m_outputEdit->setText(path);
}

void MainWindow::onChooseData() {
	const QString path = QFileDialog::getExistingDirectory(this,
		QStringLiteral("Choose where the catalogue is kept"), m_dataEdit->text());
	if (!path.isEmpty()) m_dataEdit->setText(path);
}

void MainWindow::onOpenLibrary() {
	if (m_sourceEdit->text().isEmpty()) {
		QMessageBox::information(this, QStringLiteral("Source folder required"),
			QStringLiteral("Choose the folder holding your music first."));
		return;
	}

	LibraryConfig config;
	config.sourceRoots.emplace_back(m_sourceEdit->text().toStdString());
	if (!m_outputEdit->text().isEmpty()) {
		config.outputRoot = m_outputEdit->text().toStdString();
	}
	if (!m_dataEdit->text().isEmpty()) {
		config.dataDirectory = m_dataEdit->text().toStdString();
	}
	config.offline = m_offlineCheck->isChecked();

	m_library = std::make_unique<Library>();
	auto status = m_library->open(std::move(config));
	if (!status) {
		QMessageBox::critical(this, QStringLiteral("Could not open the library"),
			qs(status.error().describe()));
		appendLog(QStringLiteral("Open failed: %1").arg(qs(status.error().describe())));
		return;
	}

	m_trackModel->setLibrary(m_library.get());
	m_albumModel->setLibrary(m_library.get());
	m_reviewWidget->setLibrary(m_library.get());

	appendLog(QStringLiteral("Library opened. Source: %1").arg(m_sourceEdit->text()));
	statusBar()->showMessage(QStringLiteral("Library open. Run Scan to build the catalogue."), 8000);
	refreshAll();
}

bool MainWindow::requireOpenLibrary() {
	if (m_library && m_library->isOpen()) return true;
	QMessageBox::information(this, QStringLiteral("No library open"),
		QStringLiteral("Open a library first: choose a source folder and press Open library."));
	return false;
}

void MainWindow::onScan() {
	if (!requireOpenLibrary()) return;
	m_library->clearCancel();

	Library* library = m_library.get();
	m_runner->start(QStringLiteral("Scanning"), [library](const ProgressCallback& progress) {
		auto result = library->scan(progress);
		return result.ok();
	});
}

void MainWindow::onResolveAlbums() {
	if (!requireOpenLibrary()) return;
	m_library->clearCancel();

	Library* library = m_library.get();
	m_runner->start(QStringLiteral("Grouping albums"), [library](const ProgressCallback& progress) {
		return library->resolveAlbums(progress).ok();
	});
}

void MainWindow::onAnalyseTempo() {
	if (!requireOpenLibrary()) return;
	m_library->clearCancel();

	Library* library = m_library.get();
	m_runner->start(QStringLiteral("Analysing tempo"), [library](const ProgressCallback& progress) {
		return library->analyseTempo(progress).ok();
	});
}

void MainWindow::onFetchArtwork() {
	if (!requireOpenLibrary()) return;
	m_library->clearCancel();

	Library* library = m_library.get();
	m_runner->start(QStringLiteral("Gathering artwork"), [library](const ProgressCallback& progress) {
		return library->fetchArtwork(progress).ok();
	});
}

void MainWindow::onFetchLyrics() {
	if (!requireOpenLibrary()) return;
	m_library->clearCancel();

	Library* library = m_library.get();
	m_runner->start(QStringLiteral("Looking up lyrics"), [library](const ProgressCallback& progress) {
		return library->fetchLyrics(progress).ok();
	});
}

void MainWindow::onPlan() {
	if (!requireOpenLibrary()) return;
	if (m_library->config().outputRoot.empty()) {
		QMessageBox::information(this, QStringLiteral("Output folder required"),
			QStringLiteral("Choose an output folder and reopen the library. A plan needs a "
				"destination before it can be produced."));
		return;
	}
	m_library->clearCancel();

	Library* library = m_library.get();
	auto* self = this;
	m_runner->start(QStringLiteral("Building the change plan"),
		[library, self](const ProgressCallback& progress) {
			auto result = library->plan(progress);
			if (!result) return false;

			const auto summary = result.value().summarise();
			const auto id = result.value().id.value;
			QMetaObject::invokeMethod(self, [self, summary, id]() {
				self->m_lastChangeSet = id;
				self->appendLog(QStringLiteral(
					"Change set %1: %2 files planned, %3 writable, %4 blocked, %5 need review. "
					"Savings %6, enrichment growth %7.")
					.arg(id)
					.arg(summary.total).arg(summary.writable).arg(summary.blocked)
					.arg(summary.needingReview)
					.arg(qs(text::formatBytes(summary.optimisationSavings)))
					.arg(qs(text::formatBytes(summary.enrichmentGrowth))));
			}, Qt::QueuedConnection);
			return true;
		});
}

void MainWindow::onExport() {
	if (!requireOpenLibrary()) return;

	std::int64_t setId = m_lastChangeSet;
	if (setId == 0) {
		auto latest = m_library->catalogue().latestChangeSet();
		if (latest && latest.value()) setId = latest.value()->value;
	}
	if (setId == 0) {
		QMessageBox::information(this, QStringLiteral("No change plan"),
			QStringLiteral("Build a change plan first."));
		return;
	}

	const auto answer = QMessageBox::question(this, QStringLiteral("Export copies"),
		QStringLiteral("Write organised copies into:\n\n%1\n\n"
			"Your source collection is not modified. Continue?")
			.arg(qs(m_library->config().outputRoot.string())),
		QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
	if (answer != QMessageBox::Yes) return;

	m_library->clearCancel();
	Library* library = m_library.get();
	auto* self = this;
	m_runner->start(QStringLiteral("Exporting copies"),
		[library, self, setId](const ProgressCallback& progress) {
			auto result = library->exportCopies(ChangeSetId(setId), progress);
			if (!result) return false;

			const ExportResult e = result.value();
			QMetaObject::invokeMethod(self, [self, e]() {
				self->appendLog(QStringLiteral(
					"Export: %1 written, %2 skipped, %3 failed, %4 written to disk.")
					.arg(e.written).arg(e.skipped).arg(e.failed)
					.arg(qs(text::formatBytes(e.bytesWritten))));
				for (const auto& failure : e.failures) {
					self->appendLog(QStringLiteral("  %1").arg(qs(failure)));
				}
			}, Qt::QueuedConnection);
			return e.failed == 0;
		});
}

void MainWindow::onVerify() {
	if (!requireOpenLibrary()) return;

	std::int64_t setId = m_lastChangeSet;
	if (setId == 0) {
		auto latest = m_library->catalogue().latestChangeSet();
		if (latest && latest.value()) setId = latest.value()->value;
	}
	if (setId == 0) return;

	m_library->clearCancel();
	Library* library = m_library.get();
	auto* self = this;
	m_runner->start(QStringLiteral("Verifying written files"),
		[library, self, setId](const ProgressCallback& progress) {
			auto result = library->verify(ChangeSetId(setId), progress);
			if (!result) return false;
			const auto count = result.value();
			QMetaObject::invokeMethod(self, [self, count]() {
				self->appendLog(QStringLiteral(
					"Verified %1 written files: content hash, MPEG payload hash and the source's "
					"own audio hash all agree.").arg(count));
			}, Qt::QueuedConnection);
			return true;
		});
}

void MainWindow::onResume() {
	if (!requireOpenLibrary()) return;

	std::int64_t setId = m_lastChangeSet;
	if (setId == 0) {
		auto latest = m_library->catalogue().latestChangeSet();
		if (latest && latest.value()) setId = latest.value()->value;
	}
	if (setId == 0) return;

	m_library->clearCancel();
	Library* library = m_library.get();
	m_runner->start(QStringLiteral("Reconciling interrupted operations"),
		[library, setId](const ProgressCallback& progress) {
			return library->recover(ChangeSetId(setId), progress).ok();
		});
}

void MainWindow::onCreateSample() {
	if (!requireOpenLibrary()) return;

	const QString path = QFileDialog::getExistingDirectory(this,
		QStringLiteral("Choose a folder for the independent test sample"));
	if (path.isEmpty()) return;

	bool ok = false;
	const int albums = QInputDialog::getInt(this, QStringLiteral("Sample size"),
		QStringLiteral("How many albums?"), 20, 1, 1000, 1, &ok);
	if (!ok) return;

	m_library->clearCancel();
	Library* library = m_library.get();
	const fs::path destination = path.toStdString();
	auto* self = this;
	m_runner->start(QStringLiteral("Creating the test sample"),
		[library, destination, albums, self](const ProgressCallback& progress) {
			auto result = library->createSample(destination, albums, 10, progress);
			if (!result) {
				const QString message = qs(result.error().describe());
				QMetaObject::invokeMethod(self, [self, message]() {
					self->appendLog(QStringLiteral("Sample failed: %1").arg(message));
				}, Qt::QueuedConnection);
				return false;
			}
			const auto copied = result.value();
			QMetaObject::invokeMethod(self, [self, copied]() {
				self->appendLog(QStringLiteral(
					"Copied %1 files. Every copy was verified by hash against its source.")
					.arg(copied));
			}, Qt::QueuedConnection);
			return true;
		});
}

void MainWindow::onWriteNamingReport() {
	if (!requireOpenLibrary()) return;

	const QString path = QFileDialog::getSaveFileName(this,
		QStringLiteral("Write naming evidence report"),
		QStringLiteral("naming-convention.md"), QStringLiteral("Markdown (*.md)"));
	if (path.isEmpty()) return;

	auto status = m_library->writeNamingConventionReport(fs::path(path.toStdString()));
	if (!status) {
		QMessageBox::warning(this, QStringLiteral("Could not write the report"),
			qs(status.error().describe()));
		return;
	}
	appendLog(QStringLiteral("Naming evidence written to %1").arg(path));
	statusBar()->showMessage(QStringLiteral("Report written."), 6000);
}

// ---------------------------------------------------------------------------
// Task plumbing
// ---------------------------------------------------------------------------

void MainWindow::onTaskStarted(const QString& title) {
	setBusy(true);
	m_progressLabel->setText(title + QStringLiteral("…"));
	m_progress->setValue(0);
	appendLog(QStringLiteral("Started: %1").arg(title));
	statusBar()->showMessage(title + QStringLiteral("…"));
}

void MainWindow::onTaskProgress(qint64 done, qint64 total, const QString& message) {
	if (total > 0) {
		m_progress->setRange(0, 100);
		m_progress->setValue(static_cast<int>((done * 100) / total));
		m_progressLabel->setText(QStringLiteral("%1  —  %2 of %3")
			.arg(message).arg(done).arg(total));
	} else {
		m_progress->setRange(0, 0);   // Busy indicator when the total is unknown.
		m_progressLabel->setText(message);
	}
}

void MainWindow::onTaskFinished(bool success, const QString& title) {
	setBusy(false);
	m_progress->setRange(0, 100);
	m_progress->setValue(success ? 100 : 0);
	m_progressLabel->setText(success
		? title + QStringLiteral(" — finished")
		: title + QStringLiteral(" — did not complete"));
	appendLog(QStringLiteral("%1: %2").arg(title).arg(success
		? QStringLiteral("finished") : QStringLiteral("did not complete")));
	statusBar()->showMessage(m_progressLabel->text(), 8000);
	refreshAll();
}

void MainWindow::onPauseToggled() {
	const bool paused = !m_runner->isPaused();
	m_runner->setPaused(paused);
	m_pauseButton->setText(paused ? QStringLiteral("Resume") : QStringLiteral("Pause"));
	appendLog(paused ? QStringLiteral("Paused.") : QStringLiteral("Resumed."));
}

void MainWindow::onCancel() {
	m_runner->cancel();
	if (m_library) m_library->requestCancel();
	appendLog(QStringLiteral("Cancellation requested; stopping at the next safe point."));
}

void MainWindow::setBusy(bool busy) {
	for (QPushButton* button : m_commandButtons) button->setEnabled(!busy);
	m_openButton->setEnabled(!busy);
	m_pauseButton->setEnabled(busy);
	m_cancelButton->setEnabled(busy);
	if (!busy) m_pauseButton->setText(QStringLiteral("Pause"));
}

// ---------------------------------------------------------------------------
// Views
// ---------------------------------------------------------------------------

TrackFilter MainWindow::currentFilter() const {
	TrackFilter filter;
	filter.searchText = m_searchEdit->text().toStdString();
	if (m_filterNoArtwork->isChecked()) filter.missingArtwork = true;
	if (m_filterNoLyrics->isChecked()) filter.missingLyrics = true;
	if (m_filterNoBpm->isChecked()) filter.missingBpm = true;
	if (m_filterGain->isChecked()) filter.hasGainFields = true;
	if (m_filterPrivacy->isChecked()) filter.hasPrivacyFindings = true;
	return filter;
}

void MainWindow::onFilterChanged() {
	m_trackModel->setFilter(currentFilter());
	m_trackCountLabel->setText(QStringLiteral("%1 tracks match.")
		.arg(m_trackModel->rowCount()));
}

void MainWindow::onTrackActivated(const QModelIndex& index) {
	if (!index.isValid() || !m_library || !m_library->isOpen()) return;

	const auto record = m_trackModel->recordAt(index.row());
	if (!record) return;

	auto plan = m_library->previewFile(record->id);
	if (!plan) {
		m_planPreview->setHtml(QStringLiteral("<i>Could not preview: %1</i>")
			.arg(qs(plan.error().describe()).toHtmlEscaped()));
		return;
	}

	const FilePlan& p = plan.value();
	QString html;
	html += QStringLiteral("<h3>%1</h3>").arg(qs(record->relativePath).toHtmlEscaped());
	html += QStringLiteral("<p><b>Destination</b>: %1</p>")
		.arg(p.destinationRelativePath.empty()
			? QStringLiteral("<i>review required — no destination is proposed</i>")
			: qs(p.destinationRelativePath).toHtmlEscaped());
	html += QStringLiteral("<p><b>Tag version written</b>: %1 "
		"<i>(the source's own version is preserved)</i></p>")
		.arg(qs(std::string(toString(p.outputContainer))));

	if (!p.naming.exceptions.empty()) {
		html += QStringLiteral("<p><b>Naming exceptions</b></p><ul>");
		for (const auto& exception : p.naming.exceptions) {
			html += QStringLiteral("<li>[%1] <b>%2</b>: %3</li>")
				.arg(exception.advisory ? QStringLiteral("advisory") : QStringLiteral("review"))
				.arg(qs(std::string(toString(exception.kind))))
				.arg(qs(exception.detail).toHtmlEscaped());
		}
		html += QStringLiteral("</ul>");
	}

	const auto changing = p.changingDecisions();
	html += QStringLiteral("<p><b>Tag changes (%1)</b></p>").arg(changing.size());
	if (changing.empty()) {
		html += QStringLiteral("<p><i>None. Every existing field is preserved.</i></p>");
	} else {
		html += QStringLiteral("<table border='1' cellpadding='4' cellspacing='0' width='100%'>"
			"<tr><th>Action</th><th>Frame</th><th>Rule</th><th>Why</th></tr>");
		for (const auto* decision : changing) {
			html += QStringLiteral("<tr><td>%1</td><td>%2%3</td><td><code>%4</code></td><td>%5</td></tr>")
				.arg(qs(std::string(toString(decision->action))))
				.arg(qs(decision->frameId).toHtmlEscaped())
				.arg(decision->description.empty() ? QString()
					: QStringLiteral(" [%1]").arg(qs(decision->description).toHtmlEscaped()))
				.arg(qs(decision->ruleId).toHtmlEscaped())
				.arg(qs(decision->reason).toHtmlEscaped());
		}
		html += QStringLiteral("</table>");
	}

	html += QStringLiteral("<p><b>Artwork</b>: %1<br>%2</p>")
		.arg(qs(std::string(toString(p.artwork.outcome))))
		.arg(qs(p.artwork.selectionReason).toHtmlEscaped());
	html += QStringLiteral("<p><b>BPM</b>: %1 — %2</p>")
		.arg(qs(std::string(toString(p.tempo.kind))))
		.arg(qs(p.tempo.reason).toHtmlEscaped());
	html += QStringLiteral("<p><b>Lyrics</b>: %1 — %2</p>")
		.arg(qs(std::string(toString(p.lyrics.state))))
		.arg(qs(p.lyrics.reason).toHtmlEscaped());

	html += QStringLiteral("<p><b>Size effect</b>: optimisation savings %1, enrichment growth %2, "
		"padding change %3</p>")
		.arg(qs(text::formatBytes(p.size.optimisationSavings)))
		.arg(qs(text::formatBytes(p.size.enrichmentGrowth)))
		.arg(qs(text::formatBytes(p.size.paddingDelta)));

	if (!p.blockers.empty()) {
		html += QStringLiteral("<p><b>Blocked</b></p><ul>");
		for (auto blocker : p.blockers) {
			html += QStringLiteral("<li>%1</li>").arg(qs(std::string(toString(blocker))));
		}
		html += QStringLiteral("</ul>");
	}
	for (const auto& note : p.notes) {
		html += QStringLiteral("<p><i>%1</i></p>").arg(qs(note).toHtmlEscaped());
	}

	m_planPreview->setHtml(html);
}

void MainWindow::onAlbumActivated(const QModelIndex& index) {
	if (!index.isValid()) return;
	const auto album = m_albumModel->albumAt(index.row());
	if (!album) return;
	m_reviewWidget->showAlbum(album->id);
}

void MainWindow::onRefreshCoverage() {
	if (!m_library || !m_library->isOpen()) {
		m_coverageBrowser->setHtml(QStringLiteral("<i>No library open.</i>"));
		return;
	}

	auto coverage = m_library->coverage();
	if (!coverage) {
		m_coverageBrowser->setHtml(QStringLiteral("<i>%1</i>")
			.arg(qs(coverage.error().describe())));
		return;
	}
	const CoverageReport& c = coverage.value();

	QString html = QStringLiteral("<table width='100%'>");
	html += htmlRow(QStringLiteral("Files"), QString::number(c.totalFiles));
	html += htmlRow(QStringLiteral("Readable"), QString::number(c.readable));
	html += htmlRow(QStringLiteral("Unreadable"), QString::number(c.unreadable));
	html += htmlRow(QStringLiteral("Total size"), qs(text::formatBytes(c.totalBytes)));
	html += htmlRow(QStringLiteral("Total duration"), qs(text::formatDuration(c.totalDurationMs)));
	html += htmlRow(QStringLiteral("Albums"), QString::number(c.albumCount));
	html += htmlRow(QStringLiteral("Albums needing review"), QString::number(c.albumsNeedingReview));
	html += htmlRow(QStringLiteral("With artwork"), QString::number(c.withArtwork));
	html += htmlRow(QStringLiteral("With lyrics"), QString::number(c.withLyrics));
	html += htmlRow(QStringLiteral("With BPM"), QString::number(c.withBpm));
	html += htmlRow(QStringLiteral("With gain fields"), QString::number(c.withGainFields));
	html += htmlRow(QStringLiteral("With privacy findings"), QString::number(c.withPrivacyFindings));
	html += htmlRow(QStringLiteral("Duplicate audio groups"),
		QString::number(c.likelyDuplicateGroups));
	html += QStringLiteral("</table>");

	const auto section = [&](const QString& title,
		const std::vector<std::pair<std::string, std::int64_t>>& rows) {
		if (rows.empty()) return;
		html += QStringLiteral("<p><b>%1</b></p><table width='100%'>").arg(title);
		for (const auto& [label, count] : rows) {
			html += htmlRow(qs(label), QString::number(count));
		}
		html += QStringLiteral("</table>");
	};
	section(QStringLiteral("Tag versions"), c.tagVersionDistribution);
	section(QStringLiteral("Artwork sizes"), c.artworkSizeDistribution);
	section(QStringLiteral("Bitrate modes"), c.bitrateModeDistribution);
	section(QStringLiteral("Missing fields"), c.missingFieldCounts);
	section(QStringLiteral("Read errors"), c.readErrorCounts);

	auto naming = m_library->namingReport();
	if (naming) {
		const NamingConformity& n = naming.value();
		html += QStringLiteral("<p><b>Naming conformity (measured)</b></p>");
		html += QStringLiteral("<p><code>%1</code></p>").arg(qs(std::string(NamingTemplate::pattern())));
		html += QStringLiteral("<table width='100%'>");
		html += htmlRow(QStringLiteral("Files examined"), QString::number(n.totalFiles));
		html += htmlRow(QStringLiteral("Matching the template"),
			QStringLiteral("%1  (%2%)").arg(n.matchingTemplate)
				.arg(static_cast<int>(n.matchPercentage())));
		html += htmlRow(QStringLiteral("Would need review"), QString::number(n.wouldNeedReview));
		html += QStringLiteral("</table>");
		if (!n.exceptionCounts.empty()) {
			html += QStringLiteral("<table width='100%'>");
			for (const auto& [kind, count] : n.exceptionCounts) {
				html += htmlRow(qs(kind), QString::number(count));
			}
			html += QStringLiteral("</table>");
		}
	}

	m_coverageBrowser->setHtml(html);
}

void MainWindow::refreshAll() {
	m_trackModel->refresh();
	m_albumModel->refresh();
	onRefreshCoverage();
	m_trackCountLabel->setText(QStringLiteral("%1 tracks match.").arg(m_trackModel->rowCount()));
}

void MainWindow::appendLog(const QString& message) {
	m_log->appendPlainText(QStringLiteral("[%1] %2")
		.arg(qs(nowIso8601())).arg(message));
}

} // namespace ml::desktop

// SPDX-License-Identifier: GPL-3.0-or-later
#include "MainWindow.hpp"
#include "Theme.hpp"
#include "mlcore/Text.hpp"

#include <mlversion/Version.hpp>

#include <QAction>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDir>
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
#include <QApplication>
#include <QShortcut>
#include <QSignalBlocker>
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
	m_player = new AudioPlayer(this);

	connect(m_player, &AudioPlayer::positionChanged, this, &MainWindow::onPlayerPosition);
	connect(m_player, &AudioPlayer::stateChanged, this, &MainWindow::onPlayerState);
	connect(m_player, &AudioPlayer::errorOccurred, this, [this](const QString& message) {
		appendLog(message);
		statusBar()->showMessage(message, 8000);
	});
	// The live analyser reads the samples being handed to the output device and
	// runs its own FFT, so the display is the audio you are hearing rather than
	// a replay of an earlier analysis.
	m_visualiserTimer = new QTimer(this);
	// The reference analyser (Winamp Classic) redraws once per audio buffer it
	// receives — 576 samples at typical rates, roughly every 13 ms, ~75 Hz.
	// 30 Hz visibly lags a drum hit behind the beat; matching its cadence more
	// closely is what makes a kick or a vocal's pitch change read instantly
	// instead of a frame or two late.
	m_visualiserTimer->setInterval(15);   // ~66 Hz
	connect(m_visualiserTimer, &QTimer::timeout, this, [this]() {
		if (!m_spectrum || m_player->state() != PlaybackState::Playing) return;
		const std::vector<float> block = m_player->visualiserBlock(2048);
		m_spectrum->pushLiveSamples(block, m_player->outputSampleRate());

		// Rate diagnostics — see m_visualiserTickCount's comment.
		++m_visualiserTickCount;
		const float lastSample = block.empty() ? 0.0f : block.back();
		if (!m_haveLastVisualiserSample || lastSample != m_lastVisualiserSample) {
			++m_visualiserFreshCount;
		}
		m_lastVisualiserSample = lastSample;
		m_haveLastVisualiserSample = true;
		if (!m_visualiserRateClock.isValid()) m_visualiserRateClock.start();
		if (m_visualiserRateClock.elapsed() >= 1000 && m_spectrumTelemetry) {
			m_spectrumTelemetry->setText(QStringLiteral(
				"LIVE · %1 Hz timer, %2 Hz fresh audio · 576 samp · log bands (width-matched) "
				"· 30 Hz–16 kHz · peak hold · out %3")
				.arg(m_visualiserTickCount).arg(m_visualiserFreshCount)
				.arg(m_player->formatDescription()));
			m_visualiserTickCount = 0;
			m_visualiserFreshCount = 0;
			m_visualiserRateClock.restart();
		}
	});

	connect(m_player, &AudioPlayer::outputDeviceChanged, this,
		[this](bool available, const QString& name) {
			if (m_deck) m_deck->setPlaybackAvailable(available, name);
			appendLog(available
				? QStringLiteral("Audio output available: %1").arg(name)
				: QStringLiteral("Audio output went away; library functions are unaffected."));
		});
	connect(m_player, &AudioPlayer::trackFinished, this, [this]() {
		appendLog(QStringLiteral("Playback reached the end of the track."));
		playNextTrack();
	});

	connect(m_runner, &TaskRunner::started, this, &MainWindow::onTaskStarted);
	connect(m_runner, &TaskRunner::progressed, this, &MainWindow::onTaskProgress);
	connect(m_runner, &TaskRunner::finished, this, &MainWindow::onTaskFinished);

	// The workbench is the primary view; the remaining tabs are the same engine
	// seen from a different angle.
	m_tabs = new QTabWidget(this);
	m_tabs->addTab(buildWorkbenchTab(), QStringLiteral("Workbench"));
	m_tabs->addTab(buildAlbumsTab(), QStringLiteral("Artwork review"));
	m_tabs->addTab(buildLibraryTab(), QStringLiteral("Library && commands"));
	m_tabs->addTab(buildJobsTab(), QStringLiteral("Jobs && history"));

	auto* shell = new QWidget(this);
	auto* shellLayout = new QVBoxLayout(shell);
	shellLayout->setContentsMargins(0, 0, 0, 0);
	shellLayout->setSpacing(0);
	shellLayout->addWidget(buildHeaderStrip());
	shellLayout->addWidget(m_tabs, 1);
	setCentralWidget(shell);

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
		m_dataEdit->setText(qs(text::pathToUtf8(LibraryConfig::defaultDataDirectory())));
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
// Header strip (the design's top diagnostic bar)
// ---------------------------------------------------------------------------

QWidget* MainWindow::buildHeaderStrip() {
	auto* strip = new QFrame(this);
	strip->setProperty("mlStrip", true);
	strip->setFixedHeight(30);

	auto* layout = new QHBoxLayout(strip);
	layout->setContentsMargins(12, 0, 12, 0);
	layout->setSpacing(14);

	m_headerTitle = new QLabel(QStringLiteral("RESONANCE  v%1")
		.arg(qs(std::string(version::kVersion))), strip);
	m_headerTitle->setFont(theme::monoFont(9, QFont::Bold));
	m_headerTitle->setStyleSheet(QStringLiteral("color:%1;").arg(theme::hex(theme::kNeonCyan)));
	layout->addWidget(m_headerTitle);

	auto* subtitle = new QLabel(QStringLiteral("— MP3 library catalogue and organiser"), strip);
	subtitle->setFont(theme::monoFont(8));
	subtitle->setProperty("mlMuted", true);
	layout->addWidget(subtitle);

	layout->addStretch(1);

	// Source protection is the single most important fact about this
	// application, so it is stated permanently rather than buried in a dialog.
	m_headerMode = new QLabel(QStringLiteral("SOURCE READ-ONLY"), strip);
	m_headerMode->setFont(theme::monoFont(8, QFont::Bold));
	m_headerMode->setStyleSheet(QStringLiteral(
		"color:%1; border:1px solid %1; border-radius:3px; padding:1px 8px;")
		.arg(theme::hex(theme::kNeonCyan)));
	layout->addWidget(m_headerMode);

	m_headerSource = new QLabel(QStringLiteral("no library open"), strip);
	m_headerSource->setFont(theme::monoFont(8));
	m_headerSource->setProperty("mlSecondary", true);
	layout->addWidget(m_headerSource);

	m_headerCounts = new QLabel(strip);
	m_headerCounts->setFont(theme::monoFont(8));
	m_headerCounts->setTextFormat(Qt::RichText);
	layout->addWidget(m_headerCounts);

	return strip;
}

void MainWindow::refreshHeaderTelemetry() {
	if (!m_library || !m_library->isOpen()) {
		m_headerSource->setText(QStringLiteral("no library open"));
		m_headerCounts->clear();
		return;
	}

	const auto roots = m_library->guard().protectedRoots();
	m_headerSource->setText(roots.empty()
		? QStringLiteral("no source root")
		: qs(text::pathToUtf8(roots.front().resolvedPath)));

	auto coverage = m_library->coverage();
	if (!coverage) return;
	const CoverageReport& c = coverage.value();

	const auto stat = [](const QString& label, qint64 value, const QColor& colour) {
		return QStringLiteral("<span style='color:%1'>%2</span> "
			"<b style='color:%3'>%4</b>")
			.arg(theme::hex(theme::kTextMuted)).arg(label)
			.arg(theme::hex(colour)).arg(value);
	};

	m_headerCounts->setText(QStringList{
		stat(QStringLiteral("FILES"), c.totalFiles, theme::kNeonCyan),
		stat(QStringLiteral("ALBUMS"), c.albumCount, theme::kNeonCyan),
		stat(QStringLiteral("REVIEW"), c.albumsNeedingReview,
			c.albumsNeedingReview > 0 ? theme::kNeonPink : theme::kTextSecondary),
		stat(QStringLiteral("UNREADABLE"), c.unreadable,
			c.unreadable > 0 ? theme::kOverloadRed : theme::kTextSecondary),
	}.join(QStringLiteral(" &nbsp;·&nbsp; ")));
}

// ---------------------------------------------------------------------------
// Workbench tab: the design's three-column layout
// ---------------------------------------------------------------------------

QWidget* MainWindow::buildWorkbenchTab() {
	auto* page = new QWidget(this);
	auto* layout = new QHBoxLayout(page);
	layout->setContentsMargins(6, 6, 6, 6);
	layout->setSpacing(6);

	auto* columns = new QSplitter(Qt::Horizontal, page);

	// --- Left: library explorer ------------------------------------------
	auto* explorerPanel = new Panel(QStringLiteral("Library Explorer"),
		QStringLiteral("cyan"), columns);
	m_explorer = new LibraryExplorer(explorerPanel->body());
	{
		auto* inner = new QVBoxLayout(explorerPanel->body());
		inner->setContentsMargins(0, 0, 0, 0);
		inner->addWidget(m_explorer);
	}
	connect(m_explorer, &LibraryExplorer::filterRequested, this, &MainWindow::onExplorerFilter);
	connect(m_explorer, &LibraryExplorer::albumRequested, this, [this](AlbumId album) {
		m_reviewWidget->showAlbum(album);
	});
	columns->addWidget(explorerPanel);

	// --- Centre: deck, spectrum, track list -------------------------------
	auto* centre = new QSplitter(Qt::Vertical, columns);

	auto* deckPanel = new Panel(QStringLiteral("Selected Track"), QStringLiteral("cyan"), centre);
	deckPanel->setSubtitle(QStringLiteral("measured from the stream, not from tags"));
	m_deck = new DeckPanel(deckPanel->body());
	{
		auto* inner = new QVBoxLayout(deckPanel->body());
		inner->setContentsMargins(0, 0, 0, 0);
		inner->addWidget(m_deck);
	}
	deckPanel->setMaximumHeight(140);
	centre->addWidget(deckPanel);

	auto* spectrumPanel = new Panel(QStringLiteral("VFD Spectrum Analyser"),
		QStringLiteral("purple"), centre);
	spectrumPanel->setSubtitle(QStringLiteral("spectrum of the decoded file"));
	{
		auto* inner = new QVBoxLayout(spectrumPanel->body());
		inner->setContentsMargins(4, 4, 4, 2);
		inner->setSpacing(3);
		m_spectrum = new VfdSpectrumWidget(spectrumPanel->body());
		inner->addWidget(m_spectrum, 1);

		m_spectrumTelemetry = new QLabel(spectrumPanel->body());
		m_spectrumTelemetry->setFont(theme::monoFont(7));
		m_spectrumTelemetry->setProperty("mlMuted", true);
		m_spectrumTelemetry->setText(QStringLiteral(
			"576 samp · Hann · 480 log bands · 30 Hz–16 kHz · peak hold · minimp3 decode"));
		inner->addWidget(m_spectrumTelemetry);
	}
	centre->addWidget(spectrumPanel);

	auto* listPanel = new Panel(QStringLiteral("Track Matrix"), QStringLiteral("cyan"), centre);
	{
		auto* inner = new QVBoxLayout(listPanel->body());
		inner->setContentsMargins(4, 4, 4, 4);
		inner->setSpacing(4);

		auto* filterRow = new QHBoxLayout();
		m_searchEdit = new QLineEdit(listPanel->body());
		m_searchEdit->setPlaceholderText(QStringLiteral("Filter title, artist, album or path…"));
		m_searchEdit->setClearButtonEnabled(true);
		m_searchEdit->setFont(theme::monoFont(9));
		filterRow->addWidget(m_searchEdit, 1);

		m_filterNoArtwork = new QCheckBox(QStringLiteral("No artwork"), listPanel->body());
		m_filterNoLyrics = new QCheckBox(QStringLiteral("No lyrics"), listPanel->body());
		m_filterNoBpm = new QCheckBox(QStringLiteral("No BPM"), listPanel->body());
		m_filterGain = new QCheckBox(QStringLiteral("Gain"), listPanel->body());
		m_filterPrivacy = new QCheckBox(QStringLiteral("Privacy"), listPanel->body());
		for (QCheckBox* box : {m_filterNoArtwork, m_filterNoLyrics, m_filterNoBpm, m_filterGain,
				m_filterPrivacy}) {
			box->setFont(theme::monoFont(8));
			filterRow->addWidget(box);
			connect(box, &QCheckBox::toggled, this, &MainWindow::onFilterChanged);
		}
		inner->addLayout(filterRow);

		m_trackModel = new TrackTableModel(this);
		m_trackTable = new QTableView(listPanel->body());
		m_trackTable->setModel(m_trackModel);
		m_trackTable->setSelectionBehavior(QAbstractItemView::SelectRows);
		m_trackTable->setSelectionMode(QAbstractItemView::SingleSelection);
		m_trackTable->setAlternatingRowColors(true);
		m_trackTable->setShowGrid(false);
		m_trackTable->verticalHeader()->setVisible(false);
		m_trackTable->verticalHeader()->setDefaultSectionSize(20);
		m_trackTable->horizontalHeader()->setStretchLastSection(true);
		m_trackTable->setFont(theme::monoFont(8));
		connect(m_trackTable, &QTableView::clicked, this, &MainWindow::onTrackActivated);
	connect(m_trackTable, &QTableView::doubleClicked, this, &MainWindow::onTrackDoubleClicked);
		inner->addWidget(m_trackTable, 1);

		m_trackCountLabel = new QLabel(QStringLiteral("No library open."), listPanel->body());
		m_trackCountLabel->setFont(theme::monoFont(8));
		m_trackCountLabel->setProperty("mlMuted", true);
		inner->addWidget(m_trackCountLabel);
	}
	centre->addWidget(listPanel);

	centre->setStretchFactor(0, 0);
	centre->setStretchFactor(1, 2);
	centre->setStretchFactor(2, 3);
	columns->addWidget(centre);

	// --- Right: inspector --------------------------------------------------
	auto* inspectorPanel = new Panel(QStringLiteral("Inspector"), QStringLiteral("pink"), columns);
	m_inspector = new InspectorPanel(inspectorPanel->body());
	{
		auto* inner = new QVBoxLayout(inspectorPanel->body());
		inner->setContentsMargins(0, 0, 0, 0);
		inner->addWidget(m_inspector);
	}
	columns->addWidget(inspectorPanel);

	columns->setStretchFactor(0, 0);
	columns->setStretchFactor(1, 1);
	columns->setStretchFactor(2, 0);
	columns->setSizes({280, 900, 340});

	layout->addWidget(columns, 1);

	// Debounced search.
	auto* searchTimer = new QTimer(this);
	searchTimer->setSingleShot(true);
	searchTimer->setInterval(250);
	connect(searchTimer, &QTimer::timeout, this, &MainWindow::onFilterChanged);
	connect(m_searchEdit, &QLineEdit::textChanged, this, [searchTimer]() { searchTimer->start(); });

	// Transport and spectrum plumbing.
	connect(m_deck, &DeckPanel::analyseRequested, this, &MainWindow::onAnalyseSpectrum);
	connect(m_deck, &DeckPanel::playPauseRequested, this, &MainWindow::onPlayPause);
	connect(m_deck, &DeckPanel::stopRequested, this, &MainWindow::onStop);
	connect(m_deck, &DeckPanel::seekRequested, this, &MainWindow::onSeek);
	connect(m_deck, &DeckPanel::volumeChanged, this, [this](double volume) {
		m_player->setVolume(volume);
	});
	// Dragging the analyser's playhead seeks the audio, so the two never
	// disagree about where in the track we are.
	connect(m_spectrum, &VfdSpectrumWidget::positionChanged, this, [this](double fraction) {
		if (m_player->state() == PlaybackState::Stopped) m_deck->setPosition(fraction);
	});

	m_deck->setPlaybackAvailable(m_player->hasOutputDevice(), m_player->outputDeviceName());

	// Space toggles playback from anywhere in the workbench.
	auto* playPause = new QShortcut(QKeySequence(Qt::Key_Space), this);
	connect(playPause, &QShortcut::activated, this, &MainWindow::onPlayPause);

	return page;
}

bool MainWindow::loadForPlayback(const FileRecord& record) {
	// Only decode when the selection actually changed.
	if (m_loadedForPlayback == record.id) return true;

	std::error_code ec;
	fs::path source;
	for (const auto& root : m_library->guard().protectedRoots()) {
		const fs::path candidate = root.resolvedPath / record.relativePath;
		if (fs::exists(candidate, ec) && !ec) { source = candidate; break; }
	}
	if (source.empty()) {
		statusBar()->showMessage(QStringLiteral("The source file is not reachable."), 6000);
		return false;
	}

	QApplication::setOverrideCursor(Qt::BusyCursor);
	auto status = m_player->load(source);
	QApplication::restoreOverrideCursor();

	if (!status) {
		appendLog(QStringLiteral("Could not load for playback: %1")
			.arg(qs(status.error().describe())));
		statusBar()->showMessage(qs(status.error().message), 8000);
		return false;
	}
	m_loadedForPlayback = record.id;
	m_player->setVolume(m_player->volume());
	appendLog(QStringLiteral("Loaded for playback: %1").arg(qs(record.relativePath)));
	return true;
}

void MainWindow::onPlayPause() {
	if (!m_library || !m_library->isOpen()) return;
	if (!m_player->hasOutputDevice()) {
		statusBar()->showMessage(QStringLiteral(
			"No audio output device is available. Every library function still works."), 8000);
		return;
	}

	const QModelIndex current = m_trackTable ? m_trackTable->currentIndex() : QModelIndex();
	if (!current.isValid()) return;
	const auto record = m_trackModel->recordAt(current.row());
	if (!record) return;

	if (!loadForPlayback(*record)) return;
	m_player->togglePlayPause();
}

void MainWindow::onStop() {
	m_player->stop();
}

void MainWindow::onSeek(double fraction) {
	m_player->seek(fraction);
	// In live mode the analyser shows the present moment and has no playhead to
	// move; only the precomputed view needs repositioning.
	if (m_spectrum && !m_spectrum->isLiveMode()) m_spectrum->setPosition(fraction);
}

void MainWindow::onPlayerPosition(qint64 positionMs, qint64 durationMs) {
	m_deck->setPlaybackPosition(positionMs, durationMs);
	// The analyser follows the real playhead while audio is playing.
	if (durationMs > 0 && m_spectrum && m_player->state() == PlaybackState::Playing) {
		m_spectrum->setPosition(static_cast<double>(positionMs) / static_cast<double>(durationMs));
	}
}

void MainWindow::onPlayerState(PlaybackState state) {
	m_deck->setPlaybackState(static_cast<int>(state));

	if (m_spectrum) {
		// While audio is playing the analyser runs live off the output samples.
		// Pausing leaves live mode on: pushLiveSamples() simply stops being fed
		// (the timer below is stopped too), so the display holds its last frame
		// rather than switching to the precomputed spectrogram — which may not
		// even exist yet if the track was played without first being analysed,
		// and would otherwise blank the panel the instant playback pauses.
		// Only a real stop reverts to the precomputed view.
		if (state == PlaybackState::Playing) {
			m_spectrum->setLiveMode(true);
		} else if (state == PlaybackState::Stopped) {
			m_spectrum->setLiveMode(false);
		}
	}

	if (state == PlaybackState::Playing) {
		m_visualiserTimer->start();
		statusBar()->showMessage(QStringLiteral("Playing — %1")
			.arg(m_player->formatDescription()), 4000);
		if (m_spectrumTelemetry) {
			// Replaced a second or so later by the rate-diagnostic text in the
			// timer lambda above; this is just what shows before the first
			// measurement window closes.
			m_spectrumTelemetry->setText(QStringLiteral(
				"LIVE · 576 samp · log bands (width-matched) · 30 Hz–16 kHz · peak hold · out %1")
				.arg(m_player->formatDescription()));
		}
		m_visualiserTickCount = 0;
		m_visualiserFreshCount = 0;
		m_visualiserRateClock.invalidate();
		m_haveLastVisualiserSample = false;
	} else {
		m_visualiserTimer->stop();
		if (m_spectrumTelemetry) {
			m_spectrumTelemetry->setText(state == PlaybackState::Paused
				? QStringLiteral("LIVE (paused) · 576 samp · log bands (width-matched) · 30 Hz–16 kHz · peak hold")
				: QStringLiteral("576 samp · Hann · 480 log bands · 30 Hz–16 kHz · peak hold · minimp3 decode"));
		}
	}
}

void MainWindow::onExplorerFilter(const TrackFilter& filter) {
	// Reflect the chosen node in the filter chips so the two never disagree.
	const QSignalBlocker b1(m_filterNoArtwork);
	const QSignalBlocker b2(m_filterNoLyrics);
	const QSignalBlocker b3(m_filterNoBpm);
	const QSignalBlocker b4(m_filterGain);
	const QSignalBlocker b5(m_filterPrivacy);
	const QSignalBlocker b6(m_searchEdit);

	m_filterNoArtwork->setChecked(filter.missingArtwork.value_or(false));
	m_filterNoLyrics->setChecked(filter.missingLyrics.value_or(false));
	m_filterNoBpm->setChecked(filter.missingBpm.value_or(false));
	m_filterGain->setChecked(filter.hasGainFields.value_or(false));
	m_filterPrivacy->setChecked(filter.hasPrivacyFindings.value_or(false));
	m_searchEdit->setText(qs(filter.searchText));

	m_trackModel->setFilter(filter);
	m_trackCountLabel->setText(QStringLiteral("%1 tracks match.").arg(m_trackModel->rowCount()));
	m_tabs->setCurrentIndex(0);
}

void MainWindow::onAnalyseSpectrum(FileId file) {
	if (!m_library || !m_library->isOpen()) return;

	auto record = m_library->readCatalogue().loadFile(file);
	if (!record || !record.value()) return;

	// Locate the source. Reading is always permitted; writing never is.
	std::error_code ec;
	fs::path source;
	for (const auto& root : m_library->guard().protectedRoots()) {
		const fs::path candidate = root.resolvedPath / record.value()->relativePath;
		if (fs::exists(candidate, ec) && !ec) { source = candidate; break; }
	}
	if (source.empty()) {
		m_spectrum->clear(QStringLiteral("The source file is not currently reachable."));
		return;
	}

	m_spectrum->clear(QStringLiteral("Decoding…"));
	QApplication::setOverrideCursor(Qt::BusyCursor);

	// Analysis is bounded: a long mix does not need to be decoded in full to
	// show its spectral character.
	DecodeOptions options;
	options.maxDurationMs = 6 * 60 * 1000;
	auto audio = Mp3Decoder::decode(source, options);

	QApplication::restoreOverrideCursor();

	if (!audio) {
		m_spectrum->clear(QStringLiteral("Could not decode: %1")
			.arg(qs(audio.error().message)));
		return;
	}

	m_spectrum->setSpectrogram(buildSpectrogram(audio.value()));
	m_spectrumTelemetry->setText(QStringLiteral(
		"576 samp · Hann · 480 log bands · 30 Hz–16 kHz · decoded %1 at %2 Hz mono · minimp3")
		.arg(qs(text::formatDuration(audio.value().durationMs)))
		.arg(audio.value().sampleRateHz));
	appendLog(QStringLiteral("Analysed spectrum: %1").arg(qs(record.value()->relativePath)));
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

	// --- Progress --------------------------------------------------------------
	// A command started from this tab should show its progress right here, not
	// only on the separate Jobs tab — otherwise "Scanning…" in the status bar
	// is the only feedback, with no way to tell progress from a stall.
	auto* progressGroup = new QGroupBox(QStringLiteral("Progress"), page);
	auto* progressLayout = new QVBoxLayout(progressGroup);
	m_libraryProgressLabel = new QLabel(QStringLiteral("Idle."), progressGroup);
	progressLayout->addWidget(m_libraryProgressLabel);
	m_libraryProgress = new QProgressBar(progressGroup);
	m_libraryProgress->setRange(0, 100);
	m_libraryProgress->setValue(0);
	progressLayout->addWidget(m_libraryProgress);
	layout->addWidget(progressGroup);

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
	m_albumTable->setMaximumHeight(360);
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
	splitter->setStretchFactor(1, 2);
	// Stretch factors alone only govern how *extra* space is distributed on a
	// later resize; QSplitter's first layout falls back to each widget's
	// sizeHint(), and QTableView's sizeHint asks for enough room to show its
	// rows without scrolling. Left alone, that let the table claim most of the
	// tab on first paint and left the review pane — candidate list, image
	// comparison, evidence browser — squeezed into a couple of visible rows.
	// An explicit initial split fixes that regardless of sizeHint. The review
	// pane's own artwork previews are capped square (see ArtworkView) rather
	// than stretching to fill the tab, so it no longer needs as much height as
	// the table now gets.
	splitter->setSizes({360, 460});
	splitter->setCollapsible(1, false);

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
	// Qt's dialogs always return forward slashes, even on Windows (its own
	// internal convention); std::filesystem::path accepts either separator
	// there, so this is display-only, but showing "E:/Music" to a Windows user
	// reads as broken. QDir::toNativeSeparators fixes only the display.
	if (!path.isEmpty()) m_sourceEdit->setText(QDir::toNativeSeparators(path));
}

void MainWindow::onChooseOutput() {
	const QString path = QFileDialog::getExistingDirectory(this,
		QStringLiteral("Choose where organised copies are written"), m_outputEdit->text());
	if (!path.isEmpty()) m_outputEdit->setText(QDir::toNativeSeparators(path));
}

void MainWindow::onChooseData() {
	const QString path = QFileDialog::getExistingDirectory(this,
		QStringLiteral("Choose where the catalogue is kept"), m_dataEdit->text());
	if (!path.isEmpty()) m_dataEdit->setText(QDir::toNativeSeparators(path));
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
	m_explorer->setLibrary(m_library.get());
	m_deck->setLibrary(m_library.get());
	m_inspector->setLibrary(m_library.get());

	appendLog(QStringLiteral("Library opened. Source: %1").arg(m_sourceEdit->text()));
	statusBar()->showMessage(QStringLiteral("Library open. Run Scan to build the catalogue."), 8000);
	refreshAll();
}

void MainWindow::openLibraryAt(const QString& source, const QString& output, const QString& data,
	bool offline) {
	if (!source.isEmpty()) m_sourceEdit->setText(source);
	if (!output.isEmpty()) m_outputEdit->setText(output);
	if (!data.isEmpty()) m_dataEdit->setText(data);
	m_offlineCheck->setChecked(offline);
	onOpenLibrary();
}

void MainWindow::selectFirstTrack() {
	if (!m_trackTable || m_trackModel->rowCount() == 0) return;
	const QModelIndex first = m_trackModel->index(0, 0);
	m_trackTable->setCurrentIndex(first);
	onTrackActivated(first);
}

void MainWindow::playSelectedTrack() {
	onPlayPause();
}

bool MainWindow::hasSelection() const {
	return m_trackTable && m_trackTable->currentIndex().isValid();
}

void MainWindow::selectTrackMatching(const QString& text) {
	if (!m_searchEdit) return;
	// Block the edit's signal so the debounce timer does not fire a second,
	// delayed filter pass: that reset the model, dropped the selection and wiped
	// an analysis that had already completed.
	{
		const QSignalBlocker blocker(m_searchEdit);
		m_searchEdit->setText(text);
	}
	onFilterChanged();
	selectFirstTrack();
}

void MainWindow::analyseSelectedTrack() {
	if (!m_trackTable) return;
	const QModelIndex current = m_trackTable->currentIndex();
	if (!current.isValid()) return;
	const auto record = m_trackModel->recordAt(current.row());
	if (!record) return;
	onAnalyseSpectrum(record->id);
	if (m_spectrum) m_spectrum->setPosition(0.32);
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
		auto latest = m_library->readCatalogue().latestChangeSet();
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
			.arg(qs(text::pathToUtf8(m_library->config().outputRoot))),
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
		auto latest = m_library->readCatalogue().latestChangeSet();
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
		auto latest = m_library->readCatalogue().latestChangeSet();
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
	const QString text = title + QStringLiteral("…");
	m_progressLabel->setText(text);
	m_progress->setRange(0, 100);
	m_progress->setValue(0);
	m_libraryProgressLabel->setText(text);
	m_libraryProgress->setRange(0, 100);
	m_libraryProgress->setValue(0);
	appendLog(QStringLiteral("Started: %1").arg(title));
	statusBar()->showMessage(text);
	m_coverageRefreshClock.start();
}

void MainWindow::onTaskProgress(qint64 done, qint64 total, const QString& message) {
	// Coverage reads through Library::coverage()'s own read-only connection, so
	// this is safe to run while the command driving this progress update is
	// still writing through the primary one. Throttled: a full recompute on
	// every tick would slow a fast scan down for no visible benefit.
	if (!m_coverageRefreshClock.isValid() || m_coverageRefreshClock.elapsed() >= 750) {
		onRefreshCoverage();
		m_coverageRefreshClock.restart();
	}
	if (total > 0) {
		const int percent = static_cast<int>((done * 100) / total);
		const QString text = QStringLiteral("%1  —  %2 of %3").arg(message).arg(done).arg(total);
		m_progress->setRange(0, 100);
		m_progress->setValue(percent);
		m_progressLabel->setText(text);
		m_libraryProgress->setRange(0, 100);
		m_libraryProgress->setValue(percent);
		m_libraryProgressLabel->setText(text);
	} else {
		// Busy indicator when the total is unknown.
		m_progress->setRange(0, 0);
		m_progressLabel->setText(message);
		m_libraryProgress->setRange(0, 0);
		m_libraryProgressLabel->setText(message);
	}
}

void MainWindow::onTaskFinished(bool success, const QString& title) {
	setBusy(false);
	const QString text = success
		? title + QStringLiteral(" — finished")
		: title + QStringLiteral(" — did not complete");
	m_progress->setRange(0, 100);
	m_progress->setValue(success ? 100 : 0);
	m_progressLabel->setText(text);
	m_libraryProgress->setRange(0, 100);
	m_libraryProgress->setValue(success ? 100 : 0);
	m_libraryProgressLabel->setText(text);
	appendLog(QStringLiteral("%1: %2").arg(title).arg(success
		? QStringLiteral("finished") : QStringLiteral("did not complete")));
	statusBar()->showMessage(text, 8000);
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

	// One selection drives the whole workbench.
	if (m_deck) m_deck->showFile(*record);
	if (m_inspector) m_inspector->showFile(record->id);
	if (m_spectrum && m_player->state() != PlaybackState::Playing) {
		m_spectrum->setSweeping(false);
		m_spectrum->clear(QStringLiteral("Press Analyse to decode this track's spectrum."));
	}

	auto plan = m_library->previewFile(record->id);
	if (!plan) return;
	if (!m_planPreview) return;

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

void MainWindow::playNextTrack() {
	if (!m_trackTable || !m_trackModel) return;

	const QModelIndex current = m_trackTable->currentIndex();
	const int nextRow = current.isValid() ? current.row() + 1 : 0;
	if (nextRow >= m_trackModel->rowCount()) return;   // End of the (filtered) list.

	const QModelIndex next = m_trackModel->index(nextRow, 0);
	m_trackTable->setCurrentIndex(next);
	// setCurrentIndex() alone emits neither clicked nor doubleClicked, so the
	// two steps a real double-click delivers are driven explicitly: refresh
	// the deck/inspector/plan preview, then load and play.
	onTrackActivated(next);
	onTrackDoubleClicked(next);
}

void MainWindow::onTrackDoubleClicked(const QModelIndex& index) {
	if (!index.isValid() || !m_library || !m_library->isOpen()) return;
	if (!m_player->hasOutputDevice()) {
		statusBar()->showMessage(QStringLiteral(
			"No audio output device is available. Every library function still works."), 8000);
		return;
	}

	const auto record = m_trackModel->recordAt(index.row());
	if (!record) return;

	// A single click already selected this row and populated the deck and
	// inspector (QTableView delivers clicked before doubleClicked); this just
	// adds starting playback. Unlike play/pause, a double-click always means
	// "play this", never "toggle", whatever the player was doing before.
	if (!loadForPlayback(*record)) return;
	m_player->play();
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
	if (m_explorer) m_explorer->refresh();
	refreshHeaderTelemetry();
	onRefreshCoverage();
	m_trackCountLabel->setText(QStringLiteral("%1 tracks match.").arg(m_trackModel->rowCount()));
}

void MainWindow::appendLog(const QString& message) {
	m_log->appendPlainText(QStringLiteral("[%1] %2")
		.arg(qs(nowIso8601())).arg(message));
}

} // namespace ml::desktop

// SPDX-License-Identifier: GPL-3.0-or-later
#include "AlbumReviewWidget.hpp"
#include "mlcore/Text.hpp"
#include "mlinfra/ImagePipeline.hpp"

#include <QCheckBox>
#include <QDesktopServices>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QListWidget>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QSlider>
#include <QSplitter>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>

namespace fs = std::filesystem;

namespace ml::desktop {

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

QString describeDefects(const ArtworkCandidate& candidate) {
	if (candidate.defects.empty()) return QStringLiteral("none observed");
	QStringList names;
	for (auto defect : candidate.defects) names << qs(std::string(toString(defect)));
	return names.join(QStringLiteral(", "));
}

} // namespace

// ---------------------------------------------------------------------------
// ArtworkView
// ---------------------------------------------------------------------------

ArtworkView::ArtworkView(QString caption, QWidget* parent)
	: QWidget(parent), m_caption(std::move(caption)) {
	auto* layout = new QVBoxLayout(this);
	layout->setContentsMargins(4, 4, 4, 4);

	m_captionLabel = new QLabel(m_caption, this);
	QFont captionFont = m_captionLabel->font();
	captionFont.setBold(true);
	m_captionLabel->setFont(captionFont);
	layout->addWidget(m_captionLabel);

	m_subtitleLabel = new QLabel(QStringLiteral("—"), this);
	m_subtitleLabel->setWordWrap(true);
	layout->addWidget(m_subtitleLabel);

	m_imageLabel = new QLabel(this);
	m_imageLabel->setAlignment(Qt::AlignCenter);
	m_imageLabel->setMinimumSize(220, 220);
	m_imageLabel->setStyleSheet(QStringLiteral("background: #2b2b2b; border: 1px solid #555;"));

	m_scroll = new QScrollArea(this);
	m_scroll->setWidget(m_imageLabel);
	m_scroll->setWidgetResizable(true);
	m_scroll->setAlignment(Qt::AlignCenter);
	layout->addWidget(m_scroll, 1);
}

void ArtworkView::setImage(const QImage& image, const QString& subtitle) {
	m_image = image;
	m_subtitleLabel->setText(subtitle);
	updateDisplay();
}

void ArtworkView::clear() {
	m_image = QImage();
	m_subtitleLabel->setText(QStringLiteral("—"));
	m_imageLabel->setText(QStringLiteral("no image"));
	m_imageLabel->setPixmap(QPixmap());
}

void ArtworkView::setZoomPercent(int percent) {
	m_zoomPercent = percent;
	updateDisplay();
}

void ArtworkView::updateDisplay() {
	if (m_image.isNull()) {
		m_imageLabel->setText(QStringLiteral("no image"));
		m_imageLabel->setPixmap(QPixmap());
		return;
	}
	m_imageLabel->setText(QString());

	if (m_zoomPercent <= 0) {
		// Fit. Both panes fit to the same box, which is what makes a 600 px asset
		// and a 3000 px asset visually comparable rather than one dwarfing the other.
		const QSize box = m_scroll->viewport()->size() - QSize(8, 8);
		m_imageLabel->setPixmap(QPixmap::fromImage(
			m_image.scaled(box, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
		m_scroll->setWidgetResizable(true);
	} else {
		// Original-pixel inspection: no smoothing, so JPEG blocking, scan moire
		// and sharpening halos are visible rather than averaged away.
		const QSize target = m_image.size() * m_zoomPercent / 100;
		m_imageLabel->setPixmap(QPixmap::fromImage(
			m_image.scaled(target, Qt::KeepAspectRatio, Qt::FastTransformation)));
		m_imageLabel->adjustSize();
		m_scroll->setWidgetResizable(false);
	}
}

// ---------------------------------------------------------------------------
// AlbumReviewWidget
// ---------------------------------------------------------------------------

AlbumReviewWidget::AlbumReviewWidget(QWidget* parent) : QWidget(parent) {
	auto* layout = new QVBoxLayout(this);

	m_titleLabel = new QLabel(QStringLiteral("Select an album"), this);
	QFont titleFont = m_titleLabel->font();
	titleFont.setPointSize(titleFont.pointSize() + 3);
	titleFont.setBold(true);
	m_titleLabel->setFont(titleFont);
	m_titleLabel->setWordWrap(true);
	layout->addWidget(m_titleLabel);

	auto* splitter = new QSplitter(Qt::Horizontal, this);

	// --- Candidate list ------------------------------------------------------
	auto* leftPanel = new QWidget(splitter);
	auto* leftLayout = new QVBoxLayout(leftPanel);
	leftLayout->setContentsMargins(0, 0, 0, 0);
	leftLayout->addWidget(new QLabel(QStringLiteral("Candidates"), leftPanel));

	m_candidateList = new QListWidget(leftPanel);
	m_candidateList->setAlternatingRowColors(true);
	leftLayout->addWidget(m_candidateList, 1);

	auto* buttonRow = new QHBoxLayout();
	m_lockButton = new QPushButton(QStringLiteral("Lock this cover"), leftPanel);
	m_rejectButton = new QPushButton(QStringLiteral("Reject"), leftPanel);
	m_importButton = new QPushButton(QStringLiteral("Import file…"), leftPanel);
	m_lockButton->setToolTip(QStringLiteral(
		"Lock this choice. A lock outranks every provider result and survives "
		"rescans and refreshes."));
	m_rejectButton->setToolTip(QStringLiteral("Reject this candidate for this album."));
	m_importButton->setToolTip(QStringLiteral(
		"Import a JPEG or PNG you already have. Works with no network access."));
	buttonRow->addWidget(m_lockButton);
	buttonRow->addWidget(m_rejectButton);
	buttonRow->addWidget(m_importButton);
	leftLayout->addLayout(buttonRow);

	auto* searchRow = new QHBoxLayout();
	auto* googleButton = new QPushButton(QStringLiteral("Google Images"), leftPanel);
	auto* appleButton = new QPushButton(QStringLiteral("Apple Music"), leftPanel);
	auto* brainzButton = new QPushButton(QStringLiteral("MusicBrainz"), leftPanel);
	googleButton->setToolTip(QStringLiteral(
		"Open an image search in your browser. No search API is called; this is a "
		"review aid that opens a page for you to look at."));
	searchRow->addWidget(googleButton);
	searchRow->addWidget(appleButton);
	searchRow->addWidget(brainzButton);
	leftLayout->addLayout(searchRow);

	splitter->addWidget(leftPanel);

	// --- Comparison ----------------------------------------------------------
	auto* centrePanel = new QWidget(splitter);
	auto* centreLayout = new QVBoxLayout(centrePanel);
	centreLayout->setContentsMargins(0, 0, 0, 0);

	auto* zoomRow = new QHBoxLayout();
	zoomRow->addWidget(new QLabel(QStringLiteral("Zoom"), centrePanel));
	m_zoomSlider = new QSlider(Qt::Horizontal, centrePanel);
	m_zoomSlider->setRange(0, 400);
	m_zoomSlider->setValue(0);
	m_zoomSlider->setToolTip(QStringLiteral(
		"0 fits each image to its pane so covers of different sizes compare fairly. "
		"100 shows original pixels without smoothing."));
	zoomRow->addWidget(m_zoomSlider, 1);
	m_zoomLabel = new QLabel(QStringLiteral("fit"), centrePanel);
	m_zoomLabel->setMinimumWidth(60);
	zoomRow->addWidget(m_zoomLabel);
	centreLayout->addLayout(zoomRow);

	auto* viewRow = new QHBoxLayout();
	m_currentView = new ArtworkView(QStringLiteral("Currently embedded"), centrePanel);
	m_candidateView = new ArtworkView(QStringLiteral("Candidate (received asset)"), centrePanel);
	m_derivativeView = new ArtworkView(QStringLiteral("600×600 output preview"), centrePanel);
	viewRow->addWidget(m_currentView, 1);
	viewRow->addWidget(m_candidateView, 1);
	viewRow->addWidget(m_derivativeView, 1);
	centreLayout->addLayout(viewRow, 1);

	splitter->addWidget(centrePanel);

	// --- Evidence ------------------------------------------------------------
	auto* rightPanel = new QWidget(splitter);
	auto* rightLayout = new QVBoxLayout(rightPanel);
	rightLayout->setContentsMargins(0, 0, 0, 0);

	rightLayout->addWidget(new QLabel(QStringLiteral("Selection outcome"), rightPanel));
	m_decisionBrowser = new QTextBrowser(rightPanel);
	m_decisionBrowser->setMaximumHeight(160);
	rightLayout->addWidget(m_decisionBrowser);

	rightLayout->addWidget(new QLabel(QStringLiteral("Evidence for this candidate"), rightPanel));
	m_evidenceBrowser = new QTextBrowser(rightPanel);
	rightLayout->addWidget(m_evidenceBrowser, 1);

	splitter->addWidget(rightPanel);
	splitter->setStretchFactor(0, 2);
	splitter->setStretchFactor(1, 5);
	splitter->setStretchFactor(2, 3);

	layout->addWidget(splitter, 1);

	connect(m_candidateList, &QListWidget::currentRowChanged,
		this, &AlbumReviewWidget::onCandidateSelected);
	connect(m_lockButton, &QPushButton::clicked, this, &AlbumReviewWidget::onLockClicked);
	connect(m_rejectButton, &QPushButton::clicked, this, &AlbumReviewWidget::onRejectClicked);
	connect(m_importButton, &QPushButton::clicked, this, &AlbumReviewWidget::onImportClicked);
	connect(googleButton, &QPushButton::clicked, this, &AlbumReviewWidget::onOpenGoogleImages);
	connect(appleButton, &QPushButton::clicked, this, &AlbumReviewWidget::onOpenAppleSearch);
	connect(brainzButton, &QPushButton::clicked, this, &AlbumReviewWidget::onOpenMusicBrainz);
	connect(m_zoomSlider, &QSlider::valueChanged, this, &AlbumReviewWidget::onZoomChanged);

	clear();
}

void AlbumReviewWidget::setLibrary(Library* library) {
	m_library = library;
	clear();
}

void AlbumReviewWidget::clear() {
	m_review.reset();
	m_titleLabel->setText(QStringLiteral("Select an album"));
	m_candidateList->clear();
	m_currentView->clear();
	m_candidateView->clear();
	m_derivativeView->clear();
	m_evidenceBrowser->clear();
	m_decisionBrowser->clear();
	m_lockButton->setEnabled(false);
	m_rejectButton->setEnabled(false);
	m_importButton->setEnabled(m_library != nullptr);
}

void AlbumReviewWidget::showAlbum(AlbumId album) {
	if (!m_library || !m_library->isOpen()) return;

	auto review = m_library->reviewAlbum(album);
	if (!review) {
		emit statusMessage(QStringLiteral("Could not load the album: %1")
			.arg(qs(review.error().describe())));
		return;
	}
	m_review = review.value();
	populate();
}

void AlbumReviewWidget::populate() {
	if (!m_review) return;

	const AlbumReview& review = *m_review;

	QString title = qs(review.album.albumArtist) + QStringLiteral(" — ") + qs(review.album.album);
	if (!review.album.editionQualifier.empty()) {
		title += QStringLiteral("  [") + qs(review.album.editionQualifier) + QStringLiteral("]");
	}
	title += QStringLiteral("   (%1 tracks)").arg(review.album.observedTrackCount);
	m_titleLabel->setText(title);

	// --- Selection outcome ---------------------------------------------------
	QString decision = QStringLiteral("<b>%1</b><br>%2")
		.arg(qs(std::string(toString(review.selection.outcome))))
		.arg(qs(review.selection.reason).toHtmlEscaped());

	if (!review.album.flags.empty()) {
		decision += QStringLiteral("<br><br><b>Album flags</b><br>");
		for (auto flag : review.album.flags) {
			decision += qs(std::string(toString(flag))) + QStringLiteral("<br>");
		}
	}
	decision += QStringLiteral("<br><b>Identity confidence</b>: %1")
		.arg(qs(std::string(toString(review.album.identityConfidence))));
	m_decisionBrowser->setHtml(decision);

	// --- Candidates ----------------------------------------------------------
	m_candidateList->clear();
	for (std::size_t i = 0; i < review.artworkCandidates.size(); ++i) {
		const ArtworkCandidate& candidate = review.artworkCandidates[i];

		QString label = qs(candidate.providerName);
		if (candidate.dimensionsMeasured) {
			label += QStringLiteral("  %1×%2").arg(candidate.measuredWidth).arg(candidate.measuredHeight);
		} else {
			label += QStringLiteral("  (unmeasured)");
		}
		if (candidate.manuallyLocked) label += QStringLiteral("  [LOCKED]");
		if (candidate.manuallyRejected) label += QStringLiteral("  [rejected]");
		if (static_cast<int>(i) == review.selection.selectedIndex) {
			label += QStringLiteral("  ← selected");
		}

		auto* item = new QListWidgetItem(label, m_candidateList);
		item->setData(Qt::UserRole, static_cast<qulonglong>(i));
	}

	// --- Currently embedded cover --------------------------------------------
	m_currentView->clear();
	for (const auto& candidate : review.artworkCandidates) {
		if (candidate.sourceType != ArtworkSourceType::ExistingEmbedded) continue;
		const QImage image = loadCandidateImage(candidate);
		if (!image.isNull()) {
			m_currentView->setImage(image, QStringLiteral("%1×%2, already in the files")
				.arg(image.width()).arg(image.height()));
		}
		break;
	}

	if (!review.artworkCandidates.empty()) {
		const int initial = review.selection.selectedIndex >= 0 ? review.selection.selectedIndex : 0;
		m_candidateList->setCurrentRow(initial);
	} else {
		m_candidateView->clear();
		m_derivativeView->clear();
		m_evidenceBrowser->setHtml(QStringLiteral(
			"<i>No artwork candidates have been gathered for this album yet. "
			"Run Artwork, or import a file.</i>"));
	}

	m_importButton->setEnabled(true);
}

QImage AlbumReviewWidget::loadCandidateImage(const ArtworkCandidate& candidate) const {
	if (!m_library || candidate.localPath.empty()) return {};

	const AssetStore store(m_library->config().dataDirectory / "assets");
	auto bytes = store.get(candidate.localPath);
	if (!bytes) return {};

	QImage image;
	image.loadFromData(reinterpret_cast<const uchar*>(bytes.value().data()),
		static_cast<int>(bytes.value().size()));
	return image;
}

QImage AlbumReviewWidget::loadDerivativeImage(const ArtworkCandidate& candidate) const {
	if (!m_library || candidate.localPath.empty()) return {};

	const AssetStore store(m_library->config().dataDirectory / "assets");
	auto bytes = store.get(candidate.localPath);
	if (!bytes) return {};

	// Generate the preview through the same pipeline the writer uses, so what is
	// shown is the actual output rather than a Qt-scaled approximation of it.
	auto derivative = ImagePipeline::makeDerivative(bytes.value().data(), bytes.value().size(),
		m_library->config().derivative);
	if (!derivative) return {};

	QImage image;
	image.loadFromData(reinterpret_cast<const uchar*>(derivative.value().jpegBytes.data()),
		static_cast<int>(derivative.value().jpegBytes.size()));
	return image;
}

void AlbumReviewWidget::showCandidate(int index) {
	if (!m_review || index < 0
		|| index >= static_cast<int>(m_review->artworkCandidates.size())) {
		m_candidateView->clear();
		m_derivativeView->clear();
		m_evidenceBrowser->clear();
		m_lockButton->setEnabled(false);
		m_rejectButton->setEnabled(false);
		return;
	}

	const ArtworkCandidate& candidate = m_review->artworkCandidates[static_cast<std::size_t>(index)];

	const QImage received = loadCandidateImage(candidate);
	if (received.isNull()) {
		m_candidateView->clear();
	} else {
		m_candidateView->setImage(received,
			QStringLiteral("%1×%2 measured · %3 · %4")
				.arg(received.width()).arg(received.height())
				.arg(qs(text::formatBytes(candidate.byteLength)))
				.arg(qs(candidate.providerName)));
	}

	const QImage derivative = loadDerivativeImage(candidate);
	if (derivative.isNull()) {
		m_derivativeView->clear();
		m_derivativeView->setImage(QImage(), QStringLiteral(
			"No 600×600 output can be produced from this source."));
	} else {
		m_derivativeView->setImage(derivative,
			QStringLiteral("%1×%2 · %3")
				.arg(derivative.width()).arg(derivative.height())
				.arg(qs(m_library->config().derivative.describe())));
	}

	// --- Evidence ------------------------------------------------------------
	QString html;
	html += QStringLiteral("<b>Cover match</b>: %1<br>")
		.arg(qs(std::string(toString(candidate.coverMatch))));
	html += QStringLiteral("<b>Source type</b>: %1<br>")
		.arg(qs(std::string(toString(candidate.sourceType))));
	html += QStringLiteral("<b>Match confidence</b>: %1<br>")
		.arg(qs(std::string(toString(candidate.matchConfidence))));
	html += QStringLiteral("<b>Observed defects</b>: %1<br>").arg(describeDefects(candidate));

	if (candidate.dimensionsMeasured) {
		html += QStringLiteral("<b>Measured size</b>: %1×%2<br>")
			.arg(candidate.measuredWidth).arg(candidate.measuredHeight);
	} else {
		html += QStringLiteral(
			"<b>Measured size</b>: <i>not measured — a candidate whose bytes have not been "
			"decoded cannot be selected</i><br>");
	}
	if (candidate.claimedWidth > 0) {
		html += QStringLiteral("<b>Claimed size</b>: %1×%2 "
			"<i>(a provider or URL claim, not evidence)</i><br>")
			.arg(candidate.claimedWidth).arg(candidate.claimedHeight);
	}

	html += QStringLiteral("<br><b>Evidence</b><br><ul>");
	for (const auto& evidence : candidate.evidence) {
		html += QStringLiteral("<li>%1 %2</li>")
			.arg(evidence.supporting ? QStringLiteral("✔") : QStringLiteral("✘"))
			.arg(qs(evidence.detail).toHtmlEscaped());
	}
	html += QStringLiteral("</ul>");

	if (index < static_cast<int>(m_review->selection.rejectionReasons.size())
		&& !m_review->selection.rejectionReasons[static_cast<std::size_t>(index)].empty()) {
		html += QStringLiteral("<br><b>Why this was not selected</b><br>%1")
			.arg(qs(m_review->selection.rejectionReasons[static_cast<std::size_t>(index)])
				.toHtmlEscaped());
	}

	if (!candidate.pageUrl.empty()) {
		html += QStringLiteral("<br><br><a href=\"%1\">Open the provider page</a>")
			.arg(qs(candidate.pageUrl));
	}

	m_evidenceBrowser->setHtml(html);
	m_evidenceBrowser->setOpenExternalLinks(true);

	m_lockButton->setEnabled(true);
	m_rejectButton->setEnabled(true);
}

void AlbumReviewWidget::onCandidateSelected() {
	showCandidate(m_candidateList->currentRow());
}

void AlbumReviewWidget::onLockClicked() {
	if (!m_review || !m_library) return;
	const int index = m_candidateList->currentRow();
	if (index < 0 || index >= static_cast<int>(m_review->artworkCandidates.size())) return;

	const ArtworkCandidate& candidate = m_review->artworkCandidates[static_cast<std::size_t>(index)];
	auto status = m_library->lockArtwork(m_review->album.id, candidate.id,
		"locked from the album review");
	if (!status) {
		QMessageBox::warning(this, QStringLiteral("Could not lock"),
			qs(status.error().describe()));
		return;
	}
	emit statusMessage(QStringLiteral("Locked artwork for %1. The lock survives rescans and "
		"provider refreshes.").arg(qs(m_review->album.album)));
	emit decisionRecorded();
	showAlbum(m_review->album.id);
}

void AlbumReviewWidget::onRejectClicked() {
	if (!m_review || !m_library) return;
	const int index = m_candidateList->currentRow();
	if (index < 0 || index >= static_cast<int>(m_review->artworkCandidates.size())) return;

	const ArtworkCandidate& candidate = m_review->artworkCandidates[static_cast<std::size_t>(index)];
	auto status = m_library->rejectArtwork(m_review->album.id, candidate.id,
		"rejected from the album review");
	if (!status) {
		QMessageBox::warning(this, QStringLiteral("Could not reject"),
			qs(status.error().describe()));
		return;
	}
	emit statusMessage(QStringLiteral("Rejected that candidate."));
	emit decisionRecorded();
	showAlbum(m_review->album.id);
}

void AlbumReviewWidget::onImportClicked() {
	if (!m_review || !m_library) return;

	const QString path = QFileDialog::getOpenFileName(this,
		QStringLiteral("Import cover artwork"), {},
		QStringLiteral("Images (*.jpg *.jpeg *.png)"));
	if (path.isEmpty()) return;

	auto status = m_library->importArtworkFile(m_review->album.id, fs::path(path.toStdString()));
	if (!status) {
		QMessageBox::warning(this, QStringLiteral("Could not import"),
			qs(status.error().describe()));
		return;
	}
	emit statusMessage(QStringLiteral("Imported and locked %1.").arg(path));
	emit decisionRecorded();
	showAlbum(m_review->album.id);
}

void AlbumReviewWidget::onOpenGoogleImages() {
	if (!m_review) return;
	QDesktopServices::openUrl(QUrl(qs(m_review->googleImagesUrl)));
}

void AlbumReviewWidget::onOpenAppleSearch() {
	if (!m_review) return;
	QDesktopServices::openUrl(QUrl(qs(m_review->appleSearchUrl)));
}

void AlbumReviewWidget::onOpenMusicBrainz() {
	if (!m_review) return;
	QDesktopServices::openUrl(QUrl(qs(m_review->musicBrainzUrl)));
}

void AlbumReviewWidget::onZoomChanged(int value) {
	m_zoomLabel->setText(value <= 0 ? QStringLiteral("fit") : QStringLiteral("%1%").arg(value));
	m_currentView->setZoomPercent(value);
	m_candidateView->setZoomPercent(value);
	m_derivativeView->setZoomPercent(value);
}

} // namespace ml::desktop

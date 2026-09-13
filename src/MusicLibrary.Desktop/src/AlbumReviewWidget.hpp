// SPDX-License-Identifier: GPL-3.0-or-later
// FN-UI-01: the album artwork review.
//
// The FRD is specific about what this screen has to make easy: comparing the
// current and candidate covers at comparable sizes, previewing the 600x600
// output, inspecting original pixels, reading the source and edition evidence
// and the condition flags, choosing a smaller but cleaner cover, and locking
// that decision.
#pragma once

#include "mlapp/Library.hpp"

#include <QLabel>
#include <QScrollArea>
#include <QWidget>

#include <optional>
#include <vector>

class QListWidget;
class QListWidgetItem;
class QPushButton;
class QTextBrowser;
class QCheckBox;
class QSlider;
class QSplitter;

namespace ml::desktop {

/// Displays one image at a chosen zoom, including 1:1 original pixels.
class ArtworkView : public QWidget {
	Q_OBJECT

public:
	explicit ArtworkView(QString caption, QWidget* parent = nullptr);

	void setImage(const QImage& image, const QString& subtitle);
	void clear();

	/// Zoom as a percentage. 0 means "fit to the available box", which is what
	/// makes two covers of different sizes comparable at a glance.
	void setZoomPercent(int percent);

	const QImage& image() const { return m_image; }

private:
	void updateDisplay();

	QString m_caption;
	QImage m_image;
	QLabel* m_captionLabel = nullptr;
	QLabel* m_subtitleLabel = nullptr;
	QLabel* m_imageLabel = nullptr;
	QScrollArea* m_scroll = nullptr;
	int m_zoomPercent = 0;
};

/// The album review screen.
class AlbumReviewWidget : public QWidget {
	Q_OBJECT

public:
	explicit AlbumReviewWidget(QWidget* parent = nullptr);

	void setLibrary(Library* library);
	void showAlbum(AlbumId album);
	void clear();

signals:
	void statusMessage(const QString& message);
	void decisionRecorded();

private slots:
	void onCandidateSelected();
	void onLockClicked();
	void onRejectClicked();
	void onImportClicked();
	void onOpenGoogleImages();
	void onOpenAppleSearch();
	void onOpenMusicBrainz();
	void onZoomChanged(int value);

private:
	void populate();
	void showCandidate(int index);
	QImage loadCandidateImage(const ArtworkCandidate& candidate) const;
	QImage loadDerivativeImage(const ArtworkCandidate& candidate) const;

	Library* m_library = nullptr;
	std::optional<AlbumReview> m_review;

	QLabel* m_titleLabel = nullptr;
	QListWidget* m_candidateList = nullptr;
	ArtworkView* m_currentView = nullptr;
	ArtworkView* m_candidateView = nullptr;
	ArtworkView* m_derivativeView = nullptr;
	QTextBrowser* m_evidenceBrowser = nullptr;
	QTextBrowser* m_decisionBrowser = nullptr;
	QPushButton* m_lockButton = nullptr;
	QPushButton* m_rejectButton = nullptr;
	QPushButton* m_importButton = nullptr;
	QSlider* m_zoomSlider = nullptr;
	QLabel* m_zoomLabel = nullptr;
};

} // namespace ml::desktop

// SPDX-License-Identifier: GPL-3.0-or-later
// The VFD dot-matrix spectrum analyser from the Resonance design.
//
// This shows real data. The spectrum is computed by decoding the selected
// track with the same minimp3 decoder and FFT the BPM analyser uses, so what is
// on screen is that file's actual spectral content.
//
// Resonance has no playback engine, so there is no live stream to follow.
// Instead of animating something invented, the widget sweeps a playhead across
// the decoded analysis and renders the spectrum at that position. The mode is
// stated in the telemetry strip rather than being dressed up as live output.
#pragma once

#include "mlinfra/AudioAnalysis.hpp"

#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>

#include <vector>

namespace ml::desktop {

/// A precomputed spectrogram: one magnitude column per analysis hop.
struct Spectrogram {
	/// Column-major: `columns[t][band]`, each value normalised to [0,1].
	std::vector<std::vector<float>> columns;
	int bandCount = 0;
	double columnRateHz = 0.0;
	std::int64_t durationMs = 0;
	std::string sourceLabel;
	int sampleRateHz = 0;

	bool valid() const { return !columns.empty() && bandCount > 0; }
};

/// Builds a spectrogram from decoded analysis audio.
///
/// Bands are spaced logarithmically across the audible range, matching how the
/// design labels its X axis (31 Hz to 16 kHz).
Spectrogram buildSpectrogram(const AnalysisAudio& audio, int bandCount = 96);

class VfdSpectrumWidget : public QWidget {
	Q_OBJECT

public:
	explicit VfdSpectrumWidget(QWidget* parent = nullptr);

	/// Replaces the displayed analysis. An invalid spectrogram clears the panel
	/// and shows why there is nothing to draw.
	void setSpectrogram(Spectrogram spectrogram);
	void clear(const QString& reason = {});

	/// Starts or stops the sweep. Stopping holds the current column.
	void setSweeping(bool sweeping);
	bool isSweeping() const { return m_sweeping; }

	/// Moves the playhead to a fraction of the track.
	void setPosition(double fraction);
	double position() const { return m_position; }

	/// Peak-hold decay. The design's meters hold a peak marker briefly.
	void setPeakHoldEnabled(bool enabled) { m_peakHold = enabled; }

	QSize minimumSizeHint() const override { return {320, 150}; }
	QSize sizeHint() const override { return {820, 230}; }

signals:
	void positionChanged(double fraction);

protected:
	void paintEvent(QPaintEvent* event) override;
	void mousePressEvent(QMouseEvent* event) override;
	void mouseMoveEvent(QMouseEvent* event) override;
	void resizeEvent(QResizeEvent* event) override;

private:
	void advance();
	void drawGraticule(QPainter& painter, const QRect& plot) const;
	void drawMatrix(QPainter& painter, const QRect& plot) const;
	void drawAxes(QPainter& painter, const QRect& plot) const;
	void drawEmpty(QPainter& painter, const QRect& plot) const;
	const std::vector<float>* currentColumn() const;
	/// Fills the level and peak arrays from the column under the playhead, so a
	/// held position renders its real spectrum rather than an empty grid.
	void seedLevelsFromCurrentColumn();

	Spectrogram m_spectrogram;
	QString m_emptyReason;

	std::vector<float> m_levels;      ///< Smoothed level per band, [0,1].
	std::vector<float> m_peaks;       ///< Peak-hold marker per band.
	std::vector<float> m_peakAge;

	QTimer m_timer;
	QElapsedTimer m_clock;
	bool m_sweeping = false;
	bool m_peakHold = true;
	double m_position = 0.0;

	// Dot-matrix geometry, as the design specifies: a 1 px cell with a 1 px gap
	// in both axes, giving the dense VFD look.
	int m_cellSize = 3;
	int m_cellGap = 1;
};

} // namespace ml::desktop

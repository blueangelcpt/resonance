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
///
/// The default band count is high enough that the display renders each
/// visible bar from its own distinct slice of spectrum at typical widget
/// widths, rather than stretching a coarser set of bands across more pixels
/// than it has data for — which reads as a much blockier, wider-barred
/// display than the dot-matrix design calls for.
Spectrogram buildSpectrogram(const AnalysisAudio& audio, int bandCount = 480);

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

	/// Feeds a block of mono samples straight from the audio output and renders
	/// their spectrum. This is the live path: the analyser shows what is being
	/// played, rather than replaying a precomputed analysis.
	void pushLiveSamples(const std::vector<float>& samples, int sampleRateHz);

	/// Switches between the live path and the precomputed spectrogram.
	void setLiveMode(bool live);
	bool isLiveMode() const { return m_liveMode; }

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

	// Live path.
	bool m_liveMode = false;
	int m_liveSampleRate = 0;
	/// A mild raised-cosine taper over the 576-sample analysis window (the
	/// reference analyser's own input size), sized once and independent of
	/// band count or rate. Not a Hann window: see ensureEnvelope()'s comment.
	std::vector<float> m_envelope;
	/// Per-bin boost curve compensating for real audio's naturally bass-heavy
	/// energy distribution, so the display doesn't read as all-bass. Rebuilt
	/// alongside the band edges, since its length is the raw bin count.
	std::vector<float> m_equalize;
	std::vector<std::size_t> m_bandEdges;
	int m_bandEdgeRate = 0;           ///< Rate the edges were built for.
	int m_bandEdgeCount = 0;          ///< Band count the edges were built for.
	/// The zero-padded transform size backing m_equalize/m_bandEdges — grows
	/// with band count (see rebuildBands), so the FFT working buffers below
	/// are resized to match rather than fixed at compile time.
	std::size_t m_liveFftSize = 0;
	std::vector<float> m_fftReal;
	std::vector<float> m_fftImag;
	/// A slow-attack... fast-attack, slow-release reference level that each
	/// push's raw band magnitudes are divided by before display. The reference
	/// analyser works in a fixed 0-255 byte range tuned for its own 8-bit
	/// visualisation data; our decoder hands us normalised float samples in a
	/// different absolute scale, so a literal constant would either pin every
	/// bar at the ceiling or never light one — this self-calibrates instead,
	/// while keeping the same fast-rise/slow-fall shape a hardware AGC has.
	float m_autoGainCeiling = 1e-6f;

	/// Rebuilds the logarithmic band edges, the equalize curve and the FFT
	/// working buffers when the output rate or the band count (which tracks
	/// the widget's current width — see pushLiveSamples) has changed since
	/// they were last built.
	void rebuildBands(int sampleRateHz, int bands);
	/// Builds m_envelope once; a no-op after the first call.
	void ensureEnvelope();
	/// Applies the reference analyser's ballistics towards a freshly computed
	/// column: a bar snaps up to a louder value instantly and falls at a fixed
	/// rate per second, rather than easing towards it (see the comment on
	/// applyColumn's definition for why).
	void applyColumn(const std::vector<float>& column, double dtSeconds);

	/// Timestamps live pushes so applyColumn's falloff is a real rate rather
	/// than a fixed per-call step, independent of how often the timer driving
	/// pushLiveSamples actually fires.
	QElapsedTimer m_liveClock;
};

} // namespace ml::desktop

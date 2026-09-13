// SPDX-License-Identifier: GPL-3.0-or-later
#include "VfdSpectrumWidget.hpp"
#include "Theme.hpp"

#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>

#include <algorithm>
#include <cmath>

namespace ml::desktop {

namespace {

constexpr float kFloorDb = -72.0f;
constexpr float kCeilingDb = 0.0f;

/// Converts a linear magnitude to a normalised position on the dB scale.
float toNormalisedDb(float magnitude) {
	if (magnitude <= 1e-9f) return 0.0f;
	const float db = 20.0f * std::log10(magnitude);
	const float clamped = std::clamp(db, kFloorDb, kCeilingDb);
	return (clamped - kFloorDb) / (kCeilingDb - kFloorDb);
}

/// The design's meter gradient: cyan at the bottom, purple through the middle,
/// pink at the top, red for the overload band.
QColor levelColour(float normalised) {
	const auto mix = [](const QColor& a, const QColor& b, float t) {
		t = std::clamp(t, 0.0f, 1.0f);
		return QColor(
			static_cast<int>(a.red() + (b.red() - a.red()) * t),
			static_cast<int>(a.green() + (b.green() - a.green()) * t),
			static_cast<int>(a.blue() + (b.blue() - a.blue()) * t));
	};

	if (normalised < 0.55f) return mix(theme::kNeonCyan, theme::kNeonPurple, normalised / 0.55f);
	if (normalised < 0.85f) return mix(theme::kNeonPurple, theme::kNeonPink, (normalised - 0.55f) / 0.30f);
	return mix(theme::kNeonPink, theme::kOverloadRed, (normalised - 0.85f) / 0.15f);
}

} // namespace

// ---------------------------------------------------------------------------
// Spectrogram
// ---------------------------------------------------------------------------

Spectrogram buildSpectrogram(const AnalysisAudio& audio, int bandCount) {
	Spectrogram result;
	if (!audio.valid() || bandCount <= 0) return result;

	constexpr std::size_t kWindow = 1024;
	constexpr std::size_t kHop = 512;
	if (audio.samples.size() < kWindow) return result;

	result.bandCount = bandCount;
	result.sampleRateHz = audio.sampleRateHz;
	result.durationMs = audio.durationMs;
	result.columnRateHz = static_cast<double>(audio.sampleRateHz) / static_cast<double>(kHop);

	// Hann window, so adjacent bins do not smear across the display.
	std::vector<float> window(kWindow);
	for (std::size_t i = 0; i < kWindow; ++i) {
		window[i] = 0.5f * (1.0f - std::cos(2.0f * 3.14159265358979f
			* static_cast<float>(i) / static_cast<float>(kWindow - 1)));
	}

	// Logarithmic band edges from 30 Hz to just under Nyquist, matching the
	// design's 31 Hz .. 16 kHz axis labels.
	const double nyquist = audio.sampleRateHz / 2.0;
	const double lowHz = 30.0;
	const double highHz = std::min(16000.0, nyquist * 0.98);
	std::vector<std::size_t> edges(static_cast<std::size_t>(bandCount) + 1);
	for (int b = 0; b <= bandCount; ++b) {
		const double t = static_cast<double>(b) / bandCount;
		const double hz = lowHz * std::pow(highHz / lowHz, t);
		const double bin = hz * kWindow / audio.sampleRateHz;
		edges[static_cast<std::size_t>(b)] = static_cast<std::size_t>(
			std::clamp(bin, 1.0, static_cast<double>(kWindow / 2 - 1)));
	}

	const std::size_t frames = (audio.samples.size() - kWindow) / kHop + 1;
	// Bound the work: a long DJ set would otherwise produce tens of thousands of
	// columns for a widget a few hundred pixels wide.
	const std::size_t stride = std::max<std::size_t>(1, frames / 4000);

	std::vector<float> real(kWindow);
	std::vector<float> imaginary(kWindow);

	for (std::size_t frame = 0; frame < frames; frame += stride) {
		const std::size_t offset = frame * kHop;
		for (std::size_t i = 0; i < kWindow; ++i) {
			real[i] = audio.samples[offset + i] * window[i];
			imaginary[i] = 0.0f;
		}
		fftRadix2(real, imaginary, false);

		std::vector<float> column(static_cast<std::size_t>(bandCount), 0.0f);
		for (int b = 0; b < bandCount; ++b) {
			const std::size_t from = edges[static_cast<std::size_t>(b)];
			const std::size_t to = std::max(from + 1, edges[static_cast<std::size_t>(b) + 1]);

			// Peak within the band reads better on a dot matrix than a mean: a
			// narrow tone stays visible instead of being averaged into the floor.
			float peak = 0.0f;
			for (std::size_t bin = from; bin < to && bin < kWindow / 2; ++bin) {
				const float magnitude = std::sqrt(real[bin] * real[bin] + imaginary[bin] * imaginary[bin]);
				peak = std::max(peak, magnitude);
			}
			// Normalise by the window's coherent gain so full scale is ~1.0.
			column[static_cast<std::size_t>(b)] = toNormalisedDb(peak * 4.0f / static_cast<float>(kWindow));
		}
		result.columns.push_back(std::move(column));
	}

	if (stride > 1) result.columnRateHz /= static_cast<double>(stride);
	return result;
}

// ---------------------------------------------------------------------------
// VfdSpectrumWidget
// ---------------------------------------------------------------------------

VfdSpectrumWidget::VfdSpectrumWidget(QWidget* parent) : QWidget(parent) {
	setMinimumHeight(150);
	setMouseTracking(false);
	setAutoFillBackground(false);

	m_timer.setInterval(33);   // ~30 Hz, enough for a smooth meter
	connect(&m_timer, &QTimer::timeout, this, &VfdSpectrumWidget::advance);

	m_emptyReason = QStringLiteral("Select a track to analyse its spectrum.");
}

void VfdSpectrumWidget::setSpectrogram(Spectrogram spectrogram) {
	m_spectrogram = std::move(spectrogram);
	m_position = 0.0;
	m_emptyReason.clear();

	const std::size_t bands = static_cast<std::size_t>(std::max(0, m_spectrogram.bandCount));
	m_levels.assign(bands, 0.0f);
	m_peaks.assign(bands, 0.0f);
	m_peakAge.assign(bands, 0.0f);

	if (!m_spectrogram.valid()) {
		m_emptyReason = QStringLiteral("No spectrum could be computed for this track.");
	} else {
		seedLevelsFromCurrentColumn();
	}
	update();
}

void VfdSpectrumWidget::clear(const QString& reason) {
	m_spectrogram = {};
	m_levels.clear();
	m_peaks.clear();
	m_peakAge.clear();
	m_position = 0.0;
	m_emptyReason = reason.isEmpty()
		? QStringLiteral("Select a track to analyse its spectrum.")
		: reason;
	setSweeping(false);
	update();
}

void VfdSpectrumWidget::setSweeping(bool sweeping) {
	if (sweeping == m_sweeping) return;
	m_sweeping = sweeping && m_spectrogram.valid();
	if (m_sweeping) {
		m_clock.restart();
		m_timer.start();
	} else {
		m_timer.stop();
	}
	update();
}

void VfdSpectrumWidget::setPosition(double fraction) {
	m_position = std::clamp(fraction, 0.0, 1.0);
	// When the sweep is not running, nothing else will fill the level array, so
	// the display would show an empty grid at a position that does have content.
	// Seed it directly from the column under the playhead.
	if (!m_sweeping) seedLevelsFromCurrentColumn();
	update();
	emit positionChanged(m_position);
}

namespace {
/// The analysis window for the live path. 1024 at 44.1 kHz is ~23 ms, which is
/// responsive enough to track a beat without flickering.
constexpr std::size_t kLiveWindow = 1024;
} // namespace

void VfdSpectrumWidget::setLiveMode(bool live) {
	if (m_liveMode == live) return;
	m_liveMode = live;
	if (live) {
		// The sweep and the live feed are two ways of driving the same display;
		// running both would fight over the level array.
		setSweeping(false);
		m_emptyReason = QStringLiteral("Waiting for audio…");
	}
	update();
}

void VfdSpectrumWidget::rebuildBands(int sampleRateHz) {
	if (sampleRateHz <= 0 || m_bandEdgeRate == sampleRateHz) return;
	m_bandEdgeRate = sampleRateHz;

	const int bands = m_levels.empty() ? 96 : static_cast<int>(m_levels.size());
	const double nyquist = sampleRateHz / 2.0;
	const double lowHz = 30.0;
	const double highHz = std::min(16000.0, nyquist * 0.98);

	m_bandEdges.assign(static_cast<std::size_t>(bands) + 1, 0);
	for (int b = 0; b <= bands; ++b) {
		const double t = static_cast<double>(b) / bands;
		const double hz = lowHz * std::pow(highHz / lowHz, t);
		const double bin = hz * static_cast<double>(kLiveWindow) / sampleRateHz;
		m_bandEdges[static_cast<std::size_t>(b)] = static_cast<std::size_t>(
			std::clamp(bin, 1.0, static_cast<double>(kLiveWindow / 2 - 1)));
	}

	if (m_window.size() != kLiveWindow) {
		m_window.resize(kLiveWindow);
		for (std::size_t i = 0; i < kLiveWindow; ++i) {
			m_window[i] = 0.5f * (1.0f - std::cos(2.0f * 3.14159265358979f
				* static_cast<float>(i) / static_cast<float>(kLiveWindow - 1)));
		}
	}
}

void VfdSpectrumWidget::applyColumn(const std::vector<float>& column, float attack, float release) {
	for (std::size_t i = 0; i < m_levels.size() && i < column.size(); ++i) {
		const float target = column[i];
		float& level = m_levels[i];
		// Fast attack, slow release: how a real meter behaves, and what stops the
		// display flickering between frames.
		level = (target > level)
			? level + (target - level) * attack
			: level + (target - level) * release;

		if (!m_peakHold) continue;
		if (level >= m_peaks[i]) {
			m_peaks[i] = level;
			m_peakAge[i] = 0.0f;
		} else {
			m_peakAge[i] += 0.033f;
			if (m_peakAge[i] > 0.7f) {
				m_peaks[i] = std::max(level, m_peaks[i] - 0.018f);
			}
		}
	}
}

void VfdSpectrumWidget::pushLiveSamples(const std::vector<float>& samples, int sampleRateHz) {
	if (!m_liveMode) return;

	constexpr int kBands = 96;
	if (m_levels.size() != static_cast<std::size_t>(kBands)) {
		m_levels.assign(kBands, 0.0f);
		m_peaks.assign(kBands, 0.0f);
		m_peakAge.assign(kBands, 0.0f);
		m_bandEdgeRate = 0;
	}
	rebuildBands(sampleRateHz);
	m_liveSampleRate = sampleRateHz;

	if (samples.size() < kLiveWindow || m_bandEdges.empty()) {
		// Not enough audio yet; decay towards silence rather than freezing.
		applyColumn(std::vector<float>(static_cast<std::size_t>(kBands), 0.0f), 0.3f, 0.12f);
		update();
		return;
	}

	m_emptyReason.clear();

	// Take the most recent window: the audio nearest the playhead.
	std::vector<float> real(kLiveWindow);
	std::vector<float> imaginary(kLiveWindow, 0.0f);
	const std::size_t offset = samples.size() - kLiveWindow;
	for (std::size_t i = 0; i < kLiveWindow; ++i) {
		real[i] = samples[offset + i] * m_window[i];
	}
	fftRadix2(real, imaginary, false);

	std::vector<float> column(static_cast<std::size_t>(kBands), 0.0f);
	for (int b = 0; b < kBands; ++b) {
		const std::size_t from = m_bandEdges[static_cast<std::size_t>(b)];
		const std::size_t to = std::max(from + 1, m_bandEdges[static_cast<std::size_t>(b) + 1]);

		float peak = 0.0f;
		for (std::size_t bin = from; bin < to && bin < kLiveWindow / 2; ++bin) {
			const float magnitude = std::sqrt(real[bin] * real[bin] + imaginary[bin] * imaginary[bin]);
			peak = std::max(peak, magnitude);
		}
		column[static_cast<std::size_t>(b)] =
			toNormalisedDb(peak * 4.0f / static_cast<float>(kLiveWindow));
	}

	applyColumn(column, 0.55f, 0.16f);
	update();
}

void VfdSpectrumWidget::seedLevelsFromCurrentColumn() {
	const std::vector<float>* column = currentColumn();
	if (!column) return;
	for (std::size_t i = 0; i < m_levels.size() && i < column->size(); ++i) {
		m_levels[i] = (*column)[i];
		m_peaks[i] = (*column)[i];
		m_peakAge[i] = 0.0f;
	}
}

const std::vector<float>* VfdSpectrumWidget::currentColumn() const {
	if (!m_spectrogram.valid()) return nullptr;
	const std::size_t count = m_spectrogram.columns.size();
	const std::size_t index = std::min(count - 1,
		static_cast<std::size_t>(m_position * static_cast<double>(count)));
	return &m_spectrogram.columns[index];
}

void VfdSpectrumWidget::advance() {
	if (!m_spectrogram.valid()) {
		m_timer.stop();
		return;
	}

	// The sweep runs at the analysis rate, so a 4-minute track takes 4 minutes to
	// traverse: the display corresponds to real positions in the file.
	const double elapsed = static_cast<double>(m_clock.restart()) / 1000.0;
	const double totalSeconds = static_cast<double>(m_spectrogram.durationMs) / 1000.0;
	if (totalSeconds > 0.0) {
		m_position += elapsed / totalSeconds;
		if (m_position >= 1.0) m_position = 0.0;   // loop
	}

	// Ballistics: fast attack, slow release, as a meter behaves.
	if (const std::vector<float>* column = currentColumn()) {
		for (std::size_t i = 0; i < m_levels.size() && i < column->size(); ++i) {
			const float target = (*column)[i];
			float& level = m_levels[i];
			level = (target > level) ? target : level + (target - level) * 0.28f;

			if (m_peakHold) {
				if (level >= m_peaks[i]) {
					m_peaks[i] = level;
					m_peakAge[i] = 0.0f;
				} else {
					m_peakAge[i] += static_cast<float>(elapsed);
					if (m_peakAge[i] > 0.8f) {
						m_peaks[i] = std::max(level, m_peaks[i] - static_cast<float>(elapsed) * 0.55f);
					}
				}
			}
		}
	}

	emit positionChanged(m_position);
	update();
}

void VfdSpectrumWidget::resizeEvent(QResizeEvent* event) {
	QWidget::resizeEvent(event);
	// Keep the cell grid dense but never sub-pixel.
	m_cellSize = (height() > 260) ? 4 : 3;
	m_cellGap = 1;
}

void VfdSpectrumWidget::mousePressEvent(QMouseEvent* event) {
	const QRect plot = rect().adjusted(38, 8, -8, -20);
	if (!plot.contains(event->pos())) return;
	setPosition(static_cast<double>(event->pos().x() - plot.left()) / std::max(1, plot.width()));
}

void VfdSpectrumWidget::mouseMoveEvent(QMouseEvent* event) {
	if ((event->buttons() & Qt::LeftButton) == 0) return;
	mousePressEvent(event);
}

void VfdSpectrumWidget::paintEvent(QPaintEvent*) {
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing, false);

	// Recessed enclosure, as the design draws it.
	painter.fillRect(rect(), theme::kSurfaceDeepVoid);
	painter.setPen(QPen(theme::kOutlineVariant, 1));
	painter.drawRect(rect().adjusted(0, 0, -1, -1));

	const QRect plot = rect().adjusted(38, 8, -8, -20);
	if (plot.width() <= 8 || plot.height() <= 8) return;

	drawGraticule(painter, plot);

	if (!m_spectrogram.valid() && !(m_liveMode && !m_levels.empty())) {
		drawEmpty(painter, plot);
		drawAxes(painter, plot);
		return;
	}

	drawMatrix(painter, plot);
	drawAxes(painter, plot);
}

void VfdSpectrumWidget::drawEmpty(QPainter& painter, const QRect& plot) const {
	painter.setPen(theme::kTextMuted);
	painter.setFont(theme::monoFont(9));
	painter.drawText(plot, Qt::AlignCenter, m_emptyReason);
}

void VfdSpectrumWidget::drawGraticule(QPainter& painter, const QRect& plot) const {
	// Oscilloscope graticule behind the matrix.
	QColor line = theme::kOutlineVariant;
	line.setAlpha(90);
	painter.setPen(QPen(line, 1, Qt::DotLine));

	for (int i = 1; i < 6; ++i) {
		const int y = plot.top() + plot.height() * i / 6;
		painter.drawLine(plot.left(), y, plot.right(), y);
	}
	for (int i = 1; i < 8; ++i) {
		const int x = plot.left() + plot.width() * i / 8;
		painter.drawLine(x, plot.top(), x, plot.bottom());
	}
}

void VfdSpectrumWidget::drawMatrix(QPainter& painter, const QRect& plot) const {
	const int bands = m_liveMode
		? static_cast<int>(m_levels.size())
		: m_spectrogram.bandCount;
	if (bands <= 0) return;

	const int step = m_cellSize + m_cellGap;
	const int rows = std::max(1, plot.height() / step);
	const int barWidth = std::max(2, plot.width() / bands);
	const int dotWidth = std::max(1, barWidth - m_cellGap);

	for (int b = 0; b < bands; ++b) {
		const float level = (static_cast<std::size_t>(b) < m_levels.size())
			? m_levels[static_cast<std::size_t>(b)] : 0.0f;
		const int litRows = static_cast<int>(std::lround(level * rows));

		const int x = plot.left() + b * barWidth;

		for (int r = 0; r < rows; ++r) {
			const int y = plot.bottom() - (r + 1) * step + m_cellGap;
			const float rowLevel = static_cast<float>(r + 1) / static_cast<float>(rows);

			if (r < litRows) {
				painter.fillRect(x, y, dotWidth, m_cellSize, levelColour(rowLevel));
			} else {
				// Unlit cells stay faintly visible, which is what gives a real VFD
				// its texture rather than a plain black gap.
				QColor unlit = theme::kSplitterMuted;
				unlit.setAlpha(70);
				painter.fillRect(x, y, dotWidth, m_cellSize, unlit);
			}
		}

		// Peak-hold marker.
		if (m_peakHold && static_cast<std::size_t>(b) < m_peaks.size()) {
			const float peak = m_peaks[static_cast<std::size_t>(b)];
			if (peak > 0.02f) {
				const int peakRow = std::min(rows - 1, static_cast<int>(std::lround(peak * rows)));
				const int y = plot.bottom() - (peakRow + 1) * step + m_cellGap;
				painter.fillRect(x, y, dotWidth, m_cellSize, theme::kPrimary);
			}
		}
	}

	// Playhead. In live mode the display is the present moment, so there is no
	// position marker to draw.
	if (m_liveMode) return;
	const int playheadX = plot.left() + static_cast<int>(m_position * plot.width());
	QColor playhead = theme::kNeonPink;
	playhead.setAlpha(150);
	painter.setPen(QPen(playhead, 1));
	painter.drawLine(playheadX, plot.top(), playheadX, plot.bottom());
}

void VfdSpectrumWidget::drawAxes(QPainter& painter, const QRect& plot) const {
	painter.setFont(theme::monoFont(7));

	// Y axis: dB markers.
	painter.setPen(theme::kTextMuted);
	const int decibels[] = {0, -12, -24, -36, -48, -60, -72};
	for (int db : decibels) {
		const float normalised = (static_cast<float>(db) - kFloorDb) / (kCeilingDb - kFloorDb);
		const int y = plot.bottom() - static_cast<int>(normalised * plot.height());
		painter.drawText(QRect(2, y - 6, 32, 12), Qt::AlignRight | Qt::AlignVCenter,
			QString::number(db));
	}

	// X axis: frequency calibration, matching the design's labels.
	const int rate = m_liveMode ? m_liveSampleRate : m_spectrogram.sampleRateHz;
	if (rate <= 0) return;

	const double lowHz = 30.0;
	const double highHz = std::min(16000.0, rate / 2.0 * 0.98);
	const struct { double hz; const char* label; } marks[] = {
		{31, "31"}, {62, "62"}, {125, "125"}, {250, "250"}, {500, "500"},
		{1000, "1k"}, {2000, "2k"}, {4000, "4k"}, {8000, "8k"}, {16000, "16k"},
	};
	for (const auto& mark : marks) {
		if (mark.hz > highHz) continue;
		const double t = std::log(mark.hz / lowHz) / std::log(highHz / lowHz);
		const int x = plot.left() + static_cast<int>(t * plot.width());
		painter.drawText(QRect(x - 16, plot.bottom() + 3, 32, 12), Qt::AlignCenter,
			QString::fromLatin1(mark.label));
	}
}

} // namespace ml::desktop

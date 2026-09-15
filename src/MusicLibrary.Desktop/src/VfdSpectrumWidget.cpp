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
		// Interpolate in float and round once, rather than letting int/float
		// arithmetic mix implicitly on every channel.
		const auto channel = [t](int from, int to) {
			const float value = static_cast<float>(from)
				+ (static_cast<float>(to) - static_cast<float>(from)) * t;
			return static_cast<int>(std::lround(value));
		};
		return QColor(channel(a.red(), b.red()), channel(a.green(), b.green()),
			channel(a.blue(), b.blue()));
	};

	if (normalised < 0.55f) return mix(theme::kNeonCyan, theme::kNeonPurple, normalised / 0.55f);
	if (normalised < 0.85f) return mix(theme::kNeonPurple, theme::kNeonPink, (normalised - 0.55f) / 0.30f);
	return mix(theme::kNeonPink, theme::kOverloadRed, (normalised - 0.85f) / 0.15f);
}

/// A dim ghost of the row's own colour for its unlit cells, the way a real VFD
/// shows the whole gradient at low brightness rather than a blank gap.
QColor darken(const QColor& colour, float factor) {
	factor = std::clamp(factor, 0.0f, 1.0f);
	return QColor(static_cast<int>(static_cast<float>(colour.red()) * factor),
		static_cast<int>(static_cast<float>(colour.green()) * factor),
		static_cast<int>(static_cast<float>(colour.blue()) * factor));
}

/// Distributes `totalBins` FFT bins across `bands` bars so each bar gets a
/// distinct, non-overlapping, non-empty range: the bare minimum of 1 bin
/// where bins are scarce, more where they are abundant.
///
/// Ported from Winamp Classic's own LogBarValueTable algorithm
/// (WACUP/vis_classic, LogBarTable.cpp — found via the actual reference
/// implementation). Picking each bar's range by rounding a log-spaced
/// *frequency position* to a bin index — the previous approach here — can
/// round two neighbouring bars to the identical bin where bins are scarce
/// (the low end), making them read the same data and show the same height:
/// a structural cause of the "staircase" artefact that persisted even after
/// zero-padding the FFT for finer bin spacing. A cumulative bin-*count*
/// distribution cannot produce that, because each bar's range starts
/// exactly where the previous one's ended: bar 0 gets bin [0], bar 1 gets
/// bin [1], and so on, with the surplus bins handed out from the high end
/// down in shrinking chunks (there are always far more bins available up
/// there) so the low end never has to share.
std::vector<std::size_t> assignBinCounts(std::size_t totalBins, int bands) {
	std::vector<std::size_t> counts(static_cast<std::size_t>(std::max(bands, 0)), 1);
	if (bands <= 0 || static_cast<std::size_t>(bands) >= totalBins) return counts;

	std::int64_t notAssigned = static_cast<std::int64_t>(totalBins) - bands;
	const double div = std::pow(static_cast<double>(notAssigned), 1.0 / bands);

	const auto assignCount = [div](std::int64_t remaining) -> std::int64_t {
		const std::int64_t n = static_cast<std::int64_t>(
			static_cast<double>(remaining) - static_cast<double>(remaining) / div + 0.5);
		return n <= 0 ? std::int64_t{1} : n;
	};

	std::int64_t assign = assignCount(notAssigned);
	while (notAssigned > 0) {
		for (int w = bands - 1; w >= 0 && notAssigned > 0; --w) {
			counts[static_cast<std::size_t>(w)] += static_cast<std::size_t>(assign);
			notAssigned -= assign;
			assign = assignCount(notAssigned);
		}
	}
	return counts;
}

/// Turns a set of per-bar bin counts into cumulative edges starting at
/// `lowBin`, so band b covers [edges[b], edges[b+1]).
std::vector<std::size_t> cumulativeEdges(std::size_t lowBin, const std::vector<std::size_t>& counts) {
	std::vector<std::size_t> edges(counts.size() + 1);
	edges[0] = lowBin;
	for (std::size_t b = 0; b < counts.size(); ++b) edges[b + 1] = edges[b] + counts[b];
	return edges;
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
	// The transform itself runs zero-padded to a much larger size. The real
	// analysis window stays at kWindow samples (unchanged time resolution and
	// coherent gain); padding it out before the transform only interpolates
	// the DFT more finely. That is what keeps the low end of a log-spaced band
	// layout from mapping dozens of adjacent bands onto the same one or two
	// raw bins: kWindow's own resolution (~43 Hz/bin at 44.1 kHz) is far
	// coarser than the band spacing down near 30 Hz.
	constexpr std::size_t kFftSize = 8192;
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

	// Bands span 30 Hz to just under Nyquist, matching the design's 31 Hz ..
	// 16 kHz axis labels — but *which* bins each band reads comes from
	// assignBinCounts's cumulative distribution, not from rounding a
	// log-spaced frequency position per band (see its comment for why).
	const double nyquist = audio.sampleRateHz / 2.0;
	const double lowHz = 30.0;
	const double highHz = std::min(16000.0, nyquist * 0.98);
	const std::size_t lowBin = static_cast<std::size_t>(
		std::clamp(lowHz * kFftSize / audio.sampleRateHz, 1.0, static_cast<double>(kFftSize / 2 - 1)));
	const std::size_t highBin = static_cast<std::size_t>(
		std::clamp(highHz * kFftSize / audio.sampleRateHz,
			static_cast<double>(lowBin + 1), static_cast<double>(kFftSize / 2)));
	const auto edges = cumulativeEdges(lowBin, assignBinCounts(highBin - lowBin, bandCount));

	const std::size_t frames = (audio.samples.size() - kWindow) / kHop + 1;
	// Bound the work: a long DJ set would otherwise produce tens of thousands of
	// columns for a widget a few hundred pixels wide.
	const std::size_t stride = std::max<std::size_t>(1, frames / 4000);

	std::vector<float> real(kFftSize, 0.0f);
	std::vector<float> imaginary(kFftSize, 0.0f);

	for (std::size_t frame = 0; frame < frames; frame += stride) {
		const std::size_t offset = frame * kHop;
		// Reset fully: fftRadix2 transforms the whole padded buffer in place,
		// so the previous frame's output lingers past kWindow unless cleared.
		std::fill(real.begin(), real.end(), 0.0f);
		std::fill(imaginary.begin(), imaginary.end(), 0.0f);
		for (std::size_t i = 0; i < kWindow; ++i) {
			real[i] = audio.samples[offset + i] * window[i];
		}
		fftRadix2(real, imaginary, false);

		std::vector<float> column(static_cast<std::size_t>(bandCount), 0.0f);
		for (int b = 0; b < bandCount; ++b) {
			const std::size_t from = edges[static_cast<std::size_t>(b)];
			const std::size_t to = std::max(from + 1, edges[static_cast<std::size_t>(b) + 1]);

			// Peak within the band reads better on a dot matrix than a mean: a
			// narrow tone stays visible instead of being averaged into the floor.
			float peak = 0.0f;
			for (std::size_t bin = from; bin < to && bin < kFftSize / 2; ++bin) {
				const float magnitude = std::sqrt(real[bin] * real[bin] + imaginary[bin] * imaginary[bin]);
				peak = std::max(peak, magnitude);
			}
			// Normalise by the window's coherent gain so full scale is ~1.0.
			// This uses kWindow, not kFftSize: zero padding adds no energy, it
			// only adds interpolated points between the real bins.
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
/// Zero-padded transform size — see the comment in buildSpectrogram() for why
/// this needs to be much larger than kLiveWindow.
constexpr std::size_t kLiveFftSize = 8192;
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

void VfdSpectrumWidget::rebuildBands(int sampleRateHz, int bands) {
	if (sampleRateHz <= 0 || bands <= 0) return;
	if (m_bandEdgeRate == sampleRateHz && m_bandEdgeCount == bands) return;
	m_bandEdgeRate = sampleRateHz;
	m_bandEdgeCount = bands;

	const double nyquist = sampleRateHz / 2.0;
	const double lowHz = 30.0;
	const double highHz = std::min(16000.0, nyquist * 0.98);
	const std::size_t lowBin = static_cast<std::size_t>(std::clamp(
		lowHz * static_cast<double>(kLiveFftSize) / sampleRateHz, 1.0,
		static_cast<double>(kLiveFftSize / 2 - 1)));
	const std::size_t highBin = static_cast<std::size_t>(std::clamp(
		highHz * static_cast<double>(kLiveFftSize) / sampleRateHz,
		static_cast<double>(lowBin + 1), static_cast<double>(kLiveFftSize / 2)));
	m_bandEdges = cumulativeEdges(lowBin, assignBinCounts(highBin - lowBin, bands));

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

	// One band per bar, one bar per two pixels (1 px bar + 1 px gap), so the
	// live path never has to stretch a coarser set of bands across more
	// columns than it has data for — that stretching is what previously
	// showed as a blockier, wider-barred display than the design calls for.
	// Recomputed on every call rather than cached against a resize signal:
	// cheap next to the FFT below, and it keeps a live resize in sync.
	const int plotWidth = std::max(0, width() - 46);
	const int kBands = std::max(8, plotWidth / 2);
	if (m_levels.size() != static_cast<std::size_t>(kBands)) {
		m_levels.assign(static_cast<std::size_t>(kBands), 0.0f);
		m_peaks.assign(static_cast<std::size_t>(kBands), 0.0f);
		m_peakAge.assign(static_cast<std::size_t>(kBands), 0.0f);
	}
	rebuildBands(sampleRateHz, kBands);
	m_liveSampleRate = sampleRateHz;

	if (samples.size() < kLiveWindow || m_bandEdges.empty()) {
		// Not enough audio yet; decay towards silence rather than freezing.
		applyColumn(std::vector<float>(static_cast<std::size_t>(kBands), 0.0f), 0.3f, 0.12f);
		update();
		return;
	}

	m_emptyReason.clear();

	// Take the most recent window: the audio nearest the playhead. Zero-padded
	// to kLiveFftSize before the transform (see buildSpectrogram()'s comment).
	std::vector<float> real(kLiveFftSize, 0.0f);
	std::vector<float> imaginary(kLiveFftSize, 0.0f);
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
		for (std::size_t bin = from; bin < to && bin < kLiveFftSize / 2; ++bin) {
			const float magnitude = std::sqrt(real[bin] * real[bin] + imaginary[bin] * imaginary[bin]);
			peak = std::max(peak, magnitude);
		}
		// kWindow (not kFftSize): zero padding adds no energy, only interpolated
		// points between the real bins.
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

	// Each row's colour depends only on its height, not on which bar it is in,
	// so compute it once per row rather than once per cell.
	std::vector<QColor> litColours(static_cast<std::size_t>(rows));
	std::vector<QColor> unlitColours(static_cast<std::size_t>(rows));
	for (int r = 0; r < rows; ++r) {
		const float rowLevel = static_cast<float>(r + 1) / static_cast<float>(rows);
		const QColor colour = levelColour(rowLevel);
		litColours[static_cast<std::size_t>(r)] = colour;
		// The unlit grid mirrors the same gradient, darkened, rather than a flat
		// grey: that ghost of the full colour range is what gives a real VFD its
		// texture instead of a plain gap.
		unlitColours[static_cast<std::size_t>(r)] = darken(colour, 0.22f);
	}

	// Classic VFD look: every bar is exactly one pixel wide with a one pixel
	// gap, so the bar count is however many columns fit the plot rather than a
	// fixed band count. The underlying analysis has far fewer distinct bands
	// (bounded by FFT resolution — see buildSpectrogram()), so neighbouring
	// columns interpolate between them, the way a dense display smooths a
	// coarser set of frequency bins.
	constexpr int kBarStep = 2;   // 1 px bar + 1 px gap
	const int barCount = std::max(1, plot.width() / kBarStep);

	for (int i = 0; i < barCount; ++i) {
		const float sourcePos = (barCount > 1 && bands > 1)
			? static_cast<float>(i) / static_cast<float>(barCount - 1) * static_cast<float>(bands - 1)
			: 0.0f;
		const int band = std::clamp(static_cast<int>(std::lround(sourcePos)), 0, bands - 1);

		const float level = (static_cast<std::size_t>(band) < m_levels.size())
			? m_levels[static_cast<std::size_t>(band)] : 0.0f;
		const int litRows = std::clamp(
			static_cast<int>(std::lround(level * static_cast<float>(rows))), 0, rows);

		const int x = plot.left() + i * kBarStep;

		for (int r = 0; r < rows; ++r) {
			const int y = plot.bottom() - (r + 1) * step + m_cellGap;
			const QColor& colour = (r < litRows) ? litColours[static_cast<std::size_t>(r)]
												  : unlitColours[static_cast<std::size_t>(r)];
			painter.fillRect(x, y, 1, m_cellSize, colour);
		}

		// Peak-hold marker. Coloured from the same per-row gradient as the bar
		// itself (not a fixed accent colour): the falling-off cap should read
		// as part of that bar, not as an unrelated highlight sitting on top.
		if (m_peakHold && static_cast<std::size_t>(band) < m_peaks.size()) {
			const float peak = m_peaks[static_cast<std::size_t>(band)];
			if (peak > 0.02f) {
				const int peakRow = std::clamp(
					static_cast<int>(std::lround(peak * static_cast<float>(rows))), 0, rows - 1);
				const int y = plot.bottom() - (peakRow + 1) * step + m_cellGap;
				painter.fillRect(x, y, 1, m_cellSize, litColours[static_cast<std::size_t>(peakRow)]);
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
		const int y = plot.bottom()
			- static_cast<int>(normalised * static_cast<float>(plot.height()));
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

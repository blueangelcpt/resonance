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

// The Y axis is labelled in dB purely as evenly-spaced tick text (the seven
// marks below are exactly evenly spaced in [0,1] because the dB values
// themselves are evenly spaced). Bar height no longer comes from a dB
// conversion — see applyColumn and the auto-gain comment on m_autoGainCeiling
// — so these two constants exist only to place those ticks.
constexpr float kFloorDb = -72.0f;
constexpr float kCeilingDb = 0.0f;

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

/// The raw bin count backing `bars` displayed bars: the smallest power of two
/// no less than 512 with at least twice as many bins as bars, so every bar
/// keeps its own bins even at the widest window.
///
/// Ported from the reference analyser's own sizing rule (WACUP/vis_classic,
/// Vis_Satan.cpp CalculateFFTVariables): it re-derives the FFT size from the
/// bar count the same way, rather than fixing a resolution up front. That
/// matters because the transform this feeds is zero-padded from a much
/// shorter real window (see ensureEnvelope) — too few output bins for the bar
/// count reintroduces the staircasing that assignBinCounts otherwise
/// prevents; too many buys nothing but noisier magnitudes for no visible gain.
std::size_t liveFftFrequencyCount(int bars) {
	std::size_t n = 512;
	while (n < 65536 && static_cast<std::size_t>(std::max(bars, 0)) > n / 2) n <<= 1;
	return n;
}

/// A per-bin boost curve, log-shaped and increasing with frequency.
///
/// Ported from the reference analyser's own FFT class (WACUP/vis_classic,
/// FFTNullsoft/fft.cpp, InitEqualizeTable): real music's energy is naturally
/// concentrated in the bass, so a display of raw magnitude alone reads as
/// "mostly dead above the midrange" no matter how the bars are grouped. This
/// curve compensates before any bar ever sees a magnitude value, the same
/// point the reference applies it.
std::vector<float> computeEqualizeTable(std::size_t frequencyCount) {
	std::vector<float> table(frequencyCount, 1.0f);
	if (frequencyCount == 0) return table;
	double bias = 0.04;
	const double n = static_cast<double>(frequencyCount);
	for (std::size_t i = 0; i < frequencyCount; ++i) {
		const double invHalf = (9.0 - bias) / n;
		table[i] = static_cast<float>(std::log10(1.0 + bias + static_cast<double>(i + 1) * invHalf));
		bias /= 1.0025;
	}
	return table;
}

/// A mild raised-cosine taper (power 0.2, not a full Hann): closer to no
/// window at all than to one, trading a little spectral leakage for bins that
/// track a sudden transient more precisely rather than smearing it across
/// neighbours — ported from the reference analyser's own envelope shape and
/// its default power (WACUP/vis_classic, FFTNullsoft/fft.cpp
/// InitEnvelopeTable; Vis_Satan.cpp's fFftEnvelope = 0.2f).
std::vector<float> computeEnvelope(std::size_t samples, float power) {
	std::vector<float> envelope(samples, 1.0f);
	if (samples == 0) return envelope;
	const float mult = 6.28318530717958647692f / static_cast<float>(samples);
	for (std::size_t i = 0; i < samples; ++i) {
		const float raised = 0.5f + 0.5f * std::sin(static_cast<float>(i) * mult - 1.57079632679f);
		envelope[i] = std::pow(raised, power);
	}
	return envelope;
}

} // namespace

// ---------------------------------------------------------------------------
// Spectrogram
// ---------------------------------------------------------------------------

Spectrogram buildSpectrogram(const AnalysisAudio& audio, int bandCount) {
	Spectrogram result;
	if (!audio.valid() || bandCount <= 0) return result;

	// 576 samples and a mild envelope, zero-padded before the transform: the
	// same window the live path uses (see ensureEnvelope's comment) — this
	// view should show the same spectral character as playback, just swept
	// across the whole file instead of following the output live.
	constexpr std::size_t kWindow = 576;
	constexpr std::size_t kHop = 288;
	if (audio.samples.size() < kWindow) return result;

	result.bandCount = bandCount;
	result.sampleRateHz = audio.sampleRateHz;
	result.durationMs = audio.durationMs;
	result.columnRateHz = static_cast<double>(audio.sampleRateHz) / static_cast<double>(kHop);

	const std::vector<float> envelope = computeEnvelope(kWindow, 0.2f);
	const std::size_t frequencyCount = liveFftFrequencyCount(bandCount);
	const std::size_t fftSize = frequencyCount * 2;
	const std::vector<float> equalize = computeEqualizeTable(frequencyCount);

	// Bands span 30 Hz to just under Nyquist, matching the design's 31 Hz ..
	// 16 kHz axis labels — but *which* bins each band reads comes from
	// assignBinCounts's cumulative distribution, not from rounding a
	// log-spaced frequency position per band (see its comment for why).
	const double nyquist = audio.sampleRateHz / 2.0;
	const double lowHz = 30.0;
	const double highHz = std::min(16000.0, nyquist * 0.98);
	const std::size_t lowBin = static_cast<std::size_t>(std::clamp(
		lowHz * static_cast<double>(fftSize) / audio.sampleRateHz, 1.0,
		static_cast<double>(frequencyCount - 1)));
	const std::size_t highBin = static_cast<std::size_t>(std::clamp(
		highHz * static_cast<double>(fftSize) / audio.sampleRateHz,
		static_cast<double>(lowBin + 1), static_cast<double>(frequencyCount)));
	const auto edges = cumulativeEdges(lowBin, assignBinCounts(highBin - lowBin, bandCount));

	const std::size_t frames = (audio.samples.size() - kWindow) / kHop + 1;
	// Bound the work: a long DJ set would otherwise produce tens of thousands of
	// columns for a widget a few hundred pixels wide.
	const std::size_t stride = std::max<std::size_t>(1, frames / 4000);

	std::vector<float> real(fftSize, 0.0f);
	std::vector<float> imaginary(fftSize, 0.0f);
	float globalPeak = 0.0f;

	for (std::size_t frame = 0; frame < frames; frame += stride) {
		const std::size_t offset = frame * kHop;
		// Reset fully: fftRadix2 transforms the whole padded buffer in place,
		// so the previous frame's output lingers past kWindow unless cleared.
		std::fill(real.begin(), real.end(), 0.0f);
		std::fill(imaginary.begin(), imaginary.end(), 0.0f);
		for (std::size_t i = 0; i < kWindow; ++i) {
			real[i] = audio.samples[offset + i] * envelope[i];
		}
		fftRadix2(real, imaginary, false);

		std::vector<float> column(static_cast<std::size_t>(bandCount), 0.0f);
		for (int b = 0; b < bandCount; ++b) {
			const std::size_t from = edges[static_cast<std::size_t>(b)];
			const std::size_t to = std::max(from + 1, edges[static_cast<std::size_t>(b) + 1]);

			// The reference analyser's default: the *average* equalised
			// magnitude across the band's bins, not its loudest bin — see the
			// comment on m_autoGainCeiling for why a bare magnitude isn't the
			// final value yet (that normalisation happens in the pass below).
			float sum = 0.0f;
			std::size_t count = 0;
			for (std::size_t bin = from; bin < to && bin < frequencyCount; ++bin) {
				const float magnitude = std::sqrt(real[bin] * real[bin] + imaginary[bin] * imaginary[bin]);
				sum += magnitude * equalize[bin];
				++count;
			}
			const float value = count > 0 ? sum / static_cast<float>(count) : 0.0f;
			column[static_cast<std::size_t>(b)] = value;
			globalPeak = std::max(globalPeak, value);
		}
		result.columns.push_back(std::move(column));
	}

	// Normalise against this file's own loudest moment rather than a fixed
	// constant, for the same reason pushLiveSamples auto-gains: there is no
	// single absolute scale that is "full" for every track.
	if (globalPeak > 1e-9f) {
		for (auto& column : result.columns) {
			for (float& value : column) value = std::clamp(value / globalPeak, 0.0f, 1.0f);
		}
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
/// The live path's analysis window: the reference analyser's own input size
/// (576 samples handed to it per call, at whatever rate its host delivers
/// audio buffers — see Vis_Satan.cpp's AtAnStDirectRender). ~13 ms at 44.1 kHz.
constexpr std::size_t kLiveWindow = 576;
} // namespace

void VfdSpectrumWidget::ensureEnvelope() {
	if (m_envelope.size() == kLiveWindow) return;
	m_envelope = computeEnvelope(kLiveWindow, 0.2f);
}

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

	const std::size_t frequencyCount = liveFftFrequencyCount(bands);
	m_liveFftSize = frequencyCount * 2;
	m_fftReal.assign(m_liveFftSize, 0.0f);
	m_fftImag.assign(m_liveFftSize, 0.0f);
	m_equalize = computeEqualizeTable(frequencyCount);

	const double nyquist = sampleRateHz / 2.0;
	const double lowHz = 30.0;
	const double highHz = std::min(16000.0, nyquist * 0.98);
	const std::size_t lowBin = static_cast<std::size_t>(std::clamp(
		lowHz * static_cast<double>(m_liveFftSize) / sampleRateHz, 1.0,
		static_cast<double>(frequencyCount - 1)));
	const std::size_t highBin = static_cast<std::size_t>(std::clamp(
		highHz * static_cast<double>(m_liveFftSize) / sampleRateHz,
		static_cast<double>(lowBin + 1), static_cast<double>(frequencyCount)));
	m_bandEdges = cumulativeEdges(lowBin, assignBinCounts(highBin - lowBin, bands));
}

// The reference analyser's own bar ballistics (WACUP/vis_classic,
// Vis_Satan.cpp AtAnStDirectRender): every render call, each bar's level is
// decremented by a fixed "falloff rate" out of 255; a louder reading then
// simply overwrites that decayed value outright rather than easing towards
// it. There is no eased attack at all — that is what makes a kick drum or a
// vocal's pitch change snap straight to its height instead of arriving a
// frame or two late through a smoothing ramp.
//
// kFalloffPerSecond below is that same behaviour re-expressed as a rate
// independent of call frequency: the reference's default falloff is 12 (out
// of 255) applied once per ~576-sample buffer at 44.1 kHz, i.e. every
// 576 / 44100 ≈ 13.06 ms — (12 / 255) / 0.01306 ≈ 3.6 full scales per second.
// Confirmed directly rather than assumed: 12 is what the user's own Winamp
// profile has "Frequency Bar Fall-off" set to.
constexpr float kFalloffPerSecond = 3.6f;

// One reference render call, in seconds — the same 576 samples at 44.1 kHz
// used to derive kFalloffPerSecond above, reused below to convert the peak
// indicator's own frame-counted settings to real time the same way.
constexpr float kReferenceFrameSeconds = 576.0f / 44100.0f;   // ≈ 13.06 ms

// The reference's peak indicator (WACUP/vis_classic, Vis_Satan.cpp
// PeakLevelFall — the effect this display's peak marker matches: hold, then
// fall at a constant rate, rather than PeakLevelNormal's hold-then-teleport
// or the other effects' curves): holds the peak flat for
// "Peak Indicator Change Rate" render calls, then falls linearly at a fixed
// 5-out-of-255-per-call rate (fall_speed, not a user setting — the same for
// everyone). The user's own profile has that rate set to 100.
//   hold:  100 calls * 13.06 ms/call                  ≈ 1.306 s
//   fall:  (5 / 255) / 0.01306 s                       ≈ 1.50 full scales/s
constexpr float kPeakHoldSeconds = 100.0f * kReferenceFrameSeconds;
constexpr float kPeakFallPerSecond = (5.0f / 255.0f) / kReferenceFrameSeconds;

void VfdSpectrumWidget::applyColumn(const std::vector<float>& column, double dtSeconds) {
	const float decay = kFalloffPerSecond * static_cast<float>(std::clamp(dtSeconds, 0.0, 0.25));
	for (std::size_t i = 0; i < m_levels.size() && i < column.size(); ++i) {
		const float target = column[i];
		float& level = m_levels[i];
		level = std::max(0.0f, level - decay);
		if (target > level) level = target;

		if (!m_peakHold) continue;
		if (level >= m_peaks[i]) {
			m_peaks[i] = level;
			m_peakAge[i] = 0.0f;
		} else {
			m_peakAge[i] += static_cast<float>(dtSeconds);
			if (m_peakAge[i] > kPeakHoldSeconds) {
				m_peaks[i] = std::max(level,
					m_peaks[i] - static_cast<float>(dtSeconds) * kPeakFallPerSecond);
			}
		}
	}
}

void VfdSpectrumWidget::pushLiveSamples(const std::vector<float>& samples, int sampleRateHz) {
	if (!m_liveMode) return;

	// A real elapsed time, not an assumed frame length: the caller's timer
	// interval is a target, not a guarantee, and after a pause or a stall the
	// gap could be much larger. Clamped inside applyColumn either way.
	double dtSeconds = 1.0 / 60.0;
	if (m_liveClock.isValid()) {
		dtSeconds = static_cast<double>(m_liveClock.restart()) / 1000.0;
	} else {
		m_liveClock.start();
	}

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
	ensureEnvelope();
	m_liveSampleRate = sampleRateHz;

	if (samples.size() < kLiveWindow || m_bandEdges.empty()) {
		// Not enough audio yet; decay towards silence rather than freezing.
		applyColumn(std::vector<float>(static_cast<std::size_t>(kBands), 0.0f), dtSeconds);
		update();
		return;
	}

	m_emptyReason.clear();

	// Take the most recent window: the audio nearest the playhead, tapered
	// and zero-padded to m_liveFftSize before the transform (see
	// liveFftFrequencyCount's comment for why that size tracks band count).
	std::fill(m_fftReal.begin(), m_fftReal.end(), 0.0f);
	std::fill(m_fftImag.begin(), m_fftImag.end(), 0.0f);
	const std::size_t offset = samples.size() - kLiveWindow;
	for (std::size_t i = 0; i < kLiveWindow; ++i) {
		m_fftReal[i] = samples[offset + i] * m_envelope[i];
	}
	fftRadix2(m_fftReal, m_fftImag, false);

	const std::size_t frequencyCount = m_liveFftSize / 2;
	std::vector<float> column(static_cast<std::size_t>(kBands), 0.0f);
	float pushPeak = 0.0f;
	for (int b = 0; b < kBands; ++b) {
		const std::size_t from = m_bandEdges[static_cast<std::size_t>(b)];
		const std::size_t to = std::max(from + 1, m_bandEdges[static_cast<std::size_t>(b) + 1]);

		// The average equalised magnitude across the band's bins (the
		// reference analyser's own default combine mode — see
		// computeEqualizeTable's comment for the equalise step), not its
		// loudest bin: averaging over a coarser, un-padded set of bins is
		// what makes the reference read as tracking the music's actual energy
		// rather than flickering on whichever single bin is momentarily loud.
		float sum = 0.0f;
		std::size_t count = 0;
		for (std::size_t bin = from; bin < to && bin < frequencyCount; ++bin) {
			const float magnitude = std::sqrt(
				m_fftReal[bin] * m_fftReal[bin] + m_fftImag[bin] * m_fftImag[bin]);
			sum += magnitude * m_equalize[bin];
			++count;
		}
		const float value = count > 0 ? sum / static_cast<float>(count) : 0.0f;
		column[static_cast<std::size_t>(b)] = value;
		pushPeak = std::max(pushPeak, value);
	}

	// Auto-gain: eases towards a louder push over ~120 ms rather than jumping
	// to it in one tick, and relaxes over a ~4 s half-life otherwise, so the
	// ceiling tracks the track's overall loudness without either pinning
	// quiet passages at zero or clipping every bar flat during a loud one.
	// See m_autoGainCeiling's comment for why this exists instead of a fixed
	// reference the way the original plugin uses one.
	//
	// The eased attack matters here in a way it doesn't for individual bars
	// (which the reference analyser itself snaps instantly — see
	// applyColumn): every bar divides by this *one shared* ceiling, so an
	// instant ceiling meant an instant, single noisy bin in any one band
	// would dim every other bar in the same tick, then let them all brighten
	// back together as it relaxed — a coordinated flicker across the whole
	// display that has nothing to do with the music, and was the actual
	// source of "jittery" once real-time updates exposed it. A short ease
	// absorbs a one-tick spike without meaningfully lagging a real, sustained
	// increase in loudness.
	const float dt = static_cast<float>(dtSeconds);
	if (pushPeak > m_autoGainCeiling) {
		const float attack = 1.0f - std::pow(0.5f, dt / 0.12f);
		m_autoGainCeiling += (pushPeak - m_autoGainCeiling) * attack;
	} else {
		m_autoGainCeiling *= std::pow(0.5f, dt / 4.0f);
	}
	m_autoGainCeiling = std::max(m_autoGainCeiling, 1e-6f);
	for (float& value : column) value = std::clamp(value / m_autoGainCeiling, 0.0f, 1.0f);

	applyColumn(column, dtSeconds);
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

	// Same ballistics as the live path (see applyColumn): instant attack, then
	// a fixed-rate linear falloff, rather than an eased release.
	if (const std::vector<float>* column = currentColumn()) {
		const float decay = kFalloffPerSecond * static_cast<float>(std::clamp(elapsed, 0.0, 0.25));
		for (std::size_t i = 0; i < m_levels.size() && i < column->size(); ++i) {
			const float target = (*column)[i];
			float& level = m_levels[i];
			level = std::max(0.0f, level - decay);
			if (target > level) level = target;

			if (m_peakHold) {
				if (level >= m_peaks[i]) {
					m_peaks[i] = level;
					m_peakAge[i] = 0.0f;
				} else {
					m_peakAge[i] += static_cast<float>(elapsed);
					if (m_peakAge[i] > kPeakHoldSeconds) {
						m_peaks[i] = std::max(level,
							m_peaks[i] - static_cast<float>(elapsed) * kPeakFallPerSecond);
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
	std::vector<QRgb> litColours(static_cast<std::size_t>(rows));
	std::vector<QRgb> unlitColours(static_cast<std::size_t>(rows));
	for (int r = 0; r < rows; ++r) {
		const float rowLevel = static_cast<float>(r + 1) / static_cast<float>(rows);
		const QColor colour = levelColour(rowLevel);
		// .rgb() (alpha forced to 0xff) is already valid premultiplied data for
		// a fully opaque colour, which every lit/unlit cell is.
		litColours[static_cast<std::size_t>(r)] = colour.rgb();
		// The unlit grid mirrors the same gradient, darkened, rather than a flat
		// grey: that ghost of the full colour range is what gives a real VFD its
		// texture instead of a plain gap.
		unlitColours[static_cast<std::size_t>(r)] = darken(colour, 0.22f).rgb();
	}

	// Classic VFD look: every bar is exactly one pixel wide with a one pixel
	// gap, so the bar count is however many columns fit the plot rather than a
	// fixed band count. The underlying analysis has far fewer distinct bands
	// (bounded by FFT resolution — see buildSpectrogram()), so neighbouring
	// columns interpolate between them, the way a dense display smooths a
	// coarser set of frequency bins.
	constexpr int kBarStep = 2;   // 1 px bar + 1 px gap
	const int barCount = std::max(1, plot.width() / kBarStep);

	// Per-bar band index and lit-row count, computed once rather than inside
	// the row loop below.
	std::vector<int> barBand(static_cast<std::size_t>(barCount));
	std::vector<int> barLitRows(static_cast<std::size_t>(barCount));
	for (int i = 0; i < barCount; ++i) {
		const float sourcePos = (barCount > 1 && bands > 1)
			? static_cast<float>(i) / static_cast<float>(barCount - 1) * static_cast<float>(bands - 1)
			: 0.0f;
		const int band = std::clamp(static_cast<int>(std::lround(sourcePos)), 0, bands - 1);
		barBand[static_cast<std::size_t>(i)] = band;

		const float level = (static_cast<std::size_t>(band) < m_levels.size())
			? m_levels[static_cast<std::size_t>(band)] : 0.0f;
		barLitRows[static_cast<std::size_t>(i)] = std::clamp(
			static_cast<int>(std::lround(level * static_cast<float>(rows))), 0, rows);
	}

	// Composited once at the end rather than one QPainter::fillRect call per
	// cell (rows * barCount of them — tens of thousands at typical widget
	// widths, and each carries far more overhead than the single pixel-ish
	// write it does). Direct scanline writes into an offscreen image, blitted
	// in one drawImage call, is what actually keeps this at frame rate.
	// Format_ARGB32_Premultiplied, filled transparent: gap pixels (the 1 px
	// bar/row spacing) stay see-through so the graticule underneath still
	// shows, exactly as the fillRect version left them untouched.
	QImage frame(plot.width(), plot.height(), QImage::Format_ARGB32_Premultiplied);
	frame.fill(0);

	const int imageHeight = frame.height();
	const int imageWidth = frame.width();
	for (int r = 0; r < rows; ++r) {
		const int cellTop = imageHeight - 1 - (r + 1) * step + m_cellGap;
		const int scanFirst = std::max(0, cellTop);
		const int scanLast = std::min(imageHeight - 1, cellTop + m_cellSize - 1);
		if (scanFirst > scanLast) continue;

		const QRgb lit = litColours[static_cast<std::size_t>(r)];
		const QRgb unlit = unlitColours[static_cast<std::size_t>(r)];
		for (int sy = scanFirst; sy <= scanLast; ++sy) {
			auto* line = reinterpret_cast<QRgb*>(frame.scanLine(sy));
			for (int i = 0; i < barCount; ++i) {
				const int x = i * kBarStep;
				if (x >= imageWidth) break;
				line[x] = (r < barLitRows[static_cast<std::size_t>(i)]) ? lit : unlit;
			}
		}
	}

	// Peak-hold marker. Coloured from the same per-row gradient as the bar
	// itself (not a fixed accent colour): the falling-off cap should read as
	// part of that bar, not as an unrelated highlight sitting on top.
	if (m_peakHold) {
		for (int i = 0; i < barCount; ++i) {
			const std::size_t band = static_cast<std::size_t>(barBand[static_cast<std::size_t>(i)]);
			if (band >= m_peaks.size()) continue;
			const float peak = m_peaks[band];
			if (peak <= 0.02f) continue;

			const int peakRow = std::clamp(
				static_cast<int>(std::lround(peak * static_cast<float>(rows))), 0, rows - 1);
			const QRgb colour = litColours[static_cast<std::size_t>(peakRow)];
			const int cellTop = imageHeight - 1 - (peakRow + 1) * step + m_cellGap;
			const int scanFirst = std::max(0, cellTop);
			const int scanLast = std::min(imageHeight - 1, cellTop + m_cellSize - 1);
			const int x = i * kBarStep;
			if (x >= imageWidth) continue;
			for (int sy = scanFirst; sy <= scanLast; ++sy) {
				reinterpret_cast<QRgb*>(frame.scanLine(sy))[x] = colour;
			}
		}
	}

	painter.drawImage(plot.topLeft(), frame);

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

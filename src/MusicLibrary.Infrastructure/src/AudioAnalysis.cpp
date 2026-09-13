// SPDX-License-Identifier: GPL-3.0-or-later
#include "mlinfra/AudioAnalysis.hpp"
#include "mlinfra/Hashing.hpp"

// MINIMP3_FLOAT_OUTPUT is set on the minimp3 interface target so every
// translation unit sees the same sample type.
#define MINIMP3_IMPLEMENTATION
#include <minimp3.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <numeric>

namespace fs = std::filesystem;

namespace ml {

namespace {

constexpr float kPi = 3.14159265358979323846f;

/// Reads a whole file into memory. Cover art and tags make an MP3 larger than
/// its audio, but a track is still a few tens of megabytes at most.
Result<std::vector<std::uint8_t>> readWholeFile(const fs::path& path) {
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file) {
		return Error{ErrorCode::IoError, "cannot open " + path.string()};
	}
	const std::streamsize size = file.tellg();
	if (size <= 0) {
		return Error{ErrorCode::IoError, "empty file: " + path.string()};
	}
	file.seekg(0, std::ios::beg);

	std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
	if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) {
		return Error{ErrorCode::IoError, "short read on " + path.string()};
	}
	return bytes;
}

/// Linear-interpolating resampler. The analysis path does not need a polyphase
/// filter: onset detection is insensitive to the aliasing this introduces above
/// 5 kHz, and the cost matters across thousands of tracks.
void appendResampled(std::vector<float>& out, const std::vector<float>& mono, int fromRate, int toRate,
	double& phase) {
	if (mono.empty()) return;
	if (fromRate == toRate) {
		out.insert(out.end(), mono.begin(), mono.end());
		return;
	}

	const double step = static_cast<double>(fromRate) / static_cast<double>(toRate);
	while (phase < static_cast<double>(mono.size()) - 1.0) {
		const std::size_t index = static_cast<std::size_t>(phase);
		const float fraction = static_cast<float>(phase - static_cast<double>(index));
		out.push_back(mono[index] * (1.0f - fraction) + mono[index + 1] * fraction);
		phase += step;
	}
	phase -= static_cast<double>(mono.size());
	if (phase < 0.0) phase = 0.0;
}

} // namespace

std::string Mp3Decoder::engineIdentifier() {
	// Pinned revision recorded in native/DEPENDENCIES.md.
	return "minimp3/ea99364";
}

Result<AnalysisAudio> Mp3Decoder::decode(const fs::path& path, DecodeOptions options) {
	auto bytes = readWholeFile(path);
	if (!bytes) return bytes.error();

	mp3dec_t decoder;
	mp3dec_init(&decoder);

	AnalysisAudio audio;
	audio.sampleRateHz = options.targetSampleRateHz;

	std::vector<float> pcm(MINIMP3_MAX_SAMPLES_PER_FRAME);
	std::vector<float> mono;
	mono.reserve(1152);

	const std::uint8_t* data = bytes.value().data();
	std::size_t remaining = bytes.value().size();
	double resamplePhase = 0.0;

	std::int64_t decodedSamples = 0;
	std::int64_t skippedSamples = 0;
	const std::int64_t skipTarget = options.skipMs > 0 ? options.skipMs : 0;
	const std::int64_t maxTarget = options.maxDurationMs > 0 ? options.maxDurationMs : 0;

	while (remaining > 0) {
		mp3dec_frame_info_t info{};
		const int samples = mp3dec_decode_frame(&decoder, data, static_cast<int>(remaining),
			pcm.data(), &info);

		if (info.frame_bytes == 0) break;   // No further syncable frame.

		data += info.frame_bytes;
		remaining -= static_cast<std::size_t>(info.frame_bytes);

		if (samples <= 0) {
			// A skipped frame at the start is normal: tags and junk precede the
			// first sync. Only count it once decoding has begun.
			if (decodedSamples > 0) ++audio.decodeErrors;
			continue;
		}

		if (audio.sourceSampleRateHz == 0) {
			audio.sourceSampleRateHz = info.hz;
			audio.sourceChannels = info.channels;
		}

		// Downmix to mono.
		mono.clear();
		if (info.channels == 1) {
			mono.assign(pcm.begin(), pcm.begin() + samples);
		} else {
			mono.reserve(static_cast<std::size_t>(samples));
			for (int i = 0; i < samples; ++i) {
				float sum = 0.0f;
				for (int c = 0; c < info.channels; ++c) {
					sum += pcm[static_cast<std::size_t>(i * info.channels + c)];
				}
				mono.push_back(sum / static_cast<float>(info.channels));
			}
		}

		decodedSamples += samples;

		// Skip region, measured in source samples.
		if (skipTarget > 0) {
			const std::int64_t skipSamples = (skipTarget * info.hz) / 1000;
			if (skippedSamples < skipSamples) {
				const std::int64_t take = std::min<std::int64_t>(skipSamples - skippedSamples, samples);
				skippedSamples += take;
				if (take >= samples) continue;
				mono.erase(mono.begin(), mono.begin() + static_cast<std::ptrdiff_t>(take));
			}
		}

		appendResampled(audio.samples, mono, info.hz, options.targetSampleRateHz, resamplePhase);

		if (maxTarget > 0) {
			const std::int64_t haveMs = (static_cast<std::int64_t>(audio.samples.size()) * 1000)
				/ options.targetSampleRateHz;
			if (haveMs >= maxTarget) break;
		}
	}

	if (audio.samples.empty()) {
		return Error{ErrorCode::ParseError, "no audio could be decoded from " + path.string()};
	}

	audio.durationMs = (static_cast<std::int64_t>(audio.samples.size()) * 1000)
		/ options.targetSampleRateHz;
	return audio;
}

Result<std::vector<std::int16_t>> Mp3Decoder::decodePcm(const fs::path& path, int& sampleRateOut,
	int& channelsOut, std::int64_t& sampleCountOut) {
	auto bytes = readWholeFile(path);
	if (!bytes) return bytes.error();

	mp3dec_t decoder;
	mp3dec_init(&decoder);

	std::vector<float> pcm(MINIMP3_MAX_SAMPLES_PER_FRAME);
	std::vector<std::int16_t> out;

	const std::uint8_t* data = bytes.value().data();
	std::size_t remaining = bytes.value().size();
	sampleRateOut = 0;
	channelsOut = 0;
	sampleCountOut = 0;

	while (remaining > 0) {
		mp3dec_frame_info_t info{};
		const int samples = mp3dec_decode_frame(&decoder, data, static_cast<int>(remaining),
			pcm.data(), &info);
		if (info.frame_bytes == 0) break;
		data += info.frame_bytes;
		remaining -= static_cast<std::size_t>(info.frame_bytes);
		if (samples <= 0) continue;

		if (sampleRateOut == 0) {
			sampleRateOut = info.hz;
			channelsOut = info.channels;
		} else if (info.hz != sampleRateOut || info.channels != channelsOut) {
			return Error{ErrorCode::Unsupported,
				"stream changes sample rate or channel count mid-file; decoded comparison is not valid"};
		}

		const std::size_t total = static_cast<std::size_t>(samples) * static_cast<std::size_t>(info.channels);
		out.reserve(out.size() + total);
		for (std::size_t i = 0; i < total; ++i) {
			const float clamped = std::clamp(pcm[i], -1.0f, 1.0f);
			out.push_back(static_cast<std::int16_t>(std::lround(clamped * 32767.0f)));
		}
		sampleCountOut += samples;
	}

	if (out.empty()) {
		return Error{ErrorCode::ParseError, "no PCM could be decoded from " + path.string()};
	}
	return out;
}

// ---------------------------------------------------------------------------
// FFT
// ---------------------------------------------------------------------------

void fftRadix2(std::vector<float>& real, std::vector<float>& imaginary, bool inverse) {
	const std::size_t n = real.size();
	if (n == 0 || (n & (n - 1)) != 0 || imaginary.size() != n) return;

	// Bit-reversal permutation.
	for (std::size_t i = 1, j = 0; i < n; ++i) {
		std::size_t bit = n >> 1;
		for (; j & bit; bit >>= 1) j ^= bit;
		j ^= bit;
		if (i < j) {
			std::swap(real[i], real[j]);
			std::swap(imaginary[i], imaginary[j]);
		}
	}

	for (std::size_t length = 2; length <= n; length <<= 1) {
		const float angle = 2.0f * kPi / static_cast<float>(length) * (inverse ? 1.0f : -1.0f);
		const float wReal = std::cos(angle);
		const float wImaginary = std::sin(angle);

		for (std::size_t i = 0; i < n; i += length) {
			float currentReal = 1.0f;
			float currentImaginary = 0.0f;
			for (std::size_t j = 0; j < length / 2; ++j) {
				const std::size_t a = i + j;
				const std::size_t b = i + j + length / 2;

				const float tReal = real[b] * currentReal - imaginary[b] * currentImaginary;
				const float tImaginary = real[b] * currentImaginary + imaginary[b] * currentReal;

				real[b] = real[a] - tReal;
				imaginary[b] = imaginary[a] - tImaginary;
				real[a] += tReal;
				imaginary[a] += tImaginary;

				const float nextReal = currentReal * wReal - currentImaginary * wImaginary;
				currentImaginary = currentReal * wImaginary + currentImaginary * wReal;
				currentReal = nextReal;
			}
		}
	}

	if (inverse) {
		for (std::size_t i = 0; i < n; ++i) {
			real[i] /= static_cast<float>(n);
			imaginary[i] /= static_cast<float>(n);
		}
	}
}

// ---------------------------------------------------------------------------
// TempoAnalyzer
// ---------------------------------------------------------------------------

TempoAnalyzer::TempoAnalyzer(TempoAnalyzerOptions options) : m_options(std::move(options)) {}

std::string TempoAnalyzer::engine() const {
	return "resonance-spectralflux/1 (" + Mp3Decoder::engineIdentifier() + ")";
}

std::string TempoAnalyzer::settingsHash() const {
	return configHash({
		"window=" + std::to_string(m_options.windowSize),
		"hop=" + std::to_string(m_options.hopSize),
		"section=" + std::to_string(m_options.sectionMs),
		"maxSections=" + std::to_string(m_options.maxSections),
		"minBpm=" + std::to_string(m_options.minBpm),
		"maxBpm=" + std::to_string(m_options.maxBpm),
		"variable=" + std::to_string(m_options.variableTempoThreshold),
		"beatless=" + std::to_string(m_options.beatlessThreshold),
		"preferredBpm=" + std::to_string(m_options.preferredBpm),
		"priorSigma=" + std::to_string(m_options.tempoPriorSigmaOctaves),
		"h2=" + std::to_string(m_options.harmonic2Weight),
		"h3=" + std::to_string(m_options.harmonic3Weight),
	});
}

std::vector<float> TempoAnalyzer::onsetEnvelope(const AnalysisAudio& audio, double& frameRateHzOut) const {
	std::vector<float> envelope;
	frameRateHzOut = 0.0;
	if (!audio.valid()) return envelope;

	const std::size_t window = static_cast<std::size_t>(m_options.windowSize);
	const std::size_t hop = static_cast<std::size_t>(m_options.hopSize);
	if (audio.samples.size() < window) return envelope;

	frameRateHzOut = static_cast<double>(audio.sampleRateHz) / static_cast<double>(hop);

	// Hann window, precomputed.
	std::vector<float> hann(window);
	for (std::size_t i = 0; i < window; ++i) {
		hann[i] = 0.5f * (1.0f - std::cos(2.0f * kPi * static_cast<float>(i)
			/ static_cast<float>(window - 1)));
	}

	const std::size_t bins = window / 2 + 1;
	std::vector<float> previousMagnitude(bins, 0.0f);
	std::vector<float> real(window);
	std::vector<float> imaginary(window);

	const std::size_t frames = (audio.samples.size() - window) / hop + 1;
	envelope.reserve(frames);

	for (std::size_t frame = 0; frame < frames; ++frame) {
		const std::size_t offset = frame * hop;
		for (std::size_t i = 0; i < window; ++i) {
			real[i] = audio.samples[offset + i] * hann[i];
			imaginary[i] = 0.0f;
		}
		fftRadix2(real, imaginary, false);

		// Spectral flux: the sum of positive magnitude changes. Only increases
		// count, because a note starting is an onset and a note ending is not.
		float flux = 0.0f;
		for (std::size_t bin = 0; bin < bins; ++bin) {
			const float magnitude = std::sqrt(real[bin] * real[bin] + imaginary[bin] * imaginary[bin]);
			// Log compression keeps a quiet hi-hat visible next to a loud kick.
			const float compressed = std::log1p(magnitude * 100.0f);
			const float difference = compressed - previousMagnitude[bin];
			if (difference > 0.0f) flux += difference;
			previousMagnitude[bin] = compressed;
		}
		envelope.push_back(flux);
	}

	if (envelope.empty()) return envelope;

	// Subtract a local mean so a loud section does not dominate a quiet one,
	// then half-wave rectify.
	const std::size_t smoothing = std::max<std::size_t>(
		1, static_cast<std::size_t>(frameRateHzOut * 0.4));
	std::vector<float> detrended(envelope.size());
	for (std::size_t i = 0; i < envelope.size(); ++i) {
		const std::size_t begin = (i >= smoothing) ? i - smoothing : 0;
		const std::size_t end = std::min(envelope.size(), i + smoothing + 1);
		const float sum = std::accumulate(envelope.begin() + static_cast<std::ptrdiff_t>(begin),
			envelope.begin() + static_cast<std::ptrdiff_t>(end), 0.0f);
		const float mean = sum / static_cast<float>(end - begin);
		detrended[i] = std::max(0.0f, envelope[i] - mean);
	}

	return detrended;
}

double TempoAnalyzer::estimateBpm(const std::vector<float>& envelope, double frameRateHz,
	double& strengthOut) const {
	strengthOut = 0.0;
	if (envelope.size() < 32 || frameRateHz <= 0.0) return 0.0;

	// Lag range in frames for the plausible BPM range.
	const int minLag = std::max(2, static_cast<int>(std::floor(frameRateHz * 60.0 / m_options.maxBpm)));
	const int maxLag = static_cast<int>(std::ceil(frameRateHz * 60.0 / m_options.minBpm));
	if (maxLag <= minLag || static_cast<std::size_t>(maxLag) >= envelope.size() / 2) {
		return 0.0;
	}

	// Mean-centre so autocorrelation measures periodicity, not overall level.
	const double mean = std::accumulate(envelope.begin(), envelope.end(), 0.0)
		/ static_cast<double>(envelope.size());
	std::vector<double> centred(envelope.size());
	for (std::size_t i = 0; i < envelope.size(); ++i) {
		centred[i] = static_cast<double>(envelope[i]) - mean;
	}

	double energy = 0.0;
	for (double v : centred) energy += v * v;
	if (energy <= 1e-9) return 0.0;

	// Comb-filter tempogram with a perceptual tempo prior.
	//
	// Autocorrelation peaks just as strongly at twice the true beat period as at
	// the period itself, so a raw peak pick returns half-tempo on most dance
	// music. Measured against the collection's existing BPM tags, a raw pick
	// agreed on 21% of tracks and was a half or double error on 33%.
	//
	// The standard correction is a Gaussian preference in log-tempo space: human
	// tempo perception clusters around 120 BPM, and a candidate an octave away
	// needs correspondingly stronger evidence to win. This does not forbid slow
	// or fast tempos, it only makes them pay for themselves. Half and double
	// alternatives are still reported, and the policy layer still routes a
	// half/double disagreement with an existing tag to review rather than
	// overwriting it.
	double bestScore = 0.0;
	int bestLag = 0;
	std::vector<double> scores(static_cast<std::size_t>(maxLag + 1), 0.0);

	const auto autocorrelation = [&](int lag) -> double {
		if (lag <= 0 || static_cast<std::size_t>(lag) >= centred.size()) return 0.0;
		double sum = 0.0;
		const std::size_t count = centred.size() - static_cast<std::size_t>(lag);
		for (std::size_t i = 0; i < count; ++i) {
			sum += centred[i] * centred[i + static_cast<std::size_t>(lag)];
		}
		return sum / energy;
	};

	const double preferredBpm = m_options.preferredBpm;
	const double sigma = m_options.tempoPriorSigmaOctaves;

	for (int lag = minLag; lag <= maxLag; ++lag) {
		const double bpm = frameRateHz * 60.0 / lag;

		// The candidate's own periodicity, plus its harmonics: a real beat period
		// repeats at 2x and 3x, so agreement there is corroboration.
		double score = autocorrelation(lag);
		score += m_options.harmonic2Weight * autocorrelation(lag * 2);
		score += m_options.harmonic3Weight * autocorrelation(lag * 3);

		if (score > 0.0 && sigma > 0.0) {
			const double octaves = std::log2(bpm / preferredBpm);
			score *= std::exp(-0.5 * (octaves / sigma) * (octaves / sigma));
		}

		scores[static_cast<std::size_t>(lag)] = score;
		if (score > bestScore) {
			bestScore = score;
			bestLag = lag;
		}
	}

	if (bestLag == 0 || bestScore <= 0.0) return 0.0;

	// Parabolic interpolation around the peak for sub-frame precision, which is
	// what makes a fractional BPM meaningful rather than quantised.
	double refinedLag = bestLag;
	if (bestLag > minLag && bestLag < maxLag) {
		const double y0 = scores[static_cast<std::size_t>(bestLag - 1)];
		const double y1 = scores[static_cast<std::size_t>(bestLag)];
		const double y2 = scores[static_cast<std::size_t>(bestLag + 1)];
		const double denominator = y0 - 2.0 * y1 + y2;
		if (std::abs(denominator) > 1e-12) {
			const double delta = 0.5 * (y0 - y2) / denominator;
			if (std::abs(delta) < 1.0) refinedLag = bestLag + delta;
		}
	}

	strengthOut = bestScore;
	return frameRateHz * 60.0 / refinedLag;
}

TempoAnalysis TempoAnalyzer::analyse(const AnalysisAudio& audio) const {
	TempoAnalysis analysis;
	analysis.engine = engine();
	analysis.settings = settingsHash();

	if (!audio.valid()) {
		analysis.evidence.push_back({"decode_failed", "no analysis audio was produced", false});
		return analysis;
	}

	if (audio.durationMs < 20000) {
		analysis.character = TempoCharacter::TooShort;
		analysis.valid = true;
		analysis.evidence.push_back({"too_short",
			"analysable audio is " + std::to_string(audio.durationMs) + " ms", false});
		return analysis;
	}

	double frameRateHz = 0.0;
	const std::vector<float> envelope = onsetEnvelope(audio, frameRateHz);
	if (envelope.empty() || frameRateHz <= 0.0) {
		analysis.evidence.push_back({"no_envelope", "the onset envelope could not be computed", false});
		return analysis;
	}

	// Beatless check, before any tempo is reported. A track with no periodic
	// onset structure must not receive an invented BPM.
	const double meanOnset = std::accumulate(envelope.begin(), envelope.end(), 0.0)
		/ static_cast<double>(envelope.size());
	if (meanOnset < m_options.beatlessThreshold) {
		analysis.character = TempoCharacter::Beatless;
		analysis.valid = true;
		analysis.evidence.push_back({"low_onset_strength",
			"mean onset strength " + std::to_string(meanOnset) + " is below the beatless threshold", false});
		return analysis;
	}

	// --- Per-section estimates ---------------------------------------------
	const std::size_t framesPerSection = static_cast<std::size_t>(
		frameRateHz * static_cast<double>(m_options.sectionMs) / 1000.0);
	std::vector<double> sectionBpms;
	std::vector<double> sectionStrengths;

	if (framesPerSection >= 64 && envelope.size() > framesPerSection) {
		const std::size_t available = envelope.size() / framesPerSection;
		const std::size_t sections = std::min<std::size_t>(available,
			static_cast<std::size_t>(m_options.maxSections));
		// Spread the sections across the whole track rather than taking the first
		// N, so a long mix is sampled from beginning to end.
		const std::size_t stride = (sections > 1) ? (available - 1) / (sections - 1) : 1;

		for (std::size_t s = 0; s < sections; ++s) {
			const std::size_t begin = s * stride * framesPerSection;
			if (begin + framesPerSection > envelope.size()) break;
			const std::vector<float> section(
				envelope.begin() + static_cast<std::ptrdiff_t>(begin),
				envelope.begin() + static_cast<std::ptrdiff_t>(begin + framesPerSection));

			double strength = 0.0;
			const double bpm = estimateBpm(section, frameRateHz, strength);
			if (bpm >= m_options.minBpm && bpm <= m_options.maxBpm) {
				sectionBpms.push_back(bpm);
				sectionStrengths.push_back(strength);
			}
		}
	}

	// --- Whole-track estimate ------------------------------------------------
	double wholeStrength = 0.0;
	const double wholeBpm = estimateBpm(envelope, frameRateHz, wholeStrength);

	if (sectionBpms.empty() && (wholeBpm < m_options.minBpm || wholeBpm > m_options.maxBpm)) {
		analysis.character = TempoCharacter::Beatless;
		analysis.valid = true;
		analysis.evidence.push_back({"no_periodicity",
			"no periodicity was found within the plausible tempo range", false});
		return analysis;
	}

	analysis.sectionBpms = sectionBpms;
	analysis.sectionsAnalysed = static_cast<int>(sectionBpms.size());

	// --- Consensus and stability ---------------------------------------------
	double consensus = wholeBpm;
	double stability = 0.0;

	if (!sectionBpms.empty()) {
		// The median resists a single mis-tracked section better than the mean.
		std::vector<double> sorted = sectionBpms;
		std::sort(sorted.begin(), sorted.end());
		consensus = sorted[sorted.size() / 2];
		if (sorted.size() % 2 == 0 && sorted.size() >= 2) {
			consensus = (sorted[sorted.size() / 2 - 1] + sorted[sorted.size() / 2]) / 2.0;
		}

		// Agreement: the fraction of sections within tolerance of the consensus,
		// after folding half and double tempo onto it. Reported as an agreement
		// measure, explicitly not as a calibrated probability.
		int agreeing = 0;
		for (double bpm : sectionBpms) {
			const double candidates[3] = {bpm, bpm * 2.0, bpm / 2.0};
			for (double candidate : candidates) {
				if (std::abs(candidate - consensus) / consensus <= m_options.variableTempoThreshold) {
					++agreeing;
					break;
				}
			}
		}
		stability = static_cast<double>(agreeing) / static_cast<double>(sectionBpms.size());

		analysis.evidence.push_back({"section_agreement",
			std::to_string(agreeing) + " of " + std::to_string(sectionBpms.size())
				+ " analysed sections agree with the consensus tempo", agreeing > 0});

		// Sections that disagree even after octave folding mean the tempo changes.
		if (stability < 0.75) {
			analysis.character = TempoCharacter::VariableTempo;
		}
	} else {
		// Only a whole-track estimate. That is weaker evidence, so stability is
		// capped to reflect it rather than claiming agreement that was not tested.
		stability = 0.5;
		analysis.evidence.push_back({"single_estimate",
			"the track was too short to split into sections; only a whole-track estimate exists", false});
	}

	if (analysis.character == TempoCharacter::Unknown) {
		analysis.character = (audio.durationMs >= 15 * 60 * 1000)
			? TempoCharacter::LongForm
			: TempoCharacter::SteadyBeat;
	}

	analysis.bpm = consensus;
	analysis.stability = stability;
	analysis.halfTempo = consensus / 2.0;
	analysis.doubleTempo = consensus * 2.0;
	analysis.valid = true;

	analysis.evidence.push_back({"onset_strength",
		"mean onset strength " + std::to_string(meanOnset), true});
	analysis.evidence.push_back({"peak_strength",
		"normalised autocorrelation peak " + std::to_string(wholeStrength), wholeStrength > 0.1});

	return analysis;
}

} // namespace ml

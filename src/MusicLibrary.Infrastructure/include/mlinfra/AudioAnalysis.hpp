// SPDX-License-Identifier: GPL-3.0-or-later
// BPM-001: local tempo analysis.
//
// Decoding is read-only and produces analysis audio in memory. The MP3 is never
// modified, re-encoded, or written back: this header has no write capability at
// all.
//
// FRD section 3 proposed benchmarking SoundTouch and aubio. This implements a
// spectral-flux onset detector with a comb-filter tempogram instead; see
// docs/adr/0003-tempo-engine.md for the reasoning, the licence consequences and
// the measurement obligation that choice carries.
#pragma once

#include "mlcore/Enrichment.hpp"
#include "mlcore/Types.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ml {

/// Decoded analysis audio: mono, float, at a reduced sample rate.
struct AnalysisAudio {
	std::vector<float> samples;
	int sampleRateHz = 0;
	std::int64_t durationMs = 0;
	int sourceChannels = 0;
	int sourceSampleRateHz = 0;
	/// Frames the decoder rejected. A few are normal at the start of a stream.
	int decodeErrors = 0;

	bool valid() const { return !samples.empty() && sampleRateHz > 0; }
};

struct DecodeOptions {
	/// Analysis rate. 11025 Hz resolves every onset a beat tracker needs while
	/// decoding four times less data than 44100.
	int targetSampleRateHz = 11025;
	/// Stop after this much audio. 0 decodes everything.
	std::int64_t maxDurationMs = 0;
	/// Skip this much from the start.
	std::int64_t skipMs = 0;
};

class Mp3Decoder {
public:
	/// Decodes an MP3 to mono analysis audio. Opens the file read-only.
	static Result<AnalysisAudio> decode(const std::filesystem::path& path, DecodeOptions options = {});

	/// Decodes the full-rate interleaved PCM of a byte range. Used by the
	/// repacking verification path, which must compare decoded samples exactly
	/// rather than compressed bytes (OPT-001).
	static Result<std::vector<std::int16_t>> decodePcm(const std::filesystem::path& path,
		int& sampleRateOut, int& channelsOut, std::int64_t& sampleCountOut);

	static std::string engineIdentifier();
};

/// Tempo-analysis settings.
///
/// At namespace scope so its default member initialisers can be used in a
/// default argument of `TempoAnalyzer` (a nested class cannot).
struct TempoAnalyzerOptions {
	/// Length of each analysed section. Several sections are analysed so a
	/// tempo change can be detected rather than averaged away.
	std::int64_t sectionMs = 30000;
	/// Maximum sections to analyse. Bounds the cost on a two-hour DJ set.
	int maxSections = 8;
	/// Plausible tempo range.
	double minBpm = 50.0;
	double maxBpm = 220.0;
	/// FFT window and hop, in samples at the analysis rate.
	int windowSize = 1024;
	int hopSize = 256;
	/// Relative spread above which sections are called variable-tempo.
	double variableTempoThreshold = 0.06;
	/// Onset-strength floor below which a track is called beatless.
	double beatlessThreshold = 0.015;
	/// Centre of the perceptual tempo prior, in BPM. Autocorrelation cannot
	/// distinguish a beat period from twice that period, so a preference is
	/// required; without one the detector returns half-tempo on most dance music.
	double preferredBpm = 120.0;
	/// Width of that prior, in octaves. Larger values weaken the preference.
	/// 0 disables it, which restores the raw peak pick.
	///
	/// Chosen by sweeping sigma against 120 tracks carrying an existing BPM tag:
	/// 0 gave 38% agreement, 0.5 gave 83%, 0.6 gave 82%, 1.0 gave 77% and 1.3
	/// gave 67%. 0.6 is taken over the marginally better 0.5 because it is the
	/// wider prior -- it forces a genuinely slow or fast track less hard -- and it
	/// had the lower octave-error rate. See docs/adr/0003-tempo-engine.md.
	double tempoPriorSigmaOctaves = 0.6;
	/// Weight given to the candidate period's 2x and 3x harmonics.
	///
	/// Zero by default. Harmonic corroboration sounds principled but measured
	/// worse at every sigma: it also rewards a spurious peak at 1.5x the beat,
	/// which is where the residual errors on this collection came from. At
	/// sigma 0.6 the same corpus scored 82% with no harmonics and 75% with
	/// weights 0.5/0.25.
	double harmonic2Weight = 0.0;
	double harmonic3Weight = 0.0;
};

/// Onset-detection and tempo estimation.
class TempoAnalyzer {
public:
	using Options = TempoAnalyzerOptions;


	explicit TempoAnalyzer(TempoAnalyzerOptions options = {});

	TempoAnalysis analyse(const AnalysisAudio& audio) const;

	/// The onset-strength envelope. Exposed so the desktop review view can draw
	/// it next to a disputed BPM.
	std::vector<float> onsetEnvelope(const AnalysisAudio& audio, double& frameRateHzOut) const;

	/// Estimates tempo from an onset envelope over one section.
	/// Returns 0 when no periodicity stands out.
	double estimateBpm(const std::vector<float>& envelope, double frameRateHz, double& strengthOut) const;

	/// Identifier and settings hash, stored with every result so a change of
	/// engine or settings re-runs rather than silently reusing (JOB-001).
	std::string engine() const;
	std::string settingsHash() const;

	const TempoAnalyzerOptions& options() const { return m_options; }

private:
	TempoAnalyzerOptions m_options;
};

/// In-place radix-2 FFT over interleaved complex data. Exposed for tests.
void fftRadix2(std::vector<float>& real, std::vector<float>& imaginary, bool inverse = false);

} // namespace ml

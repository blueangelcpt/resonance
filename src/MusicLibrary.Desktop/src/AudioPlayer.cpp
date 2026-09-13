// SPDX-License-Identifier: GPL-3.0-or-later
#include "AudioPlayer.hpp"

#include <QAudioDevice>
#include <QMediaDevices>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

#include <minimp3.h>

namespace fs = std::filesystem;

namespace ml::desktop {

namespace {

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

/// Writes one sample in the target format.
void writeSample(std::uint8_t* out, float value, QAudioFormat::SampleFormat format) {
	value = std::clamp(value, -1.0f, 1.0f);
	switch (format) {
		case QAudioFormat::UInt8: {
			const auto v = static_cast<std::uint8_t>(std::lround((value + 1.0f) * 127.5f));
			*out = v;
			break;
		}
		case QAudioFormat::Int16: {
			const auto v = static_cast<std::int16_t>(std::lround(value * 32767.0f));
			std::memcpy(out, &v, sizeof(v));
			break;
		}
		case QAudioFormat::Int32: {
			const auto v = static_cast<std::int32_t>(std::lround(
				static_cast<double>(value) * 2147483647.0));
			std::memcpy(out, &v, sizeof(v));
			break;
		}
		case QAudioFormat::Float:
		default:
			std::memcpy(out, &value, sizeof(value));
			break;
	}
}

int bytesPerSampleFor(QAudioFormat::SampleFormat format) {
	switch (format) {
		case QAudioFormat::UInt8: return 1;
		case QAudioFormat::Int16: return 2;
		case QAudioFormat::Int32: return 4;
		case QAudioFormat::Float: return 4;
		default: return 4;
	}
}

} // namespace

Result<DecodedTrack> decodeForPlayback(const fs::path& path, std::int64_t maxDurationMs) {
	auto bytes = readWholeFile(path);
	if (!bytes) return bytes.error();

	mp3dec_t decoder;
	mp3dec_init(&decoder);

	DecodedTrack track;
	track.path = path;

	std::vector<float> pcm(MINIMP3_MAX_SAMPLES_PER_FRAME);
	const std::uint8_t* data = bytes.value().data();
	std::size_t remaining = bytes.value().size();

	while (remaining > 0) {
		mp3dec_frame_info_t info{};
		const int samples = mp3dec_decode_frame(&decoder, data, static_cast<int>(remaining),
			pcm.data(), &info);
		if (info.frame_bytes == 0) break;

		data += info.frame_bytes;
		remaining -= static_cast<std::size_t>(info.frame_bytes);
		if (samples <= 0) continue;

		if (track.sampleRateHz == 0) {
			track.sampleRateHz = info.hz;
			track.channels = info.channels;
		} else if (info.hz != track.sampleRateHz || info.channels != track.channels) {
			// A stream that changes rate or channel count mid-file cannot be fed
			// to a sink opened with one fixed format. Stop at the change rather
			// than playing the rest at the wrong speed.
			break;
		}

		const std::size_t count = static_cast<std::size_t>(samples)
			* static_cast<std::size_t>(info.channels);
		track.samples.insert(track.samples.end(), pcm.begin(),
			pcm.begin() + static_cast<std::ptrdiff_t>(count));

		if (maxDurationMs > 0 && track.sampleRateHz > 0) {
			const std::int64_t haveMs = (track.frameCount() * 1000) / track.sampleRateHz;
			if (haveMs >= maxDurationMs) break;
		}
	}

	if (!track.valid()) {
		return Error{ErrorCode::ParseError, "no audio could be decoded from " + path.string()};
	}
	track.durationMs = (track.frameCount() * 1000) / track.sampleRateHz;
	return track;
}

PreparedAudio prepareForDevice(const DecodedTrack& track, const QAudioFormat& format) {
	PreparedAudio prepared;
	if (!track.valid() || !format.isValid()) return prepared;

	prepared.format = format;

	const int dstChannels = format.channelCount();
	const int dstRate = format.sampleRate();
	const int srcChannels = track.channels;
	const int srcRate = track.sampleRateHz;

	const double ratio = static_cast<double>(srcRate) / static_cast<double>(dstRate);
	const std::int64_t srcFrames = track.frameCount();
	const std::int64_t dstFrames = (ratio > 0.0)
		? static_cast<std::int64_t>(static_cast<double>(srcFrames) / ratio)
		: srcFrames;
	if (dstFrames <= 0) return prepared;

	const int bytesPerSample = bytesPerSampleFor(format.sampleFormat());
	prepared.bytesPerFrame = static_cast<std::int64_t>(bytesPerSample) * dstChannels;
	prepared.frameCount = dstFrames;
	prepared.durationMs = (dstFrames * 1000) / dstRate;
	prepared.bytes.resize(static_cast<std::size_t>(dstFrames * prepared.bytesPerFrame));
	prepared.mono.resize(static_cast<std::size_t>(dstFrames));

	for (std::int64_t f = 0; f < dstFrames; ++f) {
		// Linear interpolation between source frames. Adequate here: most files
		// already match the device rate, so this is usually a straight copy.
		const double sourcePosition = static_cast<double>(f) * ratio;
		const auto index = static_cast<std::int64_t>(sourcePosition);
		const auto fraction = static_cast<float>(sourcePosition - static_cast<double>(index));
		const std::int64_t next = std::min(index + 1, srcFrames - 1);

		float monoSum = 0.0f;
		std::uint8_t* out = prepared.bytes.data()
			+ static_cast<std::size_t>(f * prepared.bytesPerFrame);

		for (int c = 0; c < dstChannels; ++c) {
			// Map channels: mono to stereo duplicates, stereo to mono averages,
			// anything wider takes the channels it has.
			float value = 0.0f;
			if (srcChannels == dstChannels) {
				const float a = track.samples[static_cast<std::size_t>(index * srcChannels + c)];
				const float b = track.samples[static_cast<std::size_t>(next * srcChannels + c)];
				value = a + (b - a) * fraction;
			} else if (srcChannels == 1) {
				const float a = track.samples[static_cast<std::size_t>(index)];
				const float b = track.samples[static_cast<std::size_t>(next)];
				value = a + (b - a) * fraction;
			} else if (dstChannels == 1) {
				for (int sc = 0; sc < srcChannels; ++sc) {
					const float a = track.samples[static_cast<std::size_t>(index * srcChannels + sc)];
					const float b = track.samples[static_cast<std::size_t>(next * srcChannels + sc)];
					value += a + (b - a) * fraction;
				}
				value /= static_cast<float>(srcChannels);
			} else {
				const int sc = std::min(c, srcChannels - 1);
				const float a = track.samples[static_cast<std::size_t>(index * srcChannels + sc)];
				const float b = track.samples[static_cast<std::size_t>(next * srcChannels + sc)];
				value = a + (b - a) * fraction;
			}

			writeSample(out + static_cast<std::size_t>(c * bytesPerSample), value,
				format.sampleFormat());
			monoSum += value;
		}
		prepared.mono[static_cast<std::size_t>(f)] = monoSum / static_cast<float>(dstChannels);
	}

	return prepared;
}

// ---------------------------------------------------------------------------
// PcmFeed
// ---------------------------------------------------------------------------

PcmFeed::PcmFeed(QObject* parent) : QIODevice(parent) {}

void PcmFeed::setAudio(std::shared_ptr<const PreparedAudio> audio) {
	std::lock_guard<std::mutex> lock(m_mutex);
	m_audio = std::move(audio);
	m_frame = 0;
}

void PcmFeed::seekToFrame(std::int64_t frame) {
	std::lock_guard<std::mutex> lock(m_mutex);
	if (!m_audio) return;
	m_frame = std::clamp<std::int64_t>(frame, 0, m_audio->frameCount);
}

std::int64_t PcmFeed::currentFrame() const {
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_frame;
}

std::int64_t PcmFeed::frameCount() const {
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_audio ? m_audio->frameCount : 0;
}

qint64 PcmFeed::bytesAvailable() const {
	std::lock_guard<std::mutex> lock(m_mutex);
	if (!m_audio) return 0;
	const std::int64_t remaining = (m_audio->frameCount - m_frame) * m_audio->bytesPerFrame;
	return std::max<std::int64_t>(remaining, 0) + QIODevice::bytesAvailable();
}

bool PcmFeed::atEnd() const {
	std::lock_guard<std::mutex> lock(m_mutex);
	return !m_audio || m_frame >= m_audio->frameCount;
}

std::vector<float> PcmFeed::visualiserBlock(std::size_t frames) const {
	std::lock_guard<std::mutex> lock(m_mutex);
	if (!m_audio || m_audio->mono.empty() || frames == 0) return {};

	// The block ending at the playhead: what has just been handed to the device.
	const std::int64_t end = std::clamp<std::int64_t>(m_frame, 0,
		static_cast<std::int64_t>(m_audio->mono.size()));
	const std::int64_t begin = std::max<std::int64_t>(0,
		end - static_cast<std::int64_t>(frames));
	if (end <= begin) return {};

	return std::vector<float>(m_audio->mono.begin() + begin, m_audio->mono.begin() + end);
}

qint64 PcmFeed::readData(char* data, qint64 maxSize) {
	std::lock_guard<std::mutex> lock(m_mutex);
	if (!m_audio || maxSize <= 0) return 0;

	const std::int64_t remainingFrames = m_audio->frameCount - m_frame;
	if (remainingFrames <= 0) return 0;

	const std::int64_t wantFrames = std::min<std::int64_t>(remainingFrames,
		static_cast<std::int64_t>(maxSize) / m_audio->bytesPerFrame);
	if (wantFrames <= 0) return 0;

	const std::int64_t byteCount = wantFrames * m_audio->bytesPerFrame;
	const std::uint8_t* source = m_audio->bytes.data()
		+ static_cast<std::size_t>(m_frame * m_audio->bytesPerFrame);
	std::memcpy(data, source, static_cast<std::size_t>(byteCount));

	m_frame += wantFrames;
	return byteCount;
}

qint64 PcmFeed::writeData(const char*, qint64) {
	return -1;   // Read-only device.
}

// ---------------------------------------------------------------------------
// AudioPlayer
// ---------------------------------------------------------------------------

AudioPlayer::AudioPlayer(QObject* parent) : QObject(parent) {
	m_feed = new PcmFeed(this);   // Qt parent ownership.

	refreshOutputDevice();

	// A machine can have working hardware while the sound server has not yet
	// claimed it. Re-check when the device list changes rather than deciding
	// once at startup that playback is impossible.
	connect(&m_devices, &QMediaDevices::audioOutputsChanged,
		this, &AudioPlayer::refreshOutputDevice);

	m_positionTimer.setInterval(40);
	connect(&m_positionTimer, &QTimer::timeout, this, [this]() {
		emit positionChanged(positionMs(), durationMs());
		if (m_state == PlaybackState::Playing && m_feed->atEnd()) {
			stop();
			emit trackFinished();
		}
	});
}

AudioPlayer::~AudioPlayer() {
	if (m_sink) m_sink->stop();
}

void AudioPlayer::refreshOutputDevice() {
	const QAudioDevice device = QMediaDevices::defaultAudioOutput();
	const bool had = m_hasDevice;

	m_hasDevice = !device.isNull();
	m_deviceName = m_hasDevice ? device.description() : QStringLiteral("none");

	if (had != m_hasDevice) {
		// A device that appeared after a track was loaded needs the track
		// re-prepared for it, because its format may differ.
		if (m_hasDevice && m_track) {
			m_audio = std::make_shared<const PreparedAudio>(
				prepareForDevice(*m_track, negotiateFormat(*m_track)));
			m_feed->setAudio(m_audio);
			createSink();
		}
		emit outputDeviceChanged(m_hasDevice, m_deviceName);
	}
}

QAudioFormat AudioPlayer::negotiateFormat(const DecodedTrack& track) const {
	const QAudioDevice device = QMediaDevices::defaultAudioOutput();

	QAudioFormat wanted;
	wanted.setSampleRate(track.sampleRateHz);
	wanted.setChannelCount(track.channels);
	wanted.setSampleFormat(QAudioFormat::Float);
	if (device.isFormatSupported(wanted)) return wanted;

	// Float is commonly unsupported. Try 16-bit at the track's own rate before
	// giving up on the rate, since resampling is the bigger compromise.
	wanted.setSampleFormat(QAudioFormat::Int16);
	if (device.isFormatSupported(wanted)) return wanted;

	QAudioFormat preferred = device.preferredFormat();
	if (preferred.isValid()) return preferred;

	// Last resort, and a format essentially every device accepts.
	QAudioFormat fallback;
	fallback.setSampleRate(48000);
	fallback.setChannelCount(2);
	fallback.setSampleFormat(QAudioFormat::Int16);
	return fallback;
}

bool AudioPlayer::createSink() {
	if (!m_audio || !m_audio->valid() || !m_hasDevice) return false;

	const QAudioDevice device = QMediaDevices::defaultAudioOutput();
	m_sink = std::make_unique<QAudioSink>(device, m_audio->format);
	m_sink->setVolume(static_cast<qreal>(m_volume));

	// A generous buffer: the feed is memory-backed, but a short buffer makes
	// playback sensitive to scheduling on a loaded machine.
	m_sink->setBufferSize(static_cast<qsizetype>(m_audio->bytesPerFrame
		* m_audio->format.sampleRate() / 4));

	connect(m_sink.get(), &QAudioSink::stateChanged, this, [this](QAudio::State state) {
		if (state != QAudio::StoppedState || !m_sink) return;
		if (m_sink->error() != QAudio::NoError) {
			emit errorOccurred(QStringLiteral("Audio output stopped: error %1.")
				.arg(static_cast<int>(m_sink->error())));
		}
	});
	return true;
}

Status AudioPlayer::load(const fs::path& path) {
	stop();

	auto decoded = decodeForPlayback(path);
	if (!decoded) return Status(decoded.error());

	m_track = std::make_shared<const DecodedTrack>(std::move(decoded.value()));

	if (!m_hasDevice) {
		return Status(Error{ErrorCode::Unsupported,
			"no audio output device is available; library functions are unaffected"});
	}

	const QAudioFormat format = negotiateFormat(*m_track);
	m_audio = std::make_shared<const PreparedAudio>(prepareForDevice(*m_track, format));
	if (!m_audio->valid()) {
		return Status(Error{ErrorCode::Unsupported,
			"the decoded audio could not be converted to a format this output device accepts"});
	}

	m_feed->setAudio(m_audio);
	m_sink.reset();
	if (!createSink()) {
		return Status(Error{ErrorCode::Internal, "the audio output could not be opened"});
	}
	return Status::success();
}

void AudioPlayer::play() {
	if (!m_audio || !m_hasDevice) return;
	if (!m_sink && !createSink()) return;

	if (m_state == PlaybackState::Paused) {
		m_sink->resume();
	} else {
		if (!m_feed->isOpen()) m_feed->open(QIODevice::ReadOnly);
		// Restart from the beginning once the previous run reached the end.
		if (m_feed->atEnd()) m_feed->seekToFrame(0);
		m_sink->start(m_feed);
	}

	m_state = PlaybackState::Playing;
	m_positionTimer.start();
	emit stateChanged(m_state);
}

void AudioPlayer::pause() {
	if (m_state != PlaybackState::Playing || !m_sink) return;
	m_sink->suspend();
	m_state = PlaybackState::Paused;
	m_positionTimer.stop();
	emit stateChanged(m_state);
}

void AudioPlayer::stop() {
	if (m_sink) m_sink->stop();
	if (m_feed->isOpen()) m_feed->close();
	m_feed->seekToFrame(0);
	m_positionTimer.stop();

	if (m_state != PlaybackState::Stopped) {
		m_state = PlaybackState::Stopped;
		emit stateChanged(m_state);
	}
	emit positionChanged(0, durationMs());
}

void AudioPlayer::togglePlayPause() {
	if (m_state == PlaybackState::Playing) pause();
	else play();
}

void AudioPlayer::seek(double fraction) {
	if (!m_audio) return;

	const std::int64_t frame = static_cast<std::int64_t>(
		std::clamp(fraction, 0.0, 1.0) * static_cast<double>(m_audio->frameCount));

	// Seeking while the sink is running leaves already-buffered audio playing
	// from the old position. Reset the sink so the jump is immediate.
	const bool wasPlaying = (m_state == PlaybackState::Playing);
	if (wasPlaying && m_sink) m_sink->stop();

	m_feed->seekToFrame(frame);

	if (wasPlaying && m_sink) {
		if (!m_feed->isOpen()) m_feed->open(QIODevice::ReadOnly);
		m_sink->start(m_feed);
	}
	emit positionChanged(positionMs(), durationMs());
}

void AudioPlayer::setVolume(double volume) {
	m_volume = std::clamp(volume, 0.0, 1.0);
	if (m_sink) m_sink->setVolume(static_cast<qreal>(m_volume));
}

std::int64_t AudioPlayer::positionMs() const {
	if (!m_audio || m_audio->format.sampleRate() <= 0) return 0;
	return (m_feed->currentFrame() * 1000) / m_audio->format.sampleRate();
}

std::int64_t AudioPlayer::durationMs() const {
	return m_audio ? m_audio->durationMs : (m_track ? m_track->durationMs : 0);
}

QString AudioPlayer::formatDescription() const {
	if (!m_audio || !m_audio->format.isValid()) return QStringLiteral("—");

	const char* sampleType = "float";
	switch (m_audio->format.sampleFormat()) {
		case QAudioFormat::UInt8: sampleType = "u8"; break;
		case QAudioFormat::Int16: sampleType = "s16"; break;
		case QAudioFormat::Int32: sampleType = "s32"; break;
		default: break;
	}
	return QStringLiteral("%1 Hz · %2 ch · %3")
		.arg(m_audio->format.sampleRate())
		.arg(m_audio->format.channelCount())
		.arg(QString::fromLatin1(sampleType));
}

std::vector<float> AudioPlayer::visualiserBlock(std::size_t frames) const {
	return m_feed->visualiserBlock(frames);
}

int AudioPlayer::outputSampleRate() const {
	return m_audio ? m_audio->format.sampleRate() : 0;
}

} // namespace ml::desktop

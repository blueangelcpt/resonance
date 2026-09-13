// SPDX-License-Identifier: GPL-3.0-or-later
#include "AudioPlayer.hpp"

#include <QMediaDevices>
#include <QAudioDevice>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

// The decoder implementation lives in MusicLibrary.Infrastructure; only the
// declarations are needed here.
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

// ---------------------------------------------------------------------------
// PcmFeed
// ---------------------------------------------------------------------------

PcmFeed::PcmFeed(QObject* parent) : QIODevice(parent) {}

void PcmFeed::setTrack(std::shared_ptr<const DecodedTrack> track) {
	std::lock_guard<std::mutex> lock(m_mutex);
	m_track = std::move(track);
	m_frame = 0;
	m_recent.clear();
}

void PcmFeed::seekToFrame(std::int64_t frame) {
	std::lock_guard<std::mutex> lock(m_mutex);
	if (!m_track) return;
	m_frame = std::clamp<std::int64_t>(frame, 0, m_track->frameCount());
}

std::int64_t PcmFeed::currentFrame() const {
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_frame;
}

std::int64_t PcmFeed::frameCount() const {
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_track ? m_track->frameCount() : 0;
}

bool PcmFeed::atEnd() const {
	std::lock_guard<std::mutex> lock(m_mutex);
	return !m_track || m_frame >= m_track->frameCount();
}

std::vector<float> PcmFeed::latestBlock(std::size_t maxFrames) const {
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_recent.empty() || !m_track) return {};

	const std::size_t channels = static_cast<std::size_t>(m_track->channels);
	const std::size_t available = m_recent.size() / channels;
	const std::size_t frames = std::min(available, maxFrames);

	// Mono mix, which is what the spectrum wants.
	std::vector<float> mono(frames, 0.0f);
	const std::size_t start = available - frames;
	for (std::size_t f = 0; f < frames; ++f) {
		float sum = 0.0f;
		for (std::size_t c = 0; c < channels; ++c) {
			sum += m_recent[(start + f) * channels + c];
		}
		mono[f] = sum / static_cast<float>(channels);
	}
	return mono;
}

qint64 PcmFeed::readData(char* data, qint64 maxSize) {
	std::lock_guard<std::mutex> lock(m_mutex);
	if (!m_track || maxSize <= 0) return 0;

	const std::size_t channels = static_cast<std::size_t>(m_track->channels);
	const std::int64_t total = m_track->frameCount();
	if (m_frame >= total) return 0;

	const std::size_t bytesPerFrame = channels * sizeof(float);
	std::int64_t frames = static_cast<std::int64_t>(static_cast<std::size_t>(maxSize) / bytesPerFrame);
	frames = std::min(frames, total - m_frame);
	if (frames <= 0) return 0;

	const float gain = m_volume.load(std::memory_order_relaxed);
	const std::size_t count = static_cast<std::size_t>(frames) * channels;
	const float* source = m_track->samples.data() + static_cast<std::size_t>(m_frame) * channels;

	auto* out = reinterpret_cast<float*>(data);
	for (std::size_t i = 0; i < count; ++i) {
		out[i] = std::clamp(source[i] * gain, -1.0f, 1.0f);
	}

	// Keep a copy for the visualiser. Bounded so it cannot grow.
	constexpr std::size_t kRetainFrames = 4096;
	m_recent.assign(out, out + count);
	if (m_recent.size() > kRetainFrames * channels) {
		m_recent.erase(m_recent.begin(),
			m_recent.end() - static_cast<std::ptrdiff_t>(kRetainFrames * channels));
	}

	m_frame += frames;
	return static_cast<qint64>(count * sizeof(float));
}

qint64 PcmFeed::writeData(const char*, qint64) {
	// Read-only device.
	return -1;
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

	m_positionTimer.setInterval(50);
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
		// A sink that appeared after a track was loaded still needs one.
		if (m_hasDevice && m_track && !m_sink) createSink();
		emit outputDeviceChanged(m_hasDevice, m_deviceName);
	}
}

void AudioPlayer::createSink() {
	if (!m_track || !m_hasDevice) return;

	m_format.setSampleRate(m_track->sampleRateHz);
	m_format.setChannelCount(m_track->channels);
	m_format.setSampleFormat(QAudioFormat::Float);

	const QAudioDevice device = QMediaDevices::defaultAudioOutput();
	if (!device.isFormatSupported(m_format)) {
		// Fall back to whatever the device does support rather than failing.
		m_format = device.preferredFormat();
	}

	m_sink = std::make_unique<QAudioSink>(device, m_format);
	m_sink->setVolume(static_cast<qreal>(m_volume));

	connect(m_sink.get(), &QAudioSink::stateChanged, this, [this](QAudio::State state) {
		if (state == QAudio::StoppedState && m_sink && m_sink->error() != QAudio::NoError) {
			emit errorOccurred(QStringLiteral("Audio output stopped unexpectedly."));
		}
	});
}

Status AudioPlayer::load(const fs::path& path) {
	stop();

	auto decoded = decodeForPlayback(path);
	if (!decoded) return Status(decoded.error());

	m_track = std::make_shared<const DecodedTrack>(std::move(decoded.value()));
	m_feed->setTrack(m_track);
	m_feed->setVolume(1.0f);

	// The sink's format depends on the track's rate and channel count, so it is
	// rebuilt per track rather than reused.
	m_sink.reset();
	createSink();

	if (!m_hasDevice) {
		return Status(Error{ErrorCode::Unsupported,
			"no audio output device is available; library functions are unaffected"});
	}
	return Status::success();
}

void AudioPlayer::play() {
	if (!m_track || !m_hasDevice || !m_sink) return;

	if (m_state == PlaybackState::Paused) {
		m_sink->resume();
	} else {
		if (!m_feed->isOpen()) m_feed->open(QIODevice::ReadOnly);
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
	if (!m_track) return;
	const std::int64_t frame = static_cast<std::int64_t>(
		std::clamp(fraction, 0.0, 1.0) * static_cast<double>(m_track->frameCount()));
	m_feed->seekToFrame(frame);
	emit positionChanged(positionMs(), durationMs());
}

void AudioPlayer::setVolume(double volume) {
	m_volume = std::clamp(volume, 0.0, 1.0);
	if (m_sink) m_sink->setVolume(static_cast<qreal>(m_volume));
}

std::int64_t AudioPlayer::positionMs() const {
	if (!m_track || m_track->sampleRateHz <= 0) return 0;
	return (m_feed->currentFrame() * 1000) / m_track->sampleRateHz;
}

std::int64_t AudioPlayer::durationMs() const {
	return m_track ? m_track->durationMs : 0;
}

std::vector<float> AudioPlayer::visualiserBlock(std::size_t frames) const {
	return m_feed->latestBlock(frames);
}

} // namespace ml::desktop

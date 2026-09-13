// SPDX-License-Identifier: GPL-3.0-or-later
// Audio playback.
//
// Resonance is both a player and a library tool. Playback decodes the MP3 with
// the same vendored minimp3 decoder used for analysis, converts the result to
// the format the output device actually accepts, and feeds a QAudioSink.
//
// Playback is strictly read-only. It opens the source file for reading and has
// no access to a PathGuard, a TagWriter or the catalogue; it cannot modify
// anything.
#pragma once

#include "mlinfra/AudioAnalysis.hpp"

#include <QAudioFormat>
#include <QAudioSink>
#include <QIODevice>
#include <QMediaDevices>
#include <QObject>
#include <QTimer>

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>

namespace ml::desktop {

/// Decoded PCM straight from the file, at its own rate and channel count.
struct DecodedTrack {
	std::vector<float> samples;      ///< Interleaved.
	int sampleRateHz = 0;
	int channels = 0;
	std::int64_t durationMs = 0;
	std::filesystem::path path;

	bool valid() const { return !samples.empty() && sampleRateHz > 0 && channels > 0; }
	std::int64_t frameCount() const {
		return channels > 0 ? static_cast<std::int64_t>(samples.size()) / channels : 0;
	}
};

/// Decodes an MP3 to interleaved float PCM at its native rate.
///
/// Separate from `Mp3Decoder::decode`, which downmixes to mono at a reduced rate
/// for analysis. Playback needs the real thing.
Result<DecodedTrack> decodeForPlayback(const std::filesystem::path& path,
	std::int64_t maxDurationMs = 0);

/// A track converted to exactly what the output device accepts.
///
/// The conversion happens once, at load time. Feeding a device a format it did
/// not agree to is the difference between music and noise, and switching the
/// sink's format without converting the data is the same bug wearing a hat.
struct PreparedAudio {
	std::vector<std::uint8_t> bytes;   ///< Interleaved, in `format`.
	QAudioFormat format;
	std::int64_t frameCount = 0;
	std::int64_t bytesPerFrame = 0;
	std::int64_t durationMs = 0;

	/// Mono mix at the output rate, kept for the visualiser so the analyser can
	/// read the samples actually being played without re-deriving them.
	std::vector<float> mono;

	bool valid() const { return !bytes.empty() && bytesPerFrame > 0 && frameCount > 0; }
};

/// Converts decoded PCM to the device's format: sample type, rate and channels.
PreparedAudio prepareForDevice(const DecodedTrack& track, const QAudioFormat& format);

/// The QIODevice QAudioSink pulls from.
///
/// Holds its position under a mutex so the GUI thread can seek and read the
/// playhead while the audio thread pulls.
class PcmFeed : public QIODevice {
	Q_OBJECT

public:
	explicit PcmFeed(QObject* parent = nullptr);

	void setAudio(std::shared_ptr<const PreparedAudio> audio);
	void seekToFrame(std::int64_t frame);
	std::int64_t currentFrame() const;
	std::int64_t frameCount() const;

	void setGain(float gain) { m_gain.store(gain, std::memory_order_relaxed); }

	// QIODevice. A pull-mode sink asks the device how much it can supply and
	// whether it has finished; the base class answers 0 and true for an
	// unbuffered sequential device, which stalls playback before it starts.
	bool isSequential() const override { return true; }
	qint64 bytesAvailable() const override;
	bool atEnd() const override;

	/// Mono samples ending at the current playhead, for the analyser.
	std::vector<float> visualiserBlock(std::size_t frames) const;

protected:
	qint64 readData(char* data, qint64 maxSize) override;
	qint64 writeData(const char* data, qint64 maxSize) override;

private:
	mutable std::mutex m_mutex;
	std::shared_ptr<const PreparedAudio> m_audio;
	std::int64_t m_frame = 0;
	std::atomic<float> m_gain{1.0f};
};

enum class PlaybackState { Stopped, Playing, Paused };

class AudioPlayer : public QObject {
	Q_OBJECT

public:
	explicit AudioPlayer(QObject* parent = nullptr);
	~AudioPlayer() override;

	AudioPlayer(const AudioPlayer&) = delete;
	AudioPlayer& operator=(const AudioPlayer&) = delete;

	/// Decodes and prepares a track for the current output device.
	Status load(const std::filesystem::path& path);

	void play();
	void pause();
	void stop();
	void togglePlayPause();
	void seek(double fraction);

	void setVolume(double volume);
	double volume() const { return m_volume; }

	PlaybackState state() const { return m_state; }
	std::int64_t positionMs() const;
	std::int64_t durationMs() const;

	bool hasOutputDevice() const { return m_hasDevice; }
	QString outputDeviceName() const { return m_deviceName; }
	/// The format actually negotiated with the device, for the telemetry strip.
	QString formatDescription() const;

	/// Mono samples around the playhead, for the live analyser.
	std::vector<float> visualiserBlock(std::size_t frames) const;
	int outputSampleRate() const;

	void refreshOutputDevice();

signals:
	void outputDeviceChanged(bool available, const QString& name);
	void stateChanged(PlaybackState state);
	void positionChanged(std::int64_t positionMs, std::int64_t durationMs);
	void trackFinished();
	void errorOccurred(const QString& message);

private:
	/// Chooses a format the device accepts, preferring the track's own.
	QAudioFormat negotiateFormat(const DecodedTrack& track) const;
	bool createSink();

	std::unique_ptr<QAudioSink> m_sink;
	PcmFeed* m_feed = nullptr;          ///< Owned via Qt parenting.
	std::shared_ptr<const DecodedTrack> m_track;
	std::shared_ptr<const PreparedAudio> m_audio;
	QTimer m_positionTimer;
	PlaybackState m_state = PlaybackState::Stopped;
	double m_volume = 0.85;
	bool m_hasDevice = false;
	QString m_deviceName;
	QMediaDevices m_devices;
};

} // namespace ml::desktop

// SPDX-License-Identifier: GPL-3.0-or-later
// Audio playback.
//
// Resonance is both a player and a library tool. Playback decodes the MP3 with
// the same vendored minimp3 decoder used for analysis and feeds a QAudioSink,
// so there is one decoder in the application rather than two that could
// disagree about what a file contains.
//
// Playback is strictly read-only. It opens the source file for reading and has
// no access to a PathGuard, a TagWriter or the catalogue; it cannot modify
// anything.
#pragma once

#include "mlinfra/AudioAnalysis.hpp"

#include <QAudioFormat>
#include <QAudioSink>
#include <QIODevice>
#include <QObject>
#include <QMediaDevices>
#include <QTimer>

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>

namespace ml::desktop {

/// Decoded full-rate PCM for one track, plus what is needed to play it.
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

/// The QIODevice QAudioSink pulls from.
///
/// Holds a position under a mutex so the GUI thread can seek and read the
/// playhead while the audio thread is pulling.
class PcmFeed : public QIODevice {
	Q_OBJECT

public:
	explicit PcmFeed(QObject* parent = nullptr);

	void setTrack(std::shared_ptr<const DecodedTrack> track);
	void seekToFrame(std::int64_t frame);
	std::int64_t currentFrame() const;
	std::int64_t frameCount() const;

	void setVolume(float volume) { m_volume.store(volume, std::memory_order_relaxed); }
	float volume() const { return m_volume.load(std::memory_order_relaxed); }

	/// True once the feed has reached the end of the track.
	bool atEnd() const override;

	/// The most recent block handed to the audio device, for the visualiser.
	/// Copies under the same mutex, so the caller never reads a torn buffer.
	std::vector<float> latestBlock(std::size_t maxFrames) const;

protected:
	qint64 readData(char* data, qint64 maxSize) override;
	qint64 writeData(const char* data, qint64 maxSize) override;

private:
	mutable std::mutex m_mutex;
	std::shared_ptr<const DecodedTrack> m_track;
	std::int64_t m_frame = 0;
	std::vector<float> m_recent;
	std::atomic<float> m_volume{1.0f};
};

/// Transport states, mirroring what the deck's controls offer.
enum class PlaybackState { Stopped, Playing, Paused };

class AudioPlayer : public QObject {
	Q_OBJECT

public:
	explicit AudioPlayer(QObject* parent = nullptr);
	~AudioPlayer() override;

	AudioPlayer(const AudioPlayer&) = delete;
	AudioPlayer& operator=(const AudioPlayer&) = delete;

	/// Loads a track. Decoding happens on the calling thread and can take a
	/// moment for a long file; the caller shows a busy cursor.
	Status load(const std::filesystem::path& path);

	void play();
	void pause();
	void stop();
	void togglePlayPause();

	/// Seeks to a fraction of the track.
	void seek(double fraction);

	void setVolume(double volume);
	double volume() const { return m_volume; }

	PlaybackState state() const { return m_state; }
	std::int64_t positionMs() const;
	std::int64_t durationMs() const;
	std::shared_ptr<const DecodedTrack> track() const { return m_track; }

	/// True when an audio output device was found. Without one, the library
	/// functions still work and only playback is unavailable.
	bool hasOutputDevice() const { return m_hasDevice; }
	QString outputDeviceName() const { return m_deviceName; }

	/// Recent samples for the visualiser, mono-mixed.
	std::vector<float> visualiserBlock(std::size_t frames) const;

	/// Re-reads the default output device. Called when the system's device list
	/// changes, so audio arriving after startup enables playback without a
	/// restart.
	void refreshOutputDevice();

signals:
	void outputDeviceChanged(bool available, const QString& name);
	void stateChanged(PlaybackState state);
	void positionChanged(std::int64_t positionMs, std::int64_t durationMs);
	void trackFinished();
	void errorOccurred(const QString& message);

private:
	void createSink();

	std::unique_ptr<QAudioSink> m_sink;
	PcmFeed* m_feed = nullptr;          ///< Owned via Qt parenting.
	std::shared_ptr<const DecodedTrack> m_track;
	QAudioFormat m_format;
	QTimer m_positionTimer;
	PlaybackState m_state = PlaybackState::Stopped;
	double m_volume = 0.85;
	bool m_hasDevice = false;
	QString m_deviceName;
	QMediaDevices m_devices;
};

} // namespace ml::desktop

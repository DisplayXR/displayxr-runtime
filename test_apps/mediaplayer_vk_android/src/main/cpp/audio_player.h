// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// AudioPlayer — Android port of David's media/AudioPlayer, swapping FFmpeg+SDL3
// for AMediaCodec (audio decode) + AAudio (output). Decodes the file's audio
// track on its own thread and plays it; the blocking AAudio write paces playback
// to real time, so the PTS of the buffer just written is the A/V master clock
// the video decoder syncs to. Independent of VideoDecoder (its own extractor +
// fd over the same file). Silent no-op if the file has no audio track.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

struct AMediaExtractor;
struct AMediaCodec;
struct AAudioStreamStruct;  // AAudioStream is a typedef of this (aaudio/AAudio.h)

struct AudioPlayer {
	~AudioPlayer() { stop(); }

	bool openPath(const std::string &path);  // opens its own fd internally
	void stop();
	bool hasAudio() const { return open_.load(std::memory_order_relaxed); }

	void setPaused(bool p);
	void seekRelative(double deltaSeconds);
	void seekTo(double seconds)
	{
		if (open_.load(std::memory_order_relaxed))
			seekRequestUs_.store((int64_t)(seconds < 0 ? 0 : seconds * 1e6),
			                     std::memory_order_relaxed);
	}

	// Playback position in seconds (PTS of the last buffer handed to AAudio), or
	// -1 when unavailable. This is the A/V master clock.
	double clockSeconds() const
	{
		int64_t c = clockUs_.load(std::memory_order_relaxed);
		return c < 0 ? -1.0 : c / 1e6;
	}
	// Thunk for VideoDecoder::setMasterClock(fn, ctx).
	static double clockThunk(void *ctx) { return static_cast<AudioPlayer *>(ctx)->clockSeconds(); }

private:
	bool start(const std::string &mime, int sampleRate, int channels);
	void decodeLoop();

	AMediaExtractor *ex_ = nullptr;
	AMediaCodec *codec_ = nullptr;
	AAudioStreamStruct *stream_ = nullptr;  // == AAudioStream*
	std::thread thread_;
	std::atomic<bool> stop_{false};
	std::atomic<bool> open_{false};
	std::atomic<bool> paused_{false};
	std::atomic<int64_t> clockUs_{-1};
	std::atomic<int64_t> seekRequestUs_{-1};
	int sampleRate_ = 48000;
	int channels_ = 2;
	int ownedFd_ = -1;
};

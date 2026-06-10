// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0

#include "audio_player.h"

#include <aaudio/AAudio.h>
#include <android/log.h>
#include <fcntl.h>
#include <media/NDKMediaCodec.h>
#include <media/NDKMediaExtractor.h>
#include <media/NDKMediaFormat.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstring>

#define LOG_TAG "mediaplayer_vk_android"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

bool
AudioPlayer::openPath(const std::string &path)
{
	int fd = ::open(path.c_str(), O_RDONLY);
	if (fd < 0) {
		LOGE("audio open(%s) failed", path.c_str());
		return false;
	}
	struct stat st;
	int64_t length = (::fstat(fd, &st) == 0) ? (int64_t)st.st_size : 0;
	ownedFd_ = fd;

	ex_ = AMediaExtractor_new();
	if (AMediaExtractor_setDataSourceFd(ex_, fd, 0, length) != AMEDIA_OK) {
		LOGE("audio setDataSourceFd failed");
		AMediaExtractor_delete(ex_);
		ex_ = nullptr;
		::close(ownedFd_);
		ownedFd_ = -1;
		return false;
	}

	const size_t tracks = AMediaExtractor_getTrackCount(ex_);
	int audioTrack = -1;
	const char *mime = nullptr;
	int sr = 48000, ch = 2;
	for (size_t i = 0; i < tracks; ++i) {
		AMediaFormat *f = AMediaExtractor_getTrackFormat(ex_, i);
		const char *m = nullptr;
		if (AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &m) && m &&
		    std::strncmp(m, "audio/", 6) == 0) {
			audioTrack = (int)i;
			mime = m;
			AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, &sr);
			AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &ch);
			// keep f alive: configure needs it
			AMediaExtractor_selectTrack(ex_, audioTrack);
			codec_ = AMediaCodec_createDecoderByType(m);
			if (codec_ == nullptr ||
			    AMediaCodec_configure(codec_, f, nullptr, nullptr, 0) != AMEDIA_OK ||
			    AMediaCodec_start(codec_) != AMEDIA_OK) {
				LOGE("audio codec init failed");
				AMediaFormat_delete(f);
				return false;
			}
			AMediaFormat_delete(f);
			break;
		}
		AMediaFormat_delete(f);
	}
	if (audioTrack < 0) {
		LOGI("no audio track — playing silent");
		return false;
	}
	return start(mime, sr, ch);
}

bool
AudioPlayer::start(const std::string & /*mime*/, int sampleRate, int channels)
{
	sampleRate_ = sampleRate > 0 ? sampleRate : 48000;
	channels_ = channels > 0 ? channels : 2;

	AAudioStreamBuilder *builder = nullptr;
	if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) {
		LOGE("AAudio_createStreamBuilder failed");
		return false;
	}
	AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
	AAudioStreamBuilder_setSampleRate(builder, sampleRate_);
	AAudioStreamBuilder_setChannelCount(builder, channels_);
	AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
	AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
	AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_NONE);
	aaudio_result_t r = AAudioStreamBuilder_openStream(builder, &stream_);
	AAudioStreamBuilder_delete(builder);
	if (r != AAUDIO_OK || stream_ == nullptr) {
		LOGE("AAudio openStream failed: %s", AAudio_convertResultToText(r));
		return false;
	}
	AAudioStream_requestStart(stream_);
	LOGI("AudioPlayer open: %dHz %dch", sampleRate_, channels_);
	open_.store(true, std::memory_order_relaxed);
	stop_.store(false, std::memory_order_relaxed);
	thread_ = std::thread([this] { decodeLoop(); });
	return true;
}

void
AudioPlayer::setPaused(bool p)
{
	paused_.store(p, std::memory_order_relaxed);
	if (stream_) {
		if (p) {
			AAudioStream_requestPause(stream_);
		} else {
			AAudioStream_requestStart(stream_);
		}
	}
}

void
AudioPlayer::seekRelative(double deltaSeconds)
{
	if (!open_.load(std::memory_order_relaxed)) return;
	int64_t cur = clockUs_.load(std::memory_order_relaxed);
	if (cur < 0) cur = 0;
	int64_t target = cur + (int64_t)(deltaSeconds * 1e6);
	if (target < 0) target = 0;
	seekRequestUs_.store(target, std::memory_order_relaxed);
}

void
AudioPlayer::decodeLoop()
{
	bool sawInputEOS = false;
	while (!stop_.load(std::memory_order_relaxed)) {
		// seek (works while paused)
		const int64_t sk = seekRequestUs_.exchange(-1, std::memory_order_relaxed);
		if (sk >= 0) {
			AMediaExtractor_seekTo(ex_, sk, AMEDIAEXTRACTOR_SEEK_CLOSEST_SYNC);
			AMediaCodec_flush(codec_);
			if (stream_) AAudioStream_requestFlush(stream_);
			sawInputEOS = false;
			clockUs_.store(sk, std::memory_order_relaxed);
			if (stream_ && !paused_.load(std::memory_order_relaxed))
				AAudioStream_requestStart(stream_);
		}
		if (paused_.load(std::memory_order_relaxed)) {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			continue;
		}

		// feed
		if (!sawInputEOS) {
			ssize_t inIdx = AMediaCodec_dequeueInputBuffer(codec_, 2000);
			if (inIdx >= 0) {
				size_t cap = 0;
				uint8_t *ibuf = AMediaCodec_getInputBuffer(codec_, inIdx, &cap);
				ssize_t sz = AMediaExtractor_readSampleData(ex_, ibuf, cap);
				if (sz < 0) {
					AMediaCodec_queueInputBuffer(codec_, inIdx, 0, 0, 0,
					                             AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
					sawInputEOS = true;
				} else {
					int64_t pts = AMediaExtractor_getSampleTime(ex_);
					AMediaCodec_queueInputBuffer(codec_, inIdx, 0, (size_t)sz, pts, 0);
					AMediaExtractor_advance(ex_);
				}
			}
		}

		// drain → AAudio (blocking write paces to real time)
		AMediaCodecBufferInfo info;
		ssize_t outIdx = AMediaCodec_dequeueOutputBuffer(codec_, &info, 2000);
		if (outIdx >= 0) {
			if (info.size > 0) {
				size_t outSize = 0;
				uint8_t *obuf = AMediaCodec_getOutputBuffer(codec_, outIdx, &outSize);
				if (obuf != nullptr && stream_ != nullptr) {
					const int frames = (int)(info.size / (channels_ * 2));  // PCM_I16
					int written = 0;
					while (written < frames && !stop_.load(std::memory_order_relaxed)) {
						aaudio_result_t w = AAudioStream_write(
						    stream_, obuf + (size_t)written * channels_ * 2, frames - written,
						    1'000'000'000L);
						if (w < 0) break;
						written += w;
					}
					clockUs_.store(info.presentationTimeUs, std::memory_order_relaxed);
				}
			}
			const bool eos = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
			AMediaCodec_releaseOutputBuffer(codec_, outIdx, false);
			if (eos) {  // loop with the video
				AMediaExtractor_seekTo(ex_, 0, AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC);
				AMediaCodec_flush(codec_);
				sawInputEOS = false;
				clockUs_.store(0, std::memory_order_relaxed);
			}
		}
	}
}

void
AudioPlayer::stop()
{
	stop_.store(true, std::memory_order_relaxed);
	if (thread_.joinable()) thread_.join();
	if (stream_) {
		AAudioStream_requestStop(stream_);
		AAudioStream_close(stream_);
		stream_ = nullptr;
	}
	if (codec_) {
		AMediaCodec_stop(codec_);
		AMediaCodec_delete(codec_);
		codec_ = nullptr;
	}
	if (ex_) {
		AMediaExtractor_delete(ex_);
		ex_ = nullptr;
	}
	if (ownedFd_ >= 0) {
		::close(ownedFd_);
		ownedFd_ = -1;
	}
	open_.store(false, std::memory_order_relaxed);
}

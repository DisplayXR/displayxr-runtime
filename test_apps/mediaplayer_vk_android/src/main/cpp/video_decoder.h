// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// VideoDecoder — Android port of David's media/VideoDecoder, swapping FFmpeg
// for the framework-native AMediaExtractor + AMediaCodec (libmediandk). Decodes
// an SBS video on a background thread to planar YUV (NV12 or I420 — the codec's
// native ByteBuffer output) and publishes into a lock-light triple buffer; the
// shader does the BT.709 convert + per-eye downscale (sbs.frag mode 1/2), so no
// swscale runs. Loops at EOF. Hardware decode is automatic (MediaCodec picks the
// SoC decoder); software is the framework's own fallback.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct AMediaExtractor;
struct AMediaCodec;
struct AMediaFormat;

struct VideoDecoder {
	struct Frame {
		std::vector<uint8_t> y;   // luma, width x height, tightly packed
		std::vector<uint8_t> uv;  // NV12: interleaved UV (w x h/2). I420: U (w/2 x h/2)
		std::vector<uint8_t> v;   // I420 only: V (w/2 x h/2)
		int width = 0;
		int height = 0;
		bool nv12 = true;
		bool fullRange = false;
		int64_t serial = 0;
	};

	~VideoDecoder() { stop(); }

	// Open from a filesystem path (app-readable, e.g. externalDataPath) or a
	// content fd (from the SAF picker). Starts the decode thread.
	bool openPath(const std::string &path);
	bool openFd(int fd, int64_t offset, int64_t length);
	void stop();

	bool isOpen() const { return open_.load(std::memory_order_relaxed); }
	int width() const { return width_; }
	int height() const { return height_; }

	// Latest decoded frame, or nullptr if nothing new since the last call.
	// Owned by the decoder; valid until the next acquireLatest().
	const Frame *acquireLatest();

private:
	bool start();
	void decodeLoop();
	void extractFrame(uint8_t *src, int srcSize);

	AMediaExtractor *ex_ = nullptr;
	AMediaCodec *codec_ = nullptr;
	AMediaFormat *outFmt_ = nullptr;  // cached on FORMAT_CHANGED
	int ownedFd_ = -1;                // fd we opened (path) or were handed (SAF); closed in stop()
	std::thread thread_;
	std::atomic<bool> stop_{false};
	std::atomic<bool> open_{false};
	int width_ = 0;
	int height_ = 0;

	// Triple buffer (producer / handoff / consumer), pointer-swap under mutex.
	Frame buffers_[3];
	std::mutex mutex_;
	int producer_ = 0;
	int middle_ = 1;
	int consumer_ = 2;
	bool hasNew_ = false;
	int64_t serial_ = 0;
};

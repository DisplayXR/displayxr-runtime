// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0

#include "video_decoder.h"

#include <android/log.h>
#include <media/NDKMediaCodec.h>
#include <media/NDKMediaExtractor.h>
#include <media/NDKMediaFormat.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>

#define LOG_TAG "mediaplayer_vk_android"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace {
// OMX color formats (stable values; we read them from the output format).
constexpr int32_t kColorI420 = 19;       // COLOR_FormatYUV420Planar
constexpr int32_t kColorNV12 = 21;       // COLOR_FormatYUV420SemiPlanar
constexpr int32_t kColorFlexible = 0x7F420888;

int32_t
fmtInt(AMediaFormat *f, const char *key, int32_t fallback)
{
	int32_t v = 0;
	return (f && AMediaFormat_getInt32(f, key, &v)) ? v : fallback;
}
}  // namespace

bool
VideoDecoder::openPath(const std::string &path)
{
	// Open the fd ourselves and use setDataSourceFd: a raw-path setDataSource
	// runs in the media extractor's own process, which can't reach our
	// app-scoped external files dir — but it can read an fd we pass it.
	int fd = ::open(path.c_str(), O_RDONLY);
	if (fd < 0) {
		LOGE("open(%s) failed: %s", path.c_str(), strerror(errno));
		return false;
	}
	struct stat st;
	int64_t length = (::fstat(fd, &st) == 0) ? (int64_t)st.st_size : 0;
	return openFd(fd, 0, length);
}

bool
VideoDecoder::openFd(int fd, int64_t offset, int64_t length)
{
	ownedFd_ = fd;
	ex_ = AMediaExtractor_new();
	if (AMediaExtractor_setDataSourceFd(ex_, fd, offset, length) != AMEDIA_OK) {
		LOGE("AMediaExtractor_setDataSourceFd failed");
		AMediaExtractor_delete(ex_);
		ex_ = nullptr;
		::close(ownedFd_);
		ownedFd_ = -1;
		return false;
	}
	return start();
}

bool
VideoDecoder::start()
{
	const size_t tracks = AMediaExtractor_getTrackCount(ex_);
	int videoTrack = -1;
	AMediaFormat *trackFmt = nullptr;
	const char *mime = nullptr;
	for (size_t i = 0; i < tracks; ++i) {
		AMediaFormat *f = AMediaExtractor_getTrackFormat(ex_, i);
		const char *m = nullptr;
		if (AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &m) && m &&
		    std::strncmp(m, "video/", 6) == 0) {
			videoTrack = (int)i;
			trackFmt = f;
			mime = m;
			break;
		}
		AMediaFormat_delete(f);
	}
	if (videoTrack < 0) {
		LOGE("no video track");
		return false;
	}
	width_ = fmtInt(trackFmt, AMEDIAFORMAT_KEY_WIDTH, 0);
	height_ = fmtInt(trackFmt, AMEDIAFORMAT_KEY_HEIGHT, 0);
	int64_t dur = 0;
	if (AMediaFormat_getInt64(trackFmt, AMEDIAFORMAT_KEY_DURATION, &dur)) durationUs_ = dur;
	AMediaExtractor_selectTrack(ex_, videoTrack);

	codec_ = AMediaCodec_createDecoderByType(mime);
	if (codec_ == nullptr) {
		LOGE("createDecoderByType(%s) failed", mime);
		AMediaFormat_delete(trackFmt);
		return false;
	}
	// Ask for a flexible (linear, CPU-readable) YUV output rather than a tiled
	// vendor surface format, so we can copy planes out of the ByteBuffer.
	AMediaFormat_setInt32(trackFmt, AMEDIAFORMAT_KEY_COLOR_FORMAT, kColorFlexible);
	if (AMediaCodec_configure(codec_, trackFmt, nullptr, nullptr, 0) != AMEDIA_OK) {
		LOGE("AMediaCodec_configure failed");
		AMediaFormat_delete(trackFmt);
		return false;
	}
	AMediaFormat_delete(trackFmt);
	if (AMediaCodec_start(codec_) != AMEDIA_OK) {
		LOGE("AMediaCodec_start failed");
		return false;
	}
	LOGI("VideoDecoder open: %s %dx%d", mime, width_, height_);
	open_.store(true, std::memory_order_relaxed);
	stop_.store(false, std::memory_order_relaxed);
	thread_ = std::thread([this] { decodeLoop(); });
	return true;
}

void
VideoDecoder::extractFrame(uint8_t *src, int /*srcSize*/)
{
	const int32_t color = fmtInt(outFmt_, AMEDIAFORMAT_KEY_COLOR_FORMAT, kColorNV12);
	int32_t stride = fmtInt(outFmt_, AMEDIAFORMAT_KEY_STRIDE, width_);
	int32_t sliceH = fmtInt(outFmt_, AMEDIAFORMAT_KEY_SLICE_HEIGHT, height_);
	if (stride < width_) stride = width_;
	if (sliceH < height_) sliceH = height_;
	const int32_t range = fmtInt(outFmt_, AMEDIAFORMAT_KEY_COLOR_RANGE, 0);
	const bool fullRange = (range == 1);  // COLOR_RANGE_FULL=1, LIMITED=2
	const bool planar = (color == kColorI420);

	const int w = width_, h = height_;
	const int cw = (w + 1) / 2, ch = (h + 1) / 2;

	Frame &fr = buffers_[producer_];
	fr.width = w;
	fr.height = h;
	fr.nv12 = !planar;
	fr.fullRange = fullRange;

	// Y plane: copy display width out of each strided row.
	fr.y.resize((size_t)w * h);
	for (int r = 0; r < h; ++r) {
		std::memcpy(fr.y.data() + (size_t)r * w, src + (size_t)r * stride, w);
	}
	const uint8_t *chromaSrc = src + (size_t)stride * sliceH;

	if (planar) {  // I420: separate U then V planes, chroma stride = stride/2
		const int cstride = stride / 2;
		const uint8_t *uSrc = chromaSrc;
		const uint8_t *vSrc = chromaSrc + (size_t)cstride * (sliceH / 2);
		fr.uv.resize((size_t)cw * ch);
		fr.v.resize((size_t)cw * ch);
		for (int r = 0; r < ch; ++r) {
			std::memcpy(fr.uv.data() + (size_t)r * cw, uSrc + (size_t)r * cstride, cw);
			std::memcpy(fr.v.data() + (size_t)r * cw, vSrc + (size_t)r * cstride, cw);
		}
	} else {  // NV12: interleaved UV, row bytes = cw*2, source row stride = stride
		const int rowBytes = cw * 2;
		fr.uv.resize((size_t)rowBytes * ch);
		for (int r = 0; r < ch; ++r) {
			std::memcpy(fr.uv.data() + (size_t)r * rowBytes, chromaSrc + (size_t)r * stride,
			            rowBytes);
		}
	}

	std::lock_guard<std::mutex> lk(mutex_);
	fr.serial = ++serial_;
	std::swap(producer_, middle_);
	hasNew_ = true;
}

void
VideoDecoder::seekRelative(double deltaSeconds)
{
	if (!open_.load(std::memory_order_relaxed)) return;
	int64_t target = positionUs_.load(std::memory_order_relaxed) + (int64_t)(deltaSeconds * 1e6);
	if (target < 0) target = 0;
	if (durationUs_ > 0 && target > durationUs_) target = durationUs_;
	seekRequestUs_.store(target, std::memory_order_relaxed);
}

void
VideoDecoder::seekTo(double seconds)
{
	if (!open_.load(std::memory_order_relaxed)) return;
	int64_t target = (int64_t)(seconds * 1e6);
	if (target < 0) target = 0;
	if (durationUs_ > 0 && target > durationUs_) target = durationUs_;
	seekRequestUs_.store(target, std::memory_order_relaxed);
}

void
VideoDecoder::decodeLoop()
{
	using clock = std::chrono::steady_clock;
	auto wallStart = clock::now();
	int64_t firstPtsUs = -1;
	bool sawInputEOS = false;
	bool decodeOneWhilePaused = false;  // after a seek-while-paused, show the new frame

	while (!stop_.load(std::memory_order_relaxed)) {
		// ── seek (works even while paused: reposition + flush, then show one frame) ──
		const int64_t sk = seekRequestUs_.exchange(-1, std::memory_order_relaxed);
		if (sk >= 0) {
			AMediaExtractor_seekTo(ex_, sk, AMEDIAEXTRACTOR_SEEK_CLOSEST_SYNC);
			AMediaCodec_flush(codec_);
			sawInputEOS = false;
			firstPtsUs = -1;
			positionUs_.store(sk, std::memory_order_relaxed);
			decodeOneWhilePaused = paused_.load(std::memory_order_relaxed);
		}
		// ── pause: hold the current frame (don't feed/drain) unless a seek just asked
		//    for one fresh frame ──
		if (paused_.load(std::memory_order_relaxed) && !decodeOneWhilePaused) {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			continue;
		}

		// ── feed input ──
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

		// ── drain output ──
		AMediaCodecBufferInfo info;
		ssize_t outIdx = AMediaCodec_dequeueOutputBuffer(codec_, &info, 2000);
		if (outIdx >= 0) {
			if (info.size > 0 && outFmt_ != nullptr) {
				size_t outSize = 0;
				uint8_t *obuf = AMediaCodec_getOutputBuffer(codec_, outIdx, &outSize);
				if (obuf != nullptr) {
					// Pace the frame: to the audio clock if one is set (A/V master),
					// else to the frame PTS on our own wall clock.
					if (!decodeOneWhilePaused) {
						if (masterClock_ != nullptr) {
							const double audioSec = masterClock_(masterCtx_);
							if (audioSec >= 0.0) {
								const double frameSec = info.presentationTimeUs / 1e6;
								for (int guard = 0; guard < 200 &&
								                    !stop_.load(std::memory_order_relaxed) &&
								                    !paused_.load(std::memory_order_relaxed) &&
								                    masterClock_(masterCtx_) + 0.005 < frameSec;
								     ++guard) {
									std::this_thread::sleep_for(std::chrono::milliseconds(2));
								}
							}
						} else {
							if (firstPtsUs < 0) {
								firstPtsUs = info.presentationTimeUs;
								wallStart = clock::now();
							}
							const int64_t targetUs = info.presentationTimeUs - firstPtsUs;
							const int64_t elapsedUs =
							    std::chrono::duration_cast<std::chrono::microseconds>(
							        clock::now() - wallStart)
							        .count();
							if (targetUs > elapsedUs + 1000) {
								std::this_thread::sleep_for(
								    std::chrono::microseconds(targetUs - elapsedUs));
							}
						}
					}
					extractFrame(obuf, (int)info.size);
					positionUs_.store(info.presentationTimeUs, std::memory_order_relaxed);
					decodeOneWhilePaused = false;  // shown the post-seek frame; hold again
				}
			}
			const bool eos = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
			AMediaCodec_releaseOutputBuffer(codec_, outIdx, false);
			if (eos) {  // loop: seek back + flush, restart the clock
				AMediaExtractor_seekTo(ex_, 0, AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC);
				AMediaCodec_flush(codec_);
				sawInputEOS = false;
				firstPtsUs = -1;
			}
		} else if (outIdx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
			if (outFmt_) AMediaFormat_delete(outFmt_);
			outFmt_ = AMediaCodec_getOutputFormat(codec_);
			LOGI("output format changed: %s", AMediaFormat_toString(outFmt_));
		}
	}
}

const VideoDecoder::Frame *
VideoDecoder::acquireLatest()
{
	std::lock_guard<std::mutex> lk(mutex_);
	if (!hasNew_) return nullptr;
	std::swap(consumer_, middle_);
	hasNew_ = false;
	return &buffers_[consumer_];
}

void
VideoDecoder::stop()
{
	stop_.store(true, std::memory_order_relaxed);
	if (thread_.joinable()) thread_.join();
	if (codec_) {
		AMediaCodec_stop(codec_);
		AMediaCodec_delete(codec_);
		codec_ = nullptr;
	}
	if (ex_) {
		AMediaExtractor_delete(ex_);
		ex_ = nullptr;
	}
	if (outFmt_) {
		AMediaFormat_delete(outFmt_);
		outFmt_ = nullptr;
	}
	if (ownedFd_ >= 0) {
		::close(ownedFd_);
		ownedFd_ = -1;
	}
	open_.store(false, std::memory_order_relaxed);
}

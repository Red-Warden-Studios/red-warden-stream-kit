/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

// A single-producer / single-consumer ring of planar float audio frames: the hand-off between OBS's audio
// thread (producer: the audio capture callback) and the LUFS worker thread (consumer). Pure: std only, no
// libobs, no Qt. init() allocates; write(), read() and the counters never allocate, lock or log.
//
// Blocks are all-or-nothing: a write that does not fit in the free space is dropped whole and counted
// (droppedBlocks / droppedFrames), so what the consumer reads is always an in-order series of whole blocks
// with whole blocks missing, never a torn one. Positions are monotonic 64-bit frame counters (acquire /
// release), the index is position % capacity.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace inflight {

class SpscFrameRing {
public:
	// Not thread safe: call before the producer and the consumer start. Returns false for 0 channels,
	// more than 8 channels or 0 frames.
	bool init(int channels, size_t capacityFrames)
	{
		if (channels < 1 || channels > 8 || capacityFrames == 0)
			return false;
		channels_ = channels;
		cap_ = capacityFrames;
		data_.assign(static_cast<size_t>(channels) * capacityFrames, 0.0f);
		head_.store(0, std::memory_order_relaxed);
		tail_.store(0, std::memory_order_relaxed);
		droppedBlocks_.store(0, std::memory_order_relaxed);
		droppedFrames_.store(0, std::memory_order_relaxed);
		return true;
	}

	int channels() const { return channels_; }
	size_t capacity() const { return cap_; }

	// Producer thread only. planes[c] (c < channels) is `frames` samples of channel c; a null plane (or a
	// null `planes`, or gain == 0) writes silence for that channel. Every sample is multiplied by gain.
	// Returns false (and counts the drop) when the block does not fit.
	bool write(const float *const *planes, size_t frames, float gain)
	{
		if (frames == 0)
			return true;
		const std::uint64_t h = head_.load(std::memory_order_relaxed);
		const std::uint64_t t = tail_.load(std::memory_order_acquire);
		if (frames > cap_ || cap_ - static_cast<size_t>(h - t) < frames) {
			droppedBlocks_.fetch_add(1, std::memory_order_relaxed);
			droppedFrames_.fetch_add(frames, std::memory_order_relaxed);
			return false;
		}
		const size_t pos = static_cast<size_t>(h % cap_);
		const size_t first = std::min(frames, cap_ - pos);
		for (int c = 0; c < channels_; c++) {
			float *dst = &data_[static_cast<size_t>(c) * cap_];
			const float *src = planes ? planes[c] : nullptr;
			if (!src || gain == 0.0f) {
				std::memset(dst + pos, 0, first * sizeof(float));
				std::memset(dst, 0, (frames - first) * sizeof(float));
			} else if (gain == 1.0f) {
				std::memcpy(dst + pos, src, first * sizeof(float));
				std::memcpy(dst, src + first, (frames - first) * sizeof(float));
			} else {
				for (size_t i = 0; i < first; i++)
					dst[pos + i] = src[i] * gain;
				for (size_t i = first; i < frames; i++)
					dst[i - first] = src[i] * gain;
			}
		}
		head_.store(h + frames, std::memory_order_release);
		return true;
	}

	// Consumer thread only. Copies up to maxFrames frames into dst[c] (planar) and returns how many.
	size_t read(float *const *dst, size_t maxFrames)
	{
		const std::uint64_t t = tail_.load(std::memory_order_relaxed);
		const std::uint64_t h = head_.load(std::memory_order_acquire);
		const size_t n = std::min(static_cast<size_t>(h - t), maxFrames);
		if (n == 0)
			return 0;
		const size_t pos = static_cast<size_t>(t % cap_);
		const size_t first = std::min(n, cap_ - pos);
		for (int c = 0; c < channels_; c++) {
			const float *src = &data_[static_cast<size_t>(c) * cap_];
			std::memcpy(dst[c], src + pos, first * sizeof(float));
			std::memcpy(dst[c] + first, src, (n - first) * sizeof(float));
		}
		tail_.store(t + n, std::memory_order_release);
		return n;
	}

	// Either thread: frames written so far / read so far (monotonic), and the drops.
	std::uint64_t writtenFrames() const { return head_.load(std::memory_order_acquire); }
	std::uint64_t readFrames() const { return tail_.load(std::memory_order_acquire); }
	std::uint64_t droppedBlocks() const { return droppedBlocks_.load(std::memory_order_relaxed); }
	std::uint64_t droppedFrames() const { return droppedFrames_.load(std::memory_order_relaxed); }

private:
	int channels_ = 0;
	size_t cap_ = 0;
	std::vector<float> data_;
	alignas(64) std::atomic<std::uint64_t> head_{0};
	alignas(64) std::atomic<std::uint64_t> tail_{0};
	std::atomic<std::uint64_t> droppedBlocks_{0};
	std::atomic<std::uint64_t> droppedFrames_{0};
};

} // namespace inflight

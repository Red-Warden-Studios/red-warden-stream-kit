/*
Red Warden Pre-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "mic-test.hpp"

#include <obs.h>
#include <obs-audio-controls.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>

namespace preflight {
namespace {
constexpr size_t kMaxSamples = 8192; // 5 s of audio callbacks is a few hundred; this is a safety cap

qint64 nowMs()
{
	return static_cast<qint64>(os_gettime_ns() / 1000000ULL);
}
} // namespace

// Owned by MicTest, handed to the volmeter as its callback parameter. The audio thread only ever
// sees this struct (never the QObject). It is destroyed only after the callback is removed, and
// obs_volmeter_remove_callback takes the same lock the volmeter holds while dispatching, so no
// callback can still be running once removal returns.
struct MicTest::Collector {
	std::mutex m;
	std::vector<MicTest::Sample> samples;
};

MicTest::MicTest(QObject *parent) : QObject(parent) {}

MicTest::~MicTest()
{
	teardown(nullptr, nullptr, nullptr);
}

// Audio thread. Uses the post-fader peak (what reaches the stream: a muted or faded-down mic reads
// quiet here, which is exactly what we want to catch), not input_peak (pre-fader).
// Values are dBFS; unused channel slots read -inf. The magnitude (the volmeter's RMS over the callback window,
// post-fader like the peak) is collected too: the loudest channel's, the same way the peak is, for the "Typical"
// number.
void MicTest::onVolmeter(void *param, const float magnitude[], const float peak[], const float[])
{
	auto *c = static_cast<Collector *>(param);
	auto loudest = [](const float v[]) {
		float best = -200.0f;
		for (int i = 0; i < MAX_AUDIO_CHANNELS; ++i) {
			float x = v[i];
			if (std::isnan(x))
				continue;
			if (x < -200.0f) // also catches -inf
				x = -200.0f;
			if (x > 0.0f)
				x = 0.0f;
			best = std::max(best, x);
		}
		return best;
	};
	const Sample s{nowMs(), loudest(peak), loudest(magnitude)};
	std::lock_guard<std::mutex> lk(c->m);
	if (c->samples.size() < kMaxSamples)
		c->samples.push_back(s);
}

bool MicTest::start(const QString &micUuid)
{
	if (gen_.running() || micUuid.isEmpty())
		return false;
	obs_source_t *src = obs_get_source_by_uuid(micUuid.toUtf8().constData());
	if (!src)
		return false;
	if (!(obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO)) {
		obs_source_release(src);
		return false;
	}
	obs_volmeter_t *vol = obs_volmeter_create(OBS_FADER_LOG); // same mapping as OBS's mixer
	if (!vol) {
		obs_source_release(src);
		return false;
	}
	collector_ = std::make_unique<Collector>();
	src_ = src; // keep the strong ref until teardown, so the source cannot vanish under us
	vol_ = vol;
	startMs_ = nowMs();
	obs_volmeter_add_callback(vol_, &MicTest::onVolmeter, collector_.get());
	if (!obs_volmeter_attach_source(vol_, src_)) {
		teardown(nullptr, nullptr, nullptr);
		return false;
	}
	last_ = MicTestStatus::NotRun;
	clearNumbers();
	const std::uint64_t token = gen_.begin();
	// Context object = this, so the callback dies with us; the token makes a late fire from a
	// cancelled/superseded run harmless.
	QTimer::singleShot(static_cast<int>(kMicTestWindowMs), this, [this, token] { onTimeout(token); });
	emit runningChanged(true);
	return true;
}

void MicTest::clearNumbers()
{
	if (numbers_ == MicNumbers())
		return;
	numbers_ = MicNumbers();
	emit numbersChanged();
}

void MicTest::teardown(bool *muted, bool *active, std::vector<Sample> *samples)
{
	if (vol_) {
		obs_volmeter_remove_callback(vol_, &MicTest::onVolmeter, collector_.get());
		if (src_) {
			if (muted)
				*muted = obs_source_muted(src_);
			if (active)
				*active = obs_source_audio_active(src_);
		}
		obs_volmeter_detach_source(vol_);
		obs_volmeter_destroy(vol_);
		vol_ = nullptr;
	}
	if (samples && collector_) {
		std::lock_guard<std::mutex> lk(collector_->m);
		samples->swap(collector_->samples);
	}
	if (src_) {
		obs_source_release(src_);
		src_ = nullptr;
	}
	collector_.reset();
}

void MicTest::onTimeout(std::uint64_t token)
{
	if (!gen_.complete(token))
		return; // cancelled, expired or superseded since this timer was armed: publish nothing
	bool muted = false;
	bool active = false;
	std::vector<Sample> samples;
	teardown(&muted, &active, &samples);

	MicTestAnalyzer an;
	an.start(startMs_);
	for (const auto &s : samples)
		an.add(s.t, s.peak, s.mag);
	const MicTestStatus result = muted ? MicTestStatus::Muted : an.finish(active);
	const MicNumbers nums = muted ? MicNumbers() : an.numbers();

	// One line per finished test, numbers only (never audio). obs_log adds the "[rws-preflight]" prefix.
	const auto sum = an.summary();
	const char *name = "NoAudio";
	switch (result) {
	case MicTestStatus::Passed:
		name = "Passed";
		break;
	case MicTestStatus::TooQuiet:
		name = "TooQuiet";
		break;
	case MicTestStatus::Muted:
		name = "Muted";
		break;
	default:
		break;
	}
	char maxBuf[32] = "none", floorBuf[32] = "none", oldBuf[32] = "none";
	if (sum.count > 0) {
		snprintf(maxBuf, sizeof maxBuf, "%.1f", static_cast<double>(sum.maxDb));
		snprintf(floorBuf, sizeof floorBuf, "%.1f", static_cast<double>(sum.floorP10Db));
	}
	if (sum.oldFloorDb)
		snprintf(oldBuf, sizeof oldBuf, "%.1f", static_cast<double>(*sum.oldFloorDb));
	obs_log(LOG_INFO, "mic test: samples=%d max=%s floor_p10=%s old_floor=%s result=%s", sum.count, maxBuf,
		floorBuf, oldBuf, name);
	if (nums.valid)
		obs_log(LOG_INFO, "mic test numbers: loudest=%s typical=%s", formatDb(nums.loudestDb).toUtf8().constData(),
			formatDb(nums.typicalDb).toUtf8().constData());

	last_ = result;
	if (numbers_ != nums) {
		numbers_ = nums;
		emit numbersChanged();
	}
	emit runningChanged(false);
	emit finished(result);
}

void MicTest::cancel()
{
	const bool was = gen_.cancel(); // invalidates the pending finisher first
	teardown(nullptr, nullptr, nullptr);
	last_ = MicTestStatus::NotRun;
	clearNumbers();
	if (was)
		emit runningChanged(false);
}

void MicTest::expire()
{
	if (gen_.running())
		cancel(); // a result for a test that started before the expiry must never appear later
	else {
		last_ = MicTestStatus::NotRun;
		clearNumbers();
	}
}

} // namespace preflight

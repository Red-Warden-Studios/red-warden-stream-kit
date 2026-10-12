/*
Red Warden Pre-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "preflight-eval.hpp"
#include "run-generation.hpp"

#include <QObject>
#include <QString>
#include <QTimer>

#include <memory>
#include <utility>
#include <vector>

struct obs_source;
struct obs_volmeter;

namespace preflight {

// The explicit 5 s "Test mic" run. UI thread only, except the volmeter callback (audio thread),
// which only appends {time, peak, magnitude} into a mutex-guarded vector owned by a heap Collector.
class MicTest : public QObject {
	Q_OBJECT
public:
	explicit MicTest(QObject *parent = nullptr);
	~MicTest() override;

	// Resolves the source by UUID and listens to it for 5 s. False if it cannot start (no such
	// source / not an audio source / already running).
	bool start(const QString &micUuid);
	// Abort without a result; the last result goes back to NotRun.
	void cancel();
	// Forget a previous result (stream stopped, mic changed). A test still running is cancelled
	// (it must never publish a result afterwards).
	void expire();

	bool running() const { return gen_.running(); }
	MicTestStatus lastResult() const { return last_; }
	// Loudest / Typical of the last finished test (not valid before one, after cancel / expire / a new start, or
	// when the mic was muted: a muted mic's levels say nothing about the room).
	MicNumbers lastNumbers() const { return numbers_; }

signals:
	void numbersChanged(); // lastNumbers() just changed (a test finished, or the numbers were cleared)
	void runningChanged(bool running);
	void finished(preflight::MicTestStatus result);

private:
	struct Collector;
	// One volmeter callback: when, the loudest channel's peak and the loudest channel's magnitude (dBFS).
	struct Sample {
		qint64 t;
		float peak;
		float mag;
	};
	void onTimeout(std::uint64_t token);
	void clearNumbers();
	// Detach + destroy the volmeter and drop the source ref. Optionally hands back what was heard.
	void teardown(bool *muted, bool *active, std::vector<Sample> *samples);

	static void onVolmeter(void *param, const float magnitude[], const float peak[], const float inputPeak[]);

	// Every test run gets a generation token; the 5 s finisher carries it and may publish only if the
	// run is still the live one (cancel / expire / restart invalidate it). See run-generation.hpp.
	RunGeneration gen_;
	MicTestStatus last_ = MicTestStatus::NotRun;
	MicNumbers numbers_;
	qint64 startMs_ = 0;
	obs_source *src_ = nullptr; // strong ref for the whole test
	obs_volmeter *vol_ = nullptr;
	std::unique_ptr<Collector> collector_;
};

} // namespace preflight

/*
Red Warden Multistream
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "config.hpp"

#include <obs.h>

#include <QElapsedTimer>

#include <atomic>
#include <QObject>
#include <QString>

// Owns the libobs objects (service, output, encoder refs) for one
// destination. Lives on the Qt UI thread; libobs signals are marshalled
// back onto it, so state is only ever touched from one thread.
class OutputRunner : public QObject {
	Q_OBJECT

public:
	enum class State { Idle, Starting, Live, Reconnecting, Stopping, Error };

	explicit OutputRunner(QObject *parent = nullptr);
	~OutputRunner() override;

	// Returns false and fills *error if the output could not be started.
	bool start(const Destination &d, const QString &streamKey, QString *error);
	void stop();      // graceful: flushes and disconnects
	void shutdown();  // immediate: used on OBS exit / profile switch

	State state() const { return st; }
	QString detail() const { return det; }
	double kbps() const { return rateKbps; }
	int droppedFrames() const;
	int totalFrames() const;
	// Names the CURRENT obs_output: a new value for every output this runner creates (one process-wide counter,
	// so a runner that is deleted and recreated never repeats a value); 0 while it has no output. In-Flight's
	// Network light compares it between ticks to know a destination's frame counters belong to another output.
	// UI thread, like everything else here.
	uint64_t outputSerial() const { return serial; }
	bool busy() const { return st == State::Starting || st == State::Live || st == State::Reconnecting || st == State::Stopping; }

	void poll(); // call ~1/s from the UI to refresh bitrate

	// Connected, but no video frame has gone out yet. A destination that joins
	// a running encoder must wait for its next keyframe.
	bool awaitingFirstFrame() const;
	// OBS's keyframe interval in seconds when sharing its encoder; 0 = auto.
	int sharedKeyintSec() const { return sharedKeyint; }
	bool usesSharedEncoder() const { return sharedMode; }
	// Audio bitrate actually being sent while busy (kbps), 0 if unknown.
	int audioBitrateKbps() const { return audioKbps; }
	// Why the last stop happened, and whether it is the kind worth retrying
	// automatically (network trouble) rather than one that needs the user
	// (rejected key, bad URL, unsupported format).
	int lastStopCode() const { return lastCode; }
	bool lastStopRetryable() const { return lastRetryable; }

signals:
	void changed();
	// Live, but no bytes have left for kStallMs while OBS is not reconnecting.
	void stalled();
	// A connection attempt is about to be made. Feeds the connection budget.
	// (libobs' internal reconnect is off, so every attempt is a start().)
	void attempted();

private:
	static void onStart(void *data, calldata_t *cd);
	static void onStop(void *data, calldata_t *cd);

	void handleStop(int code, const QString &lastError);
	void connectSignals();
	void disconnectSignals();
	void release();
	void setState(State s, const QString &d = QString());

	obs_output_t *output = nullptr;
	obs_service_t *service = nullptr;
	obs_encoder_t *venc = nullptr;
	obs_encoder_t *aenc = nullptr;

	State st = State::Idle;
	QString det;
	QString label; // destination name, for the log
	// Bumped whenever the output is released; queued libobs callbacks from an
	// older output compare against it and are dropped (see output-runner.cpp).
	std::atomic<uint64_t> gen{0};
	uint64_t serial = 0; // see outputSerial()
	bool stopRequested = false;
	bool sharedMode = false;
	int sharedKeyint = -1;
	int audioKbps = 0;
	int lastCode = 0;
	bool lastRetryable = false;

	uint64_t lastBytes = 0;
	QElapsedTimer rateClock;
	uint64_t stallBytes = 0;
	QElapsedTimer stallClock;
	double rateKbps = 0.0;
};

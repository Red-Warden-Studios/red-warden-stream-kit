/*
Red Warden Multistream
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "output-runner.hpp"
#include "obs-audio.hpp"
#include "platforms.hpp"
#include "server-url.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <QMetaObject>
#include <QUrl>

namespace {
// libobs' own reconnect is switched OFF for every output (0 retries). Its retries
// happen inside libobs without asking the connection budget, and once reconnecting
// it keeps retrying even when the server rejects the key. Instead a dropped
// connection ends the output with OBS_OUTPUT_DISCONNECTED and the dock reconnects
// through startOne(), so every attempt passes RetryGovernor and every failure is
// classified (a rejected key is never retried).
// A live output that sends nothing for this long is treated as stalled.
constexpr qint64 kStallMs = 6000;
// Before the first frame: allow for a long keyframe interval on OBS's side.
constexpr qint64 kFirstFrameMs = 20000;

QString stopReason(int code)
{
	switch (code) {
	case OBS_OUTPUT_SUCCESS:
		return {};
	case OBS_OUTPUT_BAD_PATH:
		return QStringLiteral("Invalid server URL");
	case OBS_OUTPUT_CONNECT_FAILED:
		return QStringLiteral("Could not reach the server");
	case OBS_OUTPUT_INVALID_STREAM:
		return QStringLiteral("Server rejected the stream key");
	case OBS_OUTPUT_DISCONNECTED:
		return QStringLiteral("Connection dropped");
	case OBS_OUTPUT_UNSUPPORTED:
		return QStringLiteral("This server does not accept this codec or format");
	case OBS_OUTPUT_ENCODE_ERROR:
		return QStringLiteral("Encoder error");
	default:
		return QStringLiteral("Stopped with an error");
	}
}
// Kick ingests through Amazon IVS, whose RTMPS endpoint needs the "app"
// path: rtmps://<host>:443/app/. A bare host from a dashboard copy gets it.
QString effectiveServer(const QString &raw, QString *note)
{
	const QString server = raw.trimmed();
	const QUrl u(server);
	if (u.host().endsWith(QStringLiteral(".live-video.net")) && (u.path().isEmpty() || u.path() == "/")) {
		if (note)
			*note = QStringLiteral("IVS ingest URL had no /app path; added it");
		return QStringLiteral("rtmps://%1:443/app/").arg(u.host());
	}
	return server;
}
} // namespace

namespace {
std::atomic<uint64_t> g_outputSerial{0}; // process-wide, see OutputRunner::outputSerial()
}

OutputRunner::OutputRunner(QObject *parent) : QObject(parent) {}

OutputRunner::~OutputRunner()
{
	release();
}

void OutputRunner::setState(State s, const QString &d)
{
	st = s;
	det = d;
	emit changed();
}

bool OutputRunner::start(const Destination &d, const QString &key, QString *error)
{
	auto fail = [&](const QString &why) {
		release();
		lastCode = OBS_OUTPUT_ERROR;
		lastRetryable = false;
		if (error)
			*error = why;
		setState(State::Error, why);
		return false;
	};

	if (busy())
		return true;
	release();
	label = d.name;
	stopRequested = false;
	sharedMode = false;
	sharedKeyint = -1;
	audioKbps = 0;
	lastCode = 0;
	lastRetryable = false;

	if (d.server.trimmed().isEmpty())
		return fail(QStringLiteral("No server URL set"));
	if (key.isEmpty())
		return fail(QStringLiteral("No stream key saved"));

	const QByteArray base = ("rws-ms-" + d.id).toUtf8();

	QString serverNote;
	const QString server = effectiveServer(d.server, &serverNote);
	if (!serverNote.isEmpty())
		obs_log(LOG_INFO, "[%s] %s", d.name.toUtf8().constData(), serverNote.toUtf8().constData());

	// A malformed URL never reaches a platform, but OBS reports it as
	// "could not connect", which reads as network trouble. Catch it here so
	// it is shown as a settings problem and never retried.
	{
		const QUrl u(server);
		const QString scheme = u.scheme().toLower();
		if ((scheme != "rtmp" && scheme != "rtmps") || u.host().isEmpty())
			return fail(QStringLiteral("Invalid server URL: it needs to look like rtmp://host/app"));
		// OBS logs the server URL. Never send one carrying credentials or the key
		// (e.g. from an imported config); make the user fix it in Edit instead.
		if (!u.userInfo().isEmpty() || serverUrlContainsKey(server, key))
			return fail(QStringLiteral("The server URL contains a password or the stream key; "
						   "put only the server in it (Edit)"));
	}

	obs_data_t *ss = obs_data_create();
	obs_data_set_string(ss, "server", server.toUtf8().constData());
	obs_data_set_string(ss, "key", key.toUtf8().constData());
	obs_data_set_bool(ss, "use_auth", false);
	service = obs_service_create_private("rtmp_custom", (base + "-service").constData(), ss);
	obs_data_release(ss);
	if (!service)
		return fail(QStringLiteral("Could not create the RTMP service"));

	if (d.enc.shared) {
		obs_output_t *main = obs_frontend_get_streaming_output();
		if (!main)
			return fail(QStringLiteral("Start Streaming in OBS first (this destination shares OBS's encoder)"));
		obs_encoder_t *v = obs_output_get_video_encoder(main);
		obs_encoder_t *a = obs_output_get_audio_encoder(main, 0);
		venc = v ? obs_encoder_get_ref(v) : nullptr;
		aenc = a ? obs_encoder_get_ref(a) : nullptr;
		obs_output_release(main);
		if (!venc || !aenc)
			return fail(QStringLiteral("OBS's stream encoder is not available yet"));
		sharedMode = true;
		obs_data_t *es = obs_encoder_get_settings(venc);
		sharedKeyint = es ? (int)obs_data_get_int(es, "keyint_sec") : -1;
		obs_data_release(es);
		obs_data_t *as = obs_encoder_get_settings(aenc);
		audioKbps = as ? (int)obs_data_get_int(as, "bitrate") : 0;
		obs_data_release(as);
	} else {
		const QByteArray encId = d.enc.encoderId.isEmpty() ? QByteArray("obs_x264") : d.enc.encoderId.toUtf8();
		obs_data_t *vs = obs_data_create();
		obs_data_set_string(vs, "rate_control", "CBR");
		obs_data_set_int(vs, "bitrate", d.enc.videoBitrate);
		obs_data_set_int(vs, "keyint_sec", 2);
		venc = obs_video_encoder_create(encId.constData(), (base + "-video").constData(), vs, nullptr);
		obs_data_release(vs);
		if (!venc)
			return fail(QStringLiteral("Could not create the video encoder"));
		if (d.enc.width > 0 && d.enc.height > 0)
			obs_encoder_set_scaled_size(venc, (uint32_t)d.enc.width, (uint32_t)d.enc.height);
		obs_encoder_set_video(venc, obs_get_video());

		// Audio: by default match OBS's own stream (bitrate, track and AAC
		// encoder), so a separate encoder never sounds worse than the main.
		const ObsStreamAudio oa = obsStreamAudio();
		const int abr = d.enc.audioBitrate > 0 ? d.enc.audioBitrate : oa.bitrate;
		const int track = d.enc.audioTrack >= 0 ? qBound(0, d.enc.audioTrack, 5) : oa.track;
		obs_data_t *as = obs_data_create();
		obs_data_set_int(as, "bitrate", abr);
		const QByteArray aid = oa.encoderId.toUtf8();
		aenc = obs_audio_encoder_create(aid.constData(), (base + "-audio").constData(), as, (size_t)track, nullptr);
		if (!aenc) // CoreAudio missing or refused the settings: fall back
			aenc = obs_audio_encoder_create("ffmpeg_aac", (base + "-audio").constData(), as, (size_t)track,
							nullptr);
		obs_data_release(as);
		if (!aenc)
			return fail(QStringLiteral("Could not create the audio encoder"));
		obs_encoder_set_audio(aenc, obs_get_audio());
		audioKbps = abr;
		obs_log(LOG_INFO, "[%s] audio: %s, %d kbps, track %d", d.name.toUtf8().constData(),
			obs_encoder_get_id(aenc), abr, track + 1);
	}

	output = obs_output_create("rtmp_output", base.constData(), nullptr, nullptr);
	if (!output)
		return fail(QStringLiteral("Could not create the RTMP output"));
	obs_output_set_service(output, service);
	obs_output_set_video_encoder(output, venc);
	obs_output_set_audio_encoder(output, aenc, 0);
	obs_output_set_reconnect_settings(output, 0, 0); // see the note at the top
	serial = ++g_outputSerial;
	connectSignals();

	lastBytes = 0;
	rateKbps = 0.0;
	rateClock.restart();
	setState(State::Starting, QStringLiteral("Connecting..."));

	obs_log(LOG_INFO, "[%s] starting (%s encoder)", label.toUtf8().constData(),
		d.enc.shared ? "shared" : "separate");

	emit attempted();
	if (!obs_output_start(output)) {
		const char *le = obs_output_get_last_error(output);
		return fail(le && *le ? QString::fromUtf8(le) : QStringLiteral("OBS refused to start the output"));
	}
	return true;
}

void OutputRunner::stop()
{
	if (!output) {
		if (st != State::Error)
			setState(State::Idle);
		return;
	}
	stopRequested = true;
	if (obs_output_active(output) || obs_output_reconnecting(output)) {
		setState(State::Stopping, QStringLiteral("Stopping..."));
		obs_output_stop(output);
	} else {
		release();
		obs_log(LOG_INFO, "[%s] stopped (was not connected)", label.toUtf8().constData());
		setState(State::Idle);
	}
}

void OutputRunner::shutdown()
{
	stopRequested = true;
	release();
	st = State::Idle;
	det.clear();
}

bool OutputRunner::awaitingFirstFrame() const
{
	return output && st == State::Live && obs_output_get_total_frames(output) == 0;
}

int OutputRunner::droppedFrames() const
{
	return output ? obs_output_get_frames_dropped(output) : 0;
}

int OutputRunner::totalFrames() const
{
	return output ? obs_output_get_total_frames(output) : 0;
}

void OutputRunner::poll()
{
	if (!output || st != State::Live) {
		rateKbps = 0.0;
		return;
	}
	const uint64_t bytes = obs_output_get_total_bytes(output);
	const qint64 ms = rateClock.restart();
	if (ms > 0 && bytes >= lastBytes)
		rateKbps = double(bytes - lastBytes) * 8.0 / double(ms); // bits per ms == kbps
	lastBytes = bytes;

	// Stall watchdog: libobs reports dropped connections, but not an output
	// that stays connected and simply stops receiving packets.
	if (bytes != stallBytes) {
		stallBytes = bytes;
		stallClock.restart();
	} else if (stallClock.isValid() && !obs_output_reconnecting(output) &&
		   stallClock.elapsed() > (obs_output_get_total_frames(output) > 0 ? kStallMs : kFirstFrameMs)) {
		obs_log(LOG_WARNING, "[%s] no data sent for %lld ms; restarting", label.toUtf8().constData(),
			(long long)stallClock.elapsed());
		stallClock.restart();
		emit stalled();
	}
}

// ---- libobs signal handlers: these run on libobs threads ----------------
//
// Each callback is queued to the UI thread. Before it runs there, the runner may
// have released this output and started a new one (a stall restart, an edit while
// live). A stale "stop" would then tear down the NEW output. So every callback
// carries the generation it was emitted under and is dropped if it no longer
// matches. gen is bumped in release() after the signals are disconnected, and
// libobs holds the signal lock while a handler runs, so a handler can never read
// a generation newer than its own output's.

void OutputRunner::onStart(void *data, calldata_t *)
{
	auto *self = static_cast<OutputRunner *>(data);
	const uint64_t g = self->gen.load();
	QMetaObject::invokeMethod(
		self,
		[self, g] {
			if (g != self->gen.load() || !self->output)
				return;
			self->lastBytes = 0;
			self->rateClock.restart();
			self->stallBytes = 0;
			self->stallClock.restart();
			obs_log(LOG_INFO, "[%s] live", self->label.toUtf8().constData());
			self->setState(State::Live);
		},
		Qt::QueuedConnection);
}

void OutputRunner::onStop(void *data, calldata_t *cd)
{
	auto *self = static_cast<OutputRunner *>(data);
	const uint64_t g = self->gen.load();
	const int code = (int)calldata_int(cd, "code");
	obs_output_t *out = static_cast<obs_output_t *>(calldata_ptr(cd, "output"));
	const char *le = out ? obs_output_get_last_error(out) : nullptr;
	const QString lastError = le ? QString::fromUtf8(le) : QString();
	QMetaObject::invokeMethod(
		self,
		[self, g, code, lastError] {
			if (g != self->gen.load() || !self->output)
				return;
			self->handleStop(code, lastError);
		},
		Qt::QueuedConnection);
}
// ---- back on the UI thread ----------------------------------------------

void OutputRunner::handleStop(int code, const QString &lastError)
{
	const bool wanted = stopRequested;
	release();
	lastCode = code;
	// Network trouble is retryable. A rejected key, bad URL or unsupported
	// format is not: retrying those only hammers the platform.
	lastRetryable = !wanted && (code == OBS_OUTPUT_DISCONNECTED || code == OBS_OUTPUT_CONNECT_FAILED);
	if (code == OBS_OUTPUT_SUCCESS || wanted) {
		obs_log(LOG_INFO, "[%s] stopped", label.toUtf8().constData());
		setState(State::Idle);
		return;
	}
	QString why = stopReason(code);
	if (!lastError.isEmpty())
		why += QStringLiteral(": ") + lastError;
	obs_log(LOG_WARNING, "[%s] stopped with error %d", label.toUtf8().constData(), code);
	setState(State::Error, why);
}

void OutputRunner::connectSignals()
{
	signal_handler_t *sh = obs_output_get_signal_handler(output);
	signal_handler_connect(sh, "start", onStart, this);
	signal_handler_connect(sh, "stop", onStop, this);
}

void OutputRunner::disconnectSignals()
{
	signal_handler_t *sh = obs_output_get_signal_handler(output);
	signal_handler_disconnect(sh, "start", onStart, this);
	signal_handler_disconnect(sh, "stop", onStop, this);
}

void OutputRunner::release()
{
	if (output) {
		disconnectSignals();
		if (obs_output_active(output) || obs_output_reconnecting(output))
			obs_output_force_stop(output);
		obs_output_release(output);
		output = nullptr;
	}
	serial = 0;
	++gen; // anything still queued from that output is now stale
	if (venc) {
		obs_encoder_release(venc);
		venc = nullptr;
	}
	if (aenc) {
		obs_encoder_release(aenc);
		aenc = nullptr;
	}
	if (service) {
		obs_service_release(service);
		service = nullptr;
	}
	rateKbps = 0.0;
}

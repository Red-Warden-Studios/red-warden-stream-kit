/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "inflight-probe.hpp"
#include "audio-watch.hpp"
#include "stream-signals.hpp"

#include <obs-frontend-api.h>
#include <obs.h>

#include <algorithm>

namespace inflight {
namespace {
quint64 nonNegative(long long v)
{
	return v < 0 ? 0 : static_cast<quint64>(v);
}

// Fills one audio input's source facts (not its level; AudioWatch does that). Releases what it takes.
void probeAudio(const QString &uuid, AudioInput &a)
{
	a.configured = !uuid.isEmpty();
	if (!a.configured)
		return;
	const QByteArray u = uuid.toUtf8();
	obs_source_t *src = obs_get_source_by_uuid(u.constData()); // new reference
	if (!src)
		return;
	a.exists = true;
	a.isAudio = (obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO) != 0;
	a.muted = obs_source_muted(src);
	a.audioActive = obs_source_audio_active(src);
	obs_source_release(src);
}
} // namespace

void Probe::pushRecordingEvent(RecordingEventKind kind, qint64 atMs)
{
	RecordingEvent e;
	e.kind = kind;
	e.atMs = atMs;
	recordingEvents_.push_back(e);
	if (recordingEvents_.size() > 64)
		recordingEvents_.erase(recordingEvents_.begin());
}

Inputs Probe::gather(const Settings &s, qint64, LiveState live, StreamSignals &sig, AudioWatch &audio)
{
	Inputs in;
	in.live = live;
	in.mainStreamActive = obs_frontend_streaming_active();

	probeAudio(s.micUuid, in.mic);
	probeAudio(s.desktopUuid, in.desktop);
	audio.fill(in.mic, AudioWatch::Which::Mic);
	audio.fill(in.desktop, AudioWatch::Which::Desktop);

	if (obs_source_t *scene = obs_frontend_get_current_scene()) { // new reference
		if (const char *u = obs_source_get_uuid(scene))
			in.programSceneUuid = QString::fromUtf8(u);
		obs_source_release(scene);
	}

	in.recording.active = obs_frontend_recording_active();
	in.recording.paused = obs_frontend_recording_paused();
	in.recordingEvents.swap(recordingEvents_);

	if (obs_output_t *out = sig.output()) { // borrowed: StreamSignals holds the reference
		in.network.droppedFrames = nonNegative(obs_output_get_frames_dropped(out));
		in.network.totalFrames = nonNegative(obs_output_get_total_frames(out));
		in.network.reconnecting = obs_output_reconnecting(out);
	}

	in.pc.renderLagged = obs_get_lagged_frames();
	in.pc.renderTotal = obs_get_total_frames();
	if (video_t *video = obs_get_video()) {
		in.pc.encoderSkipped = video_output_get_skipped_frames(video);
		in.pc.encoderTotal = video_output_get_total_frames(video);
	}

	in.events = sig.drain();
	return in;
}

} // namespace inflight

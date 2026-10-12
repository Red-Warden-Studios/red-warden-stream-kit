/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "inflight-eval.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace inflight {

namespace {

constexpr int kStream = static_cast<int>(Light::Stream);
constexpr int kNetwork = static_cast<int>(Light::Network);

int severity(LightState s)
{
	switch (s) {
	case LightState::Red:
		return 4;
	case LightState::Amber:
		return 3;
	case LightState::Unknown:
		return 2;
	case LightState::Green:
		return 1;
	case LightState::Grey:
		return 0;
	}
	return 0;
}

bool isBad(LightState s)
{
	return s == LightState::Amber || s == LightState::Red;
}

Candidate make(LightState st, Reason r, double value = 0.0)
{
	Candidate c;
	c.state = st;
	c.reason = r;
	c.value = value;
	return c;
}

// Fine for now, but the condition behind `bad` is already present.
Candidate makePending(Reason bad, double value)
{
	Candidate c = make(LightState::Green, Reason::Ok, value);
	c.pendingMask = quint64(1) << static_cast<int>(bad);
	return c;
}

// The most severe candidate wins (the earliest on a tie); pending conditions are all kept.
Candidate pick(const std::vector<Candidate> &v)
{
	Candidate best = make(LightState::Green, Reason::Ok);
	quint64 mask = 0;
	for (const Candidate &c : v) {
		mask |= c.pendingMask;
		if (severity(c.state) > severity(best.state))
			best = c;
	}
	best.pendingMask = mask;
	return best;
}

double pctOf(quint64 part, quint64 whole)
{
	return whole ? 100.0 * double(part) / double(whole) : 0.0;
}

bool isNegInf(float v)
{
	return std::isinf(v) && v < 0.0f;
}

} // namespace

// ---- FrameWindow ----

void FrameWindow::add(qint64 tMs, quint64 bad, quint64 total)
{
	if (!samples_.empty()) {
		const Sample &last = samples_.back();
		if (bad < last.bad || total < last.total || tMs < last.t)
			samples_.clear();
	}
	samples_.push_back({tMs, bad, total});
	// Drop old samples but keep one at or before the cutoff as the window's start.
	const qint64 cutoff = tMs - span_;
	while (samples_.size() >= 2 && samples_[1].t <= cutoff)
		samples_.pop_front();
}

WindowStats FrameWindow::stats(qint64 nowMs) const
{
	WindowStats w;
	if (samples_.size() < 2)
		return w;
	const Sample &b = samples_.back();
	if (nowMs - b.t > kFrameStaleMs)
		return w;
	const qint64 cutoff = nowMs - span_;
	int anchor = -1;
	for (int i = 0; i < static_cast<int>(samples_.size()); ++i)
		if (samples_[i].t <= cutoff)
			anchor = i;
	if (anchor < 0)
		return w;
	const Sample &a = samples_[anchor];
	if (nowMs - a.t > span_ + kFrameAnchorSlackMs)
		return w;
	for (int i = anchor + 1; i < static_cast<int>(samples_.size()); ++i)
		if (samples_[i].t - samples_[i - 1].t > kFrameStaleMs)
			return w;
	const quint64 dTotal = b.total - a.total;
	if (dTotal < kMinWindowFrames)
		return w;
	w.filled = true;
	w.pct = pctOf(b.bad - a.bad, dTotal);
	return w;
}

// ---- incident bookkeeping ----

IncidentEvent Evaluator::makeEvent(IncidentEventKind k, int i, qint64 now) const
{
	IncidentEvent e;
	e.kind = k;
	e.light = static_cast<Light>(i);
	e.incidentId = trk_[i].id;
	e.reason = trk_[i].reason;
	e.atMs = now;
	if (i == kNetwork)
		e.outputName = trk_[i].net.outputName;
	return e;
}

void Evaluator::endIncident(int i, qint64 now, std::vector<IncidentEvent> &ev, bool turnedOff)
{
	Tracker &t = trk_[i];
	if (!t.id)
		return;
	ev.push_back(makeEvent(IncidentEventKind::Ended, i, now));
	ev.back().turnedOff = turnedOff;
	t.net = NetDetail();
	t.id = 0;
	t.state = LightState::Grey;
	t.fineSince = -1;
	t.value = 0.0;
	t.redShown = false;
	t.acked = false;
	t.snoozeUntil = 0;
}

LightResult Evaluator::apply(int i, const Candidate &c, qint64 now, std::vector<IncidentEvent> &ev)
{
	Tracker &t = trk_[i];
	bool flash = false;
	if (c.state == LightState::Grey) {
		endIncident(i, now, ev);
	} else if (isBad(c.state)) {
		t.fineSince = -1;
		// One incident is one cause: a different reason (or an explicit new event) is a new incident.
		if (t.id && (c.forceNew || t.reason != c.reason))
			endIncident(i, now, ev);
		t.net = c.net; // before the Started / Escalated event is made, so the event names the output
		if (!t.id) {
			t.id = ++nextId_;
			t.state = c.state;
			t.reason = c.reason;
			t.value = c.value;
			t.startMs = now;
			t.redShown = c.state == LightState::Red;
			t.acked = false;
			t.snoozeUntil = 0;
			flash = t.redShown;
			ev.push_back(makeEvent(IncidentEventKind::Started, i, now));
		} else if (c.state == LightState::Red && t.state == LightState::Amber) {
			t.state = LightState::Red;
			t.value = c.value;
			// An incident flashes only the first time it is red.
			flash = !t.redShown;
			t.redShown = true;
			ev.push_back(makeEvent(IncidentEventKind::Escalated, i, now));
		} else {
			t.state = c.state;
			t.value = c.value;
		}
	} else if (c.state == LightState::Green) {
		if (t.id) {
			if ((c.pendingMask >> static_cast<int>(t.reason)) & 1) {
				t.fineSince = -1;
			} else {
				if (t.fineSince < 0)
					t.fineSince = now;
				if (now - t.fineSince >= kClearAfterFineMs)
					endIncident(i, now, ev);
			}
		}
	} else if (t.id) { // Unknown: neither fine nor worse
		t.fineSince = -1;
	}

	LightResult r;
	r.reason = c.reason;
	r.state = c.state;
	r.value = c.value;
	r.net = c.net;
	if (t.id) {
		r.net = t.net;
		// The held incident names the output it began with; what is worst NOW (and who is still filling) comes
		// from this tick's candidate, so the UI can tell a stale incident from a current one.
		r.net.currentBadValid = c.net.currentBadValid;
		r.net.currentBadId = c.net.currentBadId;
		r.net.mainFilling = c.net.mainFilling;
		r.net.fillingDestinations = c.net.fillingDestinations;
		r.incidentId = t.id;
		r.value = c.state == LightState::Unknown ? c.value : t.value;
		if (c.state != LightState::Unknown) {
			r.state = t.state;
			r.reason = t.reason;
		}
		r.acknowledged = t.acked;
		if (t.snoozeUntil > now)
			r.snoozedUntilMs = t.snoozeUntil;
	}
	r.flashNow = flash && r.snoozedUntilMs == 0;
	if (t.id) {
		r.sinceMs = t.startMs;
	} else {
		if (!t.haveOut || t.outState != r.state || t.outReason != r.reason)
			t.changeMs = now;
		r.sinceMs = t.changeMs;
	}
	t.haveOut = true;
	t.outState = r.state;
	t.outReason = r.reason;
	return r;
}

// ---- sessions, clock, public calls ----

void Evaluator::resetMicHolds()
{
	micMuted_.reset();
	micNoCb_.reset();
	micQuiet_.reset();
}

void Evaluator::resetHolds()
{
	resetMicHolds();
	deskNoCb_.reset();
	deskSilence_.reset();
	netWin_.reset();
	for (auto &kv : outs_)
		kv.second.win.reset();
	renderWin_.reset();
	encWin_.reset();
	for (Tracker &t : trk_)
		t.fineSince = -1;
}

// A light that is switched off, or not set up, forgets everything it was timing.
void Evaluator::resetLightTimers(Light l, qint64 now)
{
	switch (l) {
	case Light::Mic:
		resetMicHolds();
		micHeard_ = false;
		break;
	case Light::Desktop:
		deskNoCb_.reset();
		deskSilence_.reset();
		break;
	case Light::Scene:
		sceneStart_ = now;
		break;
	case Light::Recording:
		recWasActive_ = false;
		recPrevActive_ = false;
		recRunning_ = false;
		liveSince_ = now;
		break;
	case Light::Network:
		netWin_.reset();
		for (auto &kv : outs_)
			kv.second.win.reset();
		break;
	case Light::Pc:
		renderWin_.reset();
		encWin_.reset();
		break;
	case Light::Stream:
		latched_ = false;
		break;
	}
}

void Evaluator::clockWentBack(qint64 now)
{
	resetHolds();
	sceneStart_ = now;
	liveSince_ = now;
	latchSince_ = std::min(latchSince_, now);
	for (Tracker &t : trk_) {
		t.startMs = std::min(t.startMs, now);
		t.changeMs = std::min(t.changeMs, now);
		t.snoozeUntil = std::min(t.snoozeUntil, now + kSnoozeMs);
	}
}

void Evaluator::clearLatch(qint64 now, std::vector<IncidentEvent> &ev)
{
	if (!latched_)
		return;
	latched_ = false;
	endIncident(kStream, now, ev);
}

// A new session counts every output's frames afresh; an output that is gone is forgotten.
void Evaluator::forgetOutputs()
{
	mainOut_ = OutState();
	outs_.clear();
}

void Evaluator::resetForNewSession(qint64 now, std::vector<IncidentEvent> &ev)
{
	resetHolds();
	forgetOutputs();
	liveSince_ = now;
	sceneStart_ = now;
	recWasActive_ = false;
	recPrevActive_ = false;
	recRunning_ = false;
	micHeard_ = false;
	reconnectPending_ = false;
	liveSeen_ = false;
	clearLatch(now, ev);
	for (int i = 0; i < kLightCount; ++i)
		endIncident(i, now, ev);
}

void Evaluator::endSession(qint64 now, std::vector<IncidentEvent> &ev)
{
	resetHolds();
	forgetOutputs();
	reconnectPending_ = false;
	for (int i = 0; i < kLightCount; ++i)
		if (i != kStream || !latched_)
			endIncident(i, now, ev);
}

void Evaluator::resetBaselines()
{
	netWin_.reset();
	for (auto &kv : outs_)
		kv.second.win.reset();
	renderWin_.reset();
	encWin_.reset();
}

void Evaluator::acknowledge(Light l, qint64 nowMs)
{
	const int i = static_cast<int>(l);
	Tracker &t = trk_[i];
	if (!t.id)
		return;
	if (i == kStream && latched_) {
		pending_.push_back(makeEvent(IncidentEventKind::Acknowledged, i, nowMs));
		clearLatch(nowMs, pending_);
		return;
	}
	if (t.acked)
		return;
	t.acked = true;
	pending_.push_back(makeEvent(IncidentEventKind::Acknowledged, i, nowMs));
}

void Evaluator::snooze(Light l, qint64 nowMs)
{
	Tracker &t = trk_[static_cast<int>(l)];
	if (t.id)
		t.snoozeUntil = nowMs + kSnoozeMs;
}

void Evaluator::forgetIncidentMarks(qint64 nowMs)
{
	std::vector<IncidentEvent> discarded;
	clearLatch(nowMs, discarded);
	for (Tracker &t : trk_) {
		t.acked = false;
		t.snoozeUntil = 0;
	}
	pending_.clear();
}

const HoldingScene *Evaluator::holdingFor(const QString &uuid) const
{
	if (uuid.isEmpty())
		return nullptr;
	for (const HoldingScene &h : t_.holdingScenes)
		if (h.sceneUuid == uuid)
			return &h;
	return nullptr;
}

void Evaluator::handleEvent(const StreamEvent &e, qint64 now, std::vector<IncidentEvent> &ev)
{
	switch (e.kind) {
	case StreamEventKind::Started:
		liveSeen_ = true;
		reconnectPending_ = false;
		netWin_.reset();
		clearLatch(now, ev);
		break;
	case StreamEventKind::StoppedOk:
		liveSeen_ = false;
		reconnectPending_ = false;
		break;
	case StreamEventKind::StoppedError:
		if (liveSeen_ && t_.lightOn[kStream] && t_.setupDone && !latched_) {
			latched_ = true;
			latchCode_ = e.code;
			latchSince_ = (e.atMs > 0 && e.atMs <= now) ? e.atMs : now;
		}
		liveSeen_ = false;
		reconnectPending_ = false;
		break;
	case StreamEventKind::Reconnect:
		reconnectPending_ = true;
		break;
	case StreamEventKind::ReconnectSuccess:
		reconnectPending_ = false;
		netWin_.reset();
		break;
	case StreamEventKind::OutputReplaced:
		reconnectPending_ = false;
		netWin_.reset();
		break;
	}
}

Snapshot Evaluator::update(const Inputs &in, qint64 nowMs)
{
	Snapshot snap;
	snap.events.swap(pending_);
	if (haveLast_ && nowMs < lastNow_)
		clockWentBack(nowMs);
	haveLast_ = true;
	lastNow_ = nowMs;

	if (!haveScene_ || in.programSceneUuid != sceneUuid_) {
		haveScene_ = true;
		sceneUuid_ = in.programSceneUuid;
		sceneStart_ = nowMs;
	}

	if (in.live == LiveState::Live && knownLive_ != LiveState::Live)
		resetForNewSession(nowMs, snap.events);
	else if (in.live == LiveState::Off && knownLive_ == LiveState::Live)
		endSession(nowMs, snap.events);
	if (in.live != LiveState::Unknown)
		knownLive_ = in.live;
	else {
		resetHolds();
		sceneStart_ = nowMs;
	}

	if (in.live == LiveState::Live && in.mainStreamActive)
		liveSeen_ = true;
	for (const StreamEvent &e : in.events)
		handleEvent(e, nowMs, snap.events);

	// A Network incident that names a destination ends AT ONCE when that destination is no longer being
	// judged (switched off, removed, or its connection gone for good): the light then recomputes without it
	// on this very update instead of waiting out the 10 s "fine" rule. Switched off / removed is logged as
	// "turned off", never as recovered.
	{
		const Tracker &nt = trk_[kNetwork];
		if (nt.id && nt.net.valid && !nt.net.outputId.isEmpty()) {
			const OutputSample *smp = nullptr;
			for (const OutputSample &o : in.outputs)
				if (o.id == nt.net.outputId)
					smp = &o;
			const bool judged = smp && (smp->state == OutputState::Live || smp->state == OutputState::Reconnecting);
			if (!judged)
				endIncident(kNetwork, nowMs, snap.events, !smp || smp->state == OutputState::Off);
		}
	}

	netViews_.clear();
	for (int i = 0; i < kLightCount; ++i) {
		const Candidate c = evaluateLight(static_cast<Light>(i), in, nowMs);
		snap.lights[i] = apply(i, c, nowMs, snap.events);
		if (severity(snap.lights[i].state) > severity(snap.worst))
			snap.worst = snap.lights[i].state;
	}
	snap.netOutputs = netViews_;
	return snap;
}

Candidate Evaluator::evaluateLight(Light l, const Inputs &in, qint64 now)
{
	if (!t_.setupDone) {
		resetLightTimers(l, now);
		return make(LightState::Grey, Reason::NotSetUp);
	}
	if (!t_.lightOn[static_cast<int>(l)]) {
		resetLightTimers(l, now);
		return make(LightState::Grey, Reason::Disabled);
	}
	if (l == Light::Stream && latched_)
		return make(LightState::Red, Reason::StreamDropped, latchCode_);
	if (in.live == LiveState::Off)
		return make(LightState::Grey, Reason::OffAir);
	if (in.live == LiveState::Unknown)
		return make(LightState::Unknown, Reason::LiveUnknown);
	switch (l) {
	case Light::Mic:
		return evalMic(in, now);
	case Light::Desktop:
		return evalDesktop(in, now);
	case Light::Scene:
		return evalScene(now);
	case Light::Recording:
		return evalRecording(in, now);
	case Light::Network:
		return evalNetwork(in, now);
	case Light::Pc:
		return evalPc(in, now);
	case Light::Stream:
		return in.mainStreamActive ? make(LightState::Green, Reason::Ok) : make(LightState::Grey, Reason::MainStreamOff);
	}
	return make(LightState::Unknown, Reason::LiveUnknown);
}

// ---- the rules ----

Candidate Evaluator::evalMic(const Inputs &in, qint64 now)
{
	const AudioInput &m = in.mic;
	if (!m.configured) {
		resetMicHolds();
		return make(LightState::Grey, Reason::NotSetUp);
	}
	if (!m.exists || !m.isAudio) {
		resetMicHolds();
		return make(LightState::Red, m.exists ? Reason::MicNotAudio : Reason::MicMissing);
	}

	std::vector<Candidate> found;
	// An inactive source is red at once and outranks the mute timer (the first red listed wins a tie).
	if (!m.audioActive)
		found.push_back(make(LightState::Red, Reason::MicInactive));
	// Muted: amber after micMutedAmberS, red after micMutedRedS.
	const qint64 mutedFor = micMuted_.update(m.muted, now);
	if (m.muted) {
		const double secs = double(mutedFor) / 1000.0;
		if (mutedFor >= qint64(t_.micMutedRedS) * 1000)
			found.push_back(make(LightState::Red, Reason::MicMuted, secs));
		else if (mutedFor >= qint64(t_.micMutedAmberS) * 1000)
			found.push_back(make(LightState::Amber, Reason::MicMuted, secs));
		else
			found.push_back(makePending(Reason::MicMuted, secs));
	}

	// A meter that is attached but silent about its own level cannot be trusted.
	bool noCallback = false;
	double noCbSecs = 0.0;
	// A callback time ahead of the clock (after a backwards jump) is not a real one: the freshness
	// timer then starts from now, as if the meter had just been attached.
	if (m.attached && m.lastCallbackMs > 0 && m.lastCallbackMs <= now) {
		micNoCb_.reset();
		const qint64 age = now - m.lastCallbackMs;
		noCallback = age >= kNoCallbackMs;
		noCbSecs = double(age) / 1000.0;
	} else if (m.attached) {
		const qint64 waited = micNoCb_.update(true, now);
		noCallback = waited >= kNoCallbackMs;
		noCbSecs = double(waited) / 1000.0;
	} else {
		micNoCb_.reset();
	}
	if (noCallback)
		found.push_back(make(LightState::Unknown, Reason::MicNoCallback, noCbSecs));

	// "We haven't heard you": only once the mic has been heard this session, never while muted or on a
	// holding scene (leaving one restarts the count).
	const bool metered = m.attached && !noCallback;
	if (metered && m.avgDb > static_cast<float>(t_.heardYouDbfs))
		micHeard_ = true;
	const bool quiet = t_.heardYouEnabled && micHeard_ && metered && !m.muted &&
			   !holdingFor(in.programSceneUuid) && !(m.avgDb > static_cast<float>(t_.heardYouDbfs));
	const qint64 quietFor = micQuiet_.update(quiet, now);
	if (quiet) {
		const double secs = double(quietFor) / 1000.0;
		if (quietFor >= qint64(t_.heardYouSilenceS) * 1000)
			found.push_back(make(LightState::Amber, Reason::MicNotHeard, secs));
		else
			found.push_back(makePending(Reason::MicNotHeard, secs));
	}
	return pick(found);
}

Candidate Evaluator::evalDesktop(const Inputs &in, qint64 now)
{
	const AudioInput &d = in.desktop;
	if (!d.configured) {
		deskNoCb_.reset();
		deskSilence_.reset();
		return make(LightState::Grey, Reason::NotSetUp);
	}
	if (!d.exists || !d.isAudio) {
		deskNoCb_.reset();
		deskSilence_.reset();
		return make(LightState::Red, d.exists ? Reason::DesktopNotAudio : Reason::DesktopMissing);
	}
	std::vector<Candidate> found;
	if (d.muted)
		found.push_back(make(LightState::Red, Reason::DesktopMuted));

	bool noCallback = false;
	double noCbSecs = 0.0;
	if (d.attached && d.lastCallbackMs > 0 && d.lastCallbackMs <= now) {
		deskNoCb_.reset();
		const qint64 age = now - d.lastCallbackMs;
		noCallback = age >= kNoCallbackMs;
		noCbSecs = double(age) / 1000.0;
	} else if (d.attached) {
		const qint64 waited = deskNoCb_.update(true, now);
		noCallback = waited >= kNoCallbackMs;
		noCbSecs = double(waited) / 1000.0;
	} else {
		deskNoCb_.reset();
	}
	if (noCallback)
		found.push_back(make(LightState::Unknown, Reason::DesktopNoCallback, noCbSecs));

	// Digital silence: the peak is exactly -inf on every tick. A quiet but finite level never counts.
	const bool silent = d.attached && !noCallback && !d.muted && isNegInf(d.peakDb);
	const qint64 silentFor = deskSilence_.update(silent, now);
	if (silent) {
		const double secs = double(silentFor) / 1000.0;
		if (silentFor >= qint64(t_.desktopSilenceS) * 1000)
			found.push_back(make(LightState::Amber, Reason::DesktopDigitalSilence, secs));
		else
			found.push_back(makePending(Reason::DesktopDigitalSilence, secs));
	}
	return pick(found);
}

Candidate Evaluator::evalScene(qint64 now)
{
	if (t_.holdingScenes.isEmpty())
		return make(LightState::Grey, Reason::NotSetUp);
	const HoldingScene *h = holdingFor(sceneUuid_);
	if (!h)
		return make(LightState::Green, Reason::Ok);
	const qint64 elapsed = now - sceneStart_;
	const qint64 limit = qint64(h->limitSeconds) * 1000;
	const double over = double(elapsed - limit) / 1000.0;
	if (elapsed > limit + qint64(t_.sceneRedExtraS) * 1000)
		return make(LightState::Red, Reason::SceneOverLimit, over);
	if (elapsed > limit)
		return make(LightState::Amber, Reason::SceneOverLimit, over);
	return make(LightState::Green, Reason::Ok);
}

Candidate Evaluator::evalRecording(const Inputs &in, qint64 now)
{
	if (!t_.recordIntent) {
		resetLightTimers(Light::Recording, now);
		return make(LightState::Grey, Reason::Disabled);
	}
	const bool active = in.recording.active;
	// A stop is noticed from the events, so one that was undone before this update still counts; the
	// change of the steady-state flag covers a probe that sends no events.
	// A stop counts only if recording was running before it, so a repeated Stopped with no Started in
	// between changes nothing. Several full start/stop cycles inside one update may count as one stop.
	bool running = recRunning_;
	bool stoppedNow = false;
	for (const RecordingEvent &e : in.recordingEvents) {
		if (e.kind == RecordingEventKind::Started) {
			recWasActive_ = true;
			running = true;
		} else if (e.kind == RecordingEventKind::Stopped && running) {
			stoppedNow = true;
			running = false;
		}
	}
	if (recPrevActive_ && !active && running) { // no event told us, but the flag went down
		stoppedNow = true;
		running = false;
	}
	recPrevActive_ = active;
	if (active) {
		recWasActive_ = true;
		running = true;
	}
	recRunning_ = running;
	if (stoppedNow || (!active && recWasActive_)) {
		Candidate c = make(LightState::Red, Reason::RecordingStopped);
		c.forceNew = stoppedNow;
		return c;
	}
	if (active) {
		return in.recording.paused ? make(LightState::Amber, Reason::RecordingPaused)
					   : make(LightState::Green, Reason::Ok);
	}
	const qint64 waited = now - liveSince_;
	if (waited >= qint64(t_.recordStartGraceS) * 1000)
		return make(LightState::Red, Reason::RecordingNotStarted, double(waited) / 1000.0);
	return make(LightState::Green, Reason::Ok);
}

void Evaluator::accumulate(OutState &o, quint64 bad, quint64 total, quint64 gen)
{
	// A different output (generation) or a counter that went down is a fresh output whose counters start
	// at 0: everything it shows now was counted since it started.
	const bool fresh = !o.haveLast || (o.haveGen && gen != o.gen) || bad < o.lastBad || total < o.lastTotal;
	if (fresh) {
		o.sessBad += bad;
		o.sessTotal += total;
	} else {
		o.sessBad += bad - o.lastBad;
		o.sessTotal += total - o.lastTotal;
	}
	o.haveLast = true;
	o.haveGen = true;
	o.gen = gen;
	o.lastBad = bad;
	o.lastTotal = total;
}

double Evaluator::sessionPct(const OutState &o)
{
	return o.sessTotal ? pctOf(o.sessBad, o.sessTotal) : -1.0;
}

// The Network light judges OBS's own stream and each live Multistream destination SEPARATELY (so one bad
// destination is never averaged away by good ones) and shows the worst. Every output gets the full-window
// rule: its own 30 s window, the same thresholds, the same minimum frame count; reconnecting is amber.
Candidate Evaluator::evalNetwork(const Inputs &in, qint64 now)
{
	struct Item {
		QString id, name;
		LightState st;
		Reason reason;
		double pct;
	};
	std::vector<Item> items;     // judged outputs (a verdict exists)
	std::vector<QString> filling; // outputs whose window is still filling (no verdict yet)
	const auto judge = [this](double pct) {
		return pct > t_.dropRedPct ? LightState::Red : pct > t_.dropAmberPct ? LightState::Amber : LightState::Green;
	};

	// OBS's own stream (id "").
	if (!in.mainStreamActive) {
		netWin_.reset();
		mainOut_.haveLast = false;
	} else if (in.network.reconnecting || reconnectPending_) {
		// Drops while reconnecting are not judged. The session counters are NOT forgotten: the same output keeps
		// counting through a reconnect, and forgetting them would add its whole cumulative count a second time.
		// (A new stream, or a counter that went down, is still recognised in accumulate().)
		netWin_.reset();
		items.push_back({QString(), QString(), LightState::Amber, Reason::NetworkReconnecting, 0.0});
		netViews_.push_back({QString(), LightState::Amber, Reason::NetworkReconnecting, -1.0});
	} else {
		netWin_.add(now, in.network.droppedFrames, in.network.totalFrames);
		accumulate(mainOut_, in.network.droppedFrames, in.network.totalFrames, 0);
		const WindowStats w = netWin_.stats(now);
		if (w.filled) {
			const LightState st = judge(w.pct);
			const Reason r = st == LightState::Green ? Reason::Ok : Reason::NetworkDrops;
			items.push_back({QString(), QString(), st, r, w.pct});
			netViews_.push_back({QString(), st, r, w.pct});
		} else {
			filling.push_back(QString());
			netViews_.push_back({QString(), LightState::Unknown, Reason::NetworkWindowFilling, -1.0});
		}
	}

	// Multistream destinations. An output that is not live or reconnecting loses its window and its
	// "last counters" (its next output starts at 0); one that is no longer listed is forgotten entirely.
	bool anyDest = false;
	for (const OutputSample &s : in.outputs) {
		OutState &o = outs_[s.id];
		o.name = s.name;
		if (s.state == OutputState::Live) {
			anyDest = true;
			if (o.haveGen && o.gen != s.generation)
				o.win.reset(); // the output was replaced or restarted: a new baseline
			o.win.add(now, s.dropped, s.total);
			accumulate(o, s.dropped, s.total, s.generation);
			const WindowStats w = o.win.stats(now);
			if (w.filled) {
				const LightState st = judge(w.pct);
				const Reason r = st == LightState::Green ? Reason::Ok : Reason::NetworkDrops;
				items.push_back({s.id, s.name, st, r, w.pct});
				netViews_.push_back({s.id, st, r, w.pct});
			} else {
				filling.push_back(s.id);
				netViews_.push_back({s.id, LightState::Unknown, Reason::NetworkWindowFilling, -1.0});
			}
		} else if (s.state == OutputState::Reconnecting) {
			anyDest = true;
			o.win.reset(); // same reasoning as the main stream: keep haveLast across a reconnect
			items.push_back({s.id, s.name, LightState::Amber, Reason::NetworkReconnecting, 0.0});
			netViews_.push_back({s.id, LightState::Amber, Reason::NetworkReconnecting, -1.0});
		} else {
			o.win.reset();
			o.haveLast = false;
		}
	}
	for (auto it = outs_.begin(); it != outs_.end();) {
		bool listed = false;
		for (const OutputSample &s : in.outputs)
			listed = listed || s.id == it->first;
		it = listed ? std::next(it) : outs_.erase(it);
	}

	// The worst bad output. Red beats amber; among equals a reconnecting output (nothing is getting through)
	// beats a dropping one, then the bigger drop percentage; a full tie keeps the earlier (OBS's own stream first).
	const auto reconnecting = [](const Item &a) { return a.reason == Reason::NetworkReconnecting; };
	const auto worse = [&](const Item &a, const Item &b) {
		if (severity(a.st) != severity(b.st))
			return severity(a.st) > severity(b.st);
		if (reconnecting(a) != reconnecting(b))
			return reconnecting(a);
		return a.pct > b.pct;
	};
	const Item *best = nullptr;
	for (const Item &it : items)
		if (isBad(it.st) && (!best || worse(it, *best)))
			best = &it;

	const Tracker &trk = trk_[kNetwork];
	const auto outPct = [this](const QString &id) { return id.isEmpty() ? sessionPct(mainOut_) : sessionPct(outs_[id]); };
	if (best) {
		Candidate c = make(best->st, best->reason, best->pct);
		c.net.valid = true;
		c.net.outputId = best->id;
		c.net.outputName = best->name;
		c.net.sessionPct = outPct(best->id);
		c.net.multi = anyDest;
		c.net.currentBadValid = true;
		c.net.currentBadId = best->id;
		for (const Item &it : items) {
			if (&it == best || it.st != LightState::Green)
				continue;
			if (it.id.isEmpty())
				c.net.mainFine = true;
			else
				c.net.fineDestinations.push_back(it.name);
		}
		// A different output becoming the worst is a new incident (the log names each one).
		c.forceNew = trk.id && trk.net.outputId != best->id;
		return c;
	}

	// Nothing is bad. While ANY live output is still measuring the light is Unknown, never Green: Green says
	// every outgoing stream was judged fine, and a destination that has just gone live (or was replaced or has
	// just reconnected) has no verdict yet. This also covers the open incident whose output is refilling: it
	// stays open, exactly as for OBS's own stream (neither fine nor worse). The header dot does not count this
	// reason as Unknown (headerState), so the first 30 s of a session still show a calm dot.
	if (!filling.empty()) {
		Candidate c = make(LightState::Unknown, Reason::NetworkWindowFilling, -1.0);
		for (const QString &id : filling) {
			if (id.isEmpty())
				c.net.mainFilling = true;
			else
				c.net.fillingDestinations.push_back(outs_[id].name);
		}
		return c;
	}
	if (!items.empty()) { // every live output has a verdict and none is bad
		double worstPct = 0.0;
		for (const Item &it : items)
			worstPct = std::max(worstPct, it.pct);
		return make(LightState::Green, Reason::Ok, worstPct);
	}
	return make(LightState::Grey, Reason::MainStreamOff);
}

Candidate Evaluator::evalPc(const Inputs &in, qint64 now)
{
	renderWin_.add(now, in.pc.renderLagged, in.pc.renderTotal);
	encWin_.add(now, in.pc.encoderSkipped, in.pc.encoderTotal);
	const WindowStats rs = renderWin_.stats(now);
	const WindowStats es = encWin_.stats(now);
	if (!rs.filled && !es.filled)
		return make(LightState::Unknown, Reason::PcWindowFilling, -1.0);
	auto judge = [this](const WindowStats &w) {
		if (!w.filled)
			return LightState::Grey;
		return w.pct > t_.dropRedPct ? LightState::Red : w.pct > t_.dropAmberPct ? LightState::Amber : LightState::Green;
	};
	const LightState rst = judge(rs);
	const LightState est = judge(es);
	// The worse window decides; on a tie the one with more lost frames, then render.
	const bool useEncoder = severity(est) > severity(rst) || (severity(est) == severity(rst) && es.pct > rs.pct);
	const LightState st = useEncoder ? est : rst;
	const double pct = useEncoder ? es.pct : rs.pct;
	const Reason r = st == LightState::Green ? Reason::Ok : (useEncoder ? Reason::PcEncoderLag : Reason::PcRenderLag);
	return make(st, r, pct);
}

} // namespace inflight

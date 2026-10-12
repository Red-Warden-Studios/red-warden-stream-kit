// Tests for inflight-eval.cpp: the pure light evaluator. Every time is passed in, so each case replays
// exactly. No framework: exits non-zero on failure.
#include "inflight-eval.hpp"

#include <cmath>
#include <cstdio>
#include <limits>

using namespace inflight;
using St = LightState;
using Rs = Reason;

static int failures = 0;
static int total = 0;
#define CHECK(cond, msg)                                                      \
	do {                                                                  \
		total++;                                                      \
		if (!(cond)) {                                                \
			std::printf("FAIL: %s  (line %d)\n", msg, __LINE__); \
			failures++;                                           \
		} else {                                                      \
			std::printf("ok:   %s\n", msg);                        \
		}                                                             \
	} while (0)

static const float kInf = std::numeric_limits<float>::infinity();

static AudioInput healthyAudio()
{
	AudioInput a;
	a.configured = a.exists = a.isAudio = a.audioActive = a.attached = true;
	a.peakDb = -10.0f;
	a.avgDb = -20.0f;
	return a;
}

// A scripted run: one tick is one second unless a step says otherwise. Callbacks and frame counters
// advance by themselves unless switched off.
struct Rig {
	Thresholds th;
	Evaluator ev;
	Inputs in;
	qint64 now = 100000;
	Snapshot s;
	bool autoCb = true;
	bool autoCounters = true;
	int netDrop = 0, renderLag = 0, encSkip = 0; // lost frames per tick, out of 200
	std::vector<IncidentEvent> all;
	int flashes[kLightCount] = {};
	// Multistream destinations. Each Live one adds 200 frames and `drop` lost frames per tick when
	// autoCounters is on; tick() copies them into in.outputs. Not listed here = not listed to the evaluator.
	struct Dest {
		QString id, name;
		OutputState st = OutputState::Live;
		quint64 gen = 1;
		int drop = 0;
		quint64 dropped = 0, total = 0;
	};
	std::vector<Dest> dests;
	Dest &addDest(const char *id, const char *name, int drop = 0)
	{
		Dest d;
		d.id = id;
		d.name = name;
		d.drop = drop;
		dests.push_back(d);
		return dests.back();
	}
	Dest &dest(const char *id)
	{
		for (Dest &d : dests)
			if (d.id == QString(id))
				return d;
		static Dest none;
		return none;
	}
	void removeDest(const char *id)
	{
		for (size_t i = 0; i < dests.size(); ++i)
			if (dests[i].id == QString(id)) {
				dests.erase(dests.begin() + static_cast<long>(i));
				return;
			}
	}

	Rig()
	{
		HoldingScene start;
		start.sceneUuid = "start";
		start.limitSeconds = 300;
		HoldingScene brb;
		brb.sceneUuid = "brb";
		brb.limitSeconds = 600;
		th.holdingScenes = {start, brb};
		ev.setThresholds(th);
		in.mic = healthyAudio();
		in.desktop = healthyAudio();
		in.programSceneUuid = "main";
	}
	void apply() { ev.setThresholds(th); }
	const Snapshot &tick(qint64 dt = 1000)
	{
		now += dt;
		if (autoCb) {
			if (in.mic.attached)
				in.mic.lastCallbackMs = now;
			if (in.desktop.attached)
				in.desktop.lastCallbackMs = now;
		}
		if (autoCounters) {
			in.network.totalFrames += 200;
			in.network.droppedFrames += netDrop;
			in.pc.renderTotal += 200;
			in.pc.renderLagged += renderLag;
			in.pc.encoderTotal += 200;
			in.pc.encoderSkipped += encSkip;
			for (Dest &d : dests)
				if (d.st == OutputState::Live) {
					d.total += 200;
					d.dropped += static_cast<quint64>(d.drop);
				}
		}
		in.outputs.clear();
		for (const Dest &d : dests) {
			OutputSample o;
			o.id = d.id;
			o.name = d.name;
			o.state = d.st;
			o.dropped = d.dropped;
			o.total = d.total;
			o.generation = d.gen;
			in.outputs.push_back(o);
		}
		s = ev.update(in, now);
		in.events.clear();
		in.recordingEvents.clear();
		for (const IncidentEvent &e : s.events)
			all.push_back(e);
		for (int i = 0; i < kLightCount; ++i)
			if (s.lights[i].flashNow)
				flashes[i]++;
		return s;
	}
	void ticks(int n)
	{
		for (int i = 0; i < n; ++i)
			tick();
	}
	void live()
	{
		in.live = LiveState::Live;
		in.mainStreamActive = true;
		tick();
	}
	void off()
	{
		in.live = LiveState::Off;
		in.mainStreamActive = false;
		tick();
	}
	const LightResult &L(Light l) const { return s.at(l); }
	bool is(Light l, St st, Rs rs) const { return L(l).state == st && L(l).reason == rs; }
	int count(IncidentEventKind k, Light l) const
	{
		int n = 0;
		for (const IncidentEvent &e : all)
			if (e.kind == k && e.light == l)
				n++;
		return n;
	}
	// Mutes the mic and runs until it has been muted for `secs` seconds.
	void muteFor(int secs)
	{
		in.mic.muted = true;
		tick();
		ticks(secs);
	}
	void rec(RecordingEventKind k)
	{
		RecordingEvent e;
		e.kind = k;
		e.atMs = now + 1000;
		in.recordingEvents.push_back(e);
	}
	void post(StreamEventKind k, int code = 0)
	{
		StreamEvent e;
		e.kind = k;
		e.code = code;
		e.atMs = now + 1000;
		in.events.push_back(e);
	}
};

static bool lightsAll(const Rig &r, St st, Rs rs)
{
	for (int i = 0; i < kLightCount; ++i)
		if (r.s.lights[i].state != st || r.s.lights[i].reason != rs)
			return false;
	return true;
}

// A network case with exact counters: 30 s of flat counters, then the given totals on the 30th tick.
static Rig netCase(quint64 dropped, quint64 totalFrames, int tickAt = 30)
{
	Rig r;
	r.autoCounters = false;
	r.live();
	r.ticks(tickAt - 1);
	r.in.network.droppedFrames = dropped;
	r.in.network.totalFrames = totalFrames;
	r.tick();
	return r;
}

static Rig pcCase(quint64 rl, quint64 rt, quint64 es, quint64 et)
{
	Rig r;
	r.autoCounters = false;
	r.live();
	r.ticks(29);
	r.in.pc.renderLagged = rl;
	r.in.pc.renderTotal = rt;
	r.in.pc.encoderSkipped = es;
	r.in.pc.encoderTotal = et;
	r.tick();
	return r;
}

static void testBasics()
{
	{
		Rig r;
		r.tick();
		CHECK(lightsAll(r, St::Grey, Rs::OffAir), "off air: every light is grey with reason OffAir");
		CHECK(r.s.worst == St::Grey, "off air: worst is grey");
		CHECK(r.L(Light::Mic).incidentId == 0, "off air: no incident");
	}
	{
		Rig r;
		r.th.setupDone = false;
		r.apply();
		r.live();
		CHECK(lightsAll(r, St::Grey, Rs::NotSetUp), "setup not done: every light is grey NotSetUp");
	}
	{
		Rig r;
		r.live();
		CHECK(r.is(Light::Mic, St::Green, Rs::Ok), "healthy: mic green");
		CHECK(r.is(Light::Desktop, St::Green, Rs::Ok), "healthy: desktop green");
		CHECK(r.is(Light::Scene, St::Green, Rs::Ok), "healthy: scene green");
		CHECK(r.is(Light::Recording, St::Grey, Rs::Disabled), "no record intent: recording grey");
		CHECK(r.is(Light::Network, St::Unknown, Rs::NetworkWindowFilling), "healthy: network unknown while the window fills");
		CHECK(r.L(Light::Network).value == -1.0, "window filling: value is -1");
		CHECK(r.is(Light::Pc, St::Unknown, Rs::PcWindowFilling), "healthy: pc unknown while the window fills");
		CHECK(r.is(Light::Stream, St::Green, Rs::Ok), "healthy: stream green");
		CHECK(r.s.worst == St::Unknown, "healthy: worst is unknown while the windows fill");
		r.ticks(30);
		CHECK(r.is(Light::Network, St::Green, Rs::Ok) && r.is(Light::Pc, St::Green, Rs::Ok), "healthy: both windows green once full");
		CHECK(r.s.worst == St::Green, "healthy: worst is green once the windows are full");
	}
	{
		Rig r;
		r.in.live = LiveState::Unknown;
		r.tick();
		CHECK(lightsAll(r, St::Unknown, Rs::LiveUnknown), "live unknown: every light unknown LiveUnknown");
		CHECK(r.s.worst == St::Unknown, "live unknown: worst is unknown");
	}
	{
		Rig r;
		r.th.lightOn[static_cast<int>(Light::Mic)] = false;
		r.apply();
		r.live();
		CHECK(r.is(Light::Mic, St::Grey, Rs::Disabled), "disabled light is grey Disabled while live");
		r.in.mic.exists = false;
		r.tick();
		CHECK(r.is(Light::Mic, St::Grey, Rs::Disabled) && r.L(Light::Mic).incidentId == 0, "disabled light ignores a missing source");
		r.in.live = LiveState::Off;
		r.tick();
		CHECK(r.is(Light::Mic, St::Grey, Rs::Disabled), "disabled light stays Disabled off air");
	}
	{
		Rig r;
		r.in.mic.configured = false;
		r.in.desktop.configured = false;
		r.live();
		CHECK(r.is(Light::Mic, St::Grey, Rs::NotSetUp), "unconfigured mic is grey NotSetUp");
		CHECK(r.is(Light::Desktop, St::Grey, Rs::NotSetUp), "unconfigured desktop is grey NotSetUp");
		CHECK(r.L(Light::Mic).incidentId == 0, "unconfigured mic opens no incident");
	}
	{
		Rig r;
		r.th.holdingScenes.clear();
		r.apply();
		r.live();
		CHECK(r.is(Light::Scene, St::Grey, Rs::NotSetUp), "no holding scenes: scene light is NotSetUp");
	}
	{
		Settings st;
		st.setupDone = true;
		st.lightMic = false;
		st.lightStream = false;
		st.recordIntent = true;
		st.heardYouEnabled = true;
		st.dropRedPct = 3.0;
		st.micMutedAmberS = 7;
		HoldingScene h;
		h.sceneUuid = "x";
		h.limitSeconds = 42;
		st.holdingScenes.push_back(h);
		const Thresholds t = thresholdsFromSettings(st);
		CHECK(t.setupDone && !t.lightOn[0] && t.lightOn[1] && !t.lightOn[6], "thresholds from settings: light switches");
		CHECK(t.recordIntent && t.heardYouEnabled && t.dropRedPct == 3.0 && t.micMutedAmberS == 7,
		      "thresholds from settings: values");
		CHECK(t.holdingScenes.size() == 1 && t.holdingScenes[0].limitSeconds == 42, "thresholds from settings: holding scenes");
	}
	{
		Rig r;
		r.live();
		const qint64 liveAt = r.now;
		r.ticks(5);
		CHECK(r.L(Light::Mic).sinceMs == liveAt, "a green light reports when it turned green");
	}
}

static void testMicStructural()
{
	{
		Rig r;
		r.live();
		r.in.mic.exists = false;
		r.tick();
		CHECK(r.is(Light::Mic, St::Red, Rs::MicMissing), "mic missing: red at once");
		CHECK(r.L(Light::Mic).flashNow, "mic missing: red flashes on the update it appears");
		CHECK(r.L(Light::Mic).incidentId != 0, "mic missing: an incident is open");
		CHECK(r.count(IncidentEventKind::Started, Light::Mic) == 1, "mic missing: one Started event");
		const quint64 id = r.L(Light::Mic).incidentId;
		r.tick();
		CHECK(!r.L(Light::Mic).flashNow && r.L(Light::Mic).incidentId == id, "mic missing: no second flash, same incident");
		CHECK(r.s.worst == St::Red, "mic missing: worst is red");
		CHECK(r.L(Light::Mic).sinceMs == r.now - 1000, "incident since is the time it started");
	}
	{
		Rig r;
		r.live();
		r.in.mic.isAudio = false;
		r.tick();
		CHECK(r.is(Light::Mic, St::Red, Rs::MicNotAudio), "mic not an audio source: red");
	}
	{
		Rig r;
		r.live();
		r.in.mic.audioActive = false;
		r.tick();
		CHECK(r.is(Light::Mic, St::Red, Rs::MicInactive), "mic audio inactive: red");
		r.in.mic.audioActive = true;
		r.ticks(9);
		CHECK(r.is(Light::Mic, St::Red, Rs::MicInactive), "mic inactive fixed: 8 s fine, still red");
		r.tick();
		CHECK(r.is(Light::Mic, St::Red, Rs::MicInactive), "mic inactive fixed: 9 s fine, still red");
		r.tick();
		CHECK(r.is(Light::Mic, St::Green, Rs::Ok), "mic inactive fixed: green after 10 s fine");
	}
}

static void testMicMuted()
{
	Rig r;
	r.live();
	r.in.mic.muted = true;
	r.tick();
	r.ticks(18);
	CHECK(r.is(Light::Mic, St::Green, Rs::Ok), "mic muted 18 s: still green");
	r.tick();
	CHECK(r.is(Light::Mic, St::Green, Rs::Ok), "mic muted 19 s: still green (one tick before the limit)");
	r.tick();
	CHECK(r.is(Light::Mic, St::Amber, Rs::MicMuted), "mic muted 20 s: amber");
	CHECK(!r.L(Light::Mic).flashNow, "amber never flashes");
	CHECK(r.L(Light::Mic).value == 20.0, "muted amber value is the seconds muted");
	const quint64 id = r.L(Light::Mic).incidentId;
	CHECK(id != 0, "mic muted amber: incident open");
	r.ticks(39);
	CHECK(r.is(Light::Mic, St::Amber, Rs::MicMuted), "mic muted 59 s: still amber (one tick before red)");
	r.tick();
	CHECK(r.is(Light::Mic, St::Red, Rs::MicMuted), "mic muted 60 s: red");
	CHECK(r.L(Light::Mic).flashNow, "escalation to red flashes");
	CHECK(r.L(Light::Mic).incidentId == id, "escalation keeps the incident id");
	CHECK(r.count(IncidentEventKind::Escalated, Light::Mic) == 1, "one Escalated event");
	r.ticks(30);
	CHECK(r.flashes[static_cast<int>(Light::Mic)] == 1, "the light flashed exactly once in the whole incident");
	r.in.mic.muted = false;
	r.tick();
	r.ticks(8);
	r.tick();
	CHECK(r.is(Light::Mic, St::Red, Rs::MicMuted), "unmuted: 9 s fine, still red");
	r.tick();
	CHECK(r.is(Light::Mic, St::Green, Rs::Ok), "unmuted: 10 s fine, green");
	CHECK(r.L(Light::Mic).incidentId == 0, "unmuted: incident closed");
	CHECK(r.count(IncidentEventKind::Ended, Light::Mic) == 1, "one Ended event");
}

static void testMicFlicker()
{
	{
		Rig r;
		r.live();
		r.muteFor(20);
		const quint64 id = r.L(Light::Mic).incidentId;
		r.in.mic.muted = false;
		r.ticks(3);
		r.in.mic.muted = true;
		r.ticks(5);
		CHECK(r.is(Light::Mic, St::Amber, Rs::MicMuted) && r.L(Light::Mic).incidentId == id,
		      "flicker: mute again after 3 s keeps the same amber incident");
		r.in.mic.muted = false;
		r.tick();
		r.ticks(9);
		r.tick();
		CHECK(r.is(Light::Mic, St::Green, Rs::Ok), "flicker: 10 s fine finally closes it");
		CHECK(r.count(IncidentEventKind::Started, Light::Mic) == 1, "flicker never made two incidents");
	}
	{
		Rig r;
		r.live();
		r.muteFor(60);
		CHECK(r.L(Light::Mic).state == St::Red, "muted 60 s: red");
		const quint64 id = r.L(Light::Mic).incidentId;
		r.in.mic.muted = false;
		r.ticks(3);
		r.in.mic.muted = true;
		r.ticks(2);
		CHECK(r.L(Light::Mic).state == St::Red && r.L(Light::Mic).incidentId == id, "flicker while red: stays red, same incident");
		CHECK(!r.L(Light::Mic).flashNow, "flicker while red: no new flash");
	}
	{
		Rig r;
		r.th.micMutedAmberS = 5;
		r.th.micMutedRedS = 10;
		r.apply();
		r.live();
		r.muteFor(4);
		CHECK(r.L(Light::Mic).state == St::Green, "custom mute thresholds: 4 s green");
		r.tick();
		CHECK(r.L(Light::Mic).state == St::Amber, "custom mute thresholds: 5 s amber");
		r.ticks(5);
		CHECK(r.L(Light::Mic).state == St::Red, "custom mute thresholds: 10 s red");
	}
}

static void testMicCallbacks()
{
	{
		Rig r;
		r.live();
		r.autoCb = false;
		r.tick(4999);
		CHECK(r.is(Light::Mic, St::Green, Rs::Ok), "meter silent 4.999 s: still green");
		r.tick(1);
		CHECK(r.is(Light::Mic, St::Unknown, Rs::MicNoCallback), "meter silent 5 s: unknown");
		CHECK(r.s.worst == St::Unknown, "no callback: worst is unknown");
		r.autoCb = true;
		r.tick();
		CHECK(r.is(Light::Mic, St::Green, Rs::Ok) && r.L(Light::Mic).incidentId == 0, "callbacks back: green again, no incident");
	}
	{
		Rig r;
		r.autoCb = false;
		r.in.live = LiveState::Live;
		r.in.mainStreamActive = true;
		r.tick();
		r.tick(4999);
		CHECK(r.is(Light::Mic, St::Green, Rs::Ok), "meter never called back, 4.999 s: green");
		r.tick(1);
		CHECK(r.is(Light::Mic, St::Unknown, Rs::MicNoCallback), "meter never called back, 5 s: unknown");
	}
	{
		Rig r;
		r.live();
		r.autoCb = false;
		r.in.mic.attached = false;
		r.ticks(120);
		CHECK(r.is(Light::Mic, St::Green, Rs::Ok), "meter not attached: no judgement about callbacks");
	}
}

static void testHeardYou()
{
	{
		Rig r;
		r.th.heardYouEnabled = true;
		r.apply();
		r.in.mic.avgDb = -60.0f;
		r.live();
		r.ticks(200);
		CHECK(r.is(Light::Mic, St::Green, Rs::Ok), "never heard this session: not-heard stays unarmed");
	}
	{
		Rig r;
		r.th.heardYouEnabled = true;
		r.apply();
		r.live();
		r.in.mic.avgDb = -60.0f;
		r.tick();
		r.ticks(88);
		CHECK(r.is(Light::Mic, St::Green, Rs::Ok), "heard, then quiet 88 s: green");
		r.tick();
		CHECK(r.is(Light::Mic, St::Green, Rs::Ok), "heard, then quiet 89 s: green (one tick before)");
		r.tick();
		CHECK(r.is(Light::Mic, St::Amber, Rs::MicNotHeard), "quiet 90 s: amber not heard");
		CHECK(r.L(Light::Mic).value == 90.0, "not-heard value is the seconds of quiet");
		r.in.mic.avgDb = -20.0f;
		r.tick();
		r.ticks(8);
		r.tick();
		CHECK(r.L(Light::Mic).state == St::Amber, "voice back: 9 s fine, still amber");
		r.tick();
		CHECK(r.L(Light::Mic).state == St::Green, "voice back: 10 s fine, green");
	}
	{
		Rig r;
		r.th.heardYouEnabled = true;
		r.apply();
		r.live();
		r.in.mic.avgDb = -50.0f;
		r.tick();
		r.ticks(90);
		CHECK(r.is(Light::Mic, St::Amber, Rs::MicNotHeard), "level exactly at the threshold counts as quiet");
	}
	{
		Rig r;
		r.th.heardYouEnabled = true;
		r.apply();
		r.live();
		r.in.mic.avgDb = -49.9f;
		r.ticks(200);
		CHECK(r.is(Light::Mic, St::Green, Rs::Ok), "level just above the threshold counts as heard");
	}
	{
		Rig r;
		r.live();
		r.in.mic.avgDb = -80.0f;
		r.ticks(200);
		CHECK(r.is(Light::Mic, St::Green, Rs::Ok), "not-heard switched off: never lights");
	}
	{
		Rig r;
		r.th.heardYouEnabled = true;
		r.apply();
		r.live();
		r.in.programSceneUuid = "brb";
		r.in.mic.avgDb = -80.0f;
		r.ticks(200);
		CHECK(r.is(Light::Mic, St::Green, Rs::Ok), "not-heard is suppressed on a holding scene");
		r.in.programSceneUuid = "main";
		r.tick();
		r.ticks(88);
		CHECK(r.is(Light::Mic, St::Green, Rs::Ok), "after leaving the holding scene the count restarts: 88 s green");
		r.tick();
		CHECK(r.is(Light::Mic, St::Green, Rs::Ok), "after leaving the holding scene 89 s of quiet is still green");
		r.tick();
		CHECK(r.is(Light::Mic, St::Amber, Rs::MicNotHeard), "after leaving the holding scene 90 s of quiet is amber");
	}
	{
		Rig r;
		r.th.heardYouEnabled = true;
		r.apply();
		r.live();
		r.in.mic.avgDb = -80.0f;
		r.muteFor(20);
		CHECK(r.is(Light::Mic, St::Amber, Rs::MicMuted), "muted takes priority over not-heard");
		r.in.mic.muted = false;
		r.tick();
		r.ticks(88);
		CHECK(r.L(Light::Mic).state == St::Green, "unmuted: the quiet count started fresh, 89 s green");
		r.tick();
		r.tick();
		CHECK(r.is(Light::Mic, St::Amber, Rs::MicNotHeard), "unmuted and silent for 90 s: not heard");
	}
	{
		Rig r;
		r.th.heardYouEnabled = true;
		r.apply();
		r.live();
		r.off();
		r.in.mic.avgDb = -80.0f;
		r.live();
		r.ticks(200);
		CHECK(r.is(Light::Mic, St::Green, Rs::Ok), "a new session must hear the mic again before arming");
	}
}

static void testDesktop()
{
	{
		Rig r;
		r.live();
		r.in.desktop.exists = false;
		r.tick();
		CHECK(r.is(Light::Desktop, St::Red, Rs::DesktopMissing), "desktop missing: red");
		r.in.desktop.exists = true;
		r.in.desktop.isAudio = false;
		r.tick();
		CHECK(r.is(Light::Desktop, St::Red, Rs::DesktopNotAudio), "desktop not audio: red");
	}
	{
		Rig r;
		r.live();
		r.in.desktop.muted = true;
		r.tick();
		CHECK(r.is(Light::Desktop, St::Red, Rs::DesktopMuted), "desktop muted: red with no wait");
		CHECK(r.L(Light::Desktop).flashNow, "desktop muted: flashes");
		CHECK(r.is(Light::Mic, St::Green, Rs::Ok), "desktop trouble does not touch the mic light");
	}
	{
		Rig r;
		r.live();
		r.in.desktop.peakDb = -kInf;
		r.tick();
		r.ticks(118);
		CHECK(r.is(Light::Desktop, St::Green, Rs::Ok), "desktop -inf for 118 s: green");
		r.tick();
		CHECK(r.is(Light::Desktop, St::Green, Rs::Ok), "desktop -inf for 119 s: green (one tick before)");
		r.tick();
		CHECK(r.is(Light::Desktop, St::Amber, Rs::DesktopDigitalSilence), "desktop -inf for 120 s: amber");
		r.in.desktop.peakDb = -70.0f;
		r.tick();
		r.ticks(9);
		CHECK(r.L(Light::Desktop).state == St::Amber, "desktop sound back: 9 s fine, still amber");
		r.tick();
		CHECK(r.L(Light::Desktop).state == St::Green, "desktop sound back: 10 s fine, green");
	}
	{
		Rig r;
		r.live();
		r.in.desktop.peakDb = -60.0f;
		r.ticks(600);
		CHECK(r.is(Light::Desktop, St::Green, Rs::Ok), "a quiet finite level (-60 dB) never lights it");
		r.in.desktop.peakDb = -200.0f;
		r.ticks(600);
		CHECK(r.is(Light::Desktop, St::Green, Rs::Ok), "even -200 dB is not digital silence");
	}
	{
		Rig r;
		r.live();
		r.in.desktop.peakDb = -kInf;
		r.ticks(100);
		r.in.desktop.peakDb = -80.0f;
		r.tick();
		r.in.desktop.peakDb = -kInf;
		r.ticks(100);
		CHECK(r.is(Light::Desktop, St::Green, Rs::Ok), "one finite tick resets the digital silence count");
	}
	{
		Rig r;
		r.live();
		r.in.desktop.peakDb = -kInf;
		r.autoCb = false;
		r.tick(4999);
		CHECK(r.L(Light::Desktop).state == St::Green, "desktop callbacks 4.999 s old: green");
		r.tick(1);
		CHECK(r.is(Light::Desktop, St::Unknown, Rs::DesktopNoCallback), "desktop callbacks 5 s old: unknown");
		r.ticks(300);
		CHECK(r.is(Light::Desktop, St::Unknown, Rs::DesktopNoCallback), "no callbacks: silence is not judged, stays unknown");
	}
	{
		Rig r;
		r.live();
		r.muteFor(60);
		CHECK(r.is(Light::Desktop, St::Green, Rs::Ok), "mic trouble does not touch the desktop light");
	}
}

static void testScene()
{
	{
		Rig r;
		r.live();
		r.in.programSceneUuid = "start";
		r.tick();
		r.tick(299999);
		CHECK(r.is(Light::Scene, St::Green, Rs::Ok), "holding scene 299.999 s: green");
		r.tick(1);
		CHECK(r.is(Light::Scene, St::Green, Rs::Ok), "holding scene exactly at its limit: still green");
		r.tick(1);
		CHECK(r.is(Light::Scene, St::Amber, Rs::SceneOverLimit), "holding scene one tick over its limit: amber");
		CHECK(r.L(Light::Scene).value > 0.0, "scene value is the seconds over the limit");
		CHECK(!r.L(Light::Scene).flashNow, "scene amber does not flash");
		const quint64 id = r.L(Light::Scene).incidentId;
		r.tick(299998);
		r.tick(1);
		CHECK(r.is(Light::Scene, St::Amber, Rs::SceneOverLimit), "exactly limit + extra: still amber");
		r.tick(1);
		CHECK(r.is(Light::Scene, St::Red, Rs::SceneOverLimit), "one tick past limit + extra: red");
		CHECK(r.L(Light::Scene).flashNow && r.L(Light::Scene).incidentId == id, "scene red flashes, same incident");
		r.in.programSceneUuid = "main";
		r.tick();
		r.ticks(8);
		r.tick();
		CHECK(r.L(Light::Scene).state == St::Red, "left the holding scene: 9 s fine, still red");
		r.tick();
		CHECK(r.L(Light::Scene).state == St::Green, "left the holding scene: 10 s fine, green");
	}
	{
		Rig r;
		r.live();
		r.in.programSceneUuid = "start";
		r.tick();
		r.tick(299000);
		r.in.programSceneUuid = "brb";
		r.tick();
		r.tick(400000);
		CHECK(r.is(Light::Scene, St::Green, Rs::Ok), "switching to another holding scene restarts the clock");
		r.in.programSceneUuid = "start";
		r.tick();
		r.tick(300000);
		CHECK(r.is(Light::Scene, St::Green, Rs::Ok), "going back to a holding scene restarts the clock again");
	}
	{
		Rig r;
		r.live();
		r.in.programSceneUuid = "other";
		r.tick();
		r.tick(5000000);
		CHECK(r.is(Light::Scene, St::Green, Rs::Ok), "a scene that is not a holding scene never runs the clock");
	}
	{
		Rig r;
		r.in.programSceneUuid = "start";
		r.tick();
		r.tick(1000000);
		r.live();
		r.tick(300000);
		CHECK(r.is(Light::Scene, St::Green, Rs::Ok), "the clock starts when going live, not while off air");
		r.tick(1);
		CHECK(r.is(Light::Scene, St::Amber, Rs::SceneOverLimit), "after going live the limit applies");
	}
	{
		Rig r;
		r.th.holdingScenes[0].limitSeconds = 10;
		r.th.sceneRedExtraS = 5;
		r.apply();
		r.live();
		r.in.programSceneUuid = "start";
		r.tick();
		r.ticks(10);
		CHECK(r.L(Light::Scene).state == St::Green, "custom scene limit: 10 s green");
		r.tick();
		CHECK(r.L(Light::Scene).state == St::Amber, "custom scene limit: 11 s amber");
		r.ticks(4);
		CHECK(r.L(Light::Scene).state == St::Amber, "custom scene red extra: 15 s amber");
		r.tick();
		CHECK(r.L(Light::Scene).state == St::Red, "custom scene red extra: 16 s red");
	}
}

static void testRecording()
{
	{
		Rig r;
		r.th.recordIntent = true;
		r.apply();
		r.live();
		r.tick(59999);
		CHECK(r.is(Light::Recording, St::Green, Rs::Ok), "recording not started, 59.999 s after going live: green");
		r.tick(1);
		CHECK(r.is(Light::Recording, St::Red, Rs::RecordingNotStarted), "recording not started within the grace: red");
		CHECK(r.L(Light::Recording).flashNow && r.L(Light::Recording).value == 60.0, "not started: flashes, value is the wait");
		const quint64 id = r.L(Light::Recording).incidentId;
		r.in.recording.active = true;
		r.in.recording.paused = true;
		r.tick();
		CHECK(r.is(Light::Recording, St::Amber, Rs::RecordingPaused) && r.L(Light::Recording).incidentId != id,
		      "not started then paused are two causes: a new incident");
		CHECK(r.count(IncidentEventKind::Ended, Light::Recording) == 1, "the first cause ended");
		r.in.recording.paused = false;
		r.tick();
		r.ticks(8);
		r.tick();
		CHECK(r.L(Light::Recording).state == St::Amber, "recording fine 9 s: not yet cleared");
		r.tick();
		CHECK(r.is(Light::Recording, St::Green, Rs::Ok), "recording fine 10 s: green");
	}
	{
		Rig r;
		r.th.recordIntent = true;
		r.th.recordStartGraceS = 10;
		r.apply();
		r.live();
		r.ticks(9);
		CHECK(r.L(Light::Recording).state == St::Green, "custom grace: 9 s green");
		r.tick();
		CHECK(r.L(Light::Recording).state == St::Red, "custom grace: 10 s red");
	}
	{
		Rig r;
		r.th.recordIntent = true;
		r.apply();
		r.live();
		r.ticks(30);
		r.in.recording.active = true;
		r.ticks(100);
		CHECK(r.is(Light::Recording, St::Green, Rs::Ok), "recording started inside the grace: green");
		r.in.recording.active = false;
		r.tick();
		CHECK(r.is(Light::Recording, St::Red, Rs::RecordingStopped), "recording stopped after it started: red");
		CHECK(r.L(Light::Recording).flashNow, "recording stopped: flashes");
		r.ticks(100);
		CHECK(r.L(Light::Recording).state == St::Red, "recording stays stopped: stays red");
		r.in.recording.active = true;
		r.tick();
		r.ticks(8);
		r.tick();
		CHECK(r.L(Light::Recording).state == St::Red, "recording back 9 s: not yet cleared");
		r.tick();
		CHECK(r.L(Light::Recording).state == St::Green, "recording back 10 s: green");
	}
	{
		Rig r;
		r.th.recordIntent = true;
		r.apply();
		r.live();
		r.in.recording.active = true;
		r.in.recording.paused = true;
		r.tick();
		CHECK(r.is(Light::Recording, St::Amber, Rs::RecordingPaused), "recording paused: amber with no wait");
		CHECK(!r.L(Light::Recording).flashNow, "recording paused: no flash");
		const quint64 id = r.L(Light::Recording).incidentId;
		r.in.recording.active = false;
		r.in.recording.paused = false;
		r.tick();
		CHECK(r.is(Light::Recording, St::Red, Rs::RecordingStopped), "paused then stopped: red RecordingStopped");
		CHECK(r.L(Light::Recording).incidentId != id && r.L(Light::Recording).incidentId != 0,
		      "a stop after start is a new incident");
		CHECK(r.L(Light::Recording).flashNow, "the new red incident flashes");
		CHECK(r.count(IncidentEventKind::Started, Light::Recording) == 2, "two incidents were started");
	}
	{
		Rig r;
		r.th.recordIntent = true;
		r.apply();
		r.live();
		r.in.recording.active = true;
		r.tick();
		r.off();
		CHECK(r.is(Light::Recording, St::Grey, Rs::OffAir), "recording light is grey off air");
		r.in.recording.active = false;
		r.live();
		r.ticks(59);
		CHECK(r.L(Light::Recording).state == St::Green, "a new session forgets the earlier recording");
		r.tick();
		CHECK(r.is(Light::Recording, St::Red, Rs::RecordingNotStarted), "a new session gets its own start grace");
	}
}

static void testFrameWindow()
{
	FrameWindow w;
	CHECK(!w.stats(0).filled, "frame window: empty is not filled");
	w.add(0, 0, 0);
	CHECK(!w.stats(0).filled, "frame window: one sample is not filled");
	for (int i = 1; i <= 29; ++i)
		w.add(i * 1000, 0, quint64(i) * 100);
	CHECK(!w.stats(29000).filled, "frame window: 29 s of history is not full");
	w.add(30000, 15, 3000);
	WindowStats s = w.stats(30000);
	CHECK(s.filled && s.pct == 0.5, "frame window: 30 s of history is full and exact (15 of 3000 = 0.5 %)");
	CHECK(!w.stats(36000).filled, "frame window: a stale newest sample is not judged");
	w.add(30000, 14, 3000);
	CHECK(!w.stats(30000).filled, "frame window: a counter going down starts afresh");
	FrameWindow g;
	g.add(0, 0, 0);
	g.add(1000, 0, 100);
	g.add(8000, 0, 1000);
	for (int i = 9; i <= 30; ++i)
		g.add(i * 1000, 0, quint64(i) * 100 + 1000);
	CHECK(!g.stats(30000).filled, "frame window: a gap over 5 s inside the window is not judged");
	FrameWindow m;
	m.add(0, 0, 0);
	for (int i = 1; i <= 29; ++i)
		m.add(i * 1000, 0, 0);
	m.add(30000, 0, 299);
	CHECK(!m.stats(30000).filled, "frame window: 299 frames is below the minimum");
	FrameWindow n;
	n.add(0, 0, 0);
	for (int i = 1; i <= 29; ++i)
		n.add(i * 1000, 0, 0);
	n.add(30000, 0, 300);
	CHECK(n.stats(30000).filled, "frame window: 300 frames meets the minimum");
}

static void testNetwork()
{
	{
		Rig r = netCase(0, 1000, 29);
		CHECK(r.is(Light::Network, St::Unknown, Rs::NetworkWindowFilling) && r.L(Light::Network).value == -1.0,
		      "network at 29 s: window not full, unknown filling, value -1");
	}
	{
		Rig r = netCase(0, 1000);
		CHECK(r.is(Light::Network, St::Green, Rs::Ok) && r.L(Light::Network).value == 0.0, "network at 30 s: judged, no drops, green");
	}
	{
		Rig r = netCase(21, 1000, 29);
		CHECK(r.L(Light::Network).state == St::Unknown, "network: bad counters but only 29 s of window are not judged");
	}
	{
		Rig r = netCase(5, 1000);
		CHECK(r.is(Light::Network, St::Green, Rs::Ok) && r.L(Light::Network).value == 0.5, "network exactly 0.5 %: green");
	}
	{
		Rig r = netCase(6, 1000);
		CHECK(r.is(Light::Network, St::Amber, Rs::NetworkDrops), "network 0.6 %: amber");
		CHECK(!r.L(Light::Network).flashNow, "network amber does not flash");
	}
	{
		Rig r = netCase(20, 1000);
		CHECK(r.is(Light::Network, St::Amber, Rs::NetworkDrops) && r.L(Light::Network).value == 2.0, "network exactly 2.0 %: amber");
	}
	{
		Rig r = netCase(21, 1000);
		CHECK(r.is(Light::Network, St::Red, Rs::NetworkDrops), "network 2.1 %: red");
		CHECK(r.L(Light::Network).flashNow, "network red flashes");
		CHECK(std::fabs(r.L(Light::Network).value - 2.1) < 1e-9, "network red value is the drop percentage");
	}
	{
		Rig r = netCase(150, 299);
		CHECK(r.is(Light::Network, St::Unknown, Rs::NetworkWindowFilling), "network: 299 frames is under the minimum, not judged");
	}
	{
		Rig r = netCase(150, 300);
		CHECK(r.is(Light::Network, St::Red, Rs::NetworkDrops), "network: 300 frames meets the minimum and is judged");
	}
	{
		Rig r = netCase(21, 1000);
		const quint64 id = r.L(Light::Network).incidentId;
		r.post(StreamEventKind::Reconnect);
		r.tick();
		CHECK(r.is(Light::Network, St::Amber, Rs::NetworkReconnecting), "reconnect event: amber reconnecting");
		CHECK(r.L(Light::Network).incidentId != id && r.L(Light::Network).incidentId != 0,
		      "a different cause (reconnecting instead of drops) is a new incident");
		const quint64 reconnectId = r.L(Light::Network).incidentId;
		r.tick();
		CHECK(r.is(Light::Network, St::Amber, Rs::NetworkReconnecting), "still reconnecting without a success event");
		r.post(StreamEventKind::ReconnectSuccess);
		r.in.network.totalFrames += 200;
		r.tick();
		CHECK(r.is(Light::Network, St::Unknown, Rs::NetworkWindowFilling), "after ReconnectSuccess the window restarts: unknown, no old verdict");
		CHECK(r.L(Light::Network).incidentId == reconnectId, "unknown keeps the open incident");
	}
	{
		Rig r;
		r.live();
		r.in.network.reconnecting = true;
		r.netDrop = 100;
		r.ticks(40);
		CHECK(r.is(Light::Network, St::Amber, Rs::NetworkReconnecting), "reconnecting flag: amber and drops are not judged");
		r.in.network.reconnecting = false;
		r.tick();
		r.ticks(28);
		r.tick();
		CHECK(r.is(Light::Network, St::Unknown, Rs::NetworkWindowFilling), "after reconnecting the window is still filling");
		r.tick();
		CHECK(r.is(Light::Network, St::Red, Rs::NetworkDrops), "after reconnecting a full window judges the drops again");
	}
	{
		Rig r;
		r.autoCounters = false;
		r.live();
		auto step = [&r](int n) {
			for (int i = 0; i < n; ++i) {
				r.in.network.totalFrames += 100;
				r.tick();
			}
		};
		step(20);
		r.in.network.totalFrames = 0;
		r.in.network.droppedFrames = 0;
		step(30);
		CHECK(r.is(Light::Network, St::Unknown, Rs::NetworkWindowFilling), "counter rollback restarts the window: 29 s after is not full");
		step(1);
		CHECK(r.is(Light::Network, St::Green, Rs::Ok), "counter rollback: full again 30 s after");
	}
	{
		Rig r;
		r.live();
		r.ticks(20);
		r.post(StreamEventKind::OutputReplaced);
		r.tick();
		r.ticks(28);
		r.tick();
		CHECK(r.is(Light::Network, St::Unknown, Rs::NetworkWindowFilling), "output replaced restarts the window: 29 s after is not full");
		r.tick();
		CHECK(r.is(Light::Network, St::Green, Rs::Ok), "output replaced: full again 30 s after");
	}
	{
		Rig r;
		r.live();
		r.ticks(20);
		r.ev.resetBaselines();
		r.tick();
		r.ticks(28);
		r.tick();
		CHECK(r.is(Light::Network, St::Unknown, Rs::NetworkWindowFilling), "resetBaselines restarts the window: 29 s after is not full");
		r.tick();
		CHECK(r.is(Light::Network, St::Green, Rs::Ok), "resetBaselines: full again 30 s after");
	}
	{
		Rig r;
		r.live();
		r.post(StreamEventKind::Reconnect);
		r.tick();
		r.post(StreamEventKind::ReconnectSuccess);
		r.tick();
		CHECK(r.is(Light::Network, St::Unknown, Rs::NetworkWindowFilling), "after ReconnectSuccess the light is unknown until the window refills");
		r.ticks(29);
		CHECK(r.L(Light::Network).state == St::Unknown, "29 s after recovery: still filling");
		r.tick();
		CHECK(r.L(Light::Network).state == St::Amber, "window full again: the amber is held by the 10 s rule");
		r.ticks(9);
		CHECK(r.L(Light::Network).state == St::Amber, "9 s fine after the window refilled: still amber");
		r.tick();
		CHECK(r.L(Light::Network).state == St::Green, "10 s fine after the window refilled: green");
	}
	{
		Rig r;
		r.live();
		r.in.mainStreamActive = false;
		r.tick();
		CHECK(r.is(Light::Network, St::Grey, Rs::MainStreamOff), "main stream off while live: network grey");
		CHECK(r.is(Light::Stream, St::Grey, Rs::MainStreamOff), "main stream off while live: stream grey");
		CHECK(r.is(Light::Mic, St::Green, Rs::Ok), "main stream off while live: the other lights keep watching");
		CHECK(r.is(Light::Pc, St::Unknown, Rs::PcWindowFilling), "main stream off while live: pc keeps watching");
	}
}

static void testPc()
{
	{
		Rig r = pcCase(21, 1000, 0, 1000);
		CHECK(r.is(Light::Pc, St::Red, Rs::PcRenderLag) && std::fabs(r.L(Light::Pc).value - 2.1) < 1e-9, "pc render 2.1 %: red render lag");
		CHECK(r.L(Light::Pc).flashNow, "pc red flashes");
	}
	{
		Rig r = pcCase(0, 1000, 6, 1000);
		CHECK(r.is(Light::Pc, St::Amber, Rs::PcEncoderLag), "pc encoder 0.6 %: amber encoder lag");
	}
	{
		Rig r = pcCase(5, 1000, 0, 1000);
		CHECK(r.is(Light::Pc, St::Green, Rs::Ok), "pc exactly 0.5 %: green");
	}
	{
		Rig r = pcCase(20, 1000, 0, 1000);
		CHECK(r.is(Light::Pc, St::Amber, Rs::PcRenderLag), "pc exactly 2.0 %: amber");
	}
	{
		Rig r = pcCase(6, 1000, 25, 1000);
		CHECK(r.is(Light::Pc, St::Red, Rs::PcEncoderLag), "pc worst of the two: red encoder beats amber render");
	}
	{
		Rig r = pcCase(6, 1000, 8, 1000);
		CHECK(r.is(Light::Pc, St::Amber, Rs::PcEncoderLag), "pc both amber: the larger loss is named");
	}
	{
		Rig r = pcCase(8, 1000, 6, 1000);
		CHECK(r.is(Light::Pc, St::Amber, Rs::PcRenderLag), "pc both amber, render larger: render named");
	}
	{
		Rig r = pcCase(21, 299, 0, 299);
		CHECK(r.is(Light::Pc, St::Unknown, Rs::PcWindowFilling) && r.L(Light::Pc).value == -1.0, "pc under the minimum frames: unknown filling");
	}
	{
		Rig r = pcCase(0, 299, 21, 1000);
		CHECK(r.is(Light::Pc, St::Red, Rs::PcEncoderLag), "pc: one full window is enough to judge");
	}
}

// Reset ("start fresh"): acknowledgements and snoozes are forgotten, a latched stream drop is cleared, and no
// event is left over for the (cleared) session log.
static void testForgetIncidentMarks()
{
	{
		Rig r;
		r.live();
		r.muteFor(20);
		r.ev.acknowledge(Light::Mic, r.now);
		r.ev.snooze(Light::Mic, r.now);
		r.tick();
		CHECK(r.L(Light::Mic).acknowledged && r.L(Light::Mic).snoozedUntilMs > 0, "setup: the mic incident is acknowledged and snoozed");
		const quint64 id = r.L(Light::Mic).incidentId;
		r.ev.forgetIncidentMarks(r.now);
		r.tick();
		CHECK(!r.L(Light::Mic).acknowledged, "forgetIncidentMarks: the acknowledgement is gone");
		CHECK(r.L(Light::Mic).snoozedUntilMs == 0, "forgetIncidentMarks: the snooze is gone");
		CHECK(r.L(Light::Mic).incidentId == id && r.L(Light::Mic).state != St::Green, "forgetIncidentMarks: the incident itself stays open");
		CHECK(r.count(IncidentEventKind::Acknowledged, Light::Mic) == 1, "forgetIncidentMarks: no new Acknowledged event");
	}
	{
		Rig r;
		r.live();
		r.ticks(3);
		r.post(StreamEventKind::StoppedError, -3);
		r.in.live = LiveState::Off;
		r.in.mainStreamActive = false;
		r.tick();
		CHECK(r.is(Light::Stream, St::Red, Rs::StreamDropped), "setup: a latched stream drop is red off air");
		const int endedBefore = r.count(IncidentEventKind::Ended, Light::Stream);
		r.ev.forgetIncidentMarks(r.now);
		r.tick();
		CHECK(r.is(Light::Stream, St::Grey, Rs::OffAir), "forgetIncidentMarks: the latched drop is cleared to grey");
		CHECK(r.count(IncidentEventKind::Ended, Light::Stream) == endedBefore, "forgetIncidentMarks: no Ended event reaches the cleared log");
		CHECK(r.s.worst == St::Grey, "forgetIncidentMarks: the header is grey again");
	}
	{
		Rig r;
		r.live();
		r.tick();
		r.ev.forgetIncidentMarks(r.now);
		r.tick();
		CHECK(r.is(Light::Mic, St::Green, Rs::Ok), "forgetIncidentMarks with nothing open changes nothing");
	}
}

static void testStream()
{
	{
		Rig r;
		r.live();
		r.ticks(3);
		r.post(StreamEventKind::StoppedError, -3);
		r.in.live = LiveState::Off;
		r.in.mainStreamActive = false;
		r.tick();
		CHECK(r.is(Light::Stream, St::Red, Rs::StreamDropped), "stream dropped with an error code: red StreamDropped");
		CHECK(r.L(Light::Stream).flashNow, "stream dropped: flashes once");
		CHECK(r.L(Light::Stream).value == -3.0, "stream dropped: value carries the code");
		CHECK(r.L(Light::Stream).sinceMs == r.now, "stream dropped: since is the event time");
		CHECK(r.is(Light::Mic, St::Grey, Rs::OffAir) && r.is(Light::Network, St::Grey, Rs::OffAir), "other lights go grey off air");
		CHECK(r.s.worst == St::Red, "a latched stream drop is the worst state off air");
		r.ticks(100);
		CHECK(r.is(Light::Stream, St::Red, Rs::StreamDropped), "stream drop stays latched off air");
		CHECK(r.flashes[static_cast<int>(Light::Stream)] == 1, "stream drop flashed exactly once");
		r.in.live = LiveState::Unknown;
		r.tick();
		CHECK(r.is(Light::Stream, St::Red, Rs::StreamDropped), "latched stream drop stays red while live state is unknown");
		CHECK(r.is(Light::Mic, St::Unknown, Rs::LiveUnknown), "the other lights are unknown meanwhile");
		r.in.live = LiveState::Off;
		r.tick();
		r.ev.acknowledge(Light::Stream, r.now);
		r.tick();
		CHECK(r.is(Light::Stream, St::Grey, Rs::OffAir), "acknowledge clears a latched stream drop to grey");
		CHECK(r.count(IncidentEventKind::Acknowledged, Light::Stream) == 1, "acknowledge emitted one event");
		CHECK(r.count(IncidentEventKind::Ended, Light::Stream) == 1, "the cleared drop emitted Ended");
		CHECK(r.s.worst == St::Grey, "after acknowledge the header is grey again");
	}
	{
		Rig r;
		r.live();
		r.post(StreamEventKind::StoppedError, 7);
		r.in.live = LiveState::Off;
		r.in.mainStreamActive = false;
		r.tick();
		const quint64 id = r.L(Light::Stream).incidentId;
		r.post(StreamEventKind::StoppedError, 8);
		r.tick();
		CHECK(r.L(Light::Stream).incidentId == id && r.count(IncidentEventKind::Started, Light::Stream) == 1,
		      "a second error while latched is not a second incident");
		r.live();
		CHECK(r.is(Light::Stream, St::Green, Rs::Ok) && r.L(Light::Stream).incidentId == 0, "a new session going live clears the latch");
		CHECK(r.count(IncidentEventKind::Ended, Light::Stream) == 1, "new session: Ended emitted for the drop");
	}
	{
		Rig r;
		r.live();
		r.post(StreamEventKind::StoppedOk);
		r.in.live = LiveState::Off;
		r.in.mainStreamActive = false;
		r.tick();
		CHECK(r.is(Light::Stream, St::Grey, Rs::OffAir) && r.L(Light::Stream).incidentId == 0, "a normal stop is grey, no incident");
		r.post(StreamEventKind::StoppedError, 1);
		r.tick();
		CHECK(r.is(Light::Stream, St::Grey, Rs::OffAir), "an error after a normal stop was not 'while live': no latch");
	}
	{
		Rig r;
		r.post(StreamEventKind::StoppedError, 1);
		r.tick();
		CHECK(r.is(Light::Stream, St::Grey, Rs::OffAir), "an error when it was never live: no latch");
	}
	{
		Rig r;
		r.live();
		r.ticks(5);
		r.off();
		CHECK(r.is(Light::Stream, St::Grey, Rs::OffAir), "going off air without a stop event never infers an error");
	}
	{
		Rig r;
		r.live();
		r.post(StreamEventKind::StoppedError, 5);
		r.in.mainStreamActive = false; // a Multistream destination keeps the session live
		r.tick();
		CHECK(r.is(Light::Stream, St::Red, Rs::StreamDropped), "main stream dropped while another destination is live: red");
		CHECK(r.is(Light::Network, St::Grey, Rs::MainStreamOff), "the main-stream-only lights go grey");
		r.in.mainStreamActive = true;
		r.post(StreamEventKind::Started);
		r.tick();
		CHECK(r.is(Light::Stream, St::Green, Rs::Ok), "the main stream restarted: the latch clears");
	}
	{
		Rig r;
		r.in.live = LiveState::Live;
		r.in.mainStreamActive = false;
		r.post(StreamEventKind::StoppedError, 9);
		r.tick();
		CHECK(r.is(Light::Stream, St::Grey, Rs::MainStreamOff), "an error from a stream that never went live is not 'dropped'");
	}
	{
		Rig r;
		r.th.lightOn[static_cast<int>(Light::Stream)] = false;
		r.apply();
		r.live();
		r.post(StreamEventKind::StoppedError, 2);
		r.in.live = LiveState::Off;
		r.in.mainStreamActive = false;
		r.tick();
		CHECK(r.is(Light::Stream, St::Grey, Rs::Disabled), "disabled stream light does not latch");
	}
}

static void testAcknowledgeSnooze()
{
	{
		Rig r;
		r.live();
		r.in.mic.exists = false;
		r.tick();
		r.ev.acknowledge(Light::Mic, r.now);
		r.tick();
		CHECK(r.L(Light::Mic).acknowledged, "acknowledge marks the incident acknowledged");
		CHECK(r.is(Light::Mic, St::Red, Rs::MicMissing), "acknowledge keeps the colour");
		CHECK(r.count(IncidentEventKind::Acknowledged, Light::Mic) == 1, "acknowledge emits one event");
		r.ev.acknowledge(Light::Mic, r.now);
		r.tick();
		CHECK(r.count(IncidentEventKind::Acknowledged, Light::Mic) == 1, "acknowledging twice emits no second event");
		r.ticks(50);
		CHECK(r.flashes[static_cast<int>(Light::Mic)] == 1, "an acknowledged red does not flash again");
	}
	{
		Rig r;
		r.live();
		r.ev.acknowledge(Light::Mic, r.now);
		r.tick();
		CHECK(r.count(IncidentEventKind::Acknowledged, Light::Mic) == 0 && !r.L(Light::Mic).acknowledged,
		      "acknowledge with no incident does nothing");
	}
	{
		Rig r;
		r.live();
		r.muteFor(20);
		r.ev.acknowledge(Light::Mic, r.now);
		r.tick();
		CHECK(r.L(Light::Mic).acknowledged && r.L(Light::Mic).state == St::Amber, "acknowledged amber keeps its colour");
		r.ticks(38);
		CHECK(r.L(Light::Mic).state == St::Amber, "acknowledged amber: still amber at 59 s");
		r.tick();
		CHECK(r.L(Light::Mic).state == St::Red && r.L(Light::Mic).flashNow, "the first red of an incident flashes even if the amber was acknowledged");
		CHECK(r.L(Light::Mic).acknowledged, "acknowledged stays set for the life of the incident");
	}
	{
		Rig r;
		r.live();
		r.muteFor(20);
		r.ev.snooze(Light::Mic, r.now);
		const qint64 until = r.now + kSnoozeMs;
		r.tick();
		CHECK(r.L(Light::Mic).snoozedUntilMs == until, "snooze lasts 10 minutes");
		CHECK(r.L(Light::Mic).state == St::Amber, "snoozed light still shows its colour");
		r.ticks(38);
		r.tick();
		CHECK(r.L(Light::Mic).state == St::Red && !r.L(Light::Mic).flashNow, "a red while snoozed does not flash");
		CHECK(r.s.worst == St::Red, "a snoozed red still counts for the header");
		r.tick(until - 1 - r.now);
		CHECK(r.L(Light::Mic).snoozedUntilMs == until, "snooze still active one ms before it ends");
		r.tick(1);
		CHECK(r.L(Light::Mic).snoozedUntilMs == 0 && r.L(Light::Mic).state == St::Red, "snooze over at its end time, colour unchanged");
	}
	{
		Rig r;
		r.live();
		r.ev.snooze(Light::Mic, r.now);
		r.in.mic.exists = false;
		r.tick();
		CHECK(r.L(Light::Mic).flashNow && r.L(Light::Mic).snoozedUntilMs == 0, "snooze with no incident does nothing");
	}
	{
		Rig r;
		r.live();
		r.in.mic.exists = false;
		r.tick();
		const quint64 first = r.L(Light::Mic).incidentId;
		r.ev.snooze(Light::Mic, r.now);
		r.ev.acknowledge(Light::Mic, r.now);
		r.in.mic.exists = true;
		r.tick();
		r.ticks(9);
		r.tick();
		CHECK(r.L(Light::Mic).state == St::Green && r.L(Light::Mic).incidentId == 0, "incident cleared");
		r.in.mic.exists = false;
		r.tick();
		CHECK(r.L(Light::Mic).incidentId > first, "a later incident gets a new id");
		CHECK(r.L(Light::Mic).snoozedUntilMs == 0 && !r.L(Light::Mic).acknowledged && r.L(Light::Mic).flashNow,
		      "snooze and acknowledge end with their incident");
	}
}

static void testSessionsAndClock()
{
	{
		Rig r;
		r.live();
		r.in.mic.exists = false;
		r.tick();
		const qint64 startedAt = r.now;
		r.off();
		CHECK(r.is(Light::Mic, St::Grey, Rs::OffAir) && r.L(Light::Mic).incidentId == 0, "going off air closes every incident");
		bool found = false;
		for (const IncidentEvent &e : r.all)
			if (e.kind == IncidentEventKind::Started && e.light == Light::Mic && e.reason == Rs::MicMissing && e.atMs == startedAt)
				found = true;
		CHECK(found, "Started event carries the light, reason and time");
		CHECK(r.count(IncidentEventKind::Ended, Light::Mic) == 1, "going off air emitted Ended");
	}
	{
		Rig r;
		r.live();
		r.in.mic.exists = false;
		r.tick();
		const quint64 id = r.L(Light::Mic).incidentId;
		r.in.live = LiveState::Unknown;
		r.tick();
		CHECK(r.is(Light::Mic, St::Unknown, Rs::LiveUnknown) && r.L(Light::Mic).incidentId == id, "live unknown keeps the open incident");
		r.in.live = LiveState::Live;
		r.tick();
		CHECK(r.is(Light::Mic, St::Red, Rs::MicMissing) && r.L(Light::Mic).incidentId == id && !r.L(Light::Mic).flashNow,
		      "back to live: same incident, no new flash");
	}
	{
		Rig r;
		r.live();
		r.muteFor(15);
		r.off();
		r.live();
		r.ticks(19);
		CHECK(r.L(Light::Mic).state == St::Green, "going live starts the timers afresh: 19 s muted is green");
		r.tick();
		CHECK(r.L(Light::Mic).state == St::Amber, "going live starts the timers afresh: 20 s muted is amber");
	}
	{
		Rig r;
		r.live();
		r.muteFor(15);
		r.in.live = LiveState::Unknown;
		r.tick();
		r.in.live = LiveState::Live;
		r.tick();
		r.ticks(19);
		CHECK(r.L(Light::Mic).state == St::Green, "an unknown live state restarts running timers: 19 s green");
		r.tick();
		CHECK(r.L(Light::Mic).state == St::Amber, "an unknown live state restarts running timers: 20 s amber");
	}
	{
		Rig r;
		r.live();
		r.muteFor(20);
		r.autoCb = false;
		r.tick(5000);
		CHECK(r.is(Light::Mic, St::Amber, Rs::MicMuted), "amber outranks unknown on one light");
		CHECK(r.is(Light::Desktop, St::Unknown, Rs::DesktopNoCallback), "desktop unknown meanwhile");
		CHECK(r.s.worst == St::Amber, "worst: amber outranks unknown");
		r.in.desktop.exists = false;
		r.tick();
		CHECK(r.s.worst == St::Red, "worst: red outranks amber");
	}
	{
		Rig r;
		r.live();
		r.muteFor(15);
		r.tick(-100000);
		CHECK(r.L(Light::Mic).state == St::Green, "clock going backwards does not crash or light anything");
		r.ticks(19);
		CHECK(r.L(Light::Mic).state == St::Green, "clock went backwards: the mute timer restarted, 19 s green");
		r.tick();
		CHECK(r.L(Light::Mic).state == St::Amber, "clock went backwards: 20 s after the jump is amber");
	}
	{
		Rig r;
		r.live();
		r.muteFor(19);
		r.tick(0);
		CHECK(r.L(Light::Mic).state == St::Green, "the same timestamp twice is not a backwards jump");
		r.tick();
		CHECK(r.L(Light::Mic).state == St::Amber, "the same timestamp twice leaves the timers alone");
	}
	{
		Rig r;
		r.live();
		r.in.mic.exists = false;
		r.tick();
		const quint64 id = r.L(Light::Mic).incidentId;
		r.tick(-50000);
		CHECK(r.L(Light::Mic).incidentId == id && r.L(Light::Mic).sinceMs <= r.now, "clock back: incident kept, no negative duration");
	}
	{
		Rig r;
		r.live();
		r.in.programSceneUuid = "start";
		r.tick();
		r.tick(299000);
		r.tick(-1000000);
		r.tick(300000);
		CHECK(r.L(Light::Scene).state == St::Green, "clock back: the scene clock restarted, 300 s is not over the limit");
		r.tick(1);
		CHECK(r.L(Light::Scene).state == St::Amber, "clock back: the scene limit counts from the jump");
	}
}

static void testRecordingStopUndoneBetweenTicks()
{
	{
		Rig r;
		r.th.recordIntent = true;
		r.apply();
		r.live();
		r.ticks(5);
		r.in.recording.active = true;
		r.tick();
		CHECK(r.is(Light::Recording, St::Green, Rs::Ok), "stop undone: recording running, green");
		r.rec(RecordingEventKind::Stopped);
		r.rec(RecordingEventKind::Started);
		r.tick();
		CHECK(r.is(Light::Recording, St::Red, Rs::RecordingStopped), "stop undone between updates: still a RecordingStopped incident");
		CHECK(r.L(Light::Recording).flashNow && r.L(Light::Recording).incidentId != 0, "stop undone between updates: flashes, incident open");
		r.ticks(10);
		CHECK(r.L(Light::Recording).state == St::Red, "stop undone: the incident stays until 10 s fine");
		r.tick();
		CHECK(r.is(Light::Recording, St::Green, Rs::Ok), "stop undone: closes after 10 s fine");
	}
	{
		Rig r;
		r.th.recordIntent = true;
		r.apply();
		r.live();
		r.rec(RecordingEventKind::Started);
		r.rec(RecordingEventKind::Stopped);
		r.tick();
		CHECK(r.is(Light::Recording, St::Red, Rs::RecordingStopped), "started and stopped between two updates: stopped incident");
	}
	{
		Rig r;
		r.th.recordIntent = true;
		r.apply();
		r.live();
		r.rec(RecordingEventKind::Stopped);
		r.tick();
		CHECK(r.is(Light::Recording, St::Green, Rs::Ok) && r.L(Light::Recording).incidentId == 0,
		      "a stop event before any recording started opens nothing");
	}
}

static void testWindowResetsDiscardTheVerdict()
{
	auto redNet = []() { return netCase(21, 1000); };
	{
		Rig r = redNet();
		const quint64 id = r.L(Light::Network).incidentId;
		r.post(StreamEventKind::OutputReplaced);
		r.tick();
		CHECK(r.is(Light::Network, St::Unknown, Rs::NetworkWindowFilling) && r.L(Light::Network).value == -1.0,
		      "output replaced discards the verdict: unknown, value -1");
		CHECK(r.L(Light::Network).incidentId == id && !r.L(Light::Network).flashNow, "unknown keeps the open incident id");
	}
	{
		Rig r = redNet();
		r.in.network.totalFrames = 100;
		r.in.network.droppedFrames = 0;
		r.tick();
		CHECK(r.is(Light::Network, St::Unknown, Rs::NetworkWindowFilling), "counter rollback discards the verdict");
	}
	{
		Rig r = redNet();
		r.ev.resetBaselines();
		r.tick();
		CHECK(r.is(Light::Network, St::Unknown, Rs::NetworkWindowFilling), "resetBaselines discards the verdict");
	}
	{
		Rig r = redNet();
		r.post(StreamEventKind::Started);
		r.tick();
		CHECK(r.is(Light::Network, St::Unknown, Rs::NetworkWindowFilling), "a Started event discards the verdict");
	}
	{
		Rig r = redNet();
		r.post(StreamEventKind::ReconnectSuccess);
		r.tick();
		CHECK(r.is(Light::Network, St::Unknown, Rs::NetworkWindowFilling), "ReconnectSuccess discards the verdict");
	}
	{
		Rig r = redNet();
		r.off();
		r.live();
		CHECK(r.is(Light::Network, St::Unknown, Rs::NetworkWindowFilling) && r.L(Light::Network).incidentId == 0,
		      "a new session starts with no verdict and no incident");
	}
	{
		Rig r = pcCase(21, 1000, 0, 1000);
		CHECK(r.L(Light::Pc).state == St::Red, "pc verdict is red before the reset");
		r.ev.resetBaselines();
		r.tick();
		CHECK(r.is(Light::Pc, St::Unknown, Rs::PcWindowFilling), "pc: resetBaselines discards the verdict");
	}
}

static void testMicInactiveWhileMuted()
{
	Rig r;
	r.live();
	r.in.mic.muted = true;
	r.in.mic.audioActive = false;
	r.tick();
	CHECK(r.is(Light::Mic, St::Red, Rs::MicInactive) && r.L(Light::Mic).flashNow, "muted and inactive: inactive is red at once");
	r.ticks(25);
	CHECK(r.is(Light::Mic, St::Red, Rs::MicInactive), "muted and inactive: the mute timer does not take over");
}

static void testReasonChangeIsANewIncident()
{
	Rig r;
	r.live();
	r.muteFor(20);
	const quint64 first = r.L(Light::Mic).incidentId;
	r.in.mic.audioActive = false;
	r.tick();
	CHECK(r.is(Light::Mic, St::Red, Rs::MicInactive), "muted amber then source inactive: red inactive");
	CHECK(r.L(Light::Mic).incidentId != first && r.L(Light::Mic).flashNow, "a different cause is a new incident that flashes");
	CHECK(r.count(IncidentEventKind::Ended, Light::Mic) == 1 && r.count(IncidentEventKind::Started, Light::Mic) == 2,
	      "the old incident ended and a new one started");
}

static void testFlashOncePerIncident()
{
	Rig r = netCase(21, 1000);
	const quint64 id = r.L(Light::Network).incidentId;
	CHECK(r.L(Light::Network).flashNow, "first red flashes");
	r.ev.acknowledge(Light::Network, r.now);
	r.in.network.droppedFrames = 21;
	r.in.network.totalFrames = 2000;
	r.tick();
	CHECK(r.is(Light::Network, St::Amber, Rs::NetworkDrops) && r.L(Light::Network).incidentId == id,
	      "red to amber with the same reason is the same incident");
	r.in.network.droppedFrames = 60;
	r.in.network.totalFrames = 2100;
	r.tick();
	CHECK(r.is(Light::Network, St::Red, Rs::NetworkDrops) && r.L(Light::Network).incidentId == id, "amber to red again: same incident");
	CHECK(!r.L(Light::Network).flashNow && r.flashes[static_cast<int>(Light::Network)] == 1, "no second flash in the same incident");
	CHECK(r.L(Light::Network).acknowledged, "acknowledged stays set for the life of the incident");
	CHECK(r.count(IncidentEventKind::Escalated, Light::Network) == 1, "the climb back to red is an Escalated event");
}

static void testSceneClockRestartsAcrossLiveUnknown()
{
	Rig r;
	r.live();
	r.in.programSceneUuid = "start";
	r.tick();
	r.tick(250000);
	r.in.live = LiveState::Unknown;
	r.tick();
	r.in.live = LiveState::Live;
	r.tick();
	r.tick(299000);
	CHECK(r.is(Light::Scene, St::Green, Rs::Ok), "scene clock restarted when the live state became unknown: 300 s is not over");
	r.tick(1);
	CHECK(r.is(Light::Scene, St::Amber, Rs::SceneOverLimit), "scene limit counts from the unknown period");
}

static void testDisabledLightForgetsItsTimers()
{
	{
		Rig r;
		r.live();
		r.muteFor(19);
		r.th.lightOn[static_cast<int>(Light::Mic)] = false;
		r.apply();
		r.ticks(60);
		r.th.lightOn[static_cast<int>(Light::Mic)] = true;
		r.apply();
		r.tick();
		CHECK(r.is(Light::Mic, St::Green, Rs::Ok), "re-enabled mic is green, not amber from the old mute timer");
		r.ticks(19);
		CHECK(r.is(Light::Mic, St::Green, Rs::Ok), "re-enabled mic: 19 s of fresh mute is green");
		r.tick();
		CHECK(r.is(Light::Mic, St::Amber, Rs::MicMuted), "re-enabled mic: a fresh 20 s is amber");
	}
	{
		Rig r;
		r.live();
		r.muteFor(19);
		r.th.setupDone = false;
		r.apply();
		r.ticks(60);
		r.th.setupDone = true;
		r.apply();
		r.tick();
		r.ticks(19);
		CHECK(r.L(Light::Mic).state == St::Green, "setup done again: timers started fresh, 19 s green");
		r.tick();
		CHECK(r.L(Light::Mic).state == St::Amber, "setup done again: a fresh 20 s is amber");
	}
	{
		Rig r;
		r.live();
		r.in.desktop.peakDb = -kInf;
		r.tick();
		r.ticks(100);
		r.th.lightOn[static_cast<int>(Light::Desktop)] = false;
		r.apply();
		r.ticks(10);
		r.th.lightOn[static_cast<int>(Light::Desktop)] = true;
		r.apply();
		r.tick();
		r.ticks(100);
		CHECK(r.is(Light::Desktop, St::Green, Rs::Ok), "re-enabled desktop restarts the digital silence count");
	}
}

static void testCallbackTimeInTheFutureAfterClockJump()
{
	Rig r;
	r.live();
	r.tick();
	r.autoCb = false;
	r.tick(-100000);
	CHECK(r.is(Light::Mic, St::Green, Rs::Ok), "after a backwards jump a future callback time is not trusted but not yet stale");
	r.tick(4999);
	CHECK(r.is(Light::Mic, St::Green, Rs::Ok) && r.is(Light::Desktop, St::Green, Rs::Ok), "freshness timer restarted at the jump: 4.999 s green");
	r.tick(1);
	CHECK(r.is(Light::Mic, St::Unknown, Rs::MicNoCallback), "mic: 5 s after the jump with no new callback is unknown");
	CHECK(r.is(Light::Desktop, St::Unknown, Rs::DesktopNoCallback), "desktop: 5 s after the jump with no new callback is unknown");
}

static void testRepeatedStoppedWithoutStartIsOneIncident()
{
	Rig r;
	r.th.recordIntent = true;
	r.apply();
	r.live();
	r.rec(RecordingEventKind::Started);
	r.in.recording.active = true;
	r.tick();
	r.rec(RecordingEventKind::Stopped);
	r.in.recording.active = false;
	r.tick();
	CHECK(r.is(Light::Recording, St::Red, Rs::RecordingStopped) && r.L(Light::Recording).flashNow, "start then stop: stopped incident flashes");
	const quint64 id = r.L(Light::Recording).incidentId;
	r.rec(RecordingEventKind::Stopped);
	r.tick();
	CHECK(r.L(Light::Recording).incidentId == id && !r.L(Light::Recording).flashNow,
	      "another Stopped with no Started in between: same incident, no second flash");
	CHECK(r.count(IncidentEventKind::Started, Light::Recording) == 1 && r.flashes[static_cast<int>(Light::Recording)] == 1,
	      "repeated Stopped: one incident started, one flash");
	r.rec(RecordingEventKind::Started);
	r.rec(RecordingEventKind::Stopped);
	r.tick();
	CHECK(r.L(Light::Recording).incidentId != id && r.L(Light::Recording).flashNow, "a Started before the next Stopped makes a new stop");
}

static void testMicMutedRedThenMissing()
{
	Rig r;
	r.live();
	r.muteFor(60);
	const quint64 first = r.L(Light::Mic).incidentId;
	CHECK(r.is(Light::Mic, St::Red, Rs::MicMuted) && r.L(Light::Mic).incidentId != 0, "muted 60 s: red muted incident");
	r.in.mic.exists = false;
	r.tick();
	CHECK(r.is(Light::Mic, St::Red, Rs::MicMissing), "muted red then the source goes missing: red missing");
	CHECK(r.L(Light::Mic).incidentId != first && r.L(Light::Mic).flashNow, "the missing mic is a new incident that flashes");
	CHECK(r.count(IncidentEventKind::Ended, Light::Mic) == 1 && r.count(IncidentEventKind::Started, Light::Mic) == 2,
	      "old incident Ended, new incident Started");
	CHECK(r.flashes[static_cast<int>(Light::Mic)] == 2, "each of the two incidents flashed once");
}

static void testNetworkRedReconnectAcknowledgeRefill()
{
	Rig r = netCase(21, 1000);
	const quint64 redId = r.L(Light::Network).incidentId;
	CHECK(r.L(Light::Network).flashNow, "drops red: flashes");
	r.post(StreamEventKind::Reconnect);
	r.tick();
	const quint64 reconId = r.L(Light::Network).incidentId;
	CHECK(r.is(Light::Network, St::Amber, Rs::NetworkReconnecting) && reconId != redId, "reconnect is a new incident");
	CHECK(!r.L(Light::Network).flashNow, "reconnecting amber does not flash");
	r.ev.acknowledge(Light::Network, r.now);
	r.post(StreamEventKind::ReconnectSuccess);
	r.in.network.totalFrames += 200;
	r.in.network.droppedFrames += 10;
	r.tick();
	CHECK(r.is(Light::Network, St::Unknown, Rs::NetworkWindowFilling) && r.L(Light::Network).incidentId == reconId,
	      "after success: unknown, the acknowledged incident stays open");
	CHECK(r.L(Light::Network).acknowledged, "the reconnect incident stays acknowledged");
	for (int i = 0; i < 29; ++i) {
		r.in.network.totalFrames += 200;
		r.in.network.droppedFrames += 10;
		r.tick();
	}
	CHECK(r.L(Light::Network).state == St::Unknown, "29 s after success: still filling");
	r.in.network.totalFrames += 200;
	r.in.network.droppedFrames += 10;
	r.tick();
	CHECK(r.is(Light::Network, St::Red, Rs::NetworkDrops), "window refilled with 5 % drops: red");
	const quint64 dropId = r.L(Light::Network).incidentId;
	CHECK(dropId != reconId && dropId != redId && r.L(Light::Network).flashNow, "the refilled drops are a third incident that flashes");
	CHECK(!r.L(Light::Network).acknowledged, "the new incident is not acknowledged");
	for (int i = 0; i < 20; ++i) {
		r.in.network.totalFrames += 200;
		r.in.network.droppedFrames += 10;
		r.tick();
	}
	CHECK(r.flashes[static_cast<int>(Light::Network)] == 2, "red drops flashed twice in total (first and third incident), reconnect never");
	CHECK(r.count(IncidentEventKind::Started, Light::Network) == 3 && r.count(IncidentEventKind::Ended, Light::Network) == 2,
	      "three incidents started, the first two ended");
}

static void testHoldingSceneSurvivesLongLiveUnknown()
{
	Rig r;
	r.live();
	r.in.programSceneUuid = "start";
	r.tick();
	r.in.live = LiveState::Unknown;
	r.tick();
	r.tick(700000);
	r.in.live = LiveState::Live;
	r.tick();
	CHECK(r.is(Light::Scene, St::Green, Rs::Ok) && r.L(Light::Scene).incidentId == 0,
	      "back to live on the same holding scene after a long unknown: green, no incident");
	r.tick(299000);
	CHECK(r.is(Light::Scene, St::Green, Rs::Ok), "the clock started fresh: exactly 300 s after the return is not over");
	r.tick(1);
	CHECK(r.is(Light::Scene, St::Amber, Rs::SceneOverLimit), "the limit counts from the return to live");
}


// ---- the Network light judges every outgoing stream ----

static bool near(double a, double b)
{
	return std::fabs(a - b) < 1e-9;
}

static const NetOutputView *viewOf(const Snapshot &s, const char *id)
{
	for (const NetOutputView &v : s.netOutputs)
		if (v.id == QString(id))
			return &v;
	return nullptr;
}

static void testNetworkOutputs()
{
	// One bad destination among good ones is red, although the average over all outputs is not bad.
	{
		Rig r;
		r.addDest("a", "Twitch", 6); // 3.0 % of its own frames
		r.addDest("b", "YouTube");
		r.addDest("c", "Kick");
		r.addDest("d", "Facebook");
		r.addDest("e", "TikTok");
		r.live();
		r.ticks(30);
		const double combined = 100.0 * double(30 * 6) / double(6 * 30 * 200); // 6 outputs, one dropping
		CHECK(combined <= r.th.dropAmberPct, "outputs: the combined average of all six outputs would not even be amber (0.5 %)");
		CHECK(r.is(Light::Network, St::Red, Rs::NetworkDrops), "outputs: one destination at 3 % among five clean ones: Network is RED");
		CHECK(near(r.L(Light::Network).value, 3.0), "outputs: the value is that destination's own 30 s figure (3.0 %)");
		CHECK(r.L(Light::Network).net.valid && r.L(Light::Network).net.outputId == QString("a") &&
			      r.L(Light::Network).net.outputName == QString("Twitch"),
		      "outputs: the result names the destination (id and display name)");
		CHECK(r.L(Light::Network).net.multi, "outputs: the detail knows Multistream destinations are in play");
		CHECK(r.L(Light::Network).net.mainFine && r.L(Light::Network).net.fineDestinations.size() == 4,
		      "outputs: OBS's own stream and the four other destinations are listed as fine");
		CHECK(near(r.L(Light::Network).net.sessionPct, 3.0), "outputs: session total for the named destination is 3.0 %");
		CHECK(r.L(Light::Network).flashNow, "outputs: a destination going red flashes like the main stream does");
		CHECK(r.count(IncidentEventKind::Started, Light::Network) == 1, "outputs: exactly one Network incident started");
		bool named = false;
		for (const IncidentEvent &e : r.all)
			if (e.light == Light::Network && e.kind == IncidentEventKind::Started && e.outputName == QString("Twitch"))
				named = true;
		CHECK(named, "outputs: the Started event in the session log names the destination");
		const NetOutputView *v = viewOf(r.s, "a");
		CHECK(v && v->state == St::Red && near(v->pct, 3.0), "outputs: the per-output view carries Twitch's figure for its Multistream row");
		const NetOutputView *mv = viewOf(r.s, "");
		CHECK(mv && mv->state == St::Green && near(mv->pct, 0.0), "outputs: and OBS's own stream is listed green at 0 %");
	}
	// OBS's own stream stays judged exactly as before when it is the worst.
	{
		Rig r;
		r.netDrop = 8; // main at 4 %
		r.addDest("a", "Twitch");
		r.live();
		r.ticks(30);
		CHECK(r.is(Light::Network, St::Red, Rs::NetworkDrops) && r.L(Light::Network).net.outputId.isEmpty(),
		      "outputs: the main stream at 4 % is red and the detail names no destination (id empty)");
		CHECK(!r.L(Light::Network).net.mainFine && r.L(Light::Network).net.fineDestinations.size() == 1,
		      "outputs: Twitch is listed as fine, the main stream is not");
	}
	// Worst-of selection.
	{
		Rig r;
		r.netDrop = 3; // main 1.5 % amber
		r.addDest("a", "Twitch", 2);  // 1 % amber
		r.addDest("b", "YouTube", 8); // 4 % red
		r.addDest("c", "Kick", 5);    // 2.5 % red
		r.live();
		r.ticks(30);
		CHECK(r.is(Light::Network, St::Red, Rs::NetworkDrops) && r.L(Light::Network).net.outputId == QString("b") &&
			      near(r.L(Light::Network).value, 4.0),
		      "worst-of: red beats amber and the bigger red wins: YouTube at 4 %");
		r.dest("b").drop = 0;
		r.dest("c").drop = 0;
		r.dest("a").drop = 0;
		r.ticks(0);
	}
	{
		Rig r;
		r.netDrop = 3; // main 1.5 % amber
		r.addDest("a", "Twitch", 2);  // 1.0 % amber
		r.addDest("b", "YouTube");
		r.live();
		r.ticks(30);
		CHECK(r.is(Light::Network, St::Amber, Rs::NetworkDrops) && r.L(Light::Network).net.outputId.isEmpty() &&
			      near(r.L(Light::Network).value, 1.5),
		      "worst-of: two ambers: the bigger drop wins (the main stream at 1.5 % over Twitch at 1 %)");
	}
	{
		Rig r;
		r.addDest("a", "Twitch", 2); // amber
		r.addDest("b", "YouTube");
		r.live();
		r.ticks(30);
		r.dest("b").st = OutputState::Reconnecting;
		r.tick();
		CHECK(r.is(Light::Network, St::Amber, Rs::NetworkReconnecting) && r.L(Light::Network).net.outputId == QString("b"),
		      "worst-of: among ambers a reconnecting destination outranks one that is merely dropping a little");
	}
	// Reconnecting destination: amber naming it; its frames are not judged; main stays fine.
	{
		Rig r;
		r.addDest("a", "Twitch");
		r.addDest("b", "YouTube");
		r.live();
		r.ticks(30);
		CHECK(r.is(Light::Network, St::Green, Rs::Ok), "reconnecting: all green first");
		r.dest("a").st = OutputState::Reconnecting;
		r.tick();
		CHECK(r.is(Light::Network, St::Amber, Rs::NetworkReconnecting), "reconnecting: a reconnecting destination is AMBER");
		CHECK(r.L(Light::Network).net.outputName == QString("Twitch") && r.L(Light::Network).net.mainFine &&
			      r.L(Light::Network).net.fineDestinations.size() == 1 &&
			      r.L(Light::Network).net.fineDestinations[0] == QString("YouTube"),
		      "reconnecting: it names Twitch; OBS's stream and YouTube are fine");
		CHECK(!r.L(Light::Network).flashNow, "reconnecting: amber does not flash");
		const NetOutputView *v = viewOf(r.s, "a");
		CHECK(v && v->reason == Rs::NetworkReconnecting, "reconnecting: its per-output view says reconnecting");
		// Back live as a NEW output (new generation, counters from zero): unknown until its own window refills.
		Rig::Dest &d = r.dest("a");
		d.st = OutputState::Live;
		d.gen = 2;
		d.dropped = 0;
		d.total = 0;
		d.drop = 20; // and it drops 10 % from the start
		r.tick();
		CHECK(r.L(Light::Network).state == St::Unknown && r.L(Light::Network).reason == Rs::NetworkWindowFilling,
		      "reconnecting: back live, the incident output is measuring again: unknown, the incident stays open");
		r.ticks(28);
		CHECK(r.L(Light::Network).state == St::Unknown, "reconnecting: 29 s after the reconnect the window is still not full");
		r.tick();
		r.tick();
		CHECK(r.is(Light::Network, St::Red, Rs::NetworkDrops) && r.L(Light::Network).net.outputId == QString("a"),
		      "reconnecting: judged again once its own window is full: Twitch red at its 10 %");
	}
	// A destination switched off: the light recomputes without it on that very update.
	{
		Rig r;
		r.addDest("a", "Twitch", 8);  // 4 % red
		r.addDest("b", "YouTube", 2); // 1 % amber
		r.live();
		r.ticks(30);
		CHECK(r.is(Light::Network, St::Red, Rs::NetworkDrops) && r.L(Light::Network).net.outputId == QString("a"),
		      "turned off: Twitch is the red one");
		const quint64 id = r.L(Light::Network).incidentId;
		r.dest("a").st = OutputState::Off;
		r.tick();
		CHECK(r.is(Light::Network, St::Amber, Rs::NetworkDrops) && r.L(Light::Network).net.outputId == QString("b"),
		      "turned off: at the next update the light is judged without Twitch: YouTube's amber");
		CHECK(r.L(Light::Network).incidentId != id && r.L(Light::Network).incidentId != 0,
		      "turned off: YouTube's amber is a new incident");
		bool endedOff = false;
		for (const IncidentEvent &e : r.all)
			if (e.light == Light::Network && e.kind == IncidentEventKind::Ended && e.incidentId == id)
				endedOff = e.turnedOff && e.outputName == QString("Twitch");
		CHECK(endedOff, "turned off: Twitch's incident ended as 'turned off' (flag set, destination named), not as recovered");
		// Switching Twitch off while it is the ONLY problem: green at once, no waiting out the 10 s rule.
		r.dest("b").drop = 0;
		r.ticks(31);
		CHECK(r.is(Light::Network, St::Green, Rs::Ok), "turned off: with YouTube recovered everything left is green");
	}
	{
		Rig r;
		r.addDest("a", "Twitch", 8);
		r.live();
		r.ticks(30);
		CHECK(r.L(Light::Network).state == St::Red, "turned off (only problem): red first");
		r.removeDest("a"); // removed from Multistream altogether
		r.tick();
		CHECK(r.is(Light::Network, St::Green, Rs::Ok) && r.L(Light::Network).incidentId == 0,
		      "turned off (only problem): once it is gone the light is green at the next update, no 10 s wait, incident closed");
		CHECK(r.count(IncidentEventKind::Ended, Light::Network) == 1 && r.all.back().turnedOff,
		      "turned off (only problem): one Ended event, marked turned off");
		CHECK(r.s.worst == St::Green, "turned off (only problem): the header worst state follows");
	}
	// Not being judged for another reason (connecting, retrying, failed): the incident ends, but not as "turned off".
	{
		Rig r;
		r.addDest("a", "Twitch", 8);
		r.live();
		r.ticks(30);
		r.dest("a").st = OutputState::Other;
		r.tick();
		CHECK(r.is(Light::Network, St::Green, Rs::Ok) && r.count(IncidentEventKind::Ended, Light::Network) == 1 &&
			      !r.all.back().turnedOff,
		      "other state: a destination that is neither live nor reconnecting is not judged; its incident ends without the turned-off mark");
	}
	// Switched back on mid-stream: a new output; judged again after its own window refills.
	{
		Rig r;
		r.addDest("a", "Twitch", 8);
		r.live();
		r.ticks(30);
		r.dest("a").st = OutputState::Off;
		r.ticks(5);
		CHECK(r.is(Light::Network, St::Green, Rs::Ok), "switched back on: green while it is off");
		Rig::Dest &d = r.dest("a");
		d.st = OutputState::Live;
		d.gen = 2;
		d.dropped = 0;
		d.total = 0;
		r.tick();
		// (Green says every outgoing stream was judged fine, and Twitch has no verdict yet)
		CHECK(r.is(Light::Network, St::Unknown, Rs::NetworkWindowFilling), "switched back on: while Twitch is measured the light is unknown (filling), not green");
		r.ticks(28);
		CHECK(r.is(Light::Network, St::Unknown, Rs::NetworkWindowFilling), "switched back on: 29 s in it is still not judged");
		r.ticks(2);
		CHECK(r.is(Light::Network, St::Red, Rs::NetworkDrops) && r.L(Light::Network).net.outputId == QString("a"),
		      "switched back on: judged again with its full window: red again");
	}
	// A destination replaced (new generation) restarts its baseline even when its counters do not go down.
	{
		Rig r;
		r.addDest("a", "Twitch", 8);
		r.live();
		r.ticks(30);
		CHECK(r.L(Light::Network).state == St::Red, "replaced: red first");
		r.dest("a").gen = 2; // counters keep climbing: only the generation tells it is another output
		r.tick();
		CHECK(r.is(Light::Network, St::Unknown, Rs::NetworkWindowFilling) && r.L(Light::Network).incidentId != 0,
		      "replaced: the baseline restarts at once: unknown (window filling), the incident stays open");
		r.ticks(28);
		CHECK(r.L(Light::Network).state == St::Unknown, "replaced: 29 s after the replacement still not judged");
		r.tick();
		r.tick();
		CHECK(r.is(Light::Network, St::Red, Rs::NetworkDrops), "replaced: judged again after a full window");
	}
	{
		Rig r;
		r.addDest("a", "Twitch", 8);
		r.live();
		r.ticks(30);
		Rig::Dest &d = r.dest("a");
		d.dropped = 0; // counter rollback with the SAME generation
		d.total = 0;
		r.tick();
		CHECK(r.L(Light::Network).state == St::Unknown, "replaced: a counter rollback restarts a destination's window too");
	}
	// The same destination that stops dropping recovers like the main stream: amber/red held until 10 s fine.
	{
		Rig r;
		r.addDest("a", "Twitch", 8);
		r.live();
		r.ticks(30);
		r.dest("a").drop = 0;
		r.ticks(10); // the window still holds 20 bad ticks (2.7 %)
		CHECK(r.L(Light::Network).state == St::Red, "recovery: red while the bad stretch is in the window");
		r.ticks(40);
		CHECK(r.is(Light::Network, St::Green, Rs::Ok), "recovery: green once the window is clean for 10 s");
		bool endedPlain = false;
		for (const IncidentEvent &e : r.all)
			if (e.light == Light::Network && e.kind == IncidentEventKind::Ended)
				endedPlain = !e.turnedOff && e.outputName == QString("Twitch");
		CHECK(endedPlain, "recovery: the incident ended as recovered (not turned off), and still names Twitch");
	}
	// A different output becoming the worst is a new incident; both are named.
	{
		Rig r;
		r.addDest("a", "Twitch", 8);
		r.addDest("b", "YouTube");
		r.live();
		r.ticks(30);
		const quint64 first = r.L(Light::Network).incidentId;
		r.dest("b").drop = 20; // YouTube at 10 % climbs past Twitch in the window
		r.ticks(20);
		CHECK(r.is(Light::Network, St::Red, Rs::NetworkDrops) && r.L(Light::Network).net.outputId == QString("b"),
		      "switch: YouTube overtakes Twitch as the worst output");
		CHECK(r.L(Light::Network).incidentId != first, "switch: it is a new incident");
		int startedNamed = 0;
		for (const IncidentEvent &e : r.all)
			if (e.light == Light::Network && e.kind == IncidentEventKind::Started && !e.outputName.isEmpty())
				startedNamed++;
		CHECK(startedNamed == 2, "switch: each incident's Started line names its destination");
	}
	// Session total arithmetic.
	{
		Rig r;
		r.addDest("a", "Twitch", 0);
		r.live();
		r.ticks(20); // 20 clean ticks
		r.dest("a").drop = 20; // then 10 %
		r.ticks(20);
		// ticks so far: the live() tick plus 20 + 20 = 41 ticks of 200 frames; 20 ticks of 20 lost
		CHECK(r.is(Light::Network, St::Red, Rs::NetworkDrops), "session total: red after the bad stretch");
		const double wantSession = 100.0 * double(20 * 20) / double(41 * 200);
		CHECK(near(r.L(Light::Network).net.sessionPct, wantSession), "session total: lost frames over all frames this session (arithmetic)");
		CHECK(r.L(Light::Network).net.sessionPct < r.L(Light::Network).value, "session total: lower than the 30 s figure, which holds only the bad stretch");
	}
	{
		// across a replacement: counters of the second output start at zero and are ADDED, not subtracted
		Rig r;
		r.addDest("a", "Twitch", 10);
		r.live();
		r.ticks(14); // 15 ticks of 10 lost / 200
		Rig::Dest &d = r.dest("a");
		d.gen = 2;
		d.dropped = 0;
		d.total = 0;
		d.drop = 20;
		r.ticks(31);
		CHECK(r.is(Light::Network, St::Red, Rs::NetworkDrops), "session total (replacement): red");
		const double want = 100.0 * double(15 * 10 + 31 * 20) / double(46 * 200);
		CHECK(near(r.L(Light::Network).net.sessionPct, want), "session total (replacement): both outputs' frames are counted, none twice");
		CHECK(near(r.L(Light::Network).value, 10.0), "session total (replacement): the 30 s figure only knows the new output (10 %)");
	}
	{
		// a new session starts every total afresh
		Rig r;
		r.addDest("a", "Twitch", 8);
		r.live();
		r.ticks(30);
		r.off();
		r.dest("a").dropped = 0;
		r.dest("a").total = 0;
		r.dest("a").gen = 3;
		r.live();
		r.ticks(30);
		CHECK(r.is(Light::Network, St::Red, Rs::NetworkDrops) && near(r.L(Light::Network).net.sessionPct, 4.0),
		      "session total: a new session counts from its own start (4.0 %)");
	}
	// Multistream disabled (or not loaded): no outputs at all, the main stream is judged alone as before.
	{
		Rig r;
		r.netDrop = 8;
		r.live();
		r.ticks(30);
		CHECK(r.is(Light::Network, St::Red, Rs::NetworkDrops) && r.L(Light::Network).net.outputId.isEmpty() && !r.L(Light::Network).net.multi,
		      "Multistream off: the main stream alone is judged; the detail names no destination and says no Multistream");
		CHECK(r.s.netOutputs.size() == 1 && r.s.netOutputs[0].id.isEmpty(), "Multistream off: exactly one judged output (the main stream)");
	}
	{
		Rig r;
		r.addDest("a", "Twitch");
		r.dest("a").st = OutputState::Off;
		r.live();
		r.ticks(30);
		CHECK(r.is(Light::Network, St::Green, Rs::Ok) && !r.L(Light::Network).net.multi, "Multistream installed but every destination off: main stream judged alone, green");
	}
	// A destination that joins mid-stream turns the light unknown (not green) until its window has filled.
	{
		Rig r;
		r.live();
		r.ticks(30);
		CHECK(r.is(Light::Network, St::Green, Rs::Ok), "joining: green first");
		r.addDest("a", "Twitch");
		r.tick();
		// (green only once every live output has a verdict)
		CHECK(r.is(Light::Network, St::Unknown, Rs::NetworkWindowFilling) && r.L(Light::Network).net.fillingDestinations.size() == 1 &&
			      r.L(Light::Network).net.fillingDestinations[0] == QString("Twitch") && !r.L(Light::Network).net.mainFilling,
		      "joining: a new destination being measured makes the light unknown (filling) and names it");
		const NetOutputView *v = viewOf(r.s, "a");
		CHECK(v && v->state == St::Unknown && v->pct < 0, "joining: its own view says it is still measuring");
	}
	// Main stream off while a destination is bad: the destination is still judged; with nothing to judge it is grey as before.
	{
		Rig r;
		r.addDest("a", "Twitch", 8);
		r.in.live = LiveState::Live;
		r.in.mainStreamActive = false;
		r.tick();
		r.ticks(30);
		CHECK(r.is(Light::Network, St::Red, Rs::NetworkDrops) && r.L(Light::Network).net.outputId == QString("a"),
		      "main off: a bad live destination is still judged");
	}
	{
		Rig r;
		r.addDest("a", "Twitch");
		r.dest("a").st = OutputState::Off;
		r.in.live = LiveState::Live;
		r.in.mainStreamActive = false;
		r.tick();
		CHECK(r.is(Light::Network, St::Grey, Rs::MainStreamOff), "main off and nothing else live: grey as before");
	}
	// Acknowledge still works on a destination incident.
	{
		Rig r;
		r.addDest("a", "Twitch", 8);
		r.live();
		r.ticks(30);
		r.ev.acknowledge(Light::Network, r.now);
		r.tick();
		CHECK(r.L(Light::Network).acknowledged && r.L(Light::Network).net.outputName == QString("Twitch"), "acknowledge: works on a destination's incident and keeps its name");
	}
}

// ---- Network light: thresholds, session totals and the worst-output rules ----
static void testNetworkReview()
{
	// a. A reconnect on the same output must not add its cumulative counters to the session totals a second time.
	{
		Rig r; // destination: 11 clean ticks, 5 ticks reconnecting at 10 % lost, then live again (same output) at 10 %
		r.addDest("a", "Twitch", 0);
		r.live();
		r.ticks(10);
		r.dest("a").st = OutputState::Reconnecting;
		for (int i = 0; i < 5; ++i) {
			r.dest("a").total += 200;
			r.dest("a").dropped += 20;
			r.tick();
		}
		r.dest("a").st = OutputState::Live;
		r.dest("a").drop = 20;
		r.ticks(31);
		const double want = 100.0 * double(5 * 20 + 31 * 20) / double(11 * 200 + 5 * 200 + 31 * 200);
		CHECK(r.is(Light::Network, St::Red, Rs::NetworkDrops) && r.L(Light::Network).net.outputId == QString("a"),
		      "reconnect session total (destination): red after the window refills");
		CHECK(near(r.L(Light::Network).net.sessionPct, want),
		      "reconnect session total (destination): counters that keep rising through a reconnect are counted once");
	}
	{
		Rig r; // the main stream: same shape
		r.netDrop = 0;
		r.live();
		r.ticks(10);
		r.netDrop = 20;
		r.in.network.reconnecting = true;
		r.ticks(5);
		r.in.network.reconnecting = false;
		r.ticks(31);
		const double want = 100.0 * double(36 * 20) / double(47 * 200);
		CHECK(r.is(Light::Network, St::Red, Rs::NetworkDrops) && r.L(Light::Network).net.outputId.isEmpty(),
		      "reconnect session total (main): red after the window refills");
		CHECK(near(r.L(Light::Network).net.sessionPct, want),
		      "reconnect session total (main): counters that keep rising through a reconnect are counted once");
	}
	{
		Rig r; // a counter rollback on the same generation is still a fresh output (counted from zero)
		r.addDest("a", "Twitch", 20);
		r.live();
		r.ticks(9); // 10 ticks, 200 lost of 2000
		r.dest("a").dropped = 0;
		r.dest("a").total = 0;
		r.ticks(31); // 31 ticks, 620 lost of 6200
		CHECK(near(r.L(Light::Network).net.sessionPct, 10.0), "reconnect session total: a counter rollback still starts a new epoch");
	}
	// b. The button belongs to the CURRENT worst output only.
	{
		Rig r;
		r.addDest("a", "Twitch", 20);
		r.live();
		r.ticks(30);
		const LightResult red = r.L(Light::Network);
		CHECK(red.state == St::Red && canTurnOffOutput(red) && red.net.currentBadId == QString("a"), "turn off: offered while the destination is the current worst output");
		// the output is replaced: the incident is held, but nothing is judged bad right now
		Rig::Dest &d = r.dest("a");
		d.gen = 2;
		d.dropped = 0;
		d.total = 0;
		r.tick();
		const LightResult held = r.L(Light::Network);
		CHECK(held.incidentId == red.incidentId && held.net.outputId == QString("a") && held.state == St::Unknown,
		      "turn off: the old incident is still held while the replacement fills");
		CHECK(!held.net.currentBadValid && !canTurnOffOutput(held), "turn off: ... but the button is not offered, the destination is no longer the current worst output");
		std::vector<OutputSample> cur = r.in.outputs;
		CHECK(checkTurnOff(held, QString("a"), cur) == TurnOffCheck::NotTheWorstOutput, "turn off: the last look refuses a held incident");
		CHECK(checkTurnOff(red, QString("a"), cur) == TurnOffCheck::Ok, "turn off: the last look accepts a current worst output that is live");
		CHECK(checkTurnOff(red, QString("zzz"), cur) == TurnOffCheck::NotTheWorstOutput, "turn off: the last look refuses another destination's id");
		CHECK(checkTurnOff(red, QString("a"), {}) == TurnOffCheck::OutputGone, "turn off: the last look refuses a removed destination");
		cur[0].state = OutputState::Off;
		CHECK(checkTurnOff(red, QString("a"), cur) == TurnOffCheck::OutputNotLive, "turn off: the last look refuses a destination that is already off");
		CHECK(checkTurnOff(red, QString(), r.in.outputs) == TurnOffCheck::NotTheWorstOutput, "turn off: the main stream (empty id) is never switched off from here");
	}
	{
		// the worst output changes: the other destination became worst, the first one is fine
		Rig r;
		r.addDest("a", "Twitch", 8);
		r.addDest("b", "YouTube", 0);
		r.live();
		r.ticks(30);
		const LightResult one = r.L(Light::Network);
		CHECK(one.net.outputId == QString("a") && canTurnOffOutput(one), "turn off (two): Twitch is the worst, button for Twitch");
		r.dest("a").drop = 0;
		r.dest("b").drop = 40;
		r.ticks(31);
		const LightResult two = r.L(Light::Network);
		CHECK(two.net.outputId == QString("b") && two.net.currentBadId == QString("b") && canTurnOffOutput(two),
		      "turn off (two): YouTube is now the worst and gets the button");
		CHECK(checkTurnOff(two, QString("a"), r.in.outputs) == TurnOffCheck::NotTheWorstOutput,
		      "turn off (two): a click meant for Twitch (the button of a moment ago) is refused");
	}
	// c. Green only when every live output has a verdict.
	{
		Rig r;
		r.live();
		r.ticks(30);
		CHECK(r.is(Light::Network, St::Green, Rs::Ok), "filling: main stream fine first");
		r.addDest("a", "Twitch", 0);
		r.addDest("b", "YouTube", 0);
		r.ticks(10);
		CHECK(r.is(Light::Network, St::Unknown, Rs::NetworkWindowFilling) && r.L(Light::Network).net.fillingDestinations.size() == 2 &&
			      !r.L(Light::Network).net.mainFilling,
		      "filling: two new destinations are named, the main stream is not");
		r.ticks(19);
		CHECK(r.L(Light::Network).state == St::Unknown, "filling: still unknown just before the window is full");
		r.ticks(2);
		CHECK(r.is(Light::Network, St::Green, Rs::Ok), "filling: green once every live output has a verdict");
	}
	{
		// a bad output wins over another output's filling window
		Rig r;
		r.live();
		r.ticks(30);
		r.netDrop = 20;
		r.ticks(31);
		CHECK(r.is(Light::Network, St::Red, Rs::NetworkDrops), "filling: the main stream is red");
		r.addDest("a", "Twitch", 0);
		r.ticks(3);
		CHECK(r.is(Light::Network, St::Red, Rs::NetworkDrops), "filling: a bad output is still red while another output is measured");
	}
	{
		// the main stream refilling after a reconnect, destination judged fine: unknown, naming the main stream
		Rig r;
		r.addDest("a", "Twitch", 0);
		r.live();
		r.ticks(30);
		CHECK(r.is(Light::Network, St::Green, Rs::Ok), "filling (main): both fine first");
		r.in.network.reconnecting = true;
		r.ticks(3);
		r.in.network.reconnecting = false;
		r.ticks(5);
		CHECK(r.is(Light::Network, St::Unknown, Rs::NetworkWindowFilling) && r.L(Light::Network).net.mainFilling,
		      "filling (main): the main stream measuring again makes the light unknown and names it");
	}
}

int main()
{
	testBasics();
	testMicStructural();
	testMicMuted();
	testMicFlicker();
	testMicCallbacks();
	testHeardYou();
	testDesktop();
	testScene();
	testRecording();
	testFrameWindow();
	testNetwork();
	testPc();
	testStream();
	testAcknowledgeSnooze();
	testSessionsAndClock();
	testRecordingStopUndoneBetweenTicks();
	testWindowResetsDiscardTheVerdict();
	testMicInactiveWhileMuted();
	testReasonChangeIsANewIncident();
	testFlashOncePerIncident();
	testSceneClockRestartsAcrossLiveUnknown();
	testDisabledLightForgetsItsTimers();
	testCallbackTimeInTheFutureAfterClockJump();
	testRepeatedStoppedWithoutStartIsOneIncident();
	testMicMutedRedThenMissing();
	testNetworkRedReconnectAcknowledgeRefill();
	testHoldingSceneSurvivesLongLiveUnknown();
	testForgetIncidentMarks();
	testNetworkOutputs();
	testNetworkReview();
	std::printf("%s (%d checks, %d failure%s)\n", failures ? "FAILED" : "ALL PASSED", total, failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}

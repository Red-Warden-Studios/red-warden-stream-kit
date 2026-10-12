/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

// The pure evaluator: once a second the probe hands it an Inputs struct and the current time, and it
// returns the state of the seven warning lights. Qt Core only, no libobs, no clock of its own: every
// time is a qint64 millisecond argument, so the same inputs always give the same answer.
#pragma once

#include "inflight-settings.hpp"

#include <QString>
#include <QVector>
#include <QtGlobal>

#include <array>
#include <deque>
#include <limits>
#include <map>
#include <vector>

namespace inflight {

constexpr int kLightCount = 7;

// Constants that are not user settings.
constexpr qint64 kClearAfterFineMs = 10000; // a bad light turns green only after this long fine
constexpr qint64 kNoCallbackMs = 5000;      // a meter this long without a callback is "can't tell"
constexpr qint64 kSnoozeMs = 600000;        // snooze length (10 minutes)
constexpr qint64 kFrameWindowMs = 30000;    // rolling window for frame statistics
constexpr qint64 kFrameStaleMs = 5000;      // max age of the newest sample / max gap between samples
constexpr qint64 kFrameAnchorSlackMs = 2000; // the window's start sample may be this much older than the span
// A window is only judged when it holds at least this many frames (10 fps over the 30 s window), so a
// handful of dropped frames on a near-idle output can never read as a large percentage.
constexpr quint64 kMinWindowFrames = 300;

// Display order.
enum class Light { Mic, Desktop, Scene, Recording, Network, Pc, Stream };
enum class LightState { Grey, Green, Amber, Red, Unknown };

// One code per distinct cause. The UI maps these to text; the evaluator holds no wording.
enum class Reason {
	NotSetUp,
	Disabled,
	OffAir,
	MainStreamOff,
	LiveUnknown,
	Ok,
	MicMuted,
	MicMissing,
	MicNotAudio,
	MicInactive,
	MicNoCallback,
	MicNotHeard,
	DesktopMuted,
	DesktopMissing,
	DesktopNotAudio,
	DesktopNoCallback,
	DesktopDigitalSilence,
	SceneOverLimit,
	RecordingNotStarted,
	RecordingStopped,
	RecordingPaused,
	NetworkDrops,
	NetworkReconnecting,
	NetworkWindowFilling,
	PcRenderLag,
	PcEncoderLag,
	PcWindowFilling,
	StreamDropped
};

// Everything the evaluator needs from the user's settings.
struct Thresholds {
	bool setupDone = true;
	std::array<bool, kLightCount> lightOn;
	bool recordIntent = false;
	QVector<HoldingScene> holdingScenes;
	int micMutedAmberS = 20;
	int micMutedRedS = 60;
	bool heardYouEnabled = false;
	int heardYouSilenceS = 90;
	int heardYouDbfs = -50;
	int desktopSilenceS = 120;
	int sceneRedExtraS = 300;
	int recordStartGraceS = 60;
	double dropAmberPct = 0.5;
	double dropRedPct = 2.0;
	Thresholds() { lightOn.fill(true); }
};

inline Thresholds thresholdsFromSettings(const Settings &s)
{
	Thresholds t;
	t.setupDone = s.setupDone;
	t.lightOn = {s.lightMic, s.lightDesktop, s.lightScene, s.lightRecording, s.lightNetwork, s.lightPc, s.lightStream};
	t.recordIntent = s.recordIntent;
	t.holdingScenes = s.holdingScenes;
	t.micMutedAmberS = s.micMutedAmberS;
	t.micMutedRedS = s.micMutedRedS;
	t.heardYouEnabled = s.heardYouEnabled;
	t.heardYouSilenceS = s.heardYouSilenceS;
	t.heardYouDbfs = s.heardYouDbfs;
	t.desktopSilenceS = s.desktopSilenceS;
	t.sceneRedExtraS = s.sceneRedExtraS;
	t.recordStartGraceS = s.recordStartGraceS;
	t.dropAmberPct = s.dropAmberPct;
	t.dropRedPct = s.dropRedPct;
	return t;
}

enum class LiveState { Off, Live, Unknown };

struct AudioInput {
	bool configured = false;
	bool exists = false;
	bool isAudio = false;
	bool muted = false;
	bool audioActive = false;
	bool attached = false;  // a level meter is attached to the source
	qint64 lastCallbackMs = 0; // time of the newest meter callback; 0 = never
	float peakDb = -std::numeric_limits<float>::infinity(); // -inf is a real, meaningful value
	float avgDb = -std::numeric_limits<float>::infinity();
};

enum class StreamEventKind { Started, StoppedOk, StoppedError, Reconnect, ReconnectSuccess, OutputReplaced };

struct StreamEvent {
	StreamEventKind kind = StreamEventKind::Started;
	int code = 0;
	qint64 atMs = 0;
};

enum class RecordingEventKind { Started, Stopped, Paused, Unpaused };

struct RecordingEvent {
	RecordingEventKind kind = RecordingEventKind::Started;
	qint64 atMs = 0;
};

// One Multistream destination, as the Kit hands it over once a second (read in-process on the UI thread from
// Multistream's own outputs; In-Flight never touches an obs_output of Multistream). Off covers a destination
// that is switched off, removed, or idle; Live is a connected output that has sent video; Reconnecting is a
// dropped connection Multistream is winning back; Other is everything else (connecting, retrying, paused,
// failed): not judged, not an incident either.
enum class OutputState { Off, Live, Reconnecting, Other };

struct OutputSample {
	QString id;   // Multistream's destination id: stable for the destination's life
	QString name; // what the user calls it ("Twitch")
	OutputState state = OutputState::Off;
	quint64 dropped = 0; // obs_output_get_frames_dropped of the destination's CURRENT output
	quint64 total = 0;   // obs_output_get_total_frames of the same output
	// Changes whenever the destination's obs_output is replaced or restarted, so counters of two different
	// outputs are never subtracted from each other.
	quint64 generation = 0;
};

struct Inputs {
	LiveState live = LiveState::Off;
	bool mainStreamActive = false;
	AudioInput mic;
	AudioInput desktop;
	QString programSceneUuid;
	struct Recording {
		bool active = false;
		bool paused = false;
	} recording;
	// Recording events since the previous update, oldest first. The booleans above stay the steady-state
	// truth; the events make sure a stop that was undone between two updates is still noticed.
	std::vector<RecordingEvent> recordingEvents;
	struct Network {
		bool reconnecting = false;
		quint64 droppedFrames = 0;
		quint64 totalFrames = 0;
	} network;
	struct Pc {
		quint64 renderLagged = 0;
		quint64 renderTotal = 0;
		quint64 encoderSkipped = 0;
		quint64 encoderTotal = 0;
	} pc;
	// Stream events since the previous update, oldest first.
	std::vector<StreamEvent> events;
	// Every Multistream destination (empty when Multistream is off or not loaded: the main stream is then
	// judged alone, exactly as before). The Network light judges each Live / Reconnecting one separately.
	std::vector<OutputSample> outputs;
};

// What the Network light says about WHICH output is the worst one. id is empty for OBS's own stream.
struct NetDetail {
	bool valid = false;
	QString outputId;
	QString outputName;   // empty for OBS's own stream
	double sessionPct = -1.0; // dropped share of ALL frames this output sent this session; -1 = nothing counted yet
	bool multi = false;   // at least one Multistream destination is being watched (live or reconnecting)
	bool mainFine = false; // OBS's own stream is judged and is not the worst, and is fine
	QStringList fineDestinations; // names of the other destinations that are judged and fine
	// The output that is the worst bad one RIGHT NOW (outputId above is the one the held incident names, which
	// can be an older output while its replacement fills). "Turn off" is only offered when the two match.
	bool currentBadValid = false;
	QString currentBadId; // empty = OBS's own stream
	// While the light is Unknown because windows are filling: which live outputs have no verdict yet.
	bool mainFilling = false;
	QStringList fillingDestinations;
};

struct LightResult {
	LightState state = LightState::Grey;
	Reason reason = Reason::OffAir;
	qint64 sinceMs = 0;     // when this incident, or this state, began
	quint64 incidentId = 0; // 0 = none
	bool flashNow = false;  // true on exactly the one update where an incident first turned red
	bool acknowledged = false;
	qint64 snoozedUntilMs = 0; // 0 = not snoozed
	double value = 0.0;     // drop percent, seconds, or a stream error code, depending on the reason
	NetDetail net;          // Network only: the worst output behind this result (valid while it is bad / held)
};

enum class IncidentEventKind { Started, Escalated, Ended, Acknowledged };

struct IncidentEvent {
	IncidentEventKind kind = IncidentEventKind::Started;
	Light light = Light::Mic;
	quint64 incidentId = 0;
	Reason reason = Reason::Ok;
	qint64 atMs = 0;
	QString outputName;     // Network events: the destination the incident named (empty = OBS's own stream)
	bool turnedOff = false; // Ended only: the destination was switched off or removed, it did not recover
};

// One judged output as the Network light saw it this tick (Multistream shows the drop figure on its row).
struct NetOutputView {
	QString id; // empty = OBS's own stream
	LightState state = LightState::Grey;
	Reason reason = Reason::Ok;
	double pct = -1.0; // 30 s drop percent; -1 while its window is still filling
};

struct Snapshot {
	std::array<LightResult, kLightCount> lights;
	// Worst state for the header dot: Red > Amber > Unknown > Green > Grey. Snoozed lights still count.
	LightState worst = LightState::Grey;
	std::vector<IncidentEvent> events; // emitted by this update, oldest first
	std::vector<NetOutputView> netOutputs; // every output the Network light is judging this tick
	const LightResult &at(Light l) const { return lights[static_cast<size_t>(l)]; }
};

// "Turn off <destination>" is offered only while the destination the open incident names IS the worst bad output
// right now. A held incident outlives its output's verdict (a replacement window refilling, a reconnect), and a
// click then would switch off a destination that is not currently the problem.
inline bool canTurnOffOutput(const LightResult &n)
{
	return n.incidentId != 0 && n.net.valid && !n.net.outputId.isEmpty() && n.net.currentBadValid &&
	       n.net.currentBadId == n.net.outputId && (n.state == LightState::Red || n.state == LightState::Amber);
}

enum class TurnOffCheck { Ok, NotTheWorstOutput, NotBad, OutputGone, OutputNotLive };
// The last look before the destination is switched off: the Network result of the latest update, and the
// destinations as they are right now. Anything but Ok means do nothing (and the log says why).
inline TurnOffCheck checkTurnOff(const LightResult &n, const QString &outputId, const std::vector<OutputSample> &current)
{
	if (outputId.isEmpty() || !n.net.valid || n.net.outputId != outputId || !n.net.currentBadValid ||
	    n.net.currentBadId != outputId)
		return TurnOffCheck::NotTheWorstOutput;
	if (n.incidentId == 0 || !(n.state == LightState::Red || n.state == LightState::Amber))
		return TurnOffCheck::NotBad;
	for (const OutputSample &s : current)
		if (s.id == outputId)
			return (s.state == OutputState::Live || s.state == OutputState::Reconnecting) ? TurnOffCheck::Ok
													 : TurnOffCheck::OutputNotLive;
	return TurnOffCheck::OutputGone;
}

inline const char *turnOffCheckName(TurnOffCheck c)
{
	switch (c) {
	case TurnOffCheck::Ok:
		return "ok";
	case TurnOffCheck::NotTheWorstOutput:
		return "it is not the worst output any more";
	case TurnOffCheck::NotBad:
		return "the Network light is not bad now";
	case TurnOffCheck::OutputGone:
		return "the destination no longer exists";
	case TurnOffCheck::OutputNotLive:
		return "the destination is already off or not connected";
	}
	return "";
}

// Rolling window over cumulative (bad, total) counters sampled over time.
struct WindowStats {
	bool filled = false;
	double pct = 0.0; // bad frames as a percentage of total frames in the window
};

class FrameWindow {
public:
	explicit FrameWindow(qint64 spanMs = kFrameWindowMs) : span_(spanMs) {}
	// A counter that went down, or a time that went backwards, starts the window afresh.
	void add(qint64 tMs, quint64 bad, quint64 total);
	// Filled needs: newest sample within kFrameStaleMs of nowMs, a start sample between span and
	// span + kFrameAnchorSlackMs old, no gap over kFrameStaleMs after it, and at least kMinWindowFrames
	// frames counted across the window.
	WindowStats stats(qint64 nowMs) const;
	void reset() { samples_.clear(); }

private:
	struct Sample {
		qint64 t;
		quint64 bad;
		quint64 total;
	};
	qint64 span_;
	std::deque<Sample> samples_;
};

// What one rule says about one light before the incident logic (hysteresis) is applied.
struct Candidate {
	LightState state = LightState::Green;
	Reason reason = Reason::Ok;
	double value = 0.0;
	// Bit per Reason: a bad condition is present but has not yet held long enough. While an incident
	// with that same reason is open, such a condition counts as "not fine yet", so a flicker never clears it.
	quint64 pendingMask = 0;
	bool forceNew = false; // start a fresh incident even if a different one is open
	NetDetail net;         // Network only
};

class Evaluator {
public:
	Evaluator() = default;
	explicit Evaluator(const Thresholds &t) : t_(t) {}

	void setThresholds(const Thresholds &t) { t_ = t; }
	const Thresholds &thresholds() const { return t_; }

	// Call once a second while the dock is watching. nowMs must come from one steady clock.
	Snapshot update(const Inputs &in, qint64 nowMs);

	// Marks the light's open incident acknowledged; for a latched stream drop it clears the light.
	// Does nothing when the light has no open incident. The event is emitted by the next update.
	void acknowledge(Light l, qint64 nowMs);
	// Snoozes the light's open incident for kSnoozeMs: the colour stays, the one-time flash is held back.
	// Does nothing when the light has no open incident. A snooze ends with its incident.
	void snooze(Light l, qint64 nowMs);
	// Restarts the frame windows (profile change, output replaced); the lights read Unknown until they refill.
	void resetBaselines();
	// "Start fresh for the next stream" (the Kit's Reset, only offered when no session is active): forgets
	// every acknowledgement and snooze, clears a latched stream drop, and drops the events not yet handed
	// out (the session log is cleared with it, so no stray Ended line may arrive afterwards).
	void forgetIncidentMarks(qint64 nowMs);

private:
	struct Hold {
		qint64 start = -1;
		// Returns how long the condition has held, or -1 when it does not hold now.
		qint64 update(bool cond, qint64 now)
		{
			if (!cond) {
				start = -1;
				return -1;
			}
			if (start < 0)
				start = now;
			return now - start;
		}
		void reset() { start = -1; }
	};
	struct Tracker {
		quint64 id = 0; // open incident, 0 = none
		LightState state = LightState::Grey;
		Reason reason = Reason::Ok;
		qint64 startMs = 0;
		qint64 fineSince = -1;
		double value = 0.0;
		bool redShown = false;
		bool acked = false;
		qint64 snoozeUntil = 0;
		bool haveOut = false;
		LightState outState = LightState::Grey;
		Reason outReason = Reason::Ok;
		qint64 changeMs = 0;
		NetDetail net; // Network: the output the open incident names
	};
	// Session totals + window of ONE output (a Multistream destination; OBS's own stream keeps its window in
	// netWin_ and only its totals here).
	struct OutState {
		FrameWindow win;
		QString name;
		bool haveLast = false;
		bool haveGen = false;
		quint64 gen = 0;
		quint64 lastBad = 0, lastTotal = 0;
		quint64 sessBad = 0, sessTotal = 0;
	};
	static void accumulate(OutState &o, quint64 bad, quint64 total, quint64 gen);
	static double sessionPct(const OutState &o);
	void forgetOutputs();
	Candidate evaluateLight(Light l, const Inputs &in, qint64 now);
	Candidate evalMic(const Inputs &in, qint64 now);
	Candidate evalDesktop(const Inputs &in, qint64 now);
	Candidate evalScene(qint64 now);
	Candidate evalRecording(const Inputs &in, qint64 now);
	Candidate evalNetwork(const Inputs &in, qint64 now);
	Candidate evalPc(const Inputs &in, qint64 now);
	LightResult apply(int i, const Candidate &c, qint64 now, std::vector<IncidentEvent> &ev);
	void endIncident(int i, qint64 now, std::vector<IncidentEvent> &ev, bool turnedOff = false);
	void handleEvent(const StreamEvent &e, qint64 now, std::vector<IncidentEvent> &ev);
	void clearLatch(qint64 now, std::vector<IncidentEvent> &ev);
	void resetForNewSession(qint64 now, std::vector<IncidentEvent> &ev);
	void endSession(qint64 now, std::vector<IncidentEvent> &ev);
	void resetHolds();
	void resetMicHolds();
	void resetLightTimers(Light l, qint64 now);
	void clockWentBack(qint64 now);
	const HoldingScene *holdingFor(const QString &uuid) const;
	IncidentEvent makeEvent(IncidentEventKind k, int i, qint64 now) const;

	Thresholds t_;
	bool haveLast_ = false;
	qint64 lastNow_ = 0;
	LiveState knownLive_ = LiveState::Off;
	quint64 nextId_ = 0;
	std::array<Tracker, kLightCount> trk_;
	std::vector<IncidentEvent> pending_;

	// Per-session state.
	qint64 liveSince_ = 0;
	bool recWasActive_ = false;
	bool recPrevActive_ = false;
	bool recRunning_ = false; // recording was running and no stop has used that up yet
	bool micHeard_ = false;
	bool liveSeen_ = false;
	bool reconnectPending_ = false;
	Hold micMuted_, micNoCb_, micQuiet_, deskNoCb_, deskSilence_;
	FrameWindow netWin_, renderWin_, encWin_;
	OutState mainOut_;                // OBS's own stream: session totals only
	std::map<QString, OutState> outs_; // Multistream destinations by id
	std::vector<NetOutputView> netViews_;

	// Scene clock (runs always; judged only while live).
	bool haveScene_ = false;
	QString sceneUuid_;
	qint64 sceneStart_ = 0;

	// A stream drop stays lit after going off air until acknowledged or a new session goes live.
	bool latched_ = false;
	qint64 latchSince_ = 0;
	int latchCode_ = 0;
};

} // namespace inflight

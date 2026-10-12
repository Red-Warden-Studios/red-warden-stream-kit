// Tests for inflight-ui-logic: reason wording, duration text, header state and header text.
// No framework: exits non-zero on failure.
#include "inflight-ui-logic.hpp"

#include <cstdio>

using namespace inflight;

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

static const TrFn tr = [](const char *, const char *fallback) { return QString::fromUtf8(fallback); };

static Snapshot greySnap()
{
	Snapshot s;
	for (LightResult &l : s.lights) {
		l.state = LightState::Grey;
		l.reason = Reason::OffAir;
	}
	return s;
}

static void set(Snapshot &s, Light l, LightState st, Reason r = Reason::Ok)
{
	LightResult &x = s.lights[static_cast<size_t>(l)];
	x.state = st;
	x.reason = r;
}

static void testReasons()
{
	// Every Reason, first to last. StreamDropped is the last enumerator.
	int bad = 0;
	int n = 0;
	for (int i = 0; i <= static_cast<int>(Reason::StreamDropped); i++) {
		const ReasonText t = reasonText(static_cast<Reason>(i), 12.0, tr);
		n++;
		if (t.what.isEmpty() || t.fix.isEmpty())
			bad++;
	}
	CHECK(n == 28, "28 reasons covered (a new Reason must be added to the test and to reasonText)");
	CHECK(bad == 0, "every Reason gives a non-empty what and fix");
	CHECK(reasonText(Reason::StreamDropped, -2.0, tr).what == QStringLiteral("Stream dropped (code -2)"),
	      "stream dropped shows the code");
	CHECK(reasonText(Reason::NetworkDrops, 2.34, tr).what.contains(QStringLiteral("2.3%")), "drop percent, one decimal");
	CHECK(reasonText(Reason::MicMuted, 75.0, tr).what.contains(QStringLiteral("1 min")), "timed reason shows a duration");
	CHECK(reasonText(Reason::MicMuted, 0.0, tr).what.contains(QStringLiteral("0 s")), "timed reason at zero");
	CHECK(!reasonText(Reason::MicMuted, 5.0, tr).what.contains(QStringLiteral("%1")), "no stray place marker");
	CHECK(reasonText(Reason::NetworkReconnecting, 0.0, tr).fix == QStringLiteral("Check your internet connection; OBS is retrying."),
	      "reconnecting fix wording");
	// Exact English fallback text.
	CHECK(reasonText(Reason::MicMuted, 75.0, tr).what == QStringLiteral("Your mic has been muted for 1 min"), "MicMuted what, exact");
	CHECK(reasonText(Reason::MicMuted, 75.0, tr).fix == QStringLiteral("Unmute it in the Audio Mixer."), "MicMuted fix, exact");
	CHECK(reasonText(Reason::MicNotHeard, 90.0, tr).what == QStringLiteral("Your mic has been very quiet for 1 min"),
	      "MicNotHeard what, exact (quiet, not silent)");
	CHECK(reasonText(Reason::MicNotHeard, 90.0, tr).fix ==
		      QStringLiteral("Check the mic's meter, gain and that the right device is picked."),
	      "MicNotHeard fix, exact");
	CHECK(reasonText(Reason::NetworkDrops, 3.4, tr).what == QStringLiteral("3.4% of frames dropped over the last 30 s"),
	      "NetworkDrops what, exact (percent and 30 s)");
	CHECK(reasonText(Reason::PcRenderLag, 2.1, tr).what ==
		      QStringLiteral("OBS rendered 2.1% of frames late over the last 30 s"),
	      "PcRenderLag what, exact");
	CHECK(reasonText(Reason::PcEncoderLag, 2.1, tr).what ==
		      QStringLiteral("The encoder skipped 2.1% of frames over the last 30 s"),
	      "PcEncoderLag what, exact");
	CHECK(reasonText(Reason::StreamDropped, -2.0, tr).fix ==
		      QStringLiteral("Start the stream again. If it keeps happening, check your connection and stream key."),
	      "StreamDropped fix, exact");
	CHECK(reasonText(Reason::SceneOverLimit, 125.0, tr).what ==
		      QStringLiteral("A holding scene is 2 min over its time limit"),
	      "SceneOverLimit what, exact");
	CHECK(reasonText(Reason::SceneOverLimit, 125.0, tr).fix ==
		      QStringLiteral("Switch to your next scene when you are ready."),
	      "SceneOverLimit fix, exact");
	// Labels: value-free, used when a log line has no value to show (cache-miss path).
	int emptyLabels = 0;
	int zeroSecs = 0;
	for (int i = 0; i <= static_cast<int>(Reason::StreamDropped); i++) {
		const QString lab = reasonLabel(static_cast<Reason>(i), tr);
		if (lab.isEmpty())
			emptyLabels++;
		if (lab.contains(QStringLiteral("0 s")) || lab.contains(QStringLiteral("%1")))
			zeroSecs++;
	}
	CHECK(emptyLabels == 0, "every Reason has a non-empty label");
	CHECK(zeroSecs == 0, "no label (the cache-miss path) contains a fake '0 s' or a place marker");
	CHECK(reasonLabel(Reason::MicMuted, tr) == QStringLiteral("Mic muted"), "MicMuted label, exact");
	CHECK(reasonLabel(Reason::StreamDropped, tr) == QStringLiteral("Stream dropped"), "StreamDropped label, exact");
	CHECK(reasonLabel(Reason::NetworkDrops, tr) == QStringLiteral("Frames dropped"), "NetworkDrops label, exact");
	// A translated string comes through the tr function.
	const TrFn up = [](const char *, const char *f) { return QString::fromUtf8(f).toUpper(); };
	CHECK(reasonText(Reason::Ok, 0, up).what == QStringLiteral("ALL GOOD"), "tr function is used");
}

static void testDuration()
{
	CHECK(durationText(0) == QStringLiteral("0 s"), "0 ms");
	CHECK(durationText(-5000) == QStringLiteral("0 s"), "negative is 0");
	CHECK(durationText(999) == QStringLiteral("0 s"), "999 ms rounds down");
	CHECK(durationText(45000) == QStringLiteral("45 s"), "45 s");
	CHECK(durationText(59000) == QStringLiteral("59 s"), "59 s");
	CHECK(durationText(60000) == QStringLiteral("1 min"), "60 s");
	CHECK(durationText(180000) == QStringLiteral("3 min"), "3 min");
	CHECK(durationText(3599000) == QStringLiteral("59 min"), "3599 s");
	CHECK(durationText(3600000) == QStringLiteral("1 h 00 min"), "3600 s");
	CHECK(durationText(3900000) == QStringLiteral("1 h 05 min"), "1 h 05 min");
	CHECK(durationText(3 * 3600000LL) == QStringLiteral("3 h 00 min"), "3 h");
}

static void testHeaderState()
{
	Snapshot s = greySnap();
	CHECK(headerState(s) == LightState::Grey, "all grey is grey");

	set(s, Light::Mic, LightState::Green);
	CHECK(headerState(s) == LightState::Green, "one green over greys is green");

	set(s, Light::Scene, LightState::Unknown, Reason::LiveUnknown);
	CHECK(headerState(s) == LightState::Unknown, "unknown beats green");

	set(s, Light::Network, LightState::Amber, Reason::NetworkDrops);
	CHECK(headerState(s) == LightState::Amber, "amber beats unknown");

	set(s, Light::Pc, LightState::Red, Reason::PcEncoderLag);
	CHECK(headerState(s) == LightState::Red, "red beats amber");

	Snapshot f = greySnap();
	set(f, Light::Network, LightState::Unknown, Reason::NetworkWindowFilling);
	CHECK(headerState(f) == LightState::Green, "a filling window alone gives green, not unknown");
	set(f, Light::Pc, LightState::Unknown, Reason::PcWindowFilling);
	CHECK(headerState(f) == LightState::Green, "both windows filling still green");
	set(f, Light::Mic, LightState::Green);
	CHECK(headerState(f) == LightState::Green, "filling windows plus a green is green");
	set(f, Light::Scene, LightState::Unknown, Reason::LiveUnknown);
	CHECK(headerState(f) == LightState::Unknown, "a real unknown still counts next to filling windows");
	Snapshot g = greySnap();
	set(g, Light::Mic, LightState::Unknown, Reason::MicNoCallback);
	CHECK(headerState(g) == LightState::Unknown, "non-filling unknown is unknown");

	Snapshot z = greySnap();
	set(z, Light::Mic, LightState::Red, Reason::MicMuted);
	z.lights[0].snoozedUntilMs = 999999;
	CHECK(headerState(z) == LightState::Red, "a snoozed red still counts as red");
	z.lights[0].acknowledged = true;
	CHECK(headerState(z) == LightState::Red, "an acknowledged red still counts as red");
}

static void testHeaderText()
{
	Snapshot s = greySnap();
	CHECK(headerText(LiveState::Off, s, tr) == QStringLiteral("Standing by"), "off");
	CHECK(headerText(LiveState::Live, s, tr) == QStringLiteral("Watching"), "live");
	CHECK(headerText(LiveState::Unknown, s, tr) == QStringLiteral("Checking..."), "unknown");
	set(s, Light::Stream, LightState::Red, Reason::StreamDropped);
	CHECK(headerText(LiveState::Off, s, tr) == QStringLiteral("Stream dropped"), "stream red wins when off air");
	CHECK(headerText(LiveState::Live, s, tr) == QStringLiteral("Stream dropped"), "stream red wins when live");
	CHECK(headerText(LiveState::Unknown, s, tr) == QStringLiteral("Stream dropped"), "stream red wins when unknown");
	Snapshot m = greySnap();
	set(m, Light::Mic, LightState::Red, Reason::MicMuted);
	CHECK(headerText(LiveState::Live, m, tr) == QStringLiteral("Watching"), "another red does not change the text");
}

static void testAttention()
{
	Snapshot s = greySnap();
	CHECK(!needsAttention(s, 1000), "no lights: no attention");
	set(s, Light::Mic, LightState::Red, Reason::MicMuted);
	CHECK(!needsAttention(s, 1000), "red without an open incident: no attention");
	s.lights[0].incidentId = 7;
	CHECK(needsAttention(s, 1000), "red with an open incident needs attention");
	s.lights[0].acknowledged = true;
	CHECK(!needsAttention(s, 1000), "acknowledged red does not");
	s.lights[0].acknowledged = false;
	s.lights[0].snoozedUntilMs = 5000;
	CHECK(!needsAttention(s, 1000), "snoozed red does not");
	CHECK(needsAttention(s, 5000), "an expired snooze does");
	set(s, Light::Mic, LightState::Amber, Reason::MicMuted);
	s.lights[0].snoozedUntilMs = 0;
	CHECK(!needsAttention(s, 1000), "amber never does");
}

static void testNames()
{
	for (int i = 0; i < kLightCount; i++) {
		CHECK(!lightName(static_cast<Light>(i), tr).isEmpty() && !lightShortName(static_cast<Light>(i), tr).isEmpty(),
		      "light names are non-empty");
	}
	CHECK(lightName(Light::Pc, tr) == QStringLiteral("PC keeping up"), "PC long name");
	CHECK(lightShortName(Light::Desktop, tr) == QStringLiteral("Desktop"), "desktop short name");
	CHECK(stateName(LightState::Red, tr) == QStringLiteral("Failing"), "red state name");
	CHECK(eventKindText(IncidentEventKind::Escalated, tr) == QStringLiteral("got worse"), "escalated text");
}

// The Network light names the destination it is talking about.
static LightResult netResult(Reason r, double value, const char *id, const char *name)
{
	LightResult x;
	x.state = LightState::Red;
	x.reason = r;
	x.value = value;
	x.incidentId = 7;
	x.net.valid = true;
	x.net.outputId = QString::fromUtf8(id);
	x.net.outputName = QString::fromUtf8(name);
	x.net.multi = true;
	return x;
}

static void testNetworkWording()
{
	LightResult d = netResult(Reason::NetworkDrops, 3.14, "t1", "Twitch");
	d.net.sessionPct = 1.4;
	d.net.mainFine = true;
	d.net.fineDestinations.push_back(QStringLiteral("YouTube"));
	CHECK(reasonTextFor(d, tr).what == QStringLiteral("Twitch is dropping 3.1% of its frames (last 30 s)."),
	      "network details name the destination and its 30 s figure");
	CHECK(!reasonTextFor(d, tr).fix.isEmpty(), "destination drops have a fix sentence");
	CHECK(networkSessionLine(d, tr) ==
		      QStringLiteral("This session on Twitch: 1.4% dropped ") + QChar(0x00B7) + QStringLiteral(" Main stream and YouTube are fine"),
	      "session line: the named output's session total, then who else is fine");
	d.net.fineDestinations.push_back(QStringLiteral("Kick"));
	CHECK(networkSessionLine(d, tr).endsWith(QStringLiteral("Main stream and YouTube and Kick are fine")) ||
		      networkSessionLine(d, tr).endsWith(QStringLiteral("Main stream, YouTube and Kick are fine")),
	      "session line: three fine outputs read as a list");
	LightResult rc = netResult(Reason::NetworkReconnecting, 0.0, "t1", "Twitch");
	CHECK(reasonTextFor(rc, tr).what == QStringLiteral("Twitch is reconnecting."), "a reconnecting destination is named");
	LightResult mainOnly = netResult(Reason::NetworkDrops, 3.14, "", "");
	mainOnly.net.multi = false;
	CHECK(reasonTextFor(mainOnly, tr).what == reasonText(Reason::NetworkDrops, 3.14, tr).what,
	      "OBS's own stream keeps the original wording");
	CHECK(networkSessionLine(mainOnly, tr).isEmpty(), "no session line when the result has no session figure yet");
	// The main stream alone still gets its session total; only the "others are fine" text needs several outputs.
	mainOnly.net.sessionPct = 2.5;
	CHECK(networkSessionLine(mainOnly, tr) == QStringLiteral("This session on Main stream: 2.5% dropped"),
	      "the main stream as the only output still shows its session total, with no 'are fine' text");
	mainOnly.net.mainFine = true;
	mainOnly.net.fineDestinations.push_back(QStringLiteral("Stale"));
	CHECK(networkSessionLine(mainOnly, tr) == QStringLiteral("This session on Main stream: 2.5% dropped"),
	      "without several outputs the 'are fine' text is left out even if fine names are present");
	LightResult noOut;
	noOut.net.valid = false;
	noOut.net.sessionPct = 3.0;
	CHECK(networkSessionLine(noOut, tr).isEmpty(), "no valid worst output: no session line");
	LightResult mainWorst = netResult(Reason::NetworkDrops, 3.14, "", "");
	mainWorst.net.sessionPct = 2.0;
	mainWorst.net.fineDestinations.push_back(QStringLiteral("Twitch"));
	CHECK(reasonTextFor(mainWorst, tr).what == reasonText(Reason::NetworkDrops, 3.14, tr).what,
	      "the main stream as the worst output keeps the original wording even with Multistream");
	CHECK(networkSessionLine(mainWorst, tr).startsWith(QStringLiteral("This session on Main stream: 2.0% dropped")) &&
		      networkSessionLine(mainWorst, tr).endsWith(QStringLiteral("Twitch is fine")),
	      "the main stream is named 'Main stream' in the session line");
	LightResult plain;
	plain.reason = Reason::MicMuted;
	plain.value = 75.0;
	CHECK(reasonTextFor(plain, tr).what == reasonText(Reason::MicMuted, 75.0, tr).what, "other lights: reasonTextFor is reasonText");
	CHECK(turnOffText(QStringLiteral("Twitch"), tr) == QStringLiteral("Turn off Twitch"), "button label");
	// Filling: the wording names who is still being measured; the header dot stays calm.
	LightResult fill;
	fill.state = LightState::Unknown;
	fill.reason = Reason::NetworkWindowFilling;
	fill.net.fillingDestinations.push_back(QStringLiteral("Twitch"));
	CHECK(reasonTextFor(fill, tr).what == QStringLiteral("Still measuring Twitch"), "filling: the destination still being measured is named");
	fill.net.mainFilling = true;
	CHECK(reasonTextFor(fill, tr).what == QStringLiteral("Still measuring Main stream, Twitch"), "filling: the main stream and a destination are both named");
	LightResult fillPlain;
	fillPlain.state = LightState::Unknown;
	fillPlain.reason = Reason::NetworkWindowFilling;
	CHECK(reasonTextFor(fillPlain, tr).what == reasonText(Reason::NetworkWindowFilling, -1.0, tr).what, "filling: no names, the original wording");
	Snapshot hs = greySnap();
	set(hs, Light::Mic, LightState::Green, Reason::Ok);
	hs.lights[static_cast<size_t>(Light::Network)] = fill;
	CHECK(headerState(hs) == LightState::Green, "filling with several outputs: the header dot does not show unknown just for filling windows");
	hs.lights[static_cast<size_t>(Light::Network)].reason = Reason::MicNoCallback;
	CHECK(headerState(hs) == LightState::Unknown, "filling: an Unknown for any other reason still shows");
}

int main()
{
	testReasons();
	testDuration();
	testHeaderState();
	testHeaderText();
	testAttention();
	testNames();
	testNetworkWording();
	std::printf("%d checks, %d failed\n", total, failures);
	return failures == 0 ? 0 : 1;
}

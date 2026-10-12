// Tests for preflight-eval.cpp: per-check rules, FrameWindow, MicTestAnalyzer, go-live decision.
// No framework: exits non-zero on failure.
#include "preflight-eval.hpp"

#include <cmath>
#include <cstdio>
#include <limits>

using namespace preflight;

static int failures = 0;
#define CHECK(cond, msg)                                                      \
	do {                                                                  \
		if (!(cond)) {                                                \
			std::printf("FAIL: %s  (line %d)\n", msg, __LINE__); \
			failures++;                                           \
		} else {                                                      \
			std::printf("ok:   %s\n", msg);                        \
		}                                                             \
	} while (0)

static constexpr qint64 GiB = 1024LL * 1024 * 1024;

static WindowStats win(bool filled, double ratio) { return WindowStats{filled, ratio}; }

// A fully healthy, set-up configuration: main stream only, nothing hidden by accident.
static Inputs good()
{
	Inputs in;
	in.setupDone = true;
	in.workflow.useMainStream = true;
	in.workflow.useMultistream = false;
	in.mic = {true, true, true, false};
	in.micTest = MicTestStatus::Passed;
	in.desktop = {true, true, true, false};
	in.scene = {true, true, true};
	in.mainStream.serviceType = "rtmp_common";
	in.mainStream.canTryToConnect = true;
	in.mainStream.hasServer = true;
	in.mainStream.hasKey = true;
	in.recording.intent = false;
	in.performance.live = false;
	in.performance.renderWindow = win(true, 0.0);
	in.performance.encoderWindow = win(true, 0.0);
	in.performance.networkWindow = win(true, 0.0);
	return in;
}

static CheckResult get(const Inputs &in, CheckId id)
{
	const auto r = evaluate(in);
	for (const auto &c : r)
		if (c.id == id)
			return c;
	return CheckResult{id, State::Hidden, "missing", "missing"};
}

static bool has(const CheckResult &r, const char *s) { return r.message.contains(QLatin1String(s)); }

int main()
{
	// --- baseline ---
	{
		const auto r = evaluate(good());
		CHECK(r.size() == 10, "one row per CheckId (10 rows)");
		CHECK(r[0].id == CheckId::Setup && r[9].id == CheckId::Custom, "rows are in enum order");
		CHECK(get(good(), CheckId::Setup).state == State::Hidden, "Setup row hidden once set up");
		CHECK(get(good(), CheckId::Recording).state == State::Hidden, "recording hidden without intent");
		CHECK(get(good(), CheckId::Multistream).state == State::Hidden, "multistream hidden when not in workflow");
		CHECK(get(good(), CheckId::Custom).state == State::Hidden, "custom hidden with no items");
		CHECK(decideGoLive(r).allowedWithoutAsking, "good setup may go live without asking");
	}

	// --- setup not done ---
	{
		Inputs in = good();
		in.setupDone = false;
		const auto r = evaluate(in);
		CHECK(r.size() == 10 && r[0].id == CheckId::Setup && r[0].state == State::Amber, "setup needed: Amber Setup row");
		bool others = true;
		for (int i = 1; i < r.size(); ++i)
			others = others && r[i].state == State::Hidden;
		CHECK(others, "setup needed: every other check Hidden");
		CHECK(r[0].message.contains(QLatin1String("setup")), "setup message says setup is needed");
	}

	// --- MicMuted ---
	{
		Inputs in = good();
		in.mic.selected = false;
		CHECK(get(in, CheckId::MicMuted).state == State::Amber, "mic not selected -> Amber");
		CHECK(has(get(in, CheckId::MicMuted), "choose your mic"), "mic not selected says choose your mic");
		in = good();
		in.mic.exists = false;
		CHECK(get(in, CheckId::MicMuted).state == State::Red, "mic missing -> Red");
		in = good();
		in.mic.isAudioSource = false;
		CHECK(get(in, CheckId::MicMuted).state == State::Red, "mic not an audio source -> Red");
		in = good();
		in.mic.muted = true;
		CHECK(get(in, CheckId::MicMuted).state == State::Red, "mic muted -> Red");
		CHECK(get(good(), CheckId::MicMuted).state == State::Green, "mic fine -> Green");
		in = good();
		in.enabled.mic = false;
		CHECK(get(in, CheckId::MicMuted).state == State::Hidden, "mic check disabled -> Hidden");
	}

	// --- MicTest ---
	{
		Inputs in = good();
		in.micTest = MicTestStatus::Passed;
		CHECK(get(in, CheckId::MicTest).state == State::Green, "mic test passed -> Green");
		in.micTest = MicTestStatus::NotRun;
		CHECK(get(in, CheckId::MicTest).state == State::Amber, "mic test not run -> Amber");
		CHECK(has(get(in, CheckId::MicTest), "press Test mic"), "not run says press Test mic");
		in.micTest = MicTestStatus::TooQuiet;
		CHECK(get(in, CheckId::MicTest).state == State::Amber, "mic test too quiet -> Amber");
		in.micTest = MicTestStatus::NoAudio;
		CHECK(get(in, CheckId::MicTest).state == State::Red, "mic test no audio -> Red");
		in.micTest = MicTestStatus::Muted;
		CHECK(get(in, CheckId::MicTest).state == State::Red, "mic test muted -> Red");
		in.enabled.mic = false;
		CHECK(get(in, CheckId::MicTest).state == State::Hidden, "mic test hidden when mic check disabled");
	}

	// --- Desktop ---
	{
		Inputs in = good();
		CHECK(get(in, CheckId::Desktop).state == State::Green, "desktop fine -> Green");
		in.desktop.muted = true;
		CHECK(get(in, CheckId::Desktop).state == State::Red, "desktop muted -> Red");
		in = good();
		in.desktop.exists = false;
		CHECK(get(in, CheckId::Desktop).state == State::Amber, "desktop missing -> Amber");
		in = good();
		in.desktop.isAudioSource = false;
		CHECK(get(in, CheckId::Desktop).state == State::Amber, "desktop not audio -> Amber");
		in = good();
		in.desktop.selected = false;
		CHECK(get(in, CheckId::Desktop).state == State::Hidden, "desktop not selected -> Hidden");
		in = good();
		in.enabled.desktop = false;
		CHECK(get(in, CheckId::Desktop).state == State::Hidden, "desktop disabled -> Hidden");
	}

	// --- Scene ---
	{
		Inputs in = good();
		CHECK(get(in, CheckId::Scene).state == State::Green, "scene live -> Green");
		in.scene.chosenIsLive = false;
		CHECK(get(in, CheckId::Scene).state == State::Red, "scene not live -> Red");
		in = good();
		in.scene.chosenExists = false;
		CHECK(get(in, CheckId::Scene).state == State::Amber, "chosen scene gone -> Amber");
		CHECK(has(get(in, CheckId::Scene), "pick your starting scene again"), "gone scene says pick again");
		in = good();
		in.scene.chosen = false;
		CHECK(get(in, CheckId::Scene).state == State::Amber, "no scene chosen -> Amber");
	}

	// --- MainStream ---
	{
		Inputs in = good();
		CHECK(get(in, CheckId::MainStream).state == State::Green, "can-try true -> Green");
		in.mainStream.canTryToConnect = false;
		CHECK(get(in, CheckId::MainStream).state == State::Red, "can-try false -> Red");
		in = good();
		in.workflow.useMainStream = false;
		CHECK(get(in, CheckId::MainStream).state == State::Hidden, "main stream hidden when not in workflow");

		in = good();
		in.mainStream.canTryToConnect.reset();
		CHECK(get(in, CheckId::MainStream).state == State::Green, "rtmp_common server+key (unknown can-try) -> Green");
		in.mainStream.serviceType = "rtmp_custom";
		in.mainStream.hasKey = false;
		CHECK(get(in, CheckId::MainStream).state == State::Amber, "rtmp_custom empty key -> Amber, not Red");
		CHECK(has(get(in, CheckId::MainStream), "check your stream key"), "empty key says check your stream key");
		in.mainStream.serviceType = "rtmp_common";
		CHECK(get(in, CheckId::MainStream).state == State::Amber, "rtmp_common empty key -> Amber, not Red");
		in.mainStream.hasServer = false;
		CHECK(get(in, CheckId::MainStream).state == State::Red, "no server -> Red");
		in = good();
		in.mainStream.canTryToConnect.reset();
		in.mainStream.serviceType = "whip_custom";
		in.mainStream.hasKey = false;
		CHECK(get(in, CheckId::MainStream).state == State::Unknown, "unknown service type -> Unknown");
		in.mainStream.hasServer = false;
		CHECK(get(in, CheckId::MainStream).state == State::Unknown, "unknown service type never Red on missing bits");
		// OBS 31.1.1: rtmp_common can_try_to_connect is false on an empty key -> only a nudge.
		in = good();
		in.mainStream.canTryToConnect = false;
		in.mainStream.hasKey = false;
		CHECK(get(in, CheckId::MainStream).state == State::Amber, "rtmp_common can-try false + server + no key -> Amber");
		CHECK(has(get(in, CheckId::MainStream), "check your stream key"), "that Amber says check your stream key");
		in.mainStream.serviceType = "rtmp_custom";
		CHECK(get(in, CheckId::MainStream).state == State::Amber, "rtmp_custom can-try false + server + no key -> Amber");
		in.mainStream.hasKey = true;
		CHECK(get(in, CheckId::MainStream).state == State::Red, "can-try false with a key present stays Red");
		in.mainStream.hasKey = false;
		in.mainStream.hasServer = false;
		CHECK(get(in, CheckId::MainStream).state == State::Red, "can-try false with no server stays Red");
		in.mainStream.hasServer = true;
		in.mainStream.serviceType = "whip_custom";
		CHECK(get(in, CheckId::MainStream).state == State::Red, "can-try false on another service type stays Red");
		in = good();
		in.mainStream.canTryToConnect = true;
		in.mainStream.hasKey = false;
		CHECK(get(in, CheckId::MainStream).state == State::Green, "can-try true beats empty key");

		// Unknown service types: canTryToConnect is NOT trusted.
		in = good();
		in.mainStream.serviceType = "some_vendor_service";
		in.mainStream.canTryToConnect = true;
		in.mainStream.hasServer = false;
		in.mainStream.hasKey = false;
		CHECK(get(in, CheckId::MainStream).state == State::Unknown, "unknown type, can-try true but no server/key -> Unknown (not Green)");
		in.mainStream.hasServer = true;
		CHECK(get(in, CheckId::MainStream).state == State::Unknown, "unknown type, can-try true, server only -> Unknown");
		in.mainStream.hasKey = true;
		CHECK(get(in, CheckId::MainStream).state == State::Green, "unknown type with server+key -> Green");
		in.mainStream.canTryToConnect = false;
		CHECK(get(in, CheckId::MainStream).state == State::Green, "unknown type server+key, can-try false ignored -> Green");
		in.mainStream.hasKey = false;
		CHECK(get(in, CheckId::MainStream).state == State::Unknown, "unknown type, can-try false, missing key -> Unknown, never Red");
		in.mainStream.canTryToConnect.reset();
		in.mainStream.serviceType = "";
		CHECK(get(in, CheckId::MainStream).state == State::Unknown, "empty service type, missing key -> Unknown");
		in = good();
		in.mainStream.serviceType = "whip_custom";
		in.mainStream.canTryToConnect = true;
		CHECK(get(in, CheckId::MainStream).state == State::Green, "whip_custom can-try true is trusted -> Green");
	}

	// --- Multistream ---
	{
		Inputs in = good();
		in.workflow.useMultistream = true;
		in.multistream.status = MultistreamStatus::Pending;
		CHECK(get(in, CheckId::Multistream).state == State::Unknown, "multistream pending -> Unknown");
		CHECK(has(get(in, CheckId::Multistream), "checking") || has(get(in, CheckId::Multistream), "Checking"),
		      "pending says checking");
		in.multistream.status = MultistreamStatus::NotFound;
		CHECK(get(in, CheckId::Multistream).state == State::Amber, "multistream not found -> Amber");
		CHECK(has(get(in, CheckId::Multistream), "Red Warden Multistream wasn't found - make sure it's installed and up to date."),
		      "not found message names Multistream and says install/update");
		CHECK(!has(get(in, CheckId::Multistream), "WebSocket"), "not found message no longer mentions WebSocket");
		in.multistream.status = MultistreamStatus::Error;
		CHECK(get(in, CheckId::Multistream).state == State::Unknown, "multistream error -> Unknown");
		in.multistream.status = MultistreamStatus::Ok;
		CHECK(get(in, CheckId::Multistream).state == State::Red, "multistream ok with 0 on -> Red");
		in.multistream.enabledNames = QStringList{"Twitch", "YouTube"};
		CHECK(get(in, CheckId::Multistream).state == State::Green, "multistream ok with names -> Green");
		CHECK(has(get(in, CheckId::Multistream), "Twitch, YouTube"), "names listed comma-separated");
		CHECK(has(get(in, CheckId::Multistream), "Multistream destinations ready: Twitch, YouTube."),
		      "green message says destinations ready (not sending)");
		in.workflow.useMultistream = false;
		CHECK(get(in, CheckId::Multistream).state == State::Hidden, "multistream hidden when not in workflow");
	}

	// --- Recording ---
	{
		const char *autoMsg =
			"OBS won't start recording for you - start it by hand or turn on Settings > Advanced > Automatically record when streaming";
		Inputs in = good();
		in.recording = {true, true, PathKind::File, true, true, 100 * GiB};
		CHECK(get(in, CheckId::Recording).state == State::Green, "recording healthy -> Green");
		in.recording.pathKind = PathKind::Url;
		CHECK(get(in, CheckId::Recording).state == State::Unknown, "URL output -> Unknown");
		in.recording.pathKind = PathKind::Unknown;
		CHECK(get(in, CheckId::Recording).state == State::Unknown, "unknown path kind -> Unknown");
		in.recording.pathKind = PathKind::File;
		in.recording.diskKnown = false;
		CHECK(get(in, CheckId::Recording).state == State::Unknown, "disk worker has not answered yet -> Unknown (not Green)");
		in.recording.folderExists = false; // would be Red if it were a real answer
		CHECK(get(in, CheckId::Recording).state == State::Unknown, "no disk answer yet: never Red from placeholder values");
		in.recording.diskKnown = true;
		in.recording.folderExists = false;
		CHECK(get(in, CheckId::Recording).state == State::Red, "folder missing -> Red");
		in.recording.folderExists = true;
		in.recording.folderWritable = false;
		CHECK(get(in, CheckId::Recording).state == State::Red, "folder not writable -> Red");
		in.recording.folderWritable = true;
		in.recording.freeBytes = 4 * GiB;
		CHECK(get(in, CheckId::Recording).state == State::Red, "4 GiB free -> Red");
		in.recording.freeBytes = 5 * GiB;
		CHECK(get(in, CheckId::Recording).state == State::Amber, "exactly 5 GiB free -> Amber");
		in.recording.freeBytes = 19 * GiB;
		CHECK(get(in, CheckId::Recording).state == State::Amber, "19 GiB free -> Amber");
		in.recording.freeBytes = 20 * GiB;
		CHECK(get(in, CheckId::Recording).state == State::Green, "exactly 20 GiB free -> Green");
		in.recording.freeBytes.reset();
		CHECK(get(in, CheckId::Recording).state == State::Unknown, "free space unreadable -> Unknown");

		in.recording.freeBytes = 100 * GiB;
		in.recording.autoRecordOn = false;
		CHECK(get(in, CheckId::Recording).state == State::Amber, "auto-record off downgrades Green to Amber");
		CHECK(has(get(in, CheckId::Recording), autoMsg), "auto-record message is the spec wording");
		in.recording.folderExists = false;
		CHECK(get(in, CheckId::Recording).state == State::Red, "Red recording stays Red with auto-record off");
		CHECK(has(get(in, CheckId::Recording), "doesn't exist") && has(get(in, CheckId::Recording), autoMsg),
		      "Red + auto-record off says both");
		in.recording.folderExists = true;
		in.recording.freeBytes = 10 * GiB;
		CHECK(get(in, CheckId::Recording).state == State::Amber && has(get(in, CheckId::Recording), "10.0 GB") &&
			      has(get(in, CheckId::Recording), autoMsg),
		      "low space + auto-record off: Amber says both");
		in.recording.intent = false;
		CHECK(get(in, CheckId::Recording).state == State::Hidden, "recording hidden when intent is off");
		CHECK(!has(get(in, CheckId::Recording), "\\"), "recording message carries no path");
	}

	// --- Performance ---
	{
		Inputs in = good();
		in.performance.live = false;
		CHECK(get(in, CheckId::Performance).state == State::Green, "render 0% -> Green");
		in.performance.renderWindow = win(true, 0.005);
		CHECK(get(in, CheckId::Performance).state == State::Green, "exactly 0.5% -> still Green");
		in.performance.renderWindow = win(true, 0.006);
		CHECK(get(in, CheckId::Performance).state == State::Amber, "0.6% render -> Amber");
		CHECK(has(get(in, CheckId::Performance), "rendering"), "render problem names rendering");
		in.performance.renderWindow = win(true, 0.02);
		CHECK(get(in, CheckId::Performance).state == State::Amber, "exactly 2% -> Amber");
		in.performance.renderWindow = win(true, 0.03);
		CHECK(get(in, CheckId::Performance).state == State::Red, "3% render -> Red");
		in.performance.renderWindow = win(false, 0.5);
		CHECK(get(in, CheckId::Performance).state == State::Unknown, "window not filled -> Unknown (even if ratio high)");
		CHECK(has(get(in, CheckId::Performance), "30 seconds"), "pre-live unfilled says watching for 30 seconds");

		in = good();
		in.performance.live = false;
		in.performance.networkWindow = win(true, 0.2);
		in.performance.encoderWindow = win(true, 0.2);
		CHECK(get(in, CheckId::Performance).state == State::Green, "network/encoder trouble ignored before live");
		in.performance.live = true;
		CHECK(get(in, CheckId::Performance).state == State::Red, "network trouble counts when live");

		in = good();
		in.performance.live = true;
		in.performance.networkWindow = win(true, 0.03);
		CHECK(get(in, CheckId::Performance).state == State::Red && has(get(in, CheckId::Performance), "network"),
		      "live network Red names network");
		in = good();
		in.performance.live = true;
		in.performance.encoderWindow = win(true, 0.01);
		CHECK(get(in, CheckId::Performance).state == State::Amber && has(get(in, CheckId::Performance), "encoding"),
		      "live encoder Amber names encoding");
		in.performance.networkWindow = win(true, 0.05);
		CHECK(get(in, CheckId::Performance).state == State::Red, "live: worst of the three wins");
		in = good();
		in.performance.live = true;
		in.performance.encoderWindow = win(false, 0.0);
		CHECK(get(in, CheckId::Performance).state == State::Unknown, "live: unfilled window -> Unknown");
		in.enabled.performance = false;
		CHECK(get(in, CheckId::Performance).state == State::Hidden, "performance disabled -> Hidden");
	}

	// --- Custom ---
	{
		Inputs in = good();
		in.custom = {{"Close Discord", true}, {"Phone on silent", true}};
		CHECK(get(in, CheckId::Custom).state == State::Green, "all items ticked -> Green");
		in.custom = {{"Close Discord", true}, {"Phone on silent", false}, {"Water", false}};
		const auto c = get(in, CheckId::Custom);
		CHECK(c.state == State::Red, "unticked items -> Red");
		CHECK(has(c, "Phone on silent, Water") && !has(c, "Close Discord"), "lists only unticked item texts");
	}

	// --- FrameWindow ---
	{
		// feed(): one sample per second from s0..s1 inclusive; cumulative counters = sec * rate.
		auto feed = [](FrameWindow &w, int s0, int s1, quint64 badPerSec, quint64 totPerSec) {
			for (int s = s0; s <= s1; ++s)
				w.add(qint64(s) * 1000, quint64(s) * badPerSec, quint64(s) * totPerSec);
		};

		FrameWindow e;
		CHECK(!e.stats(0).filled, "empty window not filled");
		e.add(0, 0, 0);
		CHECK(!e.stats(0).filled, "one sample not filled");

		FrameWindow a;
		feed(a, 0, 10, 1, 100);
		CHECK(!a.stats(10000).filled, "10 s of 30 s not filled");

		FrameWindow b;
		feed(b, 0, 30, 1, 100);
		CHECK(b.stats(30000).filled, "regular samples spanning exactly 30 s -> filled");

		FrameWindow c;
		feed(c, 0, 31, 1, 100);
		WindowStats s = c.stats(31000);
		CHECK(s.filled, "regular 1 s samples over 31 s -> filled");
		CHECK(s.ratio > 0.0099 && s.ratio < 0.0101, "ratio = delta bad / delta total (1/100)");
		CHECK(c.stats(33000).filled, "newest sample 2 s old is still fresh");
		CHECK(!c.stats(40000).filled, "long pause: newest sample 9 s old -> not filled");
		CHECK(!c.stats(120000).filled, "very stale window -> not filled");

		FrameWindow sp;
		sp.add(0, 0, 0);
		sp.add(30000, 3, 300);
		sp.add(70000, 7, 700);
		CHECK(!sp.stats(70000).filled, "samples at 30 s and 70 s only (sparse) -> not filled");

		FrameWindow gap;
		feed(gap, 0, 10, 1, 100);
		feed(gap, 20, 40, 1, 100);
		CHECK(!gap.stats(40000).filled, "10 s gap inside the window -> not filled");

		FrameWindow roll;
		for (int sec = 0; sec <= 70; ++sec)
			roll.add(qint64(sec) * 1000, quint64(sec < 10 ? sec : 10) * 5, quint64(sec) * 100);
		s = roll.stats(70000);
		CHECK(s.filled && s.ratio == 0.0, "old trouble rolls out of the window");

		FrameWindow rst;
		feed(rst, 0, 40, 5, 100);
		rst.add(41000, 0, 10); // counters went DOWN: OBS reset
		CHECK(!rst.stats(41000).filled, "counter reset discards history (not filled)");
		for (int sec = 42; sec <= 72; ++sec)
			rst.add(qint64(sec) * 1000, 0, quint64(10 + (sec - 41) * 100));
		s = rst.stats(72000);
		CHECK(s.filled && s.ratio == 0.0, "after reset, ratio comes from post-reset samples only");
		rst.reset();
		CHECK(!rst.stats(72000).filled, "reset() empties the window");

		FrameWindow z;
		feed(z, 0, 31, 0, 0);
		z.add(31000, 0, 0);
		CHECK(!z.stats(31000).filled, "delta total 0 -> not filled (Unknown), never ratio 0");

		FrameWindow cs(1000);
		cs.add(0, 0, 0);
		cs.add(1000, 10, 100);
		CHECK(cs.stats(1000).filled && cs.stats(1000).ratio > 0.099 && cs.stats(1000).ratio < 0.101,
		      "configurable span (1000 ms)");

		FrameWindow t2;
		t2.add(0, 5, 5);
		t2.add(100, 4, 10); // bad dropped, total rose: reset too
		CHECK(!t2.stats(100).filled && t2.stats(100).ratio == 0.0, "either counter decreasing resets");
	}

	// --- MicTestAnalyzer ---
	{
		const float inf = std::numeric_limits<float>::infinity();
		MicTestAnalyzer a;
		a.start(1000);
		a.add(1200, -55.0f);
		a.add(1800, -52.0f);
		a.add(2500, -12.0f);
		a.add(3500, -20.0f);
		CHECK(a.finish(true) == MicTestStatus::Passed, "speech well above floor -> Passed");

		a.start(0);
		a.add(100, -50.0f);
		a.add(2000, -45.0f);
		a.add(3000, -40.0f);
		CHECK(a.finish(true) == MicTestStatus::TooQuiet, "peaks below -30 dBFS -> TooQuiet");

		a.start(0);
		CHECK(a.finish(true) == MicTestStatus::NoAudio, "no samples at all -> NoAudio");

		a.start(0);
		a.add(200, -35.0f);
		a.add(900, -36.0f);
		a.add(3000, -25.0f);
		CHECK(a.finish(true) == MicTestStatus::TooQuiet, "loud room: floor -35, peak -25 -> TooQuiet");

		a.start(0);
		a.add(200, -35.0f);
		a.add(3000, -20.0f);
		CHECK(a.finish(true) == MicTestStatus::Passed, "exactly floor + 15 dB passes");

		a.start(0);
		a.add(2000, -50.0f);
		a.add(3000, -12.0f);
		CHECK(a.finish(true) == MicTestStatus::Passed,
		      "two samples: p10 floor = quietest (-50), max -12 -> Passed");

		a.start(0);
		for (int ms = 1000; ms <= 5000; ms += 500)
			a.add(ms, -25.0f);
		CHECK(a.finish(true) == MicTestStatus::TooQuiet,
		      "constant -25 dBFS from 1 s on -> TooQuiet (not Passed via a -60 default)");

		a.start(0);
		a.add(200, -10.0f);
		CHECK(a.finish(true) == MicTestStatus::TooQuiet, "single sample (floor == max) -> TooQuiet");

		a.start(0);
		a.add(2000, -30.0f);
		a.add(3000, -45.0f);
		CHECK(a.finish(true) == MicTestStatus::TooQuiet, "peak exactly -30 dBFS is not above -30");

		a.start(0);
		a.add(100, -60.0f);
		a.add(6000, -5.0f); // after the 5 s window
		CHECK(a.finish(true) == MicTestStatus::TooQuiet, "samples after the window are ignored");

		a.start(0);
		a.add(2000, -10.0f);
		a.start(10000);
		CHECK(a.finish(true) == MicTestStatus::NoAudio, "start() clears the previous test");

		a.start(0);
		for (int ms = 100; ms <= 5000; ms += 500)
			a.add(ms, -90.0f);
		CHECK(a.finish(true) == MicTestStatus::NoAudio, "constant -90 dBFS (digital silence) -> NoAudio");

		a.start(0);
		a.add(100, -inf);
		a.add(2000, -inf);
		CHECK(a.finish(true) == MicTestStatus::NoAudio, "-inf samples (silence) -> NoAudio");

		a.start(0);
		a.add(200, -50.0f);
		a.add(3000, -12.0f);
		CHECK(a.finish(false) == MicTestStatus::NoAudio, "source inactive -> NoAudio even with samples");
		CHECK(a.finish(true) == MicTestStatus::Passed, "same samples with active source -> Passed");

		// Talks from the very first instant, with natural pauses between phrases (20 % of samples
		// at -55). The old first-second-max floor read -12 here and failed; the p10 floor is -55.
		a.start(0);
		for (int i = 0; i < 50; ++i)
			a.add(qint64(i) * 100, (i % 5 == 4) ? -55.0f : -12.0f);
		CHECK(a.finish(true) == MicTestStatus::Passed, "talks from the first instant with natural pauses -> Passed");
		{
			const auto s = a.summary();
			CHECK(s.count == 50, "summary: sample count");
			CHECK(s.floorP10Db == -55.0f, "summary: p10 floor is the pause level");
			CHECK(s.maxDb == -12.0f, "summary: max");
			CHECK(s.oldFloorDb.has_value() && *s.oldFloorDb == -12.0f, "summary: old first-second floor is speech");
		}

		// Constant -25 dBFS for the whole 5 s: floor == max, so never 15 dB above the floor.
		a.start(0);
		for (int ms = 0; ms <= 5000; ms += 100)
			a.add(ms, -25.0f);
		CHECK(a.finish(true) == MicTestStatus::TooQuiet, "constant -25 dBFS for 5 s -> TooQuiet");

		// Known limitation, accepted: continuous speech with NO gaps at all (every sample at -12) has
		// floor == max, so it reads TooQuiet. A normal speaker or a noise-gated mic has gaps (breaths,
		// pauses between words), which is what the p10 floor relies on.
		a.start(0);
		for (int ms = 0; ms <= 5000; ms += 100)
			a.add(ms, -12.0f);
		CHECK(a.finish(true) == MicTestStatus::TooQuiet, "continuous speech at -12 with no gaps -> TooQuiet (accepted)");

		// Loud room: floor -35 all along, speech only reaches -25 (10 dB above floor, needs 15).
		a.start(0);
		for (int ms = 0; ms <= 5000; ms += 100)
			a.add(ms, ms == 3000 ? -25.0f : -35.0f);
		CHECK(a.finish(true) == MicTestStatus::TooQuiet, "loud room floor -35 with peaks -25 -> TooQuiet");

		a.start(0);
		CHECK(!a.summary().oldFloorDb.has_value() && a.summary().count == 0, "summary: empty -> no old floor, count 0");
		a.add(2000, -40.0f);
		CHECK(!a.summary().oldFloorDb.has_value(), "summary: no early samples -> old floor absent");
	}
	// --- decideGoLive ---
	{
		QVector<CheckResult> r = {
			{CheckId::MicMuted, State::Green, "Mic unmuted", ""},
			{CheckId::MicTest, State::Amber, "Mic test", ""},
			{CheckId::Scene, State::Red, "Starting scene", ""},
			{CheckId::MainStream, State::Unknown, "Main stream", ""},
			{CheckId::Desktop, State::Hidden, "Desktop audio", ""},
			{CheckId::Custom, State::Red, "Your checklist", ""},
		};
		auto d = decideGoLive(r);
		CHECK(!d.allowedWithoutAsking, "any Red -> must ask");
		CHECK(d.reds == QStringList({"Starting scene", "Your checklist"}), "reds list Red titles in order");
		CHECK(d.cautions == QStringList({"Mic test", "Main stream"}), "cautions list Amber + Unknown, not Hidden/Green");
		r = {{CheckId::MicMuted, State::Green, "Mic unmuted", ""},
		     {CheckId::MicTest, State::Amber, "Mic test", ""},
		     {CheckId::Desktop, State::Hidden, "Desktop audio", ""}};
		d = decideGoLive(r);
		CHECK(d.allowedWithoutAsking && d.reds.isEmpty(), "Amber only -> allowed without asking");
		CHECK(d.cautions == QStringList({"Mic test"}), "Amber only: caution listed");
		d = decideGoLive(QVector<CheckResult>());
		CHECK(d.allowedWithoutAsking && d.reds.isEmpty() && d.cautions.isEmpty(), "empty result set allowed");
	}

	// --- Mic test numbers: Loudest (max peak) and Typical (power mean of magnitude above the silence floor) ---
	{
		const float inf = std::numeric_limits<float>::infinity();
		auto approx = [](float a, float b) { return std::fabs(a - b) < 0.005f; };

		// powerMeanDb
		CHECK(approx(powerMeanDb({-20.0f, -20.0f, -20.0f}, kMicDigitalSilenceDb), -20.0f), "power mean of equal levels is that level");
		CHECK(approx(powerMeanDb({-20.0f, -30.0f}, kMicDigitalSilenceDb), -22.596f),
		      "power mean of -20 and -30 dB = 10*log10((0.01+0.001)/2) = -22.60 (a dB average would say -25)");
		CHECK(approx(powerMeanDb({-10.0f, -40.0f}, kMicDigitalSilenceDb), -13.006f), "power mean of -10 and -40 dB = -13.01");
		CHECK(approx(powerMeanDb({-10.0f, -90.0f, -200.0f, -inf}, kMicDigitalSilenceDb), -10.0f),
		      "levels at or below the floor (-90, -200, -inf) are left out, not averaged in");
		CHECK(powerMeanDb({}, kMicDigitalSilenceDb) == -inf, "empty input -> -inf");
		CHECK(powerMeanDb({-95.0f, -100.0f, -90.0f}, kMicDigitalSilenceDb) == -inf, "everything at or below the floor -> -inf");
		CHECK(approx(powerMeanDb({std::numeric_limits<float>::quiet_NaN(), -20.0f}, kMicDigitalSilenceDb), -20.0f), "NaN is skipped");

		// formatDb
		CHECK(formatDb(-8.24f) == "-8.2" && formatDb(-8.26f) == "-8.3", "one decimal, rounded (-8.24 -> -8.2, -8.26 -> -8.3)");
		CHECK(formatDb(-8.25f) == "-8.3" && formatDb(8.25f) == "8.3", "a tie rounds away from zero (exactly representable .25)");
		CHECK(formatDb(0.0f) == "0.0" && formatDb(-0.04f) == "0.0", "zero prints 0.0, never -0.0");
		CHECK(formatDb(-21.5f) == "-21.5" && formatDb(-100.0f) == "-100.0", "plain values keep one decimal");
		CHECK(formatDb(-inf) == "-inf" && formatDb(-200.0f) == "-inf" && formatDb(-250.0f) == "-inf", "-inf and the -200 dB clamp print -inf");
		CHECK(formatDb(std::numeric_limits<float>::quiet_NaN()) == "-inf", "NaN prints -inf");
		CHECK(formatDb(-199.9f) == "-199.9", "just above the clamp is still a number");

		// MicTestAnalyzer::numbers
		MicTestAnalyzer a;
		a.start(0);
		CHECK(!a.numbers().valid, "no samples: nothing to show");

		a.start(1000);
		a.add(1200, -8.24f, -21.5f);
		MicNumbers n = a.numbers();
		CHECK(n.valid && formatDb(n.loudestDb) == "-8.2" && formatDb(n.typicalDb) == "-21.5", "single sample: Loudest -8.2, Typical -21.5");

		a.start(0);
		a.add(100, -55.0f, -60.0f);
		a.add(900, -52.0f, -58.0f);
		a.add(2500, -12.0f, -20.0f);
		a.add(3500, -20.0f, -30.0f);
		n = a.numbers();
		CHECK(n.valid && approx(n.loudestDb, -12.0f), "mixed: Loudest is the max peak (-12)");
		// powers: 1e-6, 1.585e-6, 1e-2, 1e-3 -> mean 2.7507e-3 -> -25.606 dB
		CHECK(approx(n.typicalDb, -25.606f), "mixed: Typical is the power mean of the four magnitudes (-25.61)");

		a.start(0); // silence: the collector clamps -inf to -200
		a.add(100, -200.0f, -200.0f);
		a.add(600, -200.0f, -200.0f);
		n = a.numbers();
		CHECK(n.valid && n.loudestDb == -200.0f && n.typicalDb == -inf, "silence: valid, Loudest -200 (prints -inf), Typical -inf");
		CHECK(formatDb(n.loudestDb) == "-inf" && formatDb(n.typicalDb) == "-inf", "silence prints -inf for both");
		CHECK(a.finish(true) == MicTestStatus::NoAudio, "silence still decides NoAudio, as before");

		a.start(0); // heard something, but every magnitude below the silence floor
		a.add(100, -60.0f, -95.0f);
		a.add(600, -50.0f, -100.0f);
		n = a.numbers();
		CHECK(n.valid && approx(n.loudestDb, -50.0f) && n.typicalDb == -inf, "all magnitudes below the floor: Loudest real, Typical -inf");

		a.start(0); // magnitude-less add() (the old two-argument call): Loudest works, Typical has nothing
		a.add(100, -30.0f);
		a.add(600, -25.0f);
		n = a.numbers();
		CHECK(n.valid && approx(n.loudestDb, -25.0f) && n.typicalDb == -inf, "peaks only: Loudest -25, Typical -inf");

		a.start(1000); // a sine of -20 dBFS peak: every callback reads peak -20.0 and magnitude -20 - 3.0103 = -23.0103
		for (int i = 0; i < 100; ++i)
			a.add(1000 + 40 * i, -20.0f, -23.0103f);
		a.add(1000 + 5100, -5.0f, -5.0f); // outside the 5 s window: ignored
		a.add(500, -4.0f, -4.0f);          // before the start: ignored
		n = a.numbers();
		CHECK(formatDb(n.loudestDb) == "-20.0" && formatDb(n.typicalDb) == "-23.0", "a -20 dBFS sine: Loudest -20.0, Typical -23.0 (peak - 3.01 dB)");
		CHECK(a.finish(true) == MicTestStatus::TooQuiet, "...and that steady tone still decides as before (no speech above a floor)");

		// Pass / too-quiet is untouched by the magnitudes: the same peaks decide the same way with or without them.
		MicTestAnalyzer b, c;
		b.start(0);
		c.start(0);
		const qint64 ts[] = {200, 1800, 2500, 3500};
		const float ps[] = {-55.0f, -52.0f, -12.0f, -20.0f};
		for (int i = 0; i < 4; ++i) {
			b.add(ts[i], ps[i]);
			c.add(ts[i], ps[i], -40.0f);
		}
		CHECK(b.finish(true) == c.finish(true) && c.finish(true) == MicTestStatus::Passed, "magnitudes do not change Passed");
	}

	std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}

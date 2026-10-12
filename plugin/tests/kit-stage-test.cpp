// Tests for kit/kit-stage: the pure stage machine of the one-dock UI. Every time is passed in, so each
// case replays exactly. No framework: exits non-zero on failure.
#include "kit/kit-stage.hpp"

#include <cstdio>
#include <vector>

using namespace kitstage;
using S = Stage;
using L = Live;

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

static Tools tools(bool pre, bool in, bool post)
{
	Tools t;
	t.preflight = pre;
	t.inflight = in;
	t.postflight = post;
	return t;
}
using V = std::vector<Stage>;

static void testStartAndVisible()
{
	Machine m(Tools{});
	CHECK(m.stage() == S::PreFlight, "1: default start is Pre-Flight");
	CHECK(!m.peeking() && !m.sessionActive() && !m.ended() && m.resetAllowed(), "1: clean initial state");
	CHECK((m.visibleStages() == V{S::PreFlight, S::InFlight}), "1: default strip is Pre, In (Post hidden)");
	Machine a(tools(true, true, true));
	CHECK((a.visibleStages() == V{S::PreFlight, S::InFlight, S::PostFlight}), "1: three stages in order");
	Machine b(tools(false, true, true));
	CHECK(b.stage() == S::InFlight, "1: Pre-Flight off -> starts on In-Flight");
	Machine c(tools(false, false, true));
	CHECK(c.stage() == S::PostFlight, "1: only Post-Flight on -> starts there");
	CHECK((c.visibleStages() == V{S::PostFlight}), "1: strip holds only enabled stages");
}

static void testLiveStartsSession()
{
	Machine m(Tools{});
	m.onLive(L::Live, 1, 1000);
	CHECK(m.sessionActive() && m.stage() == S::InFlight, "2: Live -> session active, In-Flight shown");
	CHECK(!m.resetAllowed(), "2: reset not allowed while live");
	CHECK(m.sessionsStarted() == 1, "2: one start counted");

	Options o;
	o.autoSwitchOnLive = false;
	Machine n(Tools{}, o);
	n.onLive(L::Live, 1, 1000);
	CHECK(n.sessionActive() && n.stage() == S::PreFlight, "2: autoSwitch off -> session tracked, stage unchanged");
	n.onLive(L::Off, 1, 2000);
	n.onTick(2000 + 10000);
	CHECK(!n.sessionActive() && n.ended(), "2: autoSwitch off session still ends normally");
	CHECK(n.stage() == S::PreFlight, "2: autoSwitch off, no Post-Flight -> stage still unchanged");
}

static void testUnknown()
{
	Machine m(Tools{});
	m.onLive(L::Unknown, 0, 100);
	CHECK(!m.sessionActive() && m.stage() == S::PreFlight, "3: Unknown never starts a session");
	m.onLive(L::Live, 5, 200);
	m.onLive(L::Unknown, 5, 300);
	CHECK(m.sessionActive() && m.stage() == S::InFlight, "3: Unknown keeps session and stage");
	m.onTick(1000000);
	CHECK(m.sessionActive(), "3: Unknown never ends a session, however long");
	CHECK(!m.resetAllowed(), "3: reset refused on Unknown mid-session");

	// Unknown cancels a running off timer
	Machine k(Tools{});
	k.onLive(L::Live, 1, 0);
	k.onLive(L::Off, 1, 1000);
	CHECK(k.offTimerRunning(), "3: Off starts the timer");
	k.onLive(L::Unknown, 1, 5000);
	CHECK(!k.offTimerRunning(), "3: Unknown cancels the off timer");
	k.onTick(1000 + 10000);
	CHECK(k.sessionActive(), "3: timer cancelled by Unknown does not fire at the old deadline");
	k.onLive(L::Off, 1, 20000);
	k.onTick(20000 + 9999);
	CHECK(k.sessionActive(), "3: fresh Off after Unknown must run its own full window");
	k.onTick(20000 + 10000);
	CHECK(!k.sessionActive(), "3: ...and then ends");
}

static void testOffConfirmation()
{
	Tools t = tools(true, true, true);
	Machine m(t);
	m.onLive(L::Live, 1, 0);
	m.onLive(L::Off, 1, 1000);
	CHECK(m.sessionActive() && m.offTimerRunning(), "4: Off starts off timer, session still active");
	CHECK(!m.resetAllowed(), "4: reset refused during off-confirmation");
	m.onTick(1000 + 9999);
	CHECK(m.sessionActive(), "4: 1 ms short of offConfirmMs: still active");
	m.onTick(1000 + 10000);
	CHECK(!m.sessionActive() && m.ended(), "4: exactly offConfirmMs: session ended");
	CHECK(m.stage() == S::PostFlight, "4: end -> Post-Flight when enabled");
	CHECK(m.resetAllowed(), "4: reset allowed after end");

	// repeated Off does not restart the timer; the end can come through onLive itself
	Machine r(t);
	r.onLive(L::Live, 1, 0);
	r.onLive(L::Off, 1, 100);
	r.onLive(L::Off, 1, 5000);
	r.onLive(L::Off, 1, 10099);
	CHECK(r.sessionActive(), "4: repeated Off keeps the first timestamp (not yet)");
	r.onLive(L::Off, 1, 10100);
	CHECK(!r.sessionActive() && r.ended(), "4: end checked inside onLive too");

	// Live again before the deadline cancels
	Machine c(t);
	c.onLive(L::Live, 1, 0);
	c.onLive(L::Off, 1, 1000);
	c.onLive(L::Live, 1, 10999);
	CHECK(c.sessionActive() && !c.offTimerRunning(), "4: Live before confirmation cancels the timer");
	c.onTick(50000);
	CHECK(c.sessionActive() && !c.ended(), "4: cancelled session does not end later");
	c.onLive(L::Off, 1, 60000);
	c.onTick(60000 + 9999);
	CHECK(c.sessionActive(), "4: new streak counts from its own start");
	c.onTick(60000 + 10000);
	CHECK(c.ended(), "4: new streak ends at its own deadline");

	// stays on In-Flight when Post-Flight is off
	Machine p(Tools{});
	p.onLive(L::Live, 1, 0);
	p.onLive(L::Off, 1, 10);
	p.onTick(10 + 10000);
	CHECK(p.ended() && p.stage() == S::InFlight, "4: no Post-Flight -> stays on In-Flight");

	// Off with no session does nothing
	Machine q(t);
	q.onLive(L::Off, 0, 0);
	q.onTick(100000);
	CHECK(!q.sessionActive() && !q.ended() && !q.offTimerRunning() && q.stage() == S::PreFlight, "4: Off with no session is inert");

	// custom confirm window
	Options o;
	o.offConfirmMs = 500;
	Machine w(Tools{}, o);
	w.onLive(L::Live, 1, 0);
	w.onLive(L::Off, 1, 0);
	w.onTick(499);
	CHECK(w.sessionActive(), "4: custom window 499 < 500 holds");
	w.onTick(500);
	CHECK(!w.sessionActive(), "4: custom window 500 ends");

	// setOptions applies to the next check
	Machine so(Tools{});
	so.onLive(L::Live, 1, 0);
	so.onLive(L::Off, 1, 0);
	Options o2;
	o2.offConfirmMs = 200;
	so.setOptions(o2);
	so.onTick(200);
	CHECK(so.ended(), "4: setOptions shortens a running window");

	// peek cleared on end
	Machine pk(t);
	pk.onLive(L::Live, 1, 0);
	pk.peek(S::PreFlight);
	CHECK(pk.peeking() && pk.stage() == S::PreFlight, "8: peek shows the other stage mid-session");
	pk.onLive(L::Off, 1, 10);
	pk.onTick(10010);
	CHECK(!pk.peeking() && pk.stage() == S::PostFlight, "8: confirmed end applies on top of a peek and clears it");
}

static void testSessionIds()
{
	Machine m(Tools{});
	m.onLive(L::Live, 1, 0);
	m.onLive(L::Live, 2, 100);
	CHECK(m.sessionsStarted() == 1 && m.sessionActive(), "5: new sessionId while active is the same session");
	m.onLive(L::Live, 2, 200);
	CHECK(m.sessionsStarted() == 1, "5: repeated Live is not a restart");
	m.peek(S::PreFlight);
	m.onLive(L::Live, 3, 300);
	CHECK(m.peeking(), "5: a continuing Live does not clear a peek");
	m.onLive(L::Off, 3, 400);
	m.onTick(400 + 10000);
	CHECK(m.ended() && !m.sessionActive(), "5: session ended");
	m.onLive(L::Live, 4, 20000);
	CHECK(m.sessionsStarted() == 2 && m.sessionActive() && !m.ended(), "5: Live after end starts a new session, clears ended");
	CHECK(m.stage() == S::InFlight, "5: new session switches to In-Flight again");
	// the same id after an end still counts as a new session
	m.onLive(L::Off, 4, 20100);
	m.onTick(30100);
	m.onLive(L::Live, 4, 40000);
	CHECK(m.sessionsStarted() == 3, "5: even a repeated id after an end is a new session");
}

static void testInflightDisabled()
{
	Machine m(tools(true, false, true));
	m.onLive(L::Live, 1, 0);
	CHECK(m.stage() == S::PreFlight, "6: In-Flight off -> Live does not move the stage");
	CHECK(m.sessionActive() && !m.resetAllowed(), "6: ...but the session is tracked for reset");
	m.peek(S::PostFlight);
	m.onLive(L::Off, 1, 10);
	m.onTick(10010);
	CHECK(m.ended() && !m.sessionActive(), "6: end tracked");
	CHECK(m.stage() == S::PostFlight && m.peeking(), "6: end does not touch stage or peek when In-Flight is off");
	CHECK(m.reset(10020) && m.stage() == S::PreFlight, "6: reset works afterwards");
}

static void testDone()
{
	Machine m(Tools{});
	CHECK(m.done(0) && m.stage() == S::InFlight, "7: Done on Pre-Flight, no session -> In-Flight");
	CHECK(!m.done(1) && m.stage() == S::InFlight, "7: Done again (not on Pre-Flight) refused");

	Machine l(Tools{});
	Options o;
	o.autoSwitchOnLive = false;
	Machine n(Tools{}, o);
	n.onLive(L::Live, 1, 0);
	CHECK(!n.done(1) && n.stage() == S::PreFlight, "7: Done refused while a session is active");

	Machine d(tools(true, false, false));
	CHECK(!d.done(0) && d.stage() == S::PreFlight, "7: Done refused when In-Flight is disabled");

	Machine e(tools(false, true, false));
	CHECK(!e.done(0) && e.stage() == S::InFlight, "7: Done refused when not on Pre-Flight");

	// during off-confirmation the session is still active
	Machine f(Tools{}, o);
	f.onLive(L::Live, 1, 0);
	f.onLive(L::Off, 1, 10);
	CHECK(!f.done(20), "7: Done refused during off-confirmation");
	CHECK(f.done(10 + 10000) && f.ended(), "7: Done itself runs the off check, session ended -> allowed");
}

static void testPeek()
{
	Machine m(tools(true, true, true));
	m.peek(S::PostFlight);
	CHECK(m.stage() == S::PostFlight && m.peeking(), "8: peek shows the clicked stage");
	m.peek(S::PreFlight);
	CHECK(m.stage() == S::PreFlight && !m.peeking(), "8: peeking the underlying stage is not a peek");
	m.peek(S::InFlight);
	CHECK(m.peeking(), "8: peek In-Flight");
	CHECK(m.reset(0) && !m.peeking() && m.stage() == S::PreFlight, "8: reset clears the peek");
	m.peek(S::PostFlight);
	m.onLive(L::Live, 1, 0);
	CHECK(!m.peeking() && m.stage() == S::InFlight, "8: live start applies on top of a peek and clears it");

	Machine h(Tools{});
	h.peek(S::PostFlight);
	CHECK(!h.peeking() && h.stage() == S::PreFlight, "8: peek of a hidden stage ignored");
	h.peek(S::InFlight);
	h.onLive(L::Unknown, 0, 5);
	CHECK(h.peeking() && h.stage() == S::InFlight, "8: Unknown leaves a peek alone");
}

static void testReset()
{
	Machine m(tools(true, true, true));
	m.onLive(L::Live, 1, 0);
	CHECK(!m.reset(1) && m.stage() == S::InFlight && m.sessionActive(), "9: reset refused while Live, no change");
	m.onLive(L::Unknown, 1, 2);
	CHECK(!m.reset(3), "9: reset refused on Unknown mid-session");
	m.onLive(L::Off, 1, 10);
	CHECK(!m.reset(10 + 9999), "9: reset refused 1 ms before confirmation");
	CHECK(m.reset(10 + 10000), "9: reset at exactly the deadline ends then resets");
	CHECK(!m.ended() && m.stage() == S::PreFlight && !m.sessionActive() && !m.peeking(), "9: reset -> first stage, ended cleared");

	Machine n(tools(true, true, true));
	n.onLive(L::Live, 1, 0);
	n.onLive(L::Off, 1, 1);
	n.onTick(1 + 10000);
	CHECK(n.ended() && n.stage() == S::PostFlight, "9: ended card state");
	CHECK(n.reset(20000) && !n.ended() && n.stage() == S::PreFlight, "9: reset after end clears ended");
	Machine o(tools(false, true, true));
	o.done(0);
	o.peek(S::PostFlight);
	CHECK(o.reset(0) && o.stage() == S::InFlight, "9: reset -> first VISIBLE stage");
}

static void testClockBackwards()
{
	Machine m(Tools{});
	m.onLive(L::Live, 1, 100000);
	m.onLive(L::Off, 1, 101000);
	m.onTick(50000); // clock jumped back
	CHECK(m.sessionActive() && m.offTimerRunning(), "10: backwards clock does not fire the timer");
	m.onTick(50000 + 9999);
	CHECK(m.sessionActive(), "10: timer restarted from the new time (not yet)");
	m.onTick(50000 + 10000);
	CHECK(m.ended(), "10: ...ends a full window after the restart");

	// backwards inside onLive Off
	Machine k(Tools{});
	k.onLive(L::Live, 1, 100000);
	k.onLive(L::Off, 1, 100500);
	k.onLive(L::Off, 1, 10);
	CHECK(k.sessionActive(), "10: Off with an earlier clock restarts instead of firing");
	k.onTick(10 + 10000);
	CHECK(k.ended(), "10: ...and then ends");

	// no timer running: backwards clock is harmless; huge negatives do not overflow
	Machine q(Tools{});
	q.onLive(L::Live, 1, 5000);
	q.onTick(-9000000000000000000LL);
	CHECK(q.sessionActive(), "10: extreme negative clock with no timer is harmless");
	q.onLive(L::Off, 1, -9000000000000000000LL);
	q.onTick(9000000000000000000LL);
	CHECK(q.ended(), "10: large forward jump ends without overflow trouble");
}

static void testSetTools()
{
	Machine m(tools(true, true, true));
	m.done(0);
	m.setTools(tools(true, false, true));
	CHECK(m.stage() == S::PreFlight, "11: removing the current stage -> first visible");
	Machine p(tools(true, true, true));
	p.onLive(L::Live, 1, 0);
	p.onLive(L::Off, 1, 1);
	p.onTick(1 + 10000);
	CHECK(p.stage() == S::PostFlight, "11: setup: on Post-Flight");
	p.setTools(tools(true, true, false));
	CHECK(p.stage() == S::PreFlight, "11: Post-Flight removed -> first visible");
	p.setTools(tools(true, true, true));
	CHECK(p.stage() == S::PreFlight, "11: re-enabling does not move the stage back");

	Machine q(tools(true, true, true));
	q.peek(S::PostFlight);
	q.setTools(tools(true, true, false));
	CHECK(!q.peeking() && q.stage() == S::PreFlight, "11: a peek at a removed stage is dropped");
	q.peek(S::InFlight);
	q.setTools(tools(true, false, false));
	CHECK(q.stage() == S::PreFlight && !q.peeking(), "11: peek removed with In-Flight");

	Machine r(tools(true, true, false));
	r.done(0);
	r.setTools(tools(false, true, false));
	CHECK(r.stage() == S::InFlight, "11: Pre-Flight off while on In-Flight keeps it");
	r.setTools(tools(false, false, true));
	CHECK(r.stage() == S::PostFlight, "11: only Post-Flight left -> there");

	// nothing enabled: defined state
	Machine z(tools(true, true, true));
	z.onLive(L::Live, 1, 0);
	z.setTools(tools(false, false, false));
	CHECK(z.visibleStages().empty(), "11: no tools -> empty strip");
	CHECK(z.stage() == S::PreFlight && !z.peeking(), "11: no tools -> stage reads Pre-Flight, no peek");
	z.peek(S::InFlight);
	CHECK(!z.peeking() && z.stage() == S::PreFlight, "11: no tools -> peeks ignored");
	CHECK(z.sessionActive() && !z.resetAllowed(), "11: no tools -> live tracking continues");
	z.onLive(L::Off, 1, 5);
	z.onTick(5 + 10000);
	CHECK(z.ended() && z.stage() == S::PreFlight, "11: no tools -> session end tracked, stage fixed");
	CHECK(!z.done(0), "11: no tools -> Done refused");
	z.setTools(tools(false, true, false));
	CHECK(z.stage() == S::InFlight, "11: tool re-enabled -> first visible stage");
}

int main()
{
	testStartAndVisible();
	testLiveStartsSession();
	testUnknown();
	testOffConfirmation();
	testSessionIds();
	testInflightDisabled();
	testDone();
	testPeek();
	testReset();
	testClockBackwards();
	testSetTools();
	std::printf("%s (%d checks, %d failure%s)\n", failures ? "FAILED" : "ALL PASSED", total, failures,
		    failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}

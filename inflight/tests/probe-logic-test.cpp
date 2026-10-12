// Tests for inflight-probe-logic.cpp: reading Multistream's destination states, the live-state combination and the
// stop-code mapping. Every time is passed in. No framework: exits non-zero on failure.
#include "inflight-probe-logic.hpp"

#include <QStringList>

#include <cstdio>
#include <initializer_list>

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

static bool stateIsLive(const char *state)
{
	return anyDestinationLive(QStringList{QString::fromLatin1(state)});
}

static bool anyLive(std::initializer_list<const char *> states)
{
	QStringList l;
	for (const char *s : states)
		l << QString::fromLatin1(s);
	return anyDestinationLive(l);
}

static void testStates()
{
	CHECK(stateIsLive("live"), "state live");
	CHECK(stateIsLive("LIVE"), "state LIVE");
	CHECK(stateIsLive("Live"), "state Live");
	CHECK(stateIsLive("reconnecting"), "state reconnecting counts as live");
	CHECK(stateIsLive("Reconnecting"), "state Reconnecting counts as live");
	CHECK(!stateIsLive("connecting"), "state connecting is not live");
	CHECK(!stateIsLive("ready"), "state ready is not live");
	CHECK(!stateIsLive("off"), "state off is not live");
	CHECK(!stateIsLive("error"), "state error is not live");
	CHECK(!stateIsLive("waiting"), "state waiting is not live");
	CHECK(!stateIsLive("paused"), "state paused is not live");
	CHECK(!stateIsLive("retrying"), "state retrying is not live");
	CHECK(!stateIsLive("stopping"), "state stopping is not live");
	CHECK(!stateIsLive(""), "empty state is not live");
	CHECK(!stateIsLive("live "), "state with trailing space is not live");
	CHECK(!anyDestinationLive(QStringList()), "no destinations is not live");
	CHECK(anyLive({"off", "live"}), "one live among several is live");
	CHECK(!anyLive({"off", "error", "ready"}), "none live among several");
	CHECK(anyLive({"ready", "reconnecting", "off"}), "one reconnecting among several is live");
}

static MsReply reply(MsStatus s, bool live, qint64 at)
{
	MsReply r;
	r.status = s;
	r.anyLive = live;
	r.atMs = at;
	return r;
}

static void testCombine()
{
	const qint64 now = 100000;
	CHECK(kMsFreshMs == 10000, "freshness constant is 10000 ms");
	// Main stream active wins over everything.
	CHECK(combineLive(true, reply(MsStatus::Error, false, now), now) == LiveState::Live, "main active overrides Error");
	CHECK(combineLive(true, reply(MsStatus::Pending, false, 0), now) == LiveState::Live, "main active overrides Pending");
	CHECK(combineLive(true, reply(MsStatus::NotFound, false, now), now) == LiveState::Live, "main active overrides NotFound");
	CHECK(combineLive(true, reply(MsStatus::Ok, false, now), now) == LiveState::Live, "main active overrides Ok not live");
	CHECK(combineLive(true, reply(MsStatus::Ok, false, now - 99999), now) == LiveState::Live, "main active overrides stale");
	// Main stream not active.
	CHECK(combineLive(false, reply(MsStatus::NotFound, false, now), now) == LiveState::Off, "NotFound is Off");
	CHECK(combineLive(false, reply(MsStatus::NotFound, true, now - 50000), now) == LiveState::Off, "NotFound is Off even when old");
	CHECK(combineLive(false, reply(MsStatus::Pending, false, 0), now) == LiveState::Unknown, "Pending is Unknown");
	CHECK(combineLive(false, reply(MsStatus::Error, false, now), now) == LiveState::Unknown, "Error is Unknown");
	CHECK(combineLive(false, reply(MsStatus::Error, true, now), now) == LiveState::Unknown, "Error is Unknown even with anyLive");
	CHECK(combineLive(false, reply(MsStatus::Ok, true, now), now) == LiveState::Live, "fresh Ok anyLive is Live");
	CHECK(combineLive(false, reply(MsStatus::Ok, false, now), now) == LiveState::Off, "fresh Ok not live is Off");
	CHECK(combineLive(false, reply(MsStatus::Ok, true, now - 10000), now) == LiveState::Live, "exactly 10000 ms old is fresh (live)");
	CHECK(combineLive(false, reply(MsStatus::Ok, false, now - 10000), now) == LiveState::Off, "exactly 10000 ms old is fresh (off)");
	CHECK(combineLive(false, reply(MsStatus::Ok, true, now - 10001), now) == LiveState::Unknown, "10001 ms old is Unknown (live)");
	CHECK(combineLive(false, reply(MsStatus::Ok, false, now - 10001), now) == LiveState::Unknown, "10001 ms old is Unknown (off)");
	CHECK(combineLive(false, reply(MsStatus::Ok, true, now + 1), now) == LiveState::Unknown, "reply 1 ms in the future is Unknown");
	CHECK(combineLive(false, reply(MsStatus::Ok, false, now + 5000), now) == LiveState::Unknown, "reply 5 s in the future is Unknown");
	CHECK(combineLive(false, reply(MsStatus::Ok, true, now), now) == LiveState::Live, "reply at exactly now is fresh");
}

static void testStopCodes()
{
	CHECK(kOutputSuccess == 0, "success code is 0");
	CHECK(stopKindForCode(0) == StreamEventKind::StoppedOk, "code 0 is StoppedOk");
	CHECK(stopKindForCode(-1) == StreamEventKind::StoppedError, "code -1 (bad path) is StoppedError");
	CHECK(stopKindForCode(-2) == StreamEventKind::StoppedError, "code -2 (connect failed) is StoppedError");
	CHECK(stopKindForCode(-3) == StreamEventKind::StoppedError, "code -3 (invalid stream) is StoppedError");
	CHECK(stopKindForCode(-4) == StreamEventKind::StoppedError, "code -4 (error) is StoppedError");
	CHECK(stopKindForCode(1) == StreamEventKind::StoppedError, "any non-zero code is StoppedError");
}

int main()
{
	testStates();
	testCombine();
	testStopCodes();
	std::printf("%s (%d checks, %d failure%s)\n", failures ? "FAILED" : "ALL PASSED", total, failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}

// Tests for inflight-settings-check: every clamp, the amber/red ordering, holding-scene cleanup.
// No framework: exits non-zero on failure.
#include "inflight-settings-check.hpp"

#include <cmath>
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

static bool near(double a, double b)
{
	return std::fabs(a - b) < 1e-9;
}

static HoldingScene hs(const char *uuid, int limit, const char *label)
{
	HoldingScene h;
	h.sceneUuid = QString::fromLatin1(uuid);
	h.limitSeconds = limit;
	h.label = QString::fromLatin1(label);
	return h;
}

static void testDefaults()
{
	Settings s;
	CHECK(normalizeSettings(s).isEmpty(), "default settings: nothing changed");
	s.holdingScenes.push_back(hs("a", 300, "Starting Soon"));
	s.holdingScenes.push_back(hs("b", 600, "BRB"));
	CHECK(normalizeSettings(s).isEmpty(), "valid holding scenes: nothing changed");
	CHECK(s.holdingScenes.size() == 2, "valid holding scenes kept");
}

static void testInts()
{
	{
		Settings s;
		s.micMutedAmberS = 4;
		CHECK(normalizeSettings(s).size() == 1 && s.micMutedAmberS == 5, "mic amber below 5 -> 5");
	}
	{
		Settings s;
		s.micMutedAmberS = 5;
		CHECK(normalizeSettings(s).isEmpty() && s.micMutedAmberS == 5, "mic amber 5 is fine");
	}
	{
		Settings s;
		s.micMutedAmberS = 600;
		s.micMutedRedS = 700;
		CHECK(normalizeSettings(s).isEmpty() && s.micMutedAmberS == 600, "mic amber 600 is fine");
	}
	{
		Settings s;
		s.micMutedAmberS = 601;
		s.micMutedRedS = 700;
		const QStringList r = normalizeSettings(s);
		CHECK(r.size() == 1 && s.micMutedAmberS == 600, "mic amber above 600 -> 600");
	}
	{
		Settings s;
		s.micMutedAmberS = 20;
		s.micMutedRedS = 25;
		CHECK(normalizeSettings(s).isEmpty(), "mic red = amber + 5 is fine");
	}
	{
		Settings s;
		s.micMutedAmberS = 20;
		s.micMutedRedS = 24;
		CHECK(normalizeSettings(s).size() == 1 && s.micMutedRedS == 25, "mic red below amber+5 -> amber+5");
	}
	{
		Settings s;
		s.micMutedAmberS = 40;
		s.micMutedRedS = 30;
		CHECK(normalizeSettings(s).size() == 1 && s.micMutedRedS == 45, "mic red below amber is pushed up");
	}
	{
		Settings s;
		s.micMutedRedS = 1800;
		CHECK(normalizeSettings(s).isEmpty(), "mic red 1800 is fine");
		s.micMutedRedS = 1801;
		CHECK(normalizeSettings(s).size() == 1 && s.micMutedRedS == 1800, "mic red above 1800 -> 1800");
	}
	{
		Settings s;
		s.micMutedAmberS = 1;
		s.micMutedRedS = 2;
		CHECK(normalizeSettings(s).size() == 2 && s.micMutedAmberS == 5 && s.micMutedRedS == 10,
		      "both too low: amber 5, red 10");
	}
	{
		Settings s;
		s.heardYouSilenceS = 29;
		CHECK(normalizeSettings(s).size() == 1 && s.heardYouSilenceS == 30, "heard-you seconds 29 -> 30");
		s.heardYouSilenceS = 30;
		CHECK(normalizeSettings(s).isEmpty(), "heard-you seconds 30 fine");
		s.heardYouSilenceS = 1800;
		CHECK(normalizeSettings(s).isEmpty(), "heard-you seconds 1800 fine");
		s.heardYouSilenceS = 1801;
		CHECK(normalizeSettings(s).size() == 1 && s.heardYouSilenceS == 1800, "heard-you seconds 1801 -> 1800");
	}
	{
		Settings s;
		s.heardYouDbfs = -81;
		CHECK(normalizeSettings(s).size() == 1 && s.heardYouDbfs == -80, "dBFS -81 -> -80");
		s.heardYouDbfs = -80;
		CHECK(normalizeSettings(s).isEmpty(), "dBFS -80 fine");
		s.heardYouDbfs = -20;
		CHECK(normalizeSettings(s).isEmpty(), "dBFS -20 fine");
		s.heardYouDbfs = -19;
		CHECK(normalizeSettings(s).size() == 1 && s.heardYouDbfs == -20, "dBFS -19 -> -20");
	}
	{
		Settings s;
		s.desktopSilenceS = 29;
		CHECK(normalizeSettings(s).size() == 1 && s.desktopSilenceS == 30, "desktop silence 29 -> 30");
		s.desktopSilenceS = 1801;
		CHECK(normalizeSettings(s).size() == 1 && s.desktopSilenceS == 1800, "desktop silence 1801 -> 1800");
		s.desktopSilenceS = 1800;
		CHECK(normalizeSettings(s).isEmpty(), "desktop silence 1800 fine");
	}
	{
		Settings s;
		s.sceneRedExtraS = 59;
		CHECK(normalizeSettings(s).size() == 1 && s.sceneRedExtraS == 60, "scene extra 59 -> 60");
		s.sceneRedExtraS = 3601;
		CHECK(normalizeSettings(s).size() == 1 && s.sceneRedExtraS == 3600, "scene extra 3601 -> 3600");
		s.sceneRedExtraS = 3600;
		CHECK(normalizeSettings(s).isEmpty(), "scene extra 3600 fine");
	}
	{
		Settings s;
		s.recordStartGraceS = 9;
		CHECK(normalizeSettings(s).size() == 1 && s.recordStartGraceS == 10, "record grace 9 -> 10");
		s.recordStartGraceS = 601;
		CHECK(normalizeSettings(s).size() == 1 && s.recordStartGraceS == 600, "record grace 601 -> 600");
		s.recordStartGraceS = 600;
		CHECK(normalizeSettings(s).isEmpty(), "record grace 600 fine");
	}
}

static void testDrops()
{
	{
		Settings s;
		s.dropAmberPct = 0.05;
		CHECK(normalizeSettings(s).size() == 1 && near(s.dropAmberPct, 0.1), "drop amber 0.05 -> 0.1");
	}
	{
		Settings s;
		s.dropAmberPct = 0.1;
		s.dropRedPct = 0.2;
		CHECK(normalizeSettings(s).isEmpty(), "drop amber 0.1 / red 0.2 fine");
	}
	{
		Settings s;
		s.dropAmberPct = 10.0;
		s.dropRedPct = 10.1;
		CHECK(normalizeSettings(s).isEmpty(), "drop amber 10 / red 10.1 fine");
	}
	{
		Settings s;
		s.dropAmberPct = 12.0;
		s.dropRedPct = 30.0;
		CHECK(normalizeSettings(s).size() == 1 && near(s.dropAmberPct, 10.0), "drop amber 12 -> 10");
	}
	{
		Settings s;
		s.dropAmberPct = 1.0;
		s.dropRedPct = 1.0;
		CHECK(normalizeSettings(s).size() == 1 && near(s.dropRedPct, 1.1), "drop red equal to amber -> amber+0.1");
	}
	{
		Settings s;
		s.dropAmberPct = 3.0;
		s.dropRedPct = 2.0;
		CHECK(normalizeSettings(s).size() == 1 && near(s.dropRedPct, 3.1), "drop red below amber is pushed up");
	}
	{
		Settings s;
		s.dropRedPct = 50.0;
		CHECK(normalizeSettings(s).isEmpty(), "drop red 50 fine");
		s.dropRedPct = 50.5;
		CHECK(normalizeSettings(s).size() == 1 && near(s.dropRedPct, 50.0), "drop red 50.5 -> 50");
	}
}

static void testScenes()
{
	{
		Settings s;
		s.holdingScenes.push_back(hs("", 300, "x"));
		s.holdingScenes.push_back(hs("a", 300, "A"));
		CHECK(normalizeSettings(s).size() == 1 && s.holdingScenes.size() == 1 &&
			      s.holdingScenes[0].sceneUuid == QStringLiteral("a"),
		      "empty uuid row dropped");
	}
	{
		Settings s;
		s.holdingScenes.push_back(hs("a", 300, "First"));
		s.holdingScenes.push_back(hs("b", 300, "Other"));
		s.holdingScenes.push_back(hs("a", 900, "Second"));
		const QStringList r = normalizeSettings(s);
		CHECK(r.size() == 1 && s.holdingScenes.size() == 2, "duplicate uuid dropped");
		CHECK(s.holdingScenes[0].label == QStringLiteral("First") && s.holdingScenes[0].limitSeconds == 300,
		      "the first duplicate is the one kept");
	}
	{
		Settings s;
		s.holdingScenes.push_back(hs("a", 59, "A"));
		CHECK(normalizeSettings(s).size() == 1 && s.holdingScenes[0].limitSeconds == 60, "limit 59 -> 60");
		s.holdingScenes[0].limitSeconds = 60;
		CHECK(normalizeSettings(s).isEmpty(), "limit 60 fine");
		s.holdingScenes[0].limitSeconds = 7200;
		CHECK(normalizeSettings(s).isEmpty(), "limit 7200 fine");
		s.holdingScenes[0].limitSeconds = 7201;
		CHECK(normalizeSettings(s).size() == 1 && s.holdingScenes[0].limitSeconds == 7200, "limit 7201 -> 7200");
	}
	{
		Settings s;
		const QString forty(40, QLatin1Char('x'));
		HoldingScene h;
		h.sceneUuid = QStringLiteral("a");
		h.limitSeconds = 300;
		h.label = forty;
		s.holdingScenes.push_back(h);
		CHECK(normalizeSettings(s).isEmpty() && s.holdingScenes[0].label == forty, "label of 40 chars fine");
		s.holdingScenes[0].label = forty + QStringLiteral("yy");
		CHECK(normalizeSettings(s).size() == 1 && s.holdingScenes[0].label == forty, "label of 42 chars cut to 40");
	}
	{
		Settings s;
		s.holdingScenes.push_back(hs("a", 300, "  BRB  "));
		CHECK(normalizeSettings(s).isEmpty() && s.holdingScenes[0].label == QStringLiteral("BRB"),
		      "surrounding spaces on a label are trimmed silently");
	}
}

static void testWording()
{
	Settings s;
	s.recordStartGraceS = 5;
	const QStringList r = normalizeSettings(s);
	CHECK(r.size() == 1 && !r[0].contains(QStringLiteral("%")) && r[0].contains(QStringLiteral("5")) &&
		      r[0].contains(QStringLiteral("10")),
	      "sentence names the old and new value and has no stray place marker");
	Settings d;
	d.dropAmberPct = 1.0;
	d.dropRedPct = 0.5;
	const QStringList q = normalizeSettings(d);
	CHECK(q.size() == 1 && q[0].contains(QStringLiteral("1.1")), "drop red sentence shows the new value");
	const TrFn up = [](const char *, const char *f) { return QString::fromUtf8(f).toUpper(); };
	Settings u;
	u.recordStartGraceS = 5;
	const QStringList r2 = normalizeSettings(u, up);
	CHECK(r2.size() == 1 && r2[0].contains(QStringLiteral("RECORDING MUST START")), "tr function is used");
}

int main()
{
	testDefaults();
	testInts();
	testDrops();
	testScenes();
	testWording();
	std::printf("%d checks, %d failed\n", total, failures);
	return failures == 0 ? 0 : 1;
}

// Pre-Flight evaluation logic. Pure: Qt Core only, no libobs, no widgets.
// OBS-facing code gathers an Inputs struct; evaluate() turns it into one row per check.
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

#include <deque>
#include <limits>
#include <optional>
#include <vector>

namespace preflight {

// ---- Thresholds. These are STARTING VALUES to be calibrated live against real mics/rooms. ----
constexpr qint64 kMicTestWindowMs = 5000;   // whole test window
constexpr qint64 kMicOldFloorWindowMs = 1000; // legacy first-second floor: logged for comparison only, never decides
constexpr double kMicFloorPercentile = 0.10;  // noise floor = 10th percentile of all peaks in the window
constexpr float kMicMinPeakDb = -30.0f;     // speech must peak above this (dBFS)
constexpr float kMicAboveFloorDb = 15.0f;   // ... and at least this far above the floor
constexpr qint64 kFrameWindowMs = 30000;    // default rolling window for frame stats
constexpr qint64 kFrameStaleMs = 5000;       // max age of newest sample / max gap between samples
constexpr qint64 kFrameAnchorSlackMs = 2000; // anchor may be at most span + this old
constexpr float kMicDigitalSilenceDb = -90.0f; // every sample at or below this = no audio
constexpr double kPerfAmberRatio = 0.005;   // > 0.5 %
constexpr double kPerfRedRatio = 0.02;      // > 2 %
constexpr qint64 kFreeRedBytes = 5LL * 1024 * 1024 * 1024;
constexpr qint64 kFreeAmberBytes = 20LL * 1024 * 1024 * 1024;

enum class State { Green, Amber, Red, Unknown, Hidden };

enum class CheckId {
	Setup,
	MicMuted,
	MicTest,
	Desktop,
	Scene,
	MainStream,
	Multistream,
	Recording,
	Performance,
	Custom
};

struct CheckResult {
	CheckId id;
	State state;
	QString title;
	QString message;
};

enum class MicTestStatus { NotRun, Passed, TooQuiet, NoAudio, Muted };
enum class MultistreamStatus { Pending, NotFound, Ok, Error };
enum class PathKind { File, Url, Unknown };

struct WindowStats {
	bool filled = false;
	double ratio = 0.0;
};

struct Inputs {
	bool setupDone = false;

	struct Workflow {
		bool useMainStream = true;
		bool useMultistream = false;
	} workflow;

	struct Enabled {
		bool mic = true; // covers mic muted + mic test
		bool desktop = true;
		bool scene = true;
		bool mainStream = true;
		bool multistream = true;
		bool recording = true;
		bool performance = true;
		bool custom = true;
	} enabled;

	struct Audio {
		bool selected = false;
		bool exists = false;
		bool isAudioSource = false;
		bool muted = false;
	};
	Audio mic;
	MicTestStatus micTest = MicTestStatus::NotRun;
	Audio desktop;

	struct Scene {
		bool chosen = false;
		bool chosenExists = false;
		bool chosenIsLive = false;
	} scene;

	struct MainStream {
		QString serviceType;
		std::optional<bool> canTryToConnect;
		bool hasServer = false;
		bool hasKey = false;
	} mainStream;

	struct Multistream {
		MultistreamStatus status = MultistreamStatus::Pending;
		QStringList enabledNames;
	} multistream;

	struct Recording {
		bool intent = false;
		bool autoRecordOn = false;
		PathKind pathKind = PathKind::File;
		bool folderExists = false;
		bool folderWritable = false;
		std::optional<qint64> freeBytes;
		// False until the background disk worker has answered for the current folder.
		bool diskKnown = true;
	} recording;

	struct Performance {
		bool live = false;
		WindowStats renderWindow;
		WindowStats encoderWindow;
		WindowStats networkWindow;
	} performance;

	struct CustomItem {
		QString text;
		bool ticked = false;
	};
	QVector<CustomItem> custom;
};

// One row per CheckId, in enum order (Setup first).
QVector<CheckResult> evaluate(const Inputs &in);

// Rolling window over cumulative (bad,total) counters sampled over time.
class FrameWindow {
public:
	explicit FrameWindow(qint64 spanMs = kFrameWindowMs) : span_(spanMs) {}
	void add(qint64 tMs, quint64 bad, quint64 total);
	// "Filled" needs: newest sample within kFrameStaleMs of nowMs, an anchor between span and
	// span + kFrameAnchorSlackMs old, no gap over kFrameStaleMs between kept samples, and a nonzero
	// total-frame delta. Otherwise not filled (the dock shows Unknown).
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

// The two numbers shown under the Mic test row (dBFS). Loudest = the highest peak heard; Typical = the power mean of
// the magnitude (the volmeter's RMS) over the samples above kMicDigitalSilenceDb, the same floor the NoAudio rule
// uses. -inf means "nothing above it" (silent). valid = false: no samples, nothing to show.
struct MicNumbers {
	bool valid = false;
	float loudestDb = -std::numeric_limits<float>::infinity();
	float typicalDb = -std::numeric_limits<float>::infinity();
	bool operator==(const MicNumbers &o) const
	{
		return valid == o.valid && loudestDb == o.loudestDb && typicalDb == o.typicalDb;
	}
	bool operator!=(const MicNumbers &o) const { return !(*this == o); }
};

// Pure. 10*log10 of the mean POWER (10^(dB/10)) of the values strictly above floorDb; -inf when there is none
// (empty input, all at or below the floor, NaN skipped). A mean of dB values would be wrong: a loud second and
// four quiet ones average to something no meter ever showed.
float powerMeanDb(const std::vector<float> &levelsDb, float floorDb);
// Pure. One decimal, half away from zero, never "-0.0"; "-inf" for -inf and for anything at or below the
// collector's -200 dB clamp (a digital-silence reading is not a number worth printing).
QString formatDb(float db);

// Decides the 5-second mic test from peak-level samples. "Muted" is decided by the caller.
class MicTestAnalyzer {
public:
	void start(qint64 tMs);
	void add(qint64 tMs, float peakDb);
	// With the magnitude of the same callback (what Typical is made of). add(t, peak) alone contributes no magnitude.
	void add(qint64 tMs, float peakDb, float magnitudeDb);
	// Loudest = max peak, Typical = power mean of the magnitudes above kMicDigitalSilenceDb. Not valid without samples.
	MicNumbers numbers() const;
	// sourceActive: whether the mic source was active (producing audio) during the test.
	// Floor = 10th percentile (nearest-rank) of every peak in the window; PASS iff max > kMicMinPeakDb
	// and max >= floor + kMicAboveFloorDb.
	MicTestStatus finish(bool sourceActive) const;

	// Numbers behind the last finish(), for logging. Valid after samples were added; count == 0 means
	// the floor/max fields are meaningless.
	struct Summary {
		int count = 0;
		float floorP10Db = 0.0f;
		float maxDb = 0.0f;
		// Legacy rule (max peak in the first kMicOldFloorWindowMs). Comparison logging only.
		std::optional<float> oldFloorDb;
	};
	Summary summary() const;

private:
	qint64 start_ = 0;
	std::vector<float> peaks_; // every in-window peak, in arrival order
	std::vector<float> mags_;  // every in-window magnitude (add() with a magnitude only), in arrival order
	bool haveOld_ = false;
	float oldFloorDb_ = 0.0f;
};

struct GoLiveDecision {
	bool allowedWithoutAsking = true;
	QStringList reds;
	QStringList cautions;
};

GoLiveDecision decideGoLive(const QVector<CheckResult> &results);

} // namespace preflight

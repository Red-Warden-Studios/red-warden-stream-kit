#include "preflight-eval.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace preflight {

namespace {

CheckResult make(CheckId id, State st, const char *title, const QString &msg = QString())
{
	return CheckResult{id, st, QString::fromLatin1(title), msg};
}

CheckResult hidden(CheckId id, const char *title)
{
	return make(id, State::Hidden, title);
}

QString gb(qint64 bytes)
{
	return QString::number(double(bytes) / (1024.0 * 1024.0 * 1024.0), 'f', 1);
}

QString pct(double ratio)
{
	return QString::number(ratio * 100.0, 'f', 1);
}

CheckResult evalMicMuted(const Inputs &in)
{
	const char *t = "Mic unmuted";
	if (!in.enabled.mic)
		return hidden(CheckId::MicMuted, t);
	const auto &m = in.mic;
	if (!m.selected)
		return make(CheckId::MicMuted, State::Amber, t, "You haven't chosen your mic yet - choose your mic in Pre-Flight settings.");
	if (!m.exists || !m.isAudioSource)
		return make(CheckId::MicMuted, State::Red, t, "Your mic is missing or isn't an audio source - pick it again in Pre-Flight settings.");
	if (m.muted)
		return make(CheckId::MicMuted, State::Red, t, "Your mic is muted - click the speaker icon in OBS's Audio Mixer to unmute it.");
	return make(CheckId::MicMuted, State::Green, t, "Your mic is on.");
}

CheckResult evalMicTest(const Inputs &in)
{
	const char *t = "Mic test";
	if (!in.enabled.mic)
		return hidden(CheckId::MicTest, t);
	switch (in.micTest) {
	case MicTestStatus::Passed:
		return make(CheckId::MicTest, State::Green, t, "Your mic test passed - I heard you clearly.");
	case MicTestStatus::NotRun:
		return make(CheckId::MicTest, State::Amber, t, "Not tested yet - press Test mic and say something.");
	case MicTestStatus::TooQuiet:
		return make(CheckId::MicTest, State::Amber, t, "Your mic is very quiet - move closer or turn up its gain, then press Test mic again.");
	case MicTestStatus::NoAudio:
		return make(CheckId::MicTest, State::Red, t, "No sound is coming from your mic - check it is plugged in and picked in OBS, then test again.");
	case MicTestStatus::Muted:
		return make(CheckId::MicTest, State::Red, t, "Your mic was muted during the test - unmute it and press Test mic again.");
	}
	return make(CheckId::MicTest, State::Unknown, t, "Mic test state unknown.");
}

CheckResult evalDesktop(const Inputs &in)
{
	const char *t = "Desktop audio";
	const auto &d = in.desktop;
	if (!in.enabled.desktop || !d.selected)
		return hidden(CheckId::Desktop, t);
	if (!d.exists || !d.isAudioSource)
		return make(CheckId::Desktop, State::Amber, t, "Your desktop/game audio source is missing - pick it again in Pre-Flight settings.");
	if (d.muted)
		return make(CheckId::Desktop, State::Red, t, "Your desktop/game audio is muted - unmute it in OBS's Audio Mixer.");
	return make(CheckId::Desktop, State::Green, t, "Your desktop/game audio is on.");
}

CheckResult evalScene(const Inputs &in)
{
	const char *t = "Starting scene";
	if (!in.enabled.scene)
		return hidden(CheckId::Scene, t);
	const auto &s = in.scene;
	if (!s.chosen)
		return make(CheckId::Scene, State::Amber, t, "You haven't chosen a starting scene - pick one in Pre-Flight settings.");
	if (!s.chosenExists)
		return make(CheckId::Scene, State::Amber, t, "Your starting scene is gone - pick your starting scene again in Pre-Flight settings.");
	if (!s.chosenIsLive)
		return make(CheckId::Scene, State::Red, t, "A different scene is live - switch to your starting scene before you go live.");
	return make(CheckId::Scene, State::Green, t, "Your starting scene is live.");
}

CheckResult evalMainStream(const Inputs &in)
{
	const char *t = "Main stream";
	if (!in.enabled.mainStream || !in.workflow.useMainStream)
		return hidden(CheckId::MainStream, t);
	const auto &m = in.mainStream;
	const bool rtmp = m.serviceType == QLatin1String("rtmp_common") || m.serviceType == QLatin1String("rtmp_custom");
	// obs_service_can_try_to_connect() is only meaningful for service types whose implementation we know
	// checks server+key. Third-party/unknown service types can return true unconditionally, so for those
	// we only go Green on evidence we can see ourselves (server and key present); otherwise Unknown.
	const bool trustCan = rtmp || m.serviceType == QLatin1String("whip_custom");
	if (!trustCan) {
		if (m.hasServer && m.hasKey)
			return make(CheckId::MainStream, State::Green, t, "Your stream server and key are set.");
		return make(CheckId::MainStream, State::Unknown, t, "I can't check this kind of stream service - make sure it is set up in Settings > Stream.");
	}
	if (m.canTryToConnect.has_value()) {
		if (*m.canTryToConnect)
			return make(CheckId::MainStream, State::Green, t, "Your stream service is set up.");
		// OBS 31 reports "can't connect" for an empty key on RTMP services; that is a nudge, not a Red.
		if (rtmp && m.hasServer && !m.hasKey)
			return make(CheckId::MainStream, State::Amber, t, "No stream key found - check your stream key in Settings > Stream (some servers carry it in the address).");
		return make(CheckId::MainStream, State::Red, t, "Your stream isn't fully set up yet - check Settings > Stream.");
	}
	if (m.serviceType == QLatin1String("rtmp_common") || m.serviceType == QLatin1String("rtmp_custom")) {
		if (!m.hasServer)
			return make(CheckId::MainStream, State::Red, t, "No stream server is set - choose one in Settings > Stream.");
		if (!m.hasKey)
			return make(CheckId::MainStream, State::Amber, t, "No stream key found - check your stream key in Settings > Stream (some servers carry it in the address).");
		return make(CheckId::MainStream, State::Green, t, "Your stream server and key are set.");
	}
	return make(CheckId::MainStream, State::Unknown, t, "I can't check this kind of stream service - make sure it is set up in Settings > Stream.");
}

CheckResult evalMultistream(const Inputs &in)
{
	const char *t = "Multistream";
	if (!in.enabled.multistream || !in.workflow.useMultistream)
		return hidden(CheckId::Multistream, t);
	const auto &m = in.multistream;
	switch (m.status) {
	case MultistreamStatus::Pending:
		return make(CheckId::Multistream, State::Unknown, t, "Checking your Multistream destinations...");
	case MultistreamStatus::NotFound:
		return make(CheckId::Multistream, State::Amber, t, "Red Warden Multistream wasn't found - make sure it's installed and up to date.");
	case MultistreamStatus::Error:
		return make(CheckId::Multistream, State::Unknown, t, "Couldn't get an answer from Multistream - it will be checked again in a few seconds.");
	case MultistreamStatus::Ok:
		if (m.enabledNames.isEmpty())
			return make(CheckId::Multistream, State::Red, t, "No Multistream destinations are turned on - turn at least one on in the Multistream dock.");
		return make(CheckId::Multistream, State::Green, t,
			    QStringLiteral("Multistream destinations ready: %1.").arg(m.enabledNames.join(QStringLiteral(", "))));
	}
	return make(CheckId::Multistream, State::Unknown, t, "Multistream state unknown.");
}

CheckResult evalRecording(const Inputs &in)
{
	const char *t = "Recording";
	const auto &r = in.recording;
	if (!in.enabled.recording || !r.intent)
		return hidden(CheckId::Recording, t);
	if (r.pathKind != PathKind::File)
		return make(CheckId::Recording, State::Unknown, t, "Your recording goes to a web address or somewhere I can't check - make sure it is set up in Settings > Output.");

	if (!r.diskKnown)
		return make(CheckId::Recording, State::Unknown, t, "Checking your recording folder...");

	const QString autoNote = QStringLiteral(
		"OBS won't start recording for you - start it by hand or turn on Settings > Advanced > Automatically record when streaming");

	State st = State::Green;
	QString msg;
	if (!r.folderExists) {
		st = State::Red;
		msg = QStringLiteral("Your recording folder doesn't exist - choose a folder in Settings > Output.");
	} else if (!r.folderWritable) {
		st = State::Red;
		msg = QStringLiteral("OBS can't write to your recording folder - choose another folder in Settings > Output.");
	} else if (!r.freeBytes.has_value()) {
		return make(CheckId::Recording, State::Unknown, t, "I couldn't read how much space is left for recordings.");
	} else if (*r.freeBytes < kFreeRedBytes) {
		st = State::Red;
		msg = QStringLiteral("Only %1 GB free for recordings - free up space before you go live.").arg(gb(*r.freeBytes));
	} else if (*r.freeBytes < kFreeAmberBytes) {
		st = State::Amber;
		msg = QStringLiteral("Only %1 GB free for recordings - free up space if you plan a long stream.").arg(gb(*r.freeBytes));
	} else {
		msg = QStringLiteral("Your recording folder is ready (%1 GB free).").arg(gb(*r.freeBytes));
	}

	if (!r.autoRecordOn) {
		if (st == State::Green) {
			st = State::Amber;
			msg = autoNote + QLatin1Char('.');
		} else {
			msg += QStringLiteral(" Also: ") + autoNote + QLatin1Char('.');
		}
	}
	return CheckResult{CheckId::Recording, st, QString::fromLatin1(t), msg};
}

struct PerfItem {
	State state;
	QString msg;
};

int rank(State s)
{
	switch (s) {
	case State::Red:
		return 3;
	case State::Amber:
		return 2;
	case State::Unknown:
		return 1;
	default:
		return 0;
	}
}

PerfItem perfOne(const WindowStats &w, const char *kind, const QString &redFix, const QString &amberFix, bool live)
{
	if (!w.filled) {
		return {State::Unknown, live ? QStringLiteral("Still watching %1 - give it 30 seconds.").arg(QLatin1String(kind))
					     : QStringLiteral("Watching OBS for 30 seconds to see if it keeps up.")};
	}
	if (w.ratio > kPerfRedRatio)
		return {State::Red, QStringLiteral("%1 problem: %2% of frames lost - %3").arg(QLatin1String(kind), pct(w.ratio), redFix)};
	if (w.ratio > kPerfAmberRatio)
		return {State::Amber, QStringLiteral("Slight %1 trouble: %2% of frames lost - %3").arg(QLatin1String(kind), pct(w.ratio), amberFix)};
	return {State::Green, QStringLiteral("OBS is keeping up (%1 looks fine).").arg(QLatin1String(kind))};
}

CheckResult evalPerformance(const Inputs &in)
{
	const char *t = "OBS keeping up";
	if (!in.enabled.performance)
		return hidden(CheckId::Performance, t);
	const auto &p = in.performance;

	QVector<PerfItem> items;
	items.push_back(perfOne(p.renderWindow, "rendering",
				QStringLiteral("close other programs or lower your canvas size or FPS."),
				QStringLiteral("close other programs if it gets worse."), p.live));
	if (p.live) {
		items.push_back(perfOne(p.encoderWindow, "encoding",
					QStringLiteral("lower your bitrate or choose a lighter encoder preset."),
					QStringLiteral("lower your bitrate if it gets worse."), true));
		items.push_back(perfOne(p.networkWindow, "network",
					QStringLiteral("your internet can't keep up - lower your bitrate or use a wired connection."),
					QStringLiteral("check your internet connection."), true));
	}

	int best = 0;
	for (int i = 1; i < items.size(); ++i)
		if (rank(items[i].state) > rank(items[best].state))
			best = i;
	return CheckResult{CheckId::Performance, items[best].state, QString::fromLatin1(t), items[best].msg};
}

CheckResult evalCustom(const Inputs &in)
{
	const char *t = "Your checklist";
	if (!in.enabled.custom || in.custom.isEmpty())
		return hidden(CheckId::Custom, t);
	QStringList open;
	for (const auto &i : in.custom)
		if (!i.ticked)
			open << i.text;
	if (open.isEmpty())
		return make(CheckId::Custom, State::Green, t, QStringLiteral("All %1 of your checklist items are ticked.").arg(in.custom.size()));
	return make(CheckId::Custom, State::Red, t, QStringLiteral("Not ticked yet: %1 - tick them off before you go live.").arg(open.join(QStringLiteral(", "))));
}

} // namespace

QVector<CheckResult> evaluate(const Inputs &in)
{
	QVector<CheckResult> out;
	if (!in.setupDone) {
		out.push_back(make(CheckId::Setup, State::Amber, "Setup",
				   "Pre-Flight needs setup first - open Pre-Flight settings and tell it how you stream."));
		out.push_back(hidden(CheckId::MicMuted, "Mic unmuted"));
		out.push_back(hidden(CheckId::MicTest, "Mic test"));
		out.push_back(hidden(CheckId::Desktop, "Desktop audio"));
		out.push_back(hidden(CheckId::Scene, "Starting scene"));
		out.push_back(hidden(CheckId::MainStream, "Main stream"));
		out.push_back(hidden(CheckId::Multistream, "Multistream"));
		out.push_back(hidden(CheckId::Recording, "Recording"));
		out.push_back(hidden(CheckId::Performance, "OBS keeping up"));
		out.push_back(hidden(CheckId::Custom, "Your checklist"));
		return out;
	}
	out.push_back(hidden(CheckId::Setup, "Setup"));
	out.push_back(evalMicMuted(in));
	out.push_back(evalMicTest(in));
	out.push_back(evalDesktop(in));
	out.push_back(evalScene(in));
	out.push_back(evalMainStream(in));
	out.push_back(evalMultistream(in));
	out.push_back(evalRecording(in));
	out.push_back(evalPerformance(in));
	out.push_back(evalCustom(in));
	return out;
}

// ---- FrameWindow ----

void FrameWindow::add(qint64 tMs, quint64 bad, quint64 total)
{
	if (!samples_.empty()) {
		const Sample &last = samples_.back();
		if (bad < last.bad || total < last.total || tMs < last.t)
			samples_.clear(); // OBS counters reset (or clock went backwards): start fresh
	}
	samples_.push_back({tMs, bad, total});
	// Drop old samples but keep one at-or-before the cutoff as the window's anchor.
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
		return w; // newest sample is stale (sampling stopped)

	// Anchor = latest sample at or before nowMs - span.
	const qint64 cutoff = nowMs - span_;
	int anchor = -1;
	for (int i = 0; i < int(samples_.size()); ++i)
		if (samples_[i].t <= cutoff)
			anchor = i;
	if (anchor < 0)
		return w; // not enough history yet
	const Sample &a = samples_[anchor];
	if (nowMs - a.t > span_ + kFrameAnchorSlackMs)
		return w; // history too old / too sparse to describe the last span

	for (int i = anchor + 1; i < int(samples_.size()); ++i)
		if (samples_[i].t - samples_[i - 1].t > kFrameStaleMs)
			return w; // gap in sampling

	const quint64 dTotal = b.total - a.total;
	if (dTotal == 0)
		return w; // no frames counted: cannot judge
	w.filled = true;
	w.ratio = double(b.bad - a.bad) / double(dTotal);
	return w;
}

// ---- MicTestAnalyzer ----

float powerMeanDb(const std::vector<float> &levelsDb, float floorDb)
{
	double sum = 0.0;
	size_t n = 0;
	for (float v : levelsDb) {
		if (std::isnan(v) || !(v > floorDb))
			continue;
		sum += std::pow(10.0, static_cast<double>(v) / 10.0);
		++n;
	}
	if (n == 0)
		return -std::numeric_limits<float>::infinity();
	return static_cast<float>(10.0 * std::log10(sum / static_cast<double>(n)));
}

QString formatDb(float db)
{
	if (std::isnan(db) || db <= -200.0f)
		return QStringLiteral("-inf");
	double r = std::round(static_cast<double>(db) * 10.0) / 10.0;
	if (r == 0.0)
		r = 0.0; // -0.0 prints as "-0.0"
	return QString::number(r, 'f', 1);
}

void MicTestAnalyzer::start(qint64 tMs)
{
	start_ = tMs;
	peaks_.clear();
	mags_.clear();
	haveOld_ = false;
	oldFloorDb_ = 0.0f;
}

void MicTestAnalyzer::add(qint64 tMs, float peakDb, float magnitudeDb)
{
	const qint64 off = tMs - start_;
	if (off < 0 || off > kMicTestWindowMs)
		return;
	add(tMs, peakDb);
	mags_.push_back(magnitudeDb);
}

MicNumbers MicTestAnalyzer::numbers() const
{
	MicNumbers n;
	if (peaks_.empty())
		return n;
	n.valid = true;
	n.loudestDb = *std::max_element(peaks_.begin(), peaks_.end());
	n.typicalDb = powerMeanDb(mags_, kMicDigitalSilenceDb);
	return n;
}

void MicTestAnalyzer::add(qint64 tMs, float peakDb)
{
	const qint64 off = tMs - start_;
	if (off < 0 || off > kMicTestWindowMs)
		return;
	peaks_.push_back(peakDb);
	if (off < kMicOldFloorWindowMs) { // legacy rule, kept only so it can be logged next to the new floor
		oldFloorDb_ = haveOld_ ? std::max(oldFloorDb_, peakDb) : peakDb;
		haveOld_ = true;
	}
}

MicTestAnalyzer::Summary MicTestAnalyzer::summary() const
{
	Summary s;
	s.count = static_cast<int>(peaks_.size());
	if (peaks_.empty())
		return s;
	std::vector<float> sorted(peaks_);
	std::sort(sorted.begin(), sorted.end());
	// Nearest-rank percentile: rank = ceil(p * n), 1-based. The epsilon keeps 0.10 * 20 from
	// rounding up to rank 3 through floating-point error.
	const double n = static_cast<double>(sorted.size());
	size_t rank = static_cast<size_t>(std::ceil(kMicFloorPercentile * n - 1e-9));
	rank = std::min(std::max<size_t>(rank, 1), sorted.size());
	s.floorP10Db = sorted[rank - 1];
	s.maxDb = sorted.back();
	if (haveOld_)
		s.oldFloorDb = oldFloorDb_;
	return s;
}

MicTestStatus MicTestAnalyzer::finish(bool sourceActive) const
{
	const Summary s = summary();
	if (!sourceActive || s.count == 0 || s.maxDb <= kMicDigitalSilenceDb)
		return MicTestStatus::NoAudio;
	if (s.maxDb > kMicMinPeakDb && s.maxDb >= s.floorP10Db + kMicAboveFloorDb)
		return MicTestStatus::Passed;
	return MicTestStatus::TooQuiet;
}

// ---- Go live ----

GoLiveDecision decideGoLive(const QVector<CheckResult> &results)
{
	GoLiveDecision d;
	for (const auto &r : results) {
		if (r.state == State::Red)
			d.reds << r.title;
		else if (r.state == State::Amber || r.state == State::Unknown)
			d.cautions << r.title;
	}
	d.allowedWithoutAsking = d.reds.isEmpty();
	return d;
}

} // namespace preflight

/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "levels-panel.hpp"
#include "inflight-clock.hpp"
#include "kit-cost.hpp"
#include "levels-lufs.hpp"
#include "levels-meters.hpp"

#include <obs.h>
#include <plugin-support.h>

#include <QAbstractItemView>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

using namespace inflight;

namespace {
const QColor kGreen(0x2E, 0x9E, 0x5B);
const QColor kAmber(0xF2, 0xA6, 0x3B);
const QColor kRed(0xC0, 0x39, 0x2B);
// The reference line, its number above it and the word "Reference" share this one blue. It has to read on OBS's dark
// themes (#1e1e1e..#31363b: contrast about 4:1) and on its light one (#f0f0f0: about 3.6:1). No colour reaches 4.5:1 on
// both, so the middle of the range is picked on purpose; the old 4A90D9 was 3.1:1 on light grey.
const QColor kRefColor(0x2F, 0x7F, 0xD0);
const char *const kRefColorCss = "#2F7FD0";
// Where "What these numbers mean" goes when the host has no local manual to offer.
const char *const kLevelsManualUrl = "https://redwardenstudios.com/division/bastion/stream-kit/manual/#levels";

constexpr float kBarMinDb = -60.0f; // the bar's left end; 0 dBFS is the right end

float barFraction(float db)
{
	if (std::isnan(db) || std::isinf(db))
		return 0.0f; // silence draws no bar
	return std::clamp((db - kBarMinDb) / (0.0f - kBarMinDb), 0.0f, 1.0f);
}

// A label that elides its text to the width it is given (source names can be long) and keeps the full
// name as its tooltip.
class ElidedLabel : public QLabel {
public:
	explicit ElidedLabel(QWidget *parent) : QLabel(parent)
	{
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	}
	void setFull(const QString &s)
	{
		if (s == full_)
			return;
		full_ = s;
		setToolTip(s);
		relayout();
	}
	QSize sizeHint() const override { return QSize(80, fontMetrics().height() + 2); }
	QSize minimumSizeHint() const override { return QSize(24, fontMetrics().height() + 2); }

protected:
	void resizeEvent(QResizeEvent *e) override
	{
		QLabel::resizeEvent(e);
		relayout();
	}

private:
	void relayout() { setText(fontMetrics().elidedText(full_, Qt::ElideRight, qMax(0, width() - 2))); }
	QString full_;
};

// Colour band of a level: 0 = none (silence draws nothing), 1 green, 2 amber (> -12 dBFS), 3 red (> -3 dBFS).
int levelBand(float db)
{
	if (std::isnan(db) || std::isinf(db))
		return 0;
	if (db > -3.0f)
		return 3;
	if (db > -12.0f)
		return 2;
	return 1;
}

QColor bandColor(int band)
{
	return band == 3 ? kRed : (band == 2 ? kAmber : kGreen);
}

// The slim bar: the fill is the INSTANTANEOUS peak of the last meter window (50 ms, no hold; green / amber /
// red by level), a thin tick is the 1.5 s peak hold, the smoothed average is a marker, faint scale ticks sit
// under it. The numbers beside it carry the meaning, so colour is never the only cue.
class LevelBar : public QWidget {
public:
	explicit LevelBar(QWidget *parent) : QWidget(parent)
	{
		setFixedHeight(kBaseH);
		setMinimumWidth(60);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	}
	void setLevels(float instDb, float holdDb, float avgDb)
	{
		// Repaint only when a drawn position (1/200 of the bar) or a colour band changes.
		const int iq = qRound(barFraction(instDb) * 200.0f);
		const int hq = qRound(barFraction(holdDb) * 200.0f);
		const int aq = std::isinf(avgDb) || std::isnan(avgDb) ? -1 : qRound(barFraction(avgDb) * 200.0f);
		const int ib = levelBand(instDb);
		const int hb = levelBand(holdDb);
		if (init_ && iq == iq_ && hq == hq_ && aq == aq_ && ib == ib_ && hb == hb_)
			return;
		init_ = true;
		iq_ = iq;
		hq_ = hq;
		aq_ = aq;
		ib_ = ib;
		hb_ = hb;
		update();
	}
	// LUFS mode: the fill is the momentary loudness (one colour, a loudness has no clip band), the marker the
	// short-term loudness, no hold tick. The scale is the same -60..0 as the dB bar.
	void setLufsMode(bool on)
	{
		if (lufs_ == on)
			return;
		lufs_ = on;
		init_ = false;
		update();
	}
	// The optional reference tick (LUFS mode); NaN / -inf = none. `label` is the chosen value ("-16"), drawn in the
	// line colour right above the line; the bar grows by one text line at the top to make room for it.
	void setRef(float db, const QString &label = QString())
	{
		const int q = (std::isnan(db) || std::isinf(db)) ? -1 : qRound(barFraction(db) * 200.0f);
		const QString txt = q >= 0 ? label : QString();
		if (q == rq_ && txt == refText_)
			return;
		rq_ = q;
		refText_ = txt;
		const int pad = txt.isEmpty() ? 0 : QFontMetrics(refFont()).height();
		if (pad != topPad_) {
			topPad_ = pad;
			setFixedHeight(kBaseH + pad);
		}
		update();
	}
	QString refText() const { return refText_; }

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		const QRectF bar(0.5, 0.5, width() - 1.0, 9.0);
		p.setRenderHint(QPainter::Antialiasing);
		if (rq_ >= 0 && !refText_.isEmpty()) {
			// The reference value, centred on the line but kept inside the widget at either end; it sits in the
			// strip above the bar, so it never touches the bar or the numbers beside it.
			const QFont f = refFont();
			const QFontMetrics fm(f);
			const qreal tw = fm.horizontalAdvance(refText_);
			const qreal lx = bar.left() + 1.0 + (bar.width() - 2.0) * rq_ / 200.0;
			const qreal left = std::clamp(lx - tw / 2.0, 0.0, std::max(0.0, width() - tw));
			p.setFont(f);
			p.setPen(kRefColor);
			p.drawText(QRectF(left, 0.0, tw + 1.0, topPad_), Qt::AlignCenter, refText_);
		}
		p.translate(0, topPad_); // everything below is laid out for the bare 15 px bar
		p.setPen(QPen(QColor(128, 128, 128, 120), 1));
		p.setBrush(QColor(128, 128, 128, 45));
		p.drawRoundedRect(bar, 2, 2);
		if (iq_ > 0) {
			const qreal w = (bar.width() - 2.0) * iq_ / 200.0;
			p.setPen(Qt::NoPen);
			p.setBrush(lufs_ ? kGreen : bandColor(ib_));
			p.drawRoundedRect(QRectF(bar.left() + 1.0, bar.top() + 1.0, w, bar.height() - 2.0), 1.5, 1.5);
		}
		if (rq_ >= 0) {
			// The user's reference line (LUFS mode, off by default): a thin blue tick across the bar.
			const qreal x = bar.left() + 1.0 + (bar.width() - 2.0) * rq_ / 200.0;
			p.setPen(QPen(kRefColor, 1.5));
			p.drawLine(QPointF(x, bar.top() - 1.5), QPointF(x, bar.bottom() + 1.5));
		}
		if (hq_ > 0) {
			// The 1.5 s peak hold: a thin tick in the (brightened) colour of its band, visible on the fill too.
			const qreal x = bar.left() + 1.0 + (bar.width() - 2.0) * hq_ / 200.0;
			p.setPen(QPen(bandColor(hb_).lighter(130), 2.0)); // bright: must read on the dark track and on the fill
			p.drawLine(QPointF(x, bar.top() + 1.0), QPointF(x, bar.bottom() - 1.0));
		}
		if (aq_ >= 0) {
			const qreal x = bar.left() + 1.0 + (bar.width() - 2.0) * aq_ / 200.0;
			p.setPen(QPen(palette().color(QPalette::WindowText), 2));
			p.drawLine(QPointF(x, bar.top() - 0.5), QPointF(x, bar.bottom() + 0.5));
		}
		// Scale ticks at -50, -40, -30, -20, -10 dBFS under the bar.
		p.setRenderHint(QPainter::Antialiasing, false);
		p.setPen(QPen(QColor(128, 128, 128, 160), 1));
		for (int db = -50; db <= -10; db += 10) {
			const int x = qRound(bar.left() + 1.0 + (bar.width() - 2.0) * barFraction(static_cast<float>(db)));
			p.drawLine(x, 11, x, 13);
		}
	}

private:
	static constexpr int kBaseH = 15;
	// Small and bold: it is a label for a line, not a number to read at a glance.
	QFont refFont() const
	{
		QFont f = font();
		f.setBold(true);
		if (f.pointSizeF() > 0)
			f.setPointSizeF(std::max(6.5, f.pointSizeF() * 0.8));
		else if (f.pixelSize() > 0)
			f.setPixelSize(std::max(9, qRound(f.pixelSize() * 0.8)));
		return f;
	}
	QString refText_;
	int topPad_ = 0; // height of the strip above the bar that holds refText_ (0 = none)
	bool init_ = false;
	int iq_ = 0;  // instantaneous fill, 1/200 of the bar
	int hq_ = 0;  // peak-hold tick
	int aq_ = -1; // average marker, -1 = none
	int ib_ = 0;  // colour band of the fill
	int hb_ = 0;  // colour band of the tick
	bool lufs_ = false;
	int rq_ = -1; // reference tick, 1/200 of the bar, -1 = none
};

struct AudioEntry {
	QString name;
	QString uuid;
};

// obs_enum_sources hands out borrowed pointers: nothing is retained here, so nothing to release.
bool enumAudioInputs(void *param, obs_source_t *src)
{
	auto *out = static_cast<QVector<AudioEntry> *>(param);
	if (obs_source_get_type(src) != OBS_SOURCE_TYPE_INPUT)
		return true;
	if (!(obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO))
		return true;
	const char *n = obs_source_get_name(src);
	const char *u = obs_source_get_uuid(src);
	if (n && u)
		out->push_back({QString::fromUtf8(n), QString::fromUtf8(u)});
	return true;
}

void setIf(QLabel *lab, const QString &s)
{
	if (lab->text() != s)
		lab->setText(s);
}
} // namespace

struct LevelsPanel::Row {
	QString uuid;
	bool isMic = false;
	QString name;
	bool exists = false;    // OBS resolves the uuid to a source (checked on the 1 Hz beat, when a meter is wanted)
	bool attached = false;  // a meter for it delivered a Reading on the last tick
	bool missing = true;    // OBS has no source with this uuid (rowKind Missing), or demo row never fed
	bool warm = true;       // attached but no reading (not started yet, or no callback for kStaleMs): shows "--"
	bool haveFed = false;   // a real reading has been fed since the meter (re)started
	bool firstLogged = false; // test hook: the "first reading after" line has been logged for this start
	PeakHold hold;
	AvgSmoother avg;
	float instV = minusInf(); // the bar's fill: peak of the last fresh 50 ms window (no hold)
	float peakV = minusInf();
	float avgV = minusInf();
	QWidget *box = nullptr;
	ElidedLabel *nameL = nullptr;
	LevelBar *bar = nullptr;
	QLabel *peakL = nullptr;
	QLabel *avgL = nullptr;
	QLabel *tpL = nullptr;  // LUFS mode with true peak on: "TP -1.2 dBTP"
	QLabel *gapL = nullptr; // non-mic rows only, lives under the rows
	LufsResult lr;          // LUFS mode: the latest numbers from the worker (momentary, short-term, true peak)
	bool demoTp = false;    // demo only: lr.tpMax is a fed value
};

LevelsPanel::LevelsPanel(TrFn tr, QWidget *parent) : QWidget(parent), tr_(std::move(tr))
{
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(0, 0, 0, 0);
	root->setSpacing(4);

	toggle_ = new QToolButton(this);
	toggle_->setObjectName(QStringLiteral("ifLevelsToggle"));
	toggle_->setAutoRaise(true);
	toggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	toggle_->setArrowType(Qt::RightArrow);
	toggle_->setText(trk("InFlight.Levels.Title", "Levels"));
	toggle_->setToolTip(trk("InFlight.Levels.Tip", "Peak and average level of each source, in dBFS"));
	{
		// Header: the toggle on the left, the dB | LUFS pair on the right (shown while the panel is open).
		auto *head = new QHBoxLayout();
		head->setContentsMargins(0, 0, 0, 0);
		head->setSpacing(0);
		head->addWidget(toggle_, 0, Qt::AlignLeft);
		head->addStretch(1);
		modeBar_ = new QWidget(this);
		auto *mb = new QHBoxLayout(modeBar_);
		mb->setContentsMargins(0, 0, 0, 0);
		mb->setSpacing(2);
		dbBtn_ = new QToolButton(modeBar_);
		dbBtn_->setObjectName(QStringLiteral("ifLevelsModeDb"));
		dbBtn_->setText(trk("InFlight.Levels.ModeDb", "dB"));
		dbBtn_->setToolTip(trk("InFlight.Levels.ModeDbTip", "Peak and average level, in dBFS"));
		lufsBtn_ = new QToolButton(modeBar_);
		lufsBtn_->setObjectName(QStringLiteral("ifLevelsModeLufs"));
		lufsBtn_->setText(trk("InFlight.Levels.ModeLufs", "LUFS"));
		lufsBtn_->setToolTip(trk("InFlight.Levels.ModeLufsTip",
					 "Momentary (400 ms) and short-term (3 s) loudness, ITU-R BS.1770 / EBU R128. Not a target."));
		modeGroup_ = new QButtonGroup(this);
		modeGroup_->setExclusive(true);
		for (QToolButton *b : {dbBtn_, lufsBtn_}) {
			b->setCheckable(true);
			b->setAutoRaise(true);
			b->setCursor(Qt::PointingHandCursor);
			modeGroup_->addButton(b);
			mb->addWidget(b);
		}
		dbBtn_->setChecked(true);
		// "?" beside the dB | LUFS pair: opens the manual's Levels section. Not part of the exclusive group.
		helpBtn_ = new QToolButton(modeBar_);
		helpBtn_->setObjectName(QStringLiteral("ifLevelsHelp"));
		helpBtn_->setText(QStringLiteral("?"));
		helpBtn_->setAutoRaise(true);
		helpBtn_->setCursor(Qt::PointingHandCursor);
		helpBtn_->setToolTip(trk("InFlight.Levels.HelpTip", "What these numbers mean"));
		helpBtn_->setAccessibleName(trk("InFlight.Levels.HelpTip", "What these numbers mean"));
		mb->addSpacing(4);
		mb->addWidget(helpBtn_);
		modeBar_->setVisible(false);
		head->addWidget(modeBar_, 0, Qt::AlignRight);
		root->addLayout(head);
	}

	body_ = new QWidget(this);
	auto *bl = new QVBoxLayout(body_);
	bl->setContentsMargins(0, 0, 0, 0);
	bl->setSpacing(6);
	{
		// LUFS mode only: the true-peak switch and the optional reference line.
		lufsOpts_ = new QWidget(body_);
		auto *ol = new QHBoxLayout(lufsOpts_);
		ol->setContentsMargins(0, 0, 0, 0);
		ol->setSpacing(8);
		tpBox_ = new QCheckBox(trk("InFlight.Levels.TruePeak", "True peak"), lufsOpts_);
		tpBox_->setObjectName(QStringLiteral("ifLevelsTruePeak"));
		tpBox_->setToolTip(trk("InFlight.Levels.TruePeakTip",
				       "Show the highest true peak of the last 1.5 s (4x oversampled). Uses extra CPU, so it is off by default."));
		refBox_ = new QCheckBox(trk("InFlight.Levels.Ref", "Reference"), lufsOpts_);
		refBox_->setObjectName(QStringLiteral("ifLevelsRefOn"));
		refBox_->setToolTip(trk("InFlight.Levels.RefTip",
					"Draw a line on the bars at a loudness you choose, with the number above it. Off by default."));
		// The word wears the line's colour, so "this blue line" and "Reference" are plainly the same thing. Only the
		// text colour is set: the check box itself keeps the theme's look.
		refBox_->setStyleSheet(QStringLiteral("QCheckBox#ifLevelsRefOn { color: %1; }").arg(QLatin1String(kRefColorCss)));
		refSpin_ = new QDoubleSpinBox(lufsOpts_);
		refSpin_->setObjectName(QStringLiteral("ifLevelsRef"));
		refSpin_->setDecimals(1);
		refSpin_->setRange(kLufsRefMin, kLufsRefMax);
		refSpin_->setSingleStep(0.5);
		refSpin_->setValue(ref_);
		refSpin_->setSuffix(QStringLiteral(" LUFS"));
		refSpin_->setEnabled(false);
		ol->addWidget(tpBox_);
		ol->addWidget(refBox_);
		ol->addWidget(refSpin_);
		ol->addStretch(1);
		lufsOpts_->setVisible(false);
		bl->addWidget(lufsOpts_);
	}
	rowsLayout_ = new QVBoxLayout();
	rowsLayout_->setSpacing(6);
	bl->addLayout(rowsLayout_);
	none_ = new QLabel(trk("InFlight.Levels.None",
			       "No sources yet. Choose your microphone in settings, or add a source below."),
			   body_);
	none_->setWordWrap(true);
	bl->addWidget(none_);
	gapsLayout_ = new QVBoxLayout();
	gapsLayout_->setSpacing(2);
	bl->addLayout(gapsLayout_);
	add_ = new QComboBox(body_);
	add_->setObjectName(QStringLiteral("ifLevelsAdd"));
	add_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	add_->setMinimumContentsLength(14);
	add_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	add_->setToolTip(trk("InFlight.Levels.AddTip", "Show another audio source in the Levels panel"));
	{
		auto *ar = new QHBoxLayout();
		ar->addWidget(add_, 1);
		bl->addLayout(ar);
	}
	body_->setVisible(false);
	root->addWidget(body_);

	connect(toggle_, &QToolButton::clicked, this, [this] { setOpen(!open_, true); });
	connect(add_, QOverload<int>::of(&QComboBox::activated), this, [this](int i) {
		if (i <= 0)
			return;
		const QString u = add_->itemData(i).toString();
		if (u.isEmpty() || extras_.contains(u) || u == mic_ || u == desktop_)
			return;
		extras_.push_back(u);
		emit extrasChanged(extras_);
		QTimer::singleShot(0, this, [this] { rebuildRows(); }); // not from inside the combo's own signal
	});
	connect(dbBtn_, &QToolButton::clicked, this, [this] { setMode(LevelsMode::Db, true); });
	connect(lufsBtn_, &QToolButton::clicked, this, [this] { setMode(LevelsMode::Lufs, true); });
	connect(helpBtn_, &QToolButton::clicked, this, [this] { openHelp(); });
	connect(tpBox_, &QCheckBox::clicked, this, [this](bool on) { setTruePeak(on, true); });
	connect(refBox_, &QCheckBox::clicked, this, [this](bool on) { setLufsRef(on, refSpin_->value(), true); });
	connect(refSpin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
		[this](double v) { setLufsRef(refBox_->isChecked(), v, true); });
	lufsLog_.setSingleShot(true);
	connect(&lufsLog_, &QTimer::timeout, this, [this] { lufsLogNow(); });
	testHooks_ = qEnvironmentVariableIsSet("RWS_INFLIGHT_TEST");
	fast_.setInterval(kFastMs);
	fast_.setTimerType(Qt::PreciseTimer); // 20 Hz: the coarse timer's 5 % slack is fine, Windows' 15.6 ms ticks are not
	connect(&fast_, &QTimer::timeout, this, [this] { kitcost::Scope costScope(kitcost::LevelsFast); fastTick(); });
	text_.setInterval(kTextMs);
	connect(&text_, &QTimer::timeout, this, [this] { kitcost::Scope costScope(kitcost::LevelsText); textTick(); });
	slow_.setInterval(1000);
	connect(&slow_, &QTimer::timeout, this, [this] { kitcost::Scope costScope(kitcost::LevelsSlow); slowTick(); });
	testLog_.setSingleShot(true);
	connect(&testLog_, &QTimer::timeout, this, [this] { testLogNow(); });
	statsLog_.setSingleShot(true);
	connect(&statsLog_, &QTimer::timeout, this, [this] { logTickStats(); });
	rebuildRows();
}

LevelsPanel::~LevelsPanel()
{
	fast_.stop();
	text_.stop();
	slow_.stop();
	testLog_.stop();
	statsLog_.stop();
	lufsLog_.stop();
	delete lufs_; // removes every capture callback, then stops the worker
	lufs_ = nullptr;
	delete meters_; // detaches every meter first
	meters_ = nullptr;
	qDeleteAll(rows_);
	rows_.clear();
}

QStringList LevelsPanel::rowUuids() const
{
	QStringList out;
	for (const Row *r : rows_)
		out.push_back(r->uuid);
	return out;
}

void LevelsPanel::setSources(const QString &micUuid, const QString &desktopUuid, const QStringList &extras)
{
	QStringList clean;
	for (const QString &u : extras) {
		if (!u.isEmpty() && u != micUuid && u != desktopUuid && !clean.contains(u))
			clean.push_back(u);
	}
	const QString d = (desktopUuid == micUuid) ? QString() : desktopUuid;
	if (micUuid == mic_ && d == desktop_ && clean == extras_)
		return;
	mic_ = micUuid;
	desktop_ = d;
	extras_ = clean;
	rebuildRows();
}

void LevelsPanel::rebuildRows()
{
	for (Row *r : rows_) {
		rowsLayout_->removeWidget(r->box);
		r->box->hide();
		r->box->deleteLater();
		if (r->gapL) {
			gapsLayout_->removeWidget(r->gapL);
			r->gapL->hide();
			r->gapL->deleteLater();
		}
		delete r;
	}
	rows_.clear();

	const QFontMetrics fm(font());
	// dB mode: "Peak -100.0" / "Avg -100.0". LUFS mode: "Now -23.1" / "3s avg -24.0" (wider; "-70.0" is the practical floor).
	numWDb_ = qMax(fm.horizontalAdvance(trk("InFlight.Levels.Peak", "Peak %1").arg(QStringLiteral("-100.0"))),
		       fm.horizontalAdvance(trk("InFlight.Levels.Avg", "Avg %1").arg(QStringLiteral("-100.0")))) + 4;
	numWLufs_ = qMax(fm.horizontalAdvance(trk("InFlight.Levels.M", "Now %1").arg(QStringLiteral("-99.9"))),
			 fm.horizontalAdvance(trk("InFlight.Levels.S", "3s avg %1").arg(QStringLiteral("-99.9")))) + 12;
	// (+12, not +4: the themed label font comes out a little wider than the panel's own, and "3s avg" must not clip)

	auto addRow = [&](const QString &uuid, bool isMic, bool removable) {
		auto *r = new Row;
		r->uuid = uuid;
		r->isMic = isMic;
		r->box = new QWidget(body_);
		auto *v = new QVBoxLayout(r->box);
		v->setContentsMargins(0, 0, 0, 0);
		v->setSpacing(2);
		auto *top = new QHBoxLayout();
		top->setSpacing(4);
		r->nameL = new ElidedLabel(r->box);
		QFont nf = r->nameL->font();
		nf.setBold(true);
		r->nameL->setFont(nf);
		top->addWidget(r->nameL, 1);
		r->tpL = new QLabel(r->box);
		r->tpL->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		r->tpL->setToolTip(trk("InFlight.Levels.TpTip",
				       "True peak: the highest point of the waveform, including between samples. Keep it below -1 dBTP."));
		r->tpL->setVisible(false);
		top->addWidget(r->tpL, 0);
		if (removable) {
			auto *rm = new QToolButton(r->box);
			rm->setAutoRaise(true);
			rm->setText(QString(QChar(0x00D7))); // U+00D7 MULTIPLICATION SIGN
			rm->setCursor(Qt::PointingHandCursor);
			rm->setToolTip(trk("InFlight.Levels.Remove", "Remove this source from the Levels panel"));
			rm->setAccessibleName(trk("InFlight.Levels.Remove", "Remove this source from the Levels panel"));
			connect(rm, &QToolButton::clicked, this, [this, uuid] {
				extras_.removeAll(uuid);
				emit extrasChanged(extras_);
				rebuildRows();
			});
			top->addWidget(rm, 0, Qt::AlignVCenter);
		}
		v->addLayout(top);
		auto *bot = new QHBoxLayout();
		bot->setSpacing(6);
		r->bar = new LevelBar(r->box);
		// Bottom-aligned: with Reference on the bar grows upward (the value sits above the line) and the numbers stay level
		// with the bar itself.
		bot->addWidget(r->bar, 1, Qt::AlignBottom);
		r->peakL = new QLabel(r->box);
		r->peakL->setFixedWidth(numWDb_);
		r->peakL->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		r->avgL = new QLabel(r->box);
		r->avgL->setFixedWidth(numWDb_);
		r->avgL->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		bot->addWidget(r->peakL, 0, Qt::AlignBottom);
		bot->addWidget(r->avgL, 0, Qt::AlignBottom);
		v->addLayout(bot);
		rowsLayout_->addWidget(r->box);
		if (!isMic) {
			r->gapL = new QLabel(body_);
			r->gapL->setWordWrap(true);
			r->gapL->setVisible(false);
			gapsLayout_->addWidget(r->gapL);
		}
		rows_.push_back(r);
	};
	if (!mic_.isEmpty())
		addRow(mic_, true, false);
	if (!desktop_.isEmpty())
		addRow(desktop_, false, false);
	for (const QString &u : extras_)
		addRow(u, false, true);

	none_->setVisible(rows_.isEmpty());
	applyModeUi();
	for (Row *r : rows_) {
		if (!demo_) {
			resolveRow(*r);
			classify(*r, false); // not metered yet: the real name and "--", never "(missing source)" for a real source
		}
		refreshRow(*r, nowMs());
	}
	refreshGaps();
	refillAdd();
	if (running_) {
		wantedDirty_ = true;
		fastTick(); // the new rows get their meters now, not after the next timer beat
	}
}

void LevelsPanel::refillAdd()
{
	if (demo_) {
		if (add_->count() == 0)
			add_->addItem(trk("InFlight.Levels.Add", "+ Add source"), QString());
		return;
	}
	if (add_->view() && add_->view()->isVisible())
		return; // never rebuild a list the user is looking at
	QVector<AudioEntry> all;
	obs_enum_sources(enumAudioInputs, &all);
	const QStringList shown = rowUuids();
	QVector<AudioEntry> avail;
	for (const AudioEntry &e : all) {
		if (!shown.contains(e.uuid))
			avail.push_back(e);
	}
	std::sort(avail.begin(), avail.end(), [](const AudioEntry &a, const AudioEntry &b) {
		return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
	});
	QStringList keys; // uuid + name: a rename rebuilds the menu too
	for (const AudioEntry &e : avail)
		keys.push_back(e.uuid + QLatin1Char(0x1F) + e.name);
	if (add_->count() > 0 && keys == addShown_)
		return;
	addShown_ = keys;
	add_->clear();
	add_->addItem(trk("InFlight.Levels.Add", "+ Add source"), QString());
	for (const AudioEntry &e : avail)
		add_->addItem(e.name, e.uuid);
	add_->setCurrentIndex(0);
}

void LevelsPanel::setOpen(bool open, bool notify)
{
	if (open == open_)
		return;
	open_ = open;
	body_->setVisible(open);
	modeBar_->setVisible(open);
	toggle_->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
	if (open) {
		refillAdd();
		if (!demo_ && !running_) {
			for (Row *r : rows_) {
				resolveRow(*r);
				classify(*r, false);
				refreshRow(*r, nowMs());
			}
			refreshGaps();
		}
	}
	if (notify)
		emit openChanged(open);
	updateSlow();
	// Start at once: ask the owner for the gate verdict NOW (it answers synchronously through
	// setDockVisible, which starts the meters), never wait for the 1 Hz beat. The beat stays as the safety net.
	if (open && !closed_ && !demo_)
		emit recheckRequested();
	updateRunning();
}

// The 1 Hz beat runs whenever the panel is open (not demo, not shut down), whether or not the meters run.
void LevelsPanel::updateSlow()
{
	const bool want = open_ && !closed_ && !demo_;
	if (want == slow_.isActive())
		return;
	if (want)
		slow_.start();
	else
		slow_.stop();
}

// Name resolution only: nothing is retained (the reference from obs_get_source_by_uuid is released).
void LevelsPanel::resolveRow(Row &r)
{
	if (demo_ || r.uuid.isEmpty()) {
		r.exists = false;
		return;
	}
	obs_source_t *s = obs_get_source_by_uuid(r.uuid.toUtf8().constData());
	if (!s) {
		r.exists = false;
		return;
	}
	r.exists = true;
	if (const char *n = obs_source_get_name(s))
		r.name = QString::fromUtf8(n);
	obs_source_release(s);
}

// Row state from the pure decision (rowKind): missing / warm are what refreshRow and the gap line read.
void LevelsPanel::classify(Row &r, bool attached)
{
	r.attached = attached;
	const RowKind k = rowKind(r.exists, attached, r.haveFed);
	r.missing = (k == RowKind::Missing);
	r.warm = (k != RowKind::Reading);
}

void LevelsPanel::slowTick()
{
	if (closed_ || demo_ || !open_)
		return;
	emit recheckRequested(); // the owner re-reads the live dock state; may start or stop the meters
	for (Row *r : rows_) {
		resolveRow(*r); // a rename or a late-created source is picked up here
		if (!running_ || !r->attached) {
			r->haveFed = false;
			classify(*r, false);
			refreshRow(*r, nowMs());
		}
	}
	refreshGaps();
}

QString LevelsPanel::rowLabel(const QString &uuid) const
{
	for (const Row *r : rows_) {
		if (r->uuid == uuid)
			return r->nameL->toolTip();
	}
	return QString();
}

QString LevelsPanel::rowPeakText(const QString &uuid) const
{
	for (const Row *r : rows_) {
		if (r->uuid == uuid)
			return r->peakL->text();
	}
	return QString();
}

void LevelsPanel::setDockVisible(bool visible)
{
	dockVisible_ = visible;
	updateRunning();
}

void LevelsPanel::setSuspended(bool suspended)
{
	suspended_ = suspended;
	updateRunning();
}

void LevelsPanel::setGateOverride(bool on)
{
	override_ = on;
	updateRunning();
}

void LevelsPanel::armTestLog()
{
	testLogArmed_ = true;
}

void LevelsPanel::shutdown()
{
	if (shutDown_)
		return;
	shutDown_ = true;
	closed_ = true;
	testLog_.stop(); // nothing may log after shutdown
	statsLog_.stop();
	lufsLog_.stop();
	updateSlow();
	updateRunning(); // running -> stopped: timers off, backend stopped
	// Unconditional and idempotent: whatever backend is left (a panel that was open but gated off has none, one that
	// was switched mid-way might) is taken down here, at EXIT, while libobs is still alive and before any widget can
	// be destroyed. The destructor then finds nothing to release.
	fast_.stop();
	text_.stop();
	stopBackend();
}

void LevelsPanel::setDemoMode(bool demo)
{
	demo_ = demo;
	updateSlow();
	updateRunning();
}

// The one place the meters and the timer start and stop: attached only while the panel is open AND the
// dock is visible AND nothing suspended it.
void LevelsPanel::updateRunning()
{
	const bool want = open_ && (dockVisible_ || override_) && !suspended_ && !closed_ && !demo_;
	if (want == running_)
		return;
	running_ = want;
	if (want) {
		startMs_ = nowMs();
		for (Row *r : rows_) {
			resolveRow(*r); // the name cache the fast tick shows from; the text beat keeps it fresh
			r->hold.clear();
			r->avg.clear();
			r->haveFed = false;
			r->firstLogged = false;
			r->instV = minusInf();
			classify(*r, false); // until the first tick attaches: pending, not missing
		}
		tickCount_ = 0;
		startBackend(); // dB: creates the volmeters (attached by the first fastTick); LUFS: capture + worker
		// Attach and take in this very call, then start both timers: no wait for any beat.
		fastTick();
		fast_.start();
		text_.start();
		obs_log(LOG_INFO, "levels: running (panel open, dock visible)");
		if (testLogArmed_ && !testLogDone_) {
			testLog_.start(3000);   // both are member timers, stopped by shutdown(): nothing logs after it
			statsLog_.start(10000);
		}
	} else {
		fast_.stop();
		text_.stop();
		stopBackend();
		obs_log(LOG_INFO, "levels: stopped (meters detached)");
		// Nothing is metering now: drop the old numbers; a real source keeps its name and shows "--".
		if (!demo_ && !closed_) { // not at shutdown: libobs may be going away
			for (Row *r : rows_) {
				r->hold.clear();
				r->avg.clear();
				r->haveFed = false;
				r->instV = minusInf();
				resolveRow(*r);
				classify(*r, false);
				refreshRow(*r, nowMs());
			}
			refreshGaps();
		}
	}
}

// The fast beat (20 Hz): take what the meters collected in the last 50 ms, feed the hold and the smoother,
// draw the bars. Text is NOT rewritten here, except for a row whose state just changed (its first reading,
// or it lost its reading): that shows on this very tick, not on the next text beat.
void LevelsPanel::fastTick()
{
	if (running_ && mode_ == LevelsMode::Lufs) {
		lufsFastTick();
		return;
	}
	if (!running_ || !meters_)
		return;
	QElapsedTimer cost;
	if (testHooks_)
		cost.start();
	const qint64 t = nowMs();
	if (wantedDirty_) {
		wantedDirty_ = false;
		meters_->setWanted(rowUuids()); // later retries (a late-created source) come from the text beat
	}
	bool stateChanged = false;
	// Row i reads slot i of the meters (the rows' order is the setWanted order): no UUID matching, no string,
	// no libobs name lookup, no allocation on this path. Names come from the text beat (Row::name is a cache).
	for (int i = 0; i < rows_.size(); ++i) {
		Row *r = rows_[i];
		const LevelsMeters::SlotReading sr = meters_->takeSlot(i, t);
		const LevelsMeters::SlotReading *hit = sr.attached ? &sr : nullptr;
		const bool wasMissing = r->missing;
		const bool wasWarm = r->warm;
		if (!hit) {
			// No meter for it: either OBS has no such source (missing) or it exists but could not be
			// metered (not an audio source, attach failed): a real source keeps its name and shows "--".
			// (The name lookup is the text beat's job: no libobs source lookup at 20 Hz.)
			r->haveFed = false;
			r->hold.clear();
			r->avg.clear();
			r->instV = minusInf();
			classify(*r, false);
		} else {
			r->exists = true;
			// The bar's fill is this window's instantaneous peak; a window with no fresh reading clears it
			// (the hold tick is PeakHold's, untouched).
			r->instV = instantFill(hit->available, hit->fresh, hit->peakDb);
			if (!hit->available) {
				// No callback for a while: unavailable, which is not silence. Nothing is fed, the old
				// numbers are dropped, the row shows "--".
				r->haveFed = false;
				r->hold.clear();
				r->avg.clear();
				// (instV is already -inf here: instantFill)
			} else if (hit->fresh) {
				r->hold.feed(hit->peakDb, t);
				r->avg.feed(hit->avgDb, t); // the mean power of the window, not an interval maximum
				r->haveFed = true;
			}
			classify(*r, true);
			if (testHooks_ && r->haveFed && !r->firstLogged) {
				r->firstLogged = true;
				const QByteArray nm = r->name.toUtf8();
				obs_log(LOG_INFO, "levels: first reading after %lld ms (%s)", static_cast<long long>(t - startMs_),
					nm.constData());
			}
		}
		if (wasMissing != r->missing || wasWarm != r->warm) {
			stateChanged = true;
			refreshRowText(*r, t); // the first fresh reading is on screen on the tick that has it
		}
		refreshBar(*r, t);
	}
	if (stateChanged)
		refreshGaps();
	if (testHooks_) {
		const qint64 ns = cost.nsecsElapsed();
		++statN_;
		statSumNs_ += ns;
		if (ns > statMaxNs_)
			statMaxNs_ = ns;
	}
}

// The text beat (5 Hz): retry attaching, pick up renames and late-created sources, rewrite the numbers and
// gap lines (setText only when the text changed).
void LevelsPanel::textTick()
{
	if (running_ && mode_ == LevelsMode::Lufs) {
		lufsTextTick();
		return;
	}
	if (!running_ || !meters_)
		return;
	const qint64 t = nowMs();
	meters_->setWanted(rowUuids()); // also rebuilds the slot table: slot i = row i
	for (int i = 0; i < rows_.size(); ++i) {
		Row *r = rows_[i];
		if (!r->attached) {
			resolveRow(*r);
			classify(*r, false);
		} else {
			const QString n = meters_->sourceName(i); // the name cache: refreshed here, 5 Hz
			if (!n.isEmpty())
				r->name = n;
		}
		refreshRowText(*r, t);
	}
	refreshGaps();
	if (++tickCount_ % 5 == 0)
		refillAdd(); // about once a second: new or removed sources
}

void LevelsPanel::refreshRow(Row &r, qint64 tMs)
{
	refreshRowText(r, tMs);
	refreshBar(r, tMs);
}

// The bar: instantaneous fill, 1.5 s hold tick, average marker. Nothing for a missing or warm row.
void LevelsPanel::refreshBar(Row &r, qint64 tMs)
{
	const bool lufs = mode_ == LevelsMode::Lufs;
	r.bar->setLufsMode(lufs);
	r.bar->setRef(lufs && refOn_ ? static_cast<float>(ref_) : std::numeric_limits<float>::quiet_NaN(), refLabelText());
	if (r.missing || r.warm) {
		r.bar->setLevels(minusInf(), minusInf(), minusInf());
		return;
	}
	if (lufs) {
		// The fill is the momentary loudness, the marker the short-term one; no hold tick.
		r.bar->setLevels(r.lr.momentaryReady ? static_cast<float>(r.lr.momentary) : minusInf(), minusInf(),
				 r.lr.shortReady ? static_cast<float>(r.lr.shortTerm) : minusInf());
		return;
	}
	r.bar->setLevels(r.instV, r.hold.value(tMs), r.avg.value());
}

void LevelsPanel::refreshRowText(Row &r, qint64 tMs)
{
	if (mode_ == LevelsMode::Lufs) {
		// "Now -23.1" momentary, "3s avg -24.0" short-term, "--" until a window is ready, "-inf" for silence; "TP -1.2 dBTP"
		// (max of the last ~1.5 s) only with the true-peak switch on.
		QString nameText = r.missing ? trk("InFlight.Levels.Missing", "(missing source)") : r.name;
		QString mT, sT, tpT;
		if (r.missing || r.warm) {
			mT = trk("InFlight.Levels.M", "Now %1").arg(QStringLiteral("--"));
			sT = trk("InFlight.Levels.S", "3s avg %1").arg(QStringLiteral("--"));
			tpT = trk("InFlight.Levels.TpNone", "TP %1").arg(QStringLiteral("--"));
		} else {
			mT = trk("InFlight.Levels.M", "Now %1").arg(formatLufs(r.lr.momentaryReady, r.lr.momentary));
			sT = trk("InFlight.Levels.S", "3s avg %1").arg(formatLufs(r.lr.shortReady, r.lr.shortTerm));
			// The unit follows a number only: "TP -1.2 dBTP", but "TP --" and "TP -inf".
			const QString tv = formatLufs(r.lr.tpValid, r.lr.tpMax);
			const bool isNumber = r.lr.tpValid && std::isfinite(r.lr.tpMax);
			tpT = isNumber ? trk("InFlight.Levels.Tp", "TP %1 dBTP").arg(tv)
				       : trk("InFlight.Levels.TpNone", "TP %1").arg(tv);
		}
		r.nameL->setFull(nameText);
		setIf(r.peakL, mT);
		setIf(r.avgL, sT);
		setIf(r.tpL, tpT);
		const QString acc = truePeak_ ? QStringLiteral("%1: %2, %3, %4").arg(nameText, mT, sT, tpT)
					      : QStringLiteral("%1: %2, %3").arg(nameText, mT, sT);
		if (r.bar->accessibleName() != acc)
			r.bar->setAccessibleName(acc);
		return;
	}
	float p = minusInf();
	float a = minusInf();
	QString nameText;
	QString peakT;
	QString avgT;
	if (r.missing) {
		nameText = trk("InFlight.Levels.Missing", "(missing source)");
		peakT = trk("InFlight.Levels.Peak", "Peak %1").arg(formatDb(p));
		avgT = trk("InFlight.Levels.Avg", "Avg %1").arg(formatDb(a));
	} else if (r.warm) {
		nameText = r.name;
		peakT = trk("InFlight.Levels.Peak", "Peak %1").arg(QStringLiteral("--"));
		avgT = trk("InFlight.Levels.Avg", "Avg %1").arg(QStringLiteral("--"));
	} else {
		nameText = r.name;
		p = r.hold.value(tMs);
		a = r.avg.value();
		peakT = trk("InFlight.Levels.Peak", "Peak %1").arg(formatDb(p));
		avgT = trk("InFlight.Levels.Avg", "Avg %1").arg(formatDb(a));
	}
	r.peakV = p;
	r.avgV = a;
	r.nameL->setFull(nameText);
	setIf(r.peakL, peakT);
	setIf(r.avgL, avgT);
	const QString acc = QStringLiteral("%1: %2, %3").arg(nameText, peakT, avgT);
	if (r.bar->accessibleName() != acc)
		r.bar->setAccessibleName(acc);
}

void LevelsPanel::refreshGaps()
{
	const Row *mic = nullptr;
	for (const Row *r : rows_) {
		if (r->isMic)
			mic = r;
	}
	for (Row *r : rows_) {
		if (!r->gapL)
			continue;
		bool show = mic && !mic->missing && !mic->warm && !r->missing && !r->warm;
		const bool lufs = mode_ == LevelsMode::Lufs;
		if (lufs && show)
			show = mic->lr.shortReady && r->lr.shortReady; // the gap is short-term LU: both windows must be full
		if (r->gapL->isHidden() == show)
			r->gapL->setVisible(show);
		if (!show)
			continue;
		if (lufs) {
			const float a = static_cast<float>(mic->lr.shortTerm), b = static_cast<float>(r->lr.shortTerm);
			setIf(r->gapL, gapText(mic->name, r->name, gapDb(a, b), b, tr_, true));
			continue;
		}
		setIf(r->gapL, gapText(mic->name, r->name, gapDb(mic->avgV, r->avgV), r->avgV, tr_));
	}
}

void LevelsPanel::demoSet(const QString &uuid, const QString &name, float peakDb, float avgDb)
{
	for (Row *r : rows_) {
		if (r->uuid != uuid)
			continue;
		r->name = name;
		r->missing = false;
		r->warm = false;
		r->hold.feed(peakDb, 0);
		r->avg.feed(avgDb, 0);
		// Demo only: the fill (the instantaneous peak) sits 4 dB under the held peak so the screenshot shows the
		// hold tick beyond the fill's end, as it does on a live signal that has just dipped.
		r->instV = std::isfinite(peakDb) ? peakDb - 4.0f : peakDb;
		refreshRow(*r, 0);
	}
	refreshGaps();
}

void LevelsPanel::demoSetNoData(const QString &uuid, const QString &name)
{
	for (Row *r : rows_) {
		if (r->uuid != uuid)
			continue;
		r->name = name;
		r->missing = false;
		r->warm = true;
		refreshRow(*r, 0);
	}
	refreshGaps();
}

int LevelsPanel::attachedMeters() const
{
	return meters_ ? meters_->attachedCount() : 0;
}

// Test hook: the cost of the fast tick (meter take + feeds + bar updates; the paint itself runs later from the
// event loop and is not in it), once. Only counted under RWS_INFLIGHT_TEST.
void LevelsPanel::logTickStats()
{
	if (closed_ || statsLogged_ || statN_ == 0)
		return;
	statsLogged_ = true;
	const double meanUs = static_cast<double>(statSumNs_) / static_cast<double>(statN_) / 1000.0;
	const double maxUs = static_cast<double>(statMaxNs_) / 1000.0;
	obs_log(LOG_INFO, "levels: fast tick mean=%.1f us max=%.1f us n=%lld", meanUs, maxUs, static_cast<long long>(statN_));
}

// Test hook: one line per source, once, a few seconds after the meters first ran.
void LevelsPanel::testLogNow()
{
	if (closed_ || !running_ || testLogDone_)
		return;
	testLogDone_ = true;
	const qint64 t = nowMs();
	for (const Row *r : rows_) {
		if (r->missing) {
			obs_log(LOG_INFO, "levels: (missing source) peak=-inf avg=-inf");
			continue;
		}
		const QString pk = r->warm ? QStringLiteral("--") : formatDb(r->hold.value(t));
		const QString av = r->warm ? QStringLiteral("--") : formatDb(r->avg.value());
		const QByteArray line = QStringLiteral("levels: %1 peak=%2 avg=%3").arg(r->name, pk, av)
						.toUtf8();
		obs_log(LOG_INFO, "%s", line.constData());
	}
	for (const Row *r : rows_) {
		if (!r->gapL || r->gapL->isHidden())
			continue;
		const QByteArray line = (QStringLiteral("levels: gap ") + r->gapL->text()).toUtf8();
		obs_log(LOG_INFO, "%s", line.constData());
	}
}

// ---- LUFS mode

// The backend of the current mode, and only that one: dB = the volmeters (attached by the first fast tick),
// LUFS = audio capture callbacks plus the worker thread. UI thread.
void LevelsPanel::startBackend()
{
	if (mode_ == LevelsMode::Lufs) {
		if (!lufs_)
			lufs_ = new LevelsLufs;
		wantedDirty_ = false;
		if (!lufs_->start(rowUuids(), truePeak_))
			obs_log(LOG_WARNING, "levels: lufs capture could not start (no audio output?)");
		else if (lufsLogArmed_ && !lufsLogDone_)
			lufsLog_.start(5000);
	} else {
		if (!meters_)
			meters_ = new LevelsMeters;
		wantedDirty_ = true;
	}
}

// Removes the volmeters and, for LUFS, the capture callbacks first and then the worker (joined here).
void LevelsPanel::stopBackend()
{
	if (meters_)
		meters_->detachAll();
	if (lufs_)
		lufs_->stop();
}

void LevelsPanel::resetRowsForBackend()
{
	for (Row *r : rows_) {
		r->hold.clear();
		r->avg.clear();
		r->haveFed = false;
		r->firstLogged = false;
		r->instV = minusInf();
		r->lr = LufsResult();
		if (!demo_) {
			resolveRow(*r);
			classify(*r, false);
		}
	}
}

// Buttons, options row and per-row widgets follow mode_ / truePeak_ / the reference. Signals blocked: this only
// shows state, it never reports a user action.
void LevelsPanel::applyModeUi()
{
	const bool lufs = mode_ == LevelsMode::Lufs;
	{
		const QSignalBlocker b1(dbBtn_), b2(lufsBtn_), b3(tpBox_), b4(refBox_), b5(refSpin_);
		dbBtn_->setChecked(!lufs);
		lufsBtn_->setChecked(lufs);
		tpBox_->setChecked(truePeak_);
		refBox_->setChecked(refOn_);
		refSpin_->setValue(ref_);
	}
	refSpin_->setEnabled(refOn_);
	modeBar_->setVisible(open_);
	lufsOpts_->setVisible(lufs);
	for (Row *r : rows_) {
		r->tpL->setVisible(lufs && truePeak_);
		r->bar->setLufsMode(lufs);
		// The LUFS labels are longer; the tips explain them in plain words (dB mode's labels need none).
		const int nw = lufs ? numWLufs_ : numWDb_;
		r->peakL->setFixedWidth(nw);
		r->avgL->setFixedWidth(nw);
		r->peakL->setToolTip(lufs ? trk("InFlight.Levels.NowTip", "Loudness over the last 0.4 seconds (EBU momentary)")
					  : QString());
		r->avgL->setToolTip(lufs ? trk("InFlight.Levels.AvgTip",
					       "Average loudness over the last 3 seconds (EBU short-term). Best number for judging your mix.")
					 : QString());
	}
	toggle_->setToolTip(lufs ? trk("InFlight.Levels.TipLufs", "Momentary and short-term loudness of each source, in LUFS")
				 : trk("InFlight.Levels.Tip", "Peak and average level of each source, in dBFS"));
}

void LevelsPanel::setMode(LevelsMode m, bool notify)
{
	if (m == mode_) {
		applyModeUi(); // a click on the already-checked button: keep the pair consistent
		return;
	}
	if (running_)
		stopBackend();
	mode_ = m;
	if (!demo_)
		resetRowsForBackend();
	applyModeUi();
	const qint64 t = nowMs();
	for (Row *r : rows_)
		refreshRow(*r, t);
	refreshGaps();
	if (running_) {
		tickCount_ = 0;
		startMs_ = nowMs();
		startBackend();
		fastTick(); // the new backend shows its first numbers as soon as it has them
	}
	obs_log(LOG_INFO, "levels: mode %s", levelsModeKey(m));
	if (notify)
		emit modeChanged(m == LevelsMode::Lufs);
}

void LevelsPanel::setTruePeak(bool on, bool notify)
{
	if (on == truePeak_) {
		applyModeUi();
		return;
	}
	truePeak_ = on;
	if (lufs_ && lufs_->running())
		lufs_->setTruePeak(on);
	applyModeUi();
	const qint64 t = nowMs();
	for (Row *r : rows_)
		refreshRowText(*r, t);
	obs_log(LOG_INFO, "levels: true peak %s", on ? "on" : "off");
	if (notify)
		emit truePeakChanged(on);
}

void LevelsPanel::setLufsRef(bool on, double lufs, bool notify)
{
	const double v = clampLufsRef(lufs);
	if (on == refOn_ && v == ref_) {
		applyModeUi();
		return;
	}
	refOn_ = on;
	ref_ = v;
	applyModeUi();
	const qint64 t = nowMs();
	for (Row *r : rows_)
		refreshBar(*r, t);
	if (notify)
		emit lufsRefChanged(refOn_, ref_);
}

// The fast beat in LUFS mode (20 Hz): copy each row's numbers from the worker and draw the bars. The text only
// changes here when a row's state flips (its first reading, or it lost it); the rest is the text beat's.
void LevelsPanel::lufsFastTick()
{
	if (!running_ || mode_ != LevelsMode::Lufs || !lufs_)
		return;
	if (wantedDirty_) {
		wantedDirty_ = false;
		lufs_->start(rowUuids(), truePeak_); // the row list changed: new rings, same worker design
	}
	const qint64 t = nowMs();
	bool stateChanged = false;
	for (int i = 0; i < rows_.size(); ++i) {
		Row *r = rows_[i];
		const bool wasMissing = r->missing;
		const bool wasWarm = r->warm;
		if (!lufs_->attached(i)) {
			r->haveFed = false;
			r->lr = LufsResult();
			classify(*r, false);
		} else {
			r->exists = true;
			r->lr = lufs_->read(i);
			r->haveFed = r->lr.active; // no audio for 1.5 s is "--", never a silent-looking number
			classify(*r, true);
		}
		if (wasMissing != r->missing || wasWarm != r->warm) {
			stateChanged = true;
			refreshRowText(*r, t);
		}
		refreshBar(*r, t);
	}
	if (stateChanged)
		refreshGaps();
}

// The text beat in LUFS mode (5 Hz): format change check, late sources, names, numbers, gap lines.
void LevelsPanel::lufsTextTick()
{
	if (!running_ || mode_ != LevelsMode::Lufs || !lufs_)
		return;
	const qint64 t = nowMs();
	lufs_->restartIfFormatChanged(); // Settings > Audio applied while running
	lufs_->retryUnattached();        // a source created after the panel started
	lufs_->refreshGains();           // fallback for the sources' "volume" / "mute" signals
	for (int i = 0; i < rows_.size(); ++i) {
		Row *r = rows_[i];
		if (!lufs_->attached(i)) {
			resolveRow(*r);
			classify(*r, false);
		} else {
			const QString n = lufs_->sourceName(i);
			if (!n.isEmpty())
				r->name = n;
		}
		refreshRowText(*r, t);
	}
	refreshGaps();
	if (++tickCount_ % 5 == 0)
		refillAdd();
}

void LevelsPanel::armLufsTestLog()
{
	lufsLogArmed_ = true;
	if (running_ && mode_ == LevelsMode::Lufs && lufs_ && lufs_->running() && !lufsLogDone_ && !lufsLog_.isActive())
		lufsLog_.start(5000);
}

// Test hook: once, a few seconds after the LUFS backend started.
void LevelsPanel::lufsLogNow()
{
	if (closed_ || !running_ || mode_ != LevelsMode::Lufs || !lufs_ || lufsLogDone_)
		return;
	lufsLogDone_ = true;
	auto num = [](bool ready, double v) {
		if (!ready)
			return QStringLiteral("--");
		if (std::isinf(v))
			return v < 0 ? QStringLiteral("-inf") : QStringLiteral("inf");
		return QString::number(v, 'f', 2);
	};
	for (const Row *r : rows_) {
		if (r->missing) {
			obs_log(LOG_INFO, "lufs: (missing source) M=-- S=-- TP=--");
			continue;
		}
		const QString tp = truePeak_ ? num(r->lr.tpValid, r->lr.tpMax) : QStringLiteral("off");
		const QByteArray line = QStringLiteral("lufs: %1 M=%2 S=%3 TP=%4")
						.arg(r->name, num(r->lr.momentaryReady, r->lr.momentary),
						     num(r->lr.shortReady, r->lr.shortTerm), tp)
						.toUtf8();
		obs_log(LOG_INFO, "%s", line.constData());
	}
	const LoudnessWorker::Cost c = lufs_->workerCost();
	obs_log(LOG_INFO, "lufs: worker mean=%.1f us max=%.1f us per 10 ms n=%llu", c.meanUs, c.maxUs,
		static_cast<unsigned long long>(c.iterations));
	for (int i = 0; i < rows_.size() && i < lufs_->slotCount(); i++) {
		const QByteArray nm = rows_[i]->name.toUtf8();
		obs_log(LOG_INFO, "lufs: drops %s blocks=%llu frames=%llu", nm.constData(),
			static_cast<unsigned long long>(lufs_->read(i).droppedBlocks),
			static_cast<unsigned long long>(lufs_->read(i).droppedFrames));
	}
	obs_log(LOG_INFO, "lufs: ring overflow=%llu", static_cast<unsigned long long>(lufs_->overflowBlocks()));
	startLufsVolumeTest();
}

// Test hook (part of RWS_INFLIGHT_TEST_LUFS, UI thread): halves the first row's source volume with
// obs_source_set_volume (-6.02 dB) and logs its M 1 s later and M and S 3.8 s later (S is a 3 s window), then
// puts the volume back to 1.0. Proves the signal-driven gain path: the capture callback never reads libobs's
// volume itself.
void LevelsPanel::startLufsVolumeTest()
{
	if (rows_.isEmpty() || !lufs_ || !lufs_->attached(0))
		return;
	const QString uuid = rows_[0]->uuid;
	const QString nm = rows_[0]->name;
	obs_source_t *s = obs_get_source_by_uuid(uuid.toUtf8().constData());
	if (!s)
		return;
	obs_source_set_volume(s, 0.5f);
	obs_source_release(s);
	obs_log(LOG_INFO, "lufs: volume-move %s set to 0.5", nm.toUtf8().constData());
	auto fmt = [](const LufsResult &r, bool shortToo) {
		auto num = [](bool ok, double v) {
			return !ok ? QStringLiteral("--") : (std::isinf(v) ? QStringLiteral("-inf") : QString::number(v, 'f', 2));
		};
		QString t = QStringLiteral("M=%1").arg(num(r.momentaryReady, r.momentary));
		if (shortToo)
			t += QStringLiteral(" S=%1").arg(num(r.shortReady, r.shortTerm));
		return t;
	};
	QTimer::singleShot(1000, this, [this, nm, fmt] {
		if (closed_ || !lufs_ || !lufs_->running())
			return;
		obs_log(LOG_INFO, "lufs: volume-move %s +1s %s", nm.toUtf8().constData(),
			fmt(lufs_->read(0), false).toUtf8().constData());
	});
	QTimer::singleShot(3800, this, [this, nm, uuid, fmt] {
		if (!closed_ && lufs_ && lufs_->running())
			obs_log(LOG_INFO, "lufs: volume-move %s +3.8s %s", nm.toUtf8().constData(),
				fmt(lufs_->read(0), true).toUtf8().constData());
		obs_source_t *s2 = obs_get_source_by_uuid(uuid.toUtf8().constData());
		if (s2) {
			obs_source_set_volume(s2, 1.0f);
			obs_source_release(s2);
			obs_log(LOG_INFO, "lufs: volume-move %s restored to 1.0", nm.toUtf8().constData());
		}
	});
}

void LevelsPanel::demoSetLufs(const QString &uuid, const QString &name, bool mReady, double momentary, bool sReady,
			      double shortTerm, double tpDbtp)
{
	for (Row *r : rows_) {
		if (r->uuid != uuid)
			continue;
		r->name = name;
		r->missing = false;
		r->warm = false;
		r->lr = LufsResult();
		r->lr.active = true;
		r->lr.momentaryReady = mReady;
		r->lr.momentary = momentary;
		r->lr.shortReady = sReady;
		r->lr.shortTerm = shortTerm;
		r->lr.tpValid = !std::isnan(tpDbtp);
		r->lr.tpMax = tpDbtp;
		refreshRow(*r, 0);
	}
	refreshGaps();
}

// The reference value as the user typed it, without a trailing ".0": "-16", "-16.5".
QString LevelsPanel::refLabelText() const
{
	QString t = QString::number(ref_, 'f', 1);
	if (t.endsWith(QStringLiteral(".0")))
		t.chop(2);
	return t == QStringLiteral("-0") ? QStringLiteral("0") : t;
}

// "?" clicked. The host (the Kit dock) knows where its manual lives; without one the web manual is the answer.
void LevelsPanel::openHelp()
{
	if (helpOpener_) {
		helpOpener_(QStringLiteral("levels"));
		return;
	}
	const QUrl url{QString::fromLatin1(kLevelsManualUrl)};
	if (helpLogOnly_) {
		obs_log(LOG_INFO, "levels: help would open %s", url.toString(QUrl::FullyEncoded).toUtf8().constData());
		return;
	}
	QDesktopServices::openUrl(url);
}

void LevelsPanel::testClickHelp()
{
	if (helpBtn_)
		helpBtn_->click();
}

// Test hook: what the first row shows right now, one log line. ref='none' = no reference label drawn.
void LevelsPanel::logTestState(const char *tag) const
{
	const Row *r = rows_.isEmpty() ? nullptr : rows_.first();
	if (!r) {
		obs_log(LOG_INFO, "levels-state %s: no rows", tag);
		return;
	}
	const QString ref = r->bar->refText().isEmpty() ? QStringLiteral("none") : r->bar->refText();
	const QByteArray line = QStringLiteral("levels-state %1: mode=%2 now='%3' avg='%4' nowTip='%5' avgTip='%6' tpTip='%7' ref='%8' "
					       "refWord=%9 help='%10' helpTip='%11'")
					.arg(QString::fromLatin1(tag), QString::fromLatin1(levelsModeKey(mode_)), r->peakL->text(), r->avgL->text(),
					     r->peakL->toolTip(), r->avgL->toolTip(), r->tpL->toolTip(), ref,
					     refBox_->styleSheet().contains(QLatin1String(kRefColorCss)) ? QStringLiteral("line-colour")
												      : QStringLiteral("plain"))
					.arg(helpBtn_->text(), helpBtn_->toolTip())
					.toUtf8();
	obs_log(LOG_INFO, "%s", line.constData());
}

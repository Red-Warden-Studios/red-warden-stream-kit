/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "lights-panel.hpp"
#include "kit-cost.hpp"

#include <QAbstractButton>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QSpacerItem>
#include <QToolButton>
#include <QVBoxLayout>

#include <utility>

using namespace inflight;

namespace {
const QColor kRed(0xC0, 0x39, 0x2B);   // geranium
const QColor kAmber(0xF2, 0xA6, 0x3B); // ember
const QColor kGreen(0x2E, 0x9E, 0x5B);
const QColor kDarkInk(0x20, 0x20, 0x20);
// Grey (off) and Unknown. A fixed mid-grey rather than the palette's Mid, which in OBS's dark themes is
// nearly the background colour and would make the light invisible.
const QColor kNeutral(0x8A, 0x8F, 0x98);

// A coloured circle with a glyph, so state never depends on colour alone. `hollow` draws just the ring
// (the "off" half of the one-time red flash).
void drawMark(QPainter &p, const QRectF &r, LightState st, const QPalette &pal, bool hollow)
{
	QColor fill;
	QColor ink(Qt::white);
	QString sym;
	switch (st) {
	case LightState::Green:
		fill = kGreen;
		sym = QString(QChar(0x2713));
		break;
	case LightState::Amber:
		fill = kAmber;
		ink = kDarkInk;
		sym = QStringLiteral("!");
		break;
	case LightState::Red:
		fill = kRed;
		sym = QString(QChar(0x00D7));
		break;
	case LightState::Unknown:
		fill = kNeutral;
		ink = kDarkInk;
		sym = QStringLiteral("?");
		break;
	case LightState::Grey:
		fill = kNeutral;
		hollow = true; // off: just a ring, no glyph
		break;
	}
	Q_UNUSED(pal);
	p.setRenderHint(QPainter::Antialiasing);
	const QRectF c = r.adjusted(1, 1, -1, -1);
	if (hollow) {
		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(fill, 2));
		p.drawEllipse(c.adjusted(1, 1, -1, -1));
		ink = fill;
	} else {
		p.setPen(Qt::NoPen);
		p.setBrush(fill);
		p.drawEllipse(c);
	}
	if (!sym.isEmpty()) {
		QFont f = p.font();
		f.setBold(true);
		f.setPixelSize(qRound(r.height() * 0.62));
		p.setFont(f);
		p.setPen(ink);
		p.drawText(r, Qt::AlignCenter, sym);
	}
}

QFrame *makeCard(QWidget *parent, const char *name)
{
	auto *f = new QFrame(parent);
	f->setObjectName(QString::fromLatin1(name));
	// A translucent grey wash, so it reads on both light and dark themes (palette(alternate-base) is
	// light under OBS's dark theme while the text is light too).
	f->setStyleSheet(QStringLiteral("#%1 { background: rgba(128, 128, 128, 40); border: 1px solid rgba(128, 128, 128, 110);"
					" border-radius: 4px; }")
				 .arg(QString::fromLatin1(name)));
	return f;
}
} // namespace

// The header's dot.
class HeaderMark : public QWidget {
public:
	explicit HeaderMark(QWidget *parent) : QWidget(parent) { setFixedSize(22, 22); }
	void setState(LightState s)
	{
		if (init_ && s == state_)
			return;
		init_ = true;
		state_ = s;
		update();
	}
	LightState state() const { return state_; }

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		drawMark(p, QRectF(rect()), state_, palette(), false);
	}

private:
	LightState state_ = LightState::Grey;
	bool init_ = false;
};

// One light: a circle with a glyph and a short label. Click to open its details.
class LightButton : public QAbstractButton {
public:
	explicit LightButton(const QString &label, QWidget *parent) : QAbstractButton(parent), label_(label)
	{
		setCursor(Qt::PointingHandCursor);
		setFocusPolicy(Qt::TabFocus);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	}

	void setView(LightState st, bool snoozed, bool acked, bool selected, const QString &tip)
	{
		if (init_ && st == state_ && snoozed == snoozed_ && acked == acked_ && selected == selected_ &&
		    tip == toolTip())
			return;
		init_ = true;
		state_ = st;
		snoozed_ = snoozed;
		acked_ = acked;
		selected_ = selected;
		setToolTip(tip);
		setAccessibleName(tip);
		update();
	}
	void setFlashOff(bool off)
	{
		if (off == flashOff_)
			return;
		flashOff_ = off;
		update();
	}

	QSize sizeHint() const override { return QSize(46, 52); }
	QSize minimumSizeHint() const override { return QSize(38, 52); }

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		if (selected_) {
			QColor hl = palette().color(QPalette::Highlight);
			QColor bg = hl;
			bg.setAlpha(45);
			p.setPen(QPen(hl, 1));
			p.setBrush(bg);
			p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 5, 5);
		} else if (underMouse() || hasFocus()) {
			p.setPen(QPen(palette().color(QPalette::Mid), 1));
			p.setBrush(Qt::NoBrush);
			p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 5, 5);
		}
		const qreal d = 28;
		const QRectF circle((width() - d) / 2.0, 5, d, d);
		p.save();
		if (snoozed_)
			p.setOpacity(0.5); // dimmed, the colour and glyph stay
		drawMark(p, circle, state_, palette(), flashOff_);
		p.restore();
		if (acked_) {
			// A small ticked badge at the lower right of the circle.
			const qreal b = 12;
			const QRectF badge(circle.right() - b + 3, circle.bottom() - b + 3, b, b);
			p.setPen(QPen(kDarkInk, 1));
			p.setBrush(Qt::white);
			p.drawEllipse(badge);
			QFont f = p.font();
			f.setBold(true);
			f.setPixelSize(9);
			p.setFont(f);
			p.drawText(badge, Qt::AlignCenter, QString(QChar(0x2713)));
		}
		QFont f = font();
		f.setPixelSize(11);
		p.setFont(f);
		p.setPen(palette().color(QPalette::WindowText));
		const QRect tr(0, 38, width(), 13);
		p.drawText(tr, Qt::AlignHCenter | Qt::AlignVCenter,
			   QFontMetrics(f).elidedText(label_, Qt::ElideRight, width() - 2));
	}

private:
	QString label_;
	LightState state_ = LightState::Grey;
	bool snoozed_ = false;
	bool acked_ = false;
	bool selected_ = false;
	bool flashOff_ = false;
	bool init_ = false;
};

LightsPanel::LightsPanel(TrFn tr, QWidget *parent) : QWidget(parent), tr_(std::move(tr))
{
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(0, 0, 0, 0);
	root->setSpacing(6);

	auto *header = new QHBoxLayout();
	header->setSpacing(8);
	mark_ = new HeaderMark(this);
	header->addWidget(mark_, 0, Qt::AlignVCenter);
	headerLabel_ = new QLabel(tr_("InFlight.Status.Standing", "Standing by"), this);
	QFont hf = headerLabel_->font();
	hf.setBold(true);
	headerLabel_->setFont(hf);
	header->addWidget(headerLabel_, 1);
	auto *gear = new QToolButton(this);
	gear->setObjectName(QStringLiteral("ifGear"));
	gear->setAutoRaise(true);
	gear->setText(QString(QChar(0x2699))); // U+2699 GEAR
	QFont gf = gear->font();
	gf.setPixelSize(18);
	gear->setFont(gf);
	gear->setCursor(Qt::PointingHandCursor);
	gear->setToolTip(tr_("InFlight.Settings.GearTip", "In-Flight settings"));
	gear->setAccessibleName(tr_("InFlight.Settings.GearTip", "In-Flight settings"));
	connect(gear, &QToolButton::clicked, this, [this] { emit settingsRequested(); });
	header->addWidget(gear, 0, Qt::AlignVCenter);
	root->addLayout(header);

	auto *row = new QHBoxLayout();
	row->setSpacing(2);
	for (int i = 0; i < kLightCount; i++) {
		auto *b = new LightButton(lightShortName(static_cast<Light>(i), tr_), this);
		buttons_[i] = b;
		row->addWidget(b, 1);
		connect(b, &QAbstractButton::clicked, this, [this, i] { toggleDetails(static_cast<Light>(i)); });
	}
	root->addLayout(row);

	details_ = makeCard(this, "ifDetails");
	{
		auto *dl = new QVBoxLayout(details_);
		dl->setContentsMargins(10, 8, 10, 8);
		dl->setSpacing(4);
		auto *top = new QHBoxLayout();
		dName_ = new QLabel(details_);
		QFont nf = dName_->font();
		nf.setBold(true);
		dName_->setFont(nf);
		dState_ = new QLabel(details_);
		dState_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		top->addWidget(dName_, 1);
		top->addWidget(dState_, 0);
		dl->addLayout(top);
		dWhat_ = new QLabel(details_);
		dWhat_->setWordWrap(true);
		dl->addWidget(dWhat_);
		dSince_ = new QLabel(details_);
		dl->addWidget(dSince_);
		dFix_ = new QLabel(details_);
		dFix_->setWordWrap(true);
		dl->addWidget(dFix_);
		dNet_ = new QLabel(details_);
		dNet_->setWordWrap(true);
		dNet_->setVisible(false);
		dl->addWidget(dNet_);
		auto *btns = new QHBoxLayout();
		btns->setSpacing(6);
		// The one primary action of the Network details when a Multistream destination is the problem. It sits
		// first, in the accent colour; the text carries the meaning, the colour is only emphasis.
		turnOffBtn_ = new QPushButton(details_);
		turnOffBtn_->setObjectName(QStringLiteral("ifTurnOff"));
		turnOffBtn_->setDefault(true);
		turnOffBtn_->setCursor(Qt::PointingHandCursor);
		turnOffBtn_->setStyleSheet(QStringLiteral("QPushButton { background:%1; color:white; border:1px solid %1; border-radius:4px;"
							  " padding:4px 10px; font-weight:600; }"
							  "QPushButton:hover { background:%2; }")
						   .arg(kRed.name(), kRed.darker(115).name()));
		turnOffBtn_->setVisible(false);
		dl->addWidget(turnOffBtn_);
		ackBtn_ = new QPushButton(tr_("InFlight.Acknowledge", "Acknowledge"), details_);
		snoozeBtn_ = new QPushButton(tr_("InFlight.Snooze", "Snooze 10 min"), details_);
		btns->addWidget(ackBtn_);
		btns->addWidget(snoozeBtn_);
		btns->addStretch(1);
		dl->addLayout(btns);
		connect(turnOffBtn_, &QPushButton::clicked, this, [this] {
			if (selected_ == static_cast<int>(Light::Network) && canTurnOffOutput(last_[selected_]))
				emit turnOffRequested(last_[selected_].net.outputId, last_[selected_].net.outputName);
		});
		connect(ackBtn_, &QPushButton::clicked, this, [this] {
			if (selected_ >= 0)
				emit acknowledgeRequested(static_cast<Light>(selected_));
		});
		connect(snoozeBtn_, &QPushButton::clicked, this, [this] {
			if (selected_ >= 0)
				emit snoozeRequested(static_cast<Light>(selected_));
		});
	}
	details_->setVisible(false);
	root->addWidget(details_);

	logToggle_ = new QToolButton(this);
	logToggle_->setAutoRaise(true);
	logToggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	logToggle_->setArrowType(Qt::RightArrow);
	logToggle_->setText(tr_("InFlight.SessionLog", "Session log (%1)").arg(0));
	root->addWidget(logToggle_, 0, Qt::AlignLeft);

	log_ = new QListWidget(this);
	log_->setWordWrap(true);
	log_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	log_->setSelectionMode(QAbstractItemView::NoSelection);
	log_->setFocusPolicy(Qt::NoFocus);
	log_->setMinimumHeight(110);
	log_->setVisible(false);
	root->addWidget(log_, 1);
	root->addSpacerItem(new QSpacerItem(0, 0, QSizePolicy::Minimum, QSizePolicy::Expanding));

	connect(logToggle_, &QToolButton::clicked, this, [this] { setLogExpanded(!log_->isVisibleTo(this)); });

	flashLeft_.fill(0);
	flashTimer_.setInterval(150);
	connect(&flashTimer_, &QTimer::timeout, this, [this] { kitcost::Scope costScope(kitcost::LightsFlash); flashTick(); });
}

LightsPanel::~LightsPanel() = default;

void LightsPanel::insertSection(QWidget *w)
{
	auto *root = static_cast<QVBoxLayout *>(layout());
	w->setParent(this);
	root->insertWidget(root->indexOf(logToggle_), w);
}

QString LightsPanel::whatFor(const LightResult &r) const
{
	return reasonTextFor(r, tr_).what;
}

void LightsPanel::showDetails(Light l)
{
	if (selected_ != static_cast<int>(l))
		toggleDetails(l);
}

bool LightsPanel::turnOffVisible() const
{
	return turnOffBtn_ && turnOffBtn_->isVisibleTo(this) && details_->isVisibleTo(this);
}

QString LightsPanel::turnOffLabel() const
{
	return turnOffBtn_ ? turnOffBtn_->text() : QString();
}

bool LightsPanel::testClickTurnOff()
{
	if (!turnOffVisible())
		return false;
	turnOffBtn_->click();
	return true;
}

void LightsPanel::setLogExpanded(bool expanded)
{
	logToggle_->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
	log_->setVisible(expanded);
}

void LightsPanel::toggleDetails(Light l)
{
	const int i = static_cast<int>(l);
	selected_ = (selected_ == i) ? -1 : i;
	details_->setVisible(selected_ >= 0);
	for (int k = 0; k < kLightCount; k++)
		refreshLight(k, last_[k], lastNow_);
	refreshDetails(lastNow_);
}

void LightsPanel::refreshLight(int i, const LightResult &r, qint64 nowMs)
{
	const Light l = static_cast<Light>(i);
	const bool snoozed = r.snoozedUntilMs > nowMs;
	QString tip = QStringLiteral("%1: %2 - %3")
			      .arg(lightName(l, tr_), stateName(r.state, tr_), whatFor(r));
	if (r.acknowledged)
		tip += QStringLiteral(" (%1)").arg(tr_("InFlight.AckedTag", "acknowledged"));
	else if (snoozed)
		tip += QStringLiteral(" (%1)").arg(tr_("InFlight.SnoozedTag", "snoozed"));
	buttons_[i]->setView(r.state, snoozed, r.acknowledged, selected_ == i, tip);
}

void LightsPanel::refreshDetails(qint64 nowMs)
{
	if (selected_ < 0)
		return;
	const LightResult &r = last_[selected_];
	const Light l = static_cast<Light>(selected_);
	const bool snoozed = r.snoozedUntilMs > nowMs;
	const ReasonText t = reasonTextFor(r, tr_);

	auto setIf = [](QLabel *lab, const QString &s) {
		if (lab->text() != s)
			lab->setText(s);
	};
	setIf(dName_, lightName(l, tr_));
	QString st = stateName(r.state, tr_);
	if (r.acknowledged)
		st += QStringLiteral(" (%1)").arg(tr_("InFlight.AckedTag", "acknowledged"));
	else if (snoozed)
		st += QStringLiteral(" (%1)").arg(tr_("InFlight.SnoozedTag", "snoozed"));
	setIf(dState_, st);
	setIf(dWhat_, t.what);
	// "Since hh:mm:ss": the incident's start on the wall clock, via the same offset the log uses.
	const bool showSince = r.incidentId != 0 && r.sinceMs > 0;
	dSince_->setVisible(showSince);
	if (showSince)
		setIf(dSince_, tr_("InFlight.Details.Since", "Since %1").arg(wallTime(r.sinceMs)));
	setIf(dFix_, t.fix);
	// Network: this session's figure for the worst output and who else is fine, and the button to turn a
	// Multistream destination off (only while one is the problem: the incident exists and names a destination).
	const QString netLine = l == Light::Network ? networkSessionLine(r, tr_) : QString();
	dNet_->setVisible(!netLine.isEmpty());
	setIf(dNet_, netLine);
	const bool offable = l == Light::Network && canTurnOffOutput(r);
	turnOffBtn_->setVisible(offable);
	if (offable) {
		const QString label = turnOffText(r.net.outputName, tr_);
		if (turnOffBtn_->text() != label)
			turnOffBtn_->setText(label);
	}
	const bool open = r.incidentId != 0;
	ackBtn_->setEnabled(open && !r.acknowledged);
	snoozeBtn_->setEnabled(open && !r.acknowledged && !snoozed);
}

void LightsPanel::startFlash(int i)
{
	if (closed_)
		return;
	flashLeft_[i] = 6; // off, on, off, on, off, on, then steady
	buttons_[i]->setFlashOff(true);
	if (!flashTimer_.isActive())
		flashTimer_.start();
}

void LightsPanel::flashTick()
{
	if (closed_) {
		flashTimer_.stop();
		return;
	}
	bool any = false;
	for (int i = 0; i < kLightCount; i++) {
		if (flashLeft_[i] <= 0)
			continue;
		flashLeft_[i]--;
		buttons_[i]->setFlashOff(flashLeft_[i] > 0 && flashLeft_[i] % 2 == 0);
		if (flashLeft_[i] > 0)
			any = true;
	}
	if (!any)
		flashTimer_.stop();
}

void LightsPanel::clearLog()
{
	log_->clear();
	logCount_ = 0;
	logToggle_->setText(tr_("InFlight.SessionLog", "Session log (%1)").arg(0));
}

void LightsPanel::addLogLine(const QString &line)
{
	log_->insertItem(0, line);
	while (log_->count() > kMaxLog)
		delete log_->takeItem(log_->count() - 1);
	logCount_ = log_->count();
	logToggle_->setText(tr_("InFlight.SessionLog", "Session log (%1)").arg(logCount_));
}

// Wall-clock choice: the event's atMs is on the controller's steady clock. Each snapshot arrives with
// the steady nowMs it was taken at, so the offset (wall now minus steady now) is captured right here, per
// snapshot, and an event's wall time is offset + atMs. Events are at most one tick old, so the error is
// well under a second, and a system clock change only affects events logged afterwards.
void LightsPanel::appendEvents(const Snapshot &s, qint64 nowMs)
{
	Q_UNUSED(nowMs);
	for (const IncidentEvent &e : s.events) {
		const QString when = wallTime(e.atMs);
		// A cache miss (an incident that started and ended inside one snapshot) has no value to show,
		// so it gets the value-free label, never reasonText with an invented 0.
		// A Network incident on a Multistream destination always names it, even from the value-free label; an
		// incident that ended because the destination was switched off or removed says so instead of "ended".
		QString what = openWhat_.contains(e.incidentId) ? openWhat_.value(e.incidentId) : reasonLabel(e.reason, tr_);
		if (!openWhat_.contains(e.incidentId) && !e.outputName.isEmpty())
			what += QStringLiteral(" (%1)").arg(e.outputName);
		QString kind = eventKindText(e.kind, tr_);
		if (e.kind == IncidentEventKind::Ended && e.turnedOff)
			kind = tr_("InFlight.Event.EndedOff", "ended (turned off)");
		addLogLine(QStringLiteral("%1  %2  %3  %4").arg(when, lightName(e.light, tr_), kind, what));
	}
}

QString LightsPanel::wallTime(qint64 steadyMs) const
{
	return QDateTime::fromMSecsSinceEpoch(wallOffset_ + steadyMs).toString(QStringLiteral("HH:mm:ss"));
}

void LightsPanel::shutdown()
{
	closed_ = true;
	flashTimer_.stop();
	flashLeft_.fill(0);
	for (LightButton *b : buttons_)
		b->setFlashOff(false);
}

void LightsPanel::setSnapshot(const Snapshot &s, LiveState live, qint64 nowMs)
{
	if (closed_)
		return;
	wallOffset_ = QDateTime::currentMSecsSinceEpoch() - nowMs;
	// Remember the wording of every open incident first, so the log lines for its events (including
	// "ended", when the light no longer shows it) can say what it was.
	for (const LightResult &r : s.lights) {
		if (r.incidentId != 0)
			openWhat_.insert(r.incidentId, whatFor(r));
	}
	appendEvents(s, nowMs);
	for (auto it = openWhat_.begin(); it != openWhat_.end();) {
		bool open = false;
		for (const LightResult &r : s.lights)
			open = open || r.incidentId == it.key();
		if (open)
			++it;
		else
			it = openWhat_.erase(it);
	}

	mark_->setState(headerState(s));
	const QString ht = headerText(live, s, tr_);
	if (headerLabel_->text() != ht)
		headerLabel_->setText(ht);

	bool anyNewRed = false;
	last_ = s.lights;
	lastNow_ = nowMs;
	haveSnapshot_ = true;
	for (int i = 0; i < kLightCount; i++) {
		refreshLight(i, s.lights[i], nowMs);
		if (s.lights[i].flashNow) {
			anyNewRed = true;
			startFlash(i);
		}
	}
	refreshDetails(nowMs);
	if (anyNewRed)
		emit newRed();
}

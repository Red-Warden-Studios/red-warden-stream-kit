/*
Red Warden Stream Kit
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "kit-dock.hpp"

#include "dock-ui.hpp"
#include "inflight-dock.hpp"
#include "kit-cost.hpp"
#include "kit-paths.hpp"
#include "kit-settings-dialog.hpp"
#include "levels-panel.hpp"
#include "multistream-dock.hpp"
#include "preflight-dock.hpp"

#include <obs-module.h>
#include <obs.h>
#include <plugin-support.h>
#include <util/bmem.h>
#include <util/config-file.h>

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QDesktopServices>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QPainter>
#include <QPalette>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QMap>
#include <QScrollBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <chrono>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>

using kitstage::Stage;

namespace {

const char *kKitPageUrl = "https://redwardenstudios.com/division/bastion/stream-kit/";
// The online manual: where "?" goes when no manual folder sits beside the plugin.
const char *kManualWebUrl = "https://redwardenstudios.com/division/bastion/stream-kit/manual/";

QColor mixc(const QColor &a, const QColor &b, double t)
{
	const float f = (float)t;
	return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * f, a.greenF() + (b.greenF() - a.greenF()) * f,
				a.blueF() + (b.blueF() - a.blueF()) * f);
}

// Test hooks only in an OBS that runs from a .testbed folder (same rule as Multistream's hooks): an
// environment variable alone can never change a real user's OBS.
bool testHooksEnabled()
{
	static const bool on =
		QCoreApplication::applicationDirPath().contains(QStringLiteral("/.testbed/"), Qt::CaseInsensitive);
	return on;
}

// Test-only CPU cost counters (RWS_KIT_TEST_COST=1, testbed OBS only): one "kit cost:" line per counter, see kit-cost.hpp.
void logKitCost()
{
	for (int i = 0; i < kitcost::SlotCount; i++) {
		const kitcost::Counter &c = kitcost::counters()[i];
		obs_log(LOG_INFO, "kit cost: %s calls=%llu total_us=%llu max_us=%llu", kitcost::slotName(i),
			(unsigned long long)c.calls.load(), (unsigned long long)(c.totalNs.load() / 1000),
			(unsigned long long)(c.maxNs.load() / 1000));
	}
}

// Multistream's own view of its destinations -> what In-Flight's evaluator judges (the two tools know nothing of each other;
// the Kit is where they meet).
std::vector<inflight::OutputSample> toOutputSamples(const std::vector<MultistreamDock::OutputSnapshot> &in)
{
	using S = MultistreamDock::OutputSnapshot::State;
	std::vector<inflight::OutputSample> out;
	out.reserve(in.size());
	for (const MultistreamDock::OutputSnapshot &s : in) {
		inflight::OutputSample o;
		o.id = s.id;
		o.name = s.name;
		o.state = s.state == S::Live          ? inflight::OutputState::Live
			  : s.state == S::Reconnecting ? inflight::OutputState::Reconnecting
			  : s.state == S::Other        ? inflight::OutputState::Other
						       : inflight::OutputState::Off;
		o.dropped = s.dropped;
		o.total = s.total;
		o.generation = s.serial;
		out.push_back(std::move(o));
	}
	return out;
}

kitstage::Tools toolsFor(const KitDock::Panels &p)
{
	kitstage::Tools t;
	t.preflight = p.preflight != nullptr;
	t.inflight = p.inflight != nullptr;
	t.postflight = false; // Kit 1.2.0
	return t;
}

kitstage::Options optionsFor(const kitsettings::Settings &s)
{
	kitstage::Options o;
	o.autoSwitchOnLive = s.autoSwitchOnLive;
	return o;
}

QString stageName(Stage s)
{
	switch (s) {
	case Stage::PreFlight:
		return QStringLiteral("Pre-Flight");
	case Stage::InFlight:
		return QStringLiteral("In-Flight");
	case Stage::PostFlight:
		return QStringLiteral("Post-Flight");
	}
	return QString();
}

// A gear drawn by hand: U+2699 has no glyph in OBS's default font and rendered blank (Pre-Flight found out).
QIcon gearIcon(const QColor &c, qreal dpr)
{
	const int px = 16;
	QPixmap pm(int(px * dpr), int(px * dpr));
	pm.setDevicePixelRatio(dpr);
	pm.fill(Qt::transparent);
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing);
	QPen pen(c, 1.7, Qt::SolidLine, Qt::RoundCap);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	const QPointF mid(px / 2.0, px / 2.0);
	p.drawEllipse(mid, 2.6, 2.6);
	for (int i = 0; i < 8; ++i) {
		p.save();
		p.translate(mid);
		p.rotate(i * 45.0);
		p.drawLine(QPointF(0, -5.0), QPointF(0, -7.0));
		p.restore();
	}
	return QIcon(pm);
}

} // namespace

// ---- the section header: chevron, title, one-line summary ------------------------------------------------------
class KitSectionHeader : public QAbstractButton {
public:
	explicit KitSectionHeader(QWidget *parent) : QAbstractButton(parent)
	{
		setCursor(Qt::PointingHandCursor);
		setFocusPolicy(Qt::StrongFocus);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	}
	void setTitle(const QString &t)
	{
		title_ = t;
		describe();
		update();
	}
	void setSummary(const QString &s)
	{
		summary_ = s;
		describe();
		update();
	}
	void setExpanded(bool e)
	{
		expanded_ = e;
		describe();
		update();
	}
	bool expanded() const { return expanded_; }
	QSize sizeHint() const override { return QSize(160, QFontMetrics(font()).height() + 20); }
	QSize minimumSizeHint() const override { return QSize(80, sizeHint().height()); }

protected:
	void paintEvent(QPaintEvent *) override
	{
		const ui::Theme t = ui::Theme::from(palette());
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		QColor bg = mixc(t.window, t.text, t.dark ? 0.09 : 0.07);
		if (underMouse() || isDown())
			bg = mixc(t.window, t.text, t.dark ? 0.14 : 0.11);
		p.fillRect(rect(), bg);
		// chevron
		const qreal cy = height() / 2.0;
		QPainterPath path;
		if (expanded_) {
			path.moveTo(12, cy - 2.5);
			path.lineTo(16, cy + 1.5);
			path.lineTo(20, cy - 2.5);
		} else {
			path.moveTo(14, cy - 4);
			path.lineTo(18, cy);
			path.lineTo(14, cy + 4);
		}
		p.setPen(QPen(t.secondary, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		p.setBrush(Qt::NoBrush);
		p.drawPath(path);
		// title
		QFont bold = font();
		bold.setBold(true);
		p.setFont(bold);
		const QFontMetrics bfm(bold);
		const int left = 30;
		p.setPen(t.text);
		const QRect titleRect(left, 0, width() - left - 8, height());
		p.drawText(titleRect, Qt::AlignVCenter | Qt::AlignLeft, title_);
		// summary, right aligned in what is left of the row
		const int used = bfm.horizontalAdvance(title_) + 14;
		const QRect sumRect(left + used, 0, width() - left - used - 12, height());
		if (sumRect.width() > 20) {
			QFont small = font();
			small.setPointSizeF(qMax(7.0, small.pointSizeF() - 1.0));
			p.setFont(small);
			p.setPen(t.secondary);
			p.drawText(sumRect, Qt::AlignVCenter | Qt::AlignRight,
				   QFontMetrics(small).elidedText(summary_, Qt::ElideRight, sumRect.width()));
		}
		if (hasFocus()) {
			p.setPen(QPen(palette().color(QPalette::Highlight), 1.5));
			p.setBrush(Qt::NoBrush);
			p.drawRect(QRectF(rect()).adjusted(1, 1, -1, -1));
		}
	}
	void enterEvent(QEnterEvent *e) override
	{
		QAbstractButton::enterEvent(e);
		update();
	}
	void leaveEvent(QEvent *e) override
	{
		QAbstractButton::leaveEvent(e);
		update();
	}

private:
	void describe()
	{
		setAccessibleName(QStringLiteral("%1, %2%3")
					  .arg(title_, expanded_ ? QStringLiteral("expanded") : QStringLiteral("collapsed"),
					       summary_.isEmpty() ? QString() : QStringLiteral(", ") + summary_));
	}
	QString title_, summary_;
	bool expanded_ = true;
};

// ---- KitDock ------------------------------------------------------------------------------------------------------

qint64 KitDock::nowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
		.count();
}

QString KitDock::clockText(qint64 ms)
{
	if (ms < 0)
		ms = 0;
	const qint64 secs = ms / 1000;
	return QStringLiteral("%1:%2:%3")
		.arg(secs / 3600)
		.arg((secs / 60) % 60, 2, 10, QLatin1Char('0'))
		.arg(secs % 60, 2, 10, QLatin1Char('0'));
}

static QString shotName(const std::string &what); // defined with the test hooks below

KitDock::KitDock(const Panels &panels, const kitsettings::Settings &settings, QWidget *parent)
	: QWidget(parent), panels_{panels.multistream, panels.preflight, panels.inflight}, settings_(settings), machine_(toolsFor(panels), optionsFor(settings))
{
	setObjectName(QStringLiteral("rwsKitDock"));
	if (testHooksEnabled() && qEnvironmentVariable("RWS_KIT_TEST_COST") == QStringLiteral("1")) {
		kitcost::setEnabled(true);
		auto *costLog = new QTimer(this);
		costLog->setInterval(60000);
		connect(costLog, &QTimer::timeout, this, [] { logKitCost(); });
		costLog->start();
		// A hidden testbed OBS never shows its dock, so the Levels gate would stay shut and the meters would never run: count the
		// dock as visible (the same override In-Flight's own Levels test uses). Only a panel that is open (seeded) is affected.
		QTimer::singleShot(2500, this, [this] {
			if (!closing_ && panels_.inflight && panels_.inflight->levelsPanel())
				panels_.inflight->levelsPanel()->setGateOverride(true);
		});
	}
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(0, 0, 0, 0);
	root->setSpacing(0);
	buildHeader(root);
	buildBody(root);

	if (panels_.inflight) {
		connect(panels_.inflight, &InflightDock::liveReport, this, &KitDock::onLiveReport);
		connect(panels_.inflight, &InflightDock::attentionChanged, this, &KitDock::onAttentionChanged);
		connect(panels_.inflight, &InflightDock::taskbarAlertRequested, this, &KitDock::onTaskbarAlert);
	}
	if (panels_.preflight) {
		connect(panels_.preflight, &PreflightDock::doneRequested, this, &KitDock::doDone);
		panels_.preflight->setDoneVisible(!panels_.inflight.isNull());
		connect(panels_.preflight, &PreflightDock::levelsRequested, this, [this] {
			if (closing_ || !panels_.inflight || !panels_.inflight->levelsPanel())
				return;
			LevelsPanel *lp = panels_.inflight->levelsPanel();
			lp->setOpen(!lp->isOpen(), true); // persists; openChanged refreshes the sections and the gate
			// Opened from the Pre-Flight button: scroll to it, or the meters (which run only while on screen) would
			// sit below the fold, off.
			if (lp->isOpen())
				QTimer::singleShot(60, this, [this] {
					if (!closing_ && levelsWrap_ && levelsWrap_->isVisible())
						scroll_->ensureWidgetVisible(levelsWrap_, 0, 12);
				});
		});
	}
	if (panels_.multistream) {
		msSummary_ = panels_.multistream->summary();
		msSection_.head->setSummary(msSummary_);
		connect(panels_.multistream, &MultistreamDock::summaryChanged, this, [this](const QString &s) {
			msSummary_ = s;
			if (msSection_.head)
				msSection_.head->setSummary(s);
		});
	}

	// Pre-Flight's Multistream check and In-Flight's live-state check ask Multistream directly, in this process, on their
	// own UI-thread timers (copied out, nothing held): no obs-websocket vendor call, no worker thread, nothing to wait for
	// at exit. Wired only when Multistream is loaded; without it the tools see "Multistream not found", as before.
	// destinationsNow() answers "not available" once Multistream is closing. QPointer: the lambdas must never outlive the panel.
	if (panels_.multistream) {
		QPointer<MultistreamDock> ms(panels_.multistream);
		if (panels_.preflight)
			panels_.preflight->setMultistreamSource(
				[ms]() -> std::optional<std::vector<preflight::MultistreamDestination>> {
					if (!ms)
						return std::nullopt;
					const auto list = ms->destinationsNow();
					if (!list)
						return std::nullopt;
					std::vector<preflight::MultistreamDestination> out;
					out.reserve(list->size());
					for (const auto &d : *list)
						out.push_back({d.id, d.name, d.enabled});
					return out;
				});
		if (panels_.inflight)
			panels_.inflight->setMultistreamSource([ms]() -> std::optional<QStringList> {
				if (!ms)
					return std::nullopt;
				const auto list = ms->destinationsNow();
				if (!list)
					return std::nullopt;
				QStringList states;
				for (const auto &d : *list)
					states << d.state;
				return states;
			});
	}

	// The Network light covers every outgoing stream: with both tools on, In-Flight reads Multistream's destinations
	// in this process on its own 1 Hz tick (UI thread, copied out, nothing held), can turn one off through the
	// destination's own switch, and tells Multistream which rows to badge. With Multistream switched off there is no
	// bridge and the main stream is judged alone. QPointer: the lambdas must never outlive the panel.
	if (panels_.inflight && panels_.multistream) {
		QPointer<MultistreamDock> ms(panels_.multistream);
		InflightController::OutputBridge bridge;
		bridge.read = [ms] {
			return ms ? toOutputSamples(ms->outputSnapshots()) : std::vector<inflight::OutputSample>();
		};
		bridge.turnOff = [ms](const QString &id) {
			if (ms)
				ms->setOn(id, false);
		};
		bridge.showDrops = [ms](const std::map<QString, double> &pctById) {
			if (ms)
				ms->setDropFigures(pctById);
		};
		panels_.inflight->setOutputBridge(std::move(bridge));
		obs_log(LOG_INFO, "network light: judging OBS's stream and every Multistream destination");
		// (RWS_KIT_TEST itself is NOT needed, and must stay unset: it would also start the Kit's own screenshot tour.)
		if (testHooksEnabled() && qEnvironmentVariableIsSet("RWS_KIT_TEST_NET") &&
		    !qEnvironmentVariableIsEmpty("RWS_KIT_TEST_SHOT_DIR"))
			connect(panels_.inflight, &InflightDock::testNetRed, this, [this] {
				// Offscreen grab (QWidget::grab) of the whole Kit dock while the Network light is red on a destination.
				QTimer::singleShot(400, this, [this] {
					if (!closing_)
						saveShot(shotName("net-red"));
				});
			});
	}

	// Test copy only: photograph the whole Kit dock (offscreen) when Pre-Flight's mic test numbers have just appeared.
	if (panels_.preflight && testHooksEnabled() && qEnvironmentVariableIsSet("RWS_KIT_TEST_MICSHOT") &&
	    !qEnvironmentVariableIsEmpty("RWS_KIT_TEST_SHOT_DIR"))
		connect(panels_.preflight, &PreflightDock::micNumbersShown, this, [this](bool shown) {
			if (!shown)
				return;
			QTimer::singleShot(700, this, [this] {
				if (!closing_)
					saveShot(shotName("mic-numbers"));
			});
		});

	tick_.setInterval(1000);
	tick_.setTimerType(Qt::CoarseTimer);
	connect(&tick_, &QTimer::timeout, this, [this] {
		kitcost::Scope costScope(kitcost::KitTick);
		onTick();
	});
	tick_.start();

	// ONE update check for the whole Kit, whichever tools are on. Well after OBS has finished starting; never
	// in the test copy (no network in the harness, no surprise notices). Scheduled only if updates are on
	// now (turning them on mid-session may wait until the next OBS start); when the timer fires the shared
	// preference is read again, so turning them off meanwhile wins.
	connect(&updates_, &UpdateChecker::updateAvailable, this, &KitDock::showUpdate);
	const QString cfgRoot = QString::fromUtf8(kitpaths::pluginConfigRoot().c_str());
	if (updatecheck::loadSharedCheckUpdates(cfgRoot) && !testHooksEnabled())
		QTimer::singleShot(15000, this, [this, cfgRoot] {
			if (closing_)
				return;
			if (updatecheck::loadSharedCheckUpdates(cfgRoot))
				updates_.start(QString::fromLatin1(updatecheck::kKitVersion));
		});

	applyTheme();
	setShowAll(settings_.showAll, false);
	refreshView();
	installTestHooks();
}

KitDock::~KitDock()
{
	shutdown();
}

void KitDock::shutdown()
{
	if (closing_)
		return;
	closing_ = true;
	if (kitcost::enabled())
		logKitCost(); // the exit line of every counter
	tick_.stop();
	updates_.stop(); // join the update-check thread before the plugin can unload
	// A dialog open at EXIT must end before libobs and Qt tear the main window down (its parent). Only dialogs this
	// Kit owns are touched, each one tracked where it is opened - never "whatever modal widget the application has",
	// which could be OBS's own or another plugin's dialog:
	//   - the Kit settings dialog (settingsDlg_, here);
	//   - Pre-Flight's settings dialog and go-live box (PreflightDock::modal_), In-Flight's settings dialog
	//     (InflightDock::dialog_), Multistream's edit / import / report dialogs, message boxes and the upload-speed
	//     prompt (MultistreamDock::modals_), and the Twitch bandwidth test and the Remove question opened over an
	//     edit dialog (EditDialog::nested_, ended by EditDialog::reject()).
	// The tools' own dialogs are rejected by their EXIT handlers (PreflightDock / InflightDock / MultistreamDock
	// ::shutdown), which plugin-main's kitFrontendEvent runs before this one (OBS would call the Kit's callback
	// FIRST, it being registered last, so plugin-main orders the whole EXIT itself).
	if (settingsDlg_)
		settingsDlg_->reject(); // the last thing done here: nothing follows that a deletion could hurt
}

QDockWidget *KitDock::hostDock() const
{
	for (QWidget *w = parentWidget(); w; w = w->parentWidget())
		if (auto *d = qobject_cast<QDockWidget *>(w))
			return d;
	return nullptr;
}

// ---- building ---------------------------------------------------------------------------------------------------

void KitDock::buildHeader(QVBoxLayout *root)
{
	header_ = new QWidget(this);
	header_->setObjectName(QStringLiteral("kitHeader"));
	auto *hv = new QVBoxLayout(header_);
	hv->setContentsMargins(12, 10, 12, 8);
	hv->setSpacing(8);

	auto *row1 = new QHBoxLayout();
	row1->setSpacing(6);
	strip_ = new QWidget(header_);
	auto *sl = new QHBoxLayout(strip_);
	sl->setContentsMargins(0, 0, 0, 0);
	sl->setSpacing(4);
	const auto stages = machine_.visibleStages();
	for (size_t i = 0; i < stages.size(); ++i) {
		if (i > 0) {
			auto *arrow = new QLabel(QString(QChar(0x203A)), strip_); // single right-pointing angle quotation mark
			arrow->setObjectName(QStringLiteral("kitStageArrow"));
			sl->addWidget(arrow);
			stageArrows_.push_back(arrow);
		}
		auto *b = new QPushButton(stageName(stages[i]), strip_);
		b->setObjectName(QStringLiteral("kitStageBtn"));
		b->setCheckable(true);
		b->setCursor(Qt::PointingHandCursor);
		b->setToolTip(QStringLiteral("Look at %1 (the dock switches by itself when the stream starts or ends)")
				      .arg(stageName(stages[i])));
		const Stage st = stages[i];
		connect(b, &QPushButton::clicked, this, [this, st] {
			machine_.peek(st);
			refreshView();
		});
		sl->addWidget(b);
		stageButtons_.push_back({st, b});
	}
	sl->addStretch(1);
	row1->addWidget(strip_, 1);

	helpBtn_ = new QToolButton(header_);
	helpBtn_->setObjectName(QStringLiteral("kitIconBtn"));
	helpBtn_->setText(QStringLiteral("?"));
	helpBtn_->setToolTip(QStringLiteral("Open the Stream Kit manual"));
	helpBtn_->setAccessibleName(QStringLiteral("Open the Stream Kit manual"));
	helpBtn_->setCursor(Qt::PointingHandCursor);
	helpBtn_->setFixedSize(30, 30);
	connect(helpBtn_, &QToolButton::clicked, this, &KitDock::openManual);
	row1->addWidget(helpBtn_);

	gearBtn_ = new QToolButton(header_);
	gearBtn_->setObjectName(QStringLiteral("kitIconBtn"));
	gearBtn_->setToolTip(QStringLiteral("Kit settings"));
	gearBtn_->setAccessibleName(QStringLiteral("Kit settings"));
	gearBtn_->setCursor(Qt::PointingHandCursor);
	gearBtn_->setFixedSize(30, 30);
	connect(gearBtn_, &QToolButton::clicked, this, &KitDock::openSettings);
	row1->addWidget(gearBtn_);
	hv->addLayout(row1);

	auto *row2 = new QHBoxLayout();
	row2->setSpacing(8);
	showAllBox_ = new QCheckBox(QStringLiteral("Show all tools"), header_);
	showAllBox_->setToolTip(QStringLiteral("Every tool stacked in one list instead of one stage at a time"));
	connect(showAllBox_, &QCheckBox::toggled, this, [this](bool on) { setShowAll(on, true); });
	row2->addWidget(showAllBox_);
	liveLabel_ = new QLabel(header_);
	liveLabel_->setObjectName(QStringLiteral("kitLive"));
	liveLabel_->setVisible(false);
	row2->addWidget(liveLabel_, 1, Qt::AlignCenter);
	row2->addStretch(0);
	resetBtn_ = new QPushButton(QStringLiteral("Reset"), header_);
	resetBtn_->setObjectName(QStringLiteral("kitResetBtn"));
	resetBtn_->setCursor(Qt::PointingHandCursor);
	connect(resetBtn_, &QPushButton::clicked, this, &KitDock::doReset);
	row2->addWidget(resetBtn_);
	hv->addLayout(row2);
	root->addWidget(header_);

	// The Kit-level update notice: slim, under the header.
	updateBanner_ = new QLabel(this);
	updateBanner_->setObjectName(QStringLiteral("kitUpdateBanner"));
	updateBanner_->setWordWrap(true);
	updateBanner_->setTextFormat(Qt::RichText);
	updateBanner_->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
	updateBanner_->setVisible(false);
	connect(updateBanner_, &QLabel::linkActivated, this, [this](const QString &link) {
		if (link == QLatin1String("download"))
			QDesktopServices::openUrl(QUrl(pendingUpdate_.url));
		updateBanner_->setVisible(false); // Download or Not now: gone until the next OBS start
	});
	root->addWidget(updateBanner_);
}

QFrame *KitDock::buildEndedCard(QWidget *parent)
{
	auto *card = new QFrame(parent);
	card->setObjectName(QStringLiteral("kitEndedCard"));
	auto *v = new QVBoxLayout(card);
	v->setContentsMargins(14, 14, 14, 14);
	v->setSpacing(10);
	auto *top = new QHBoxLayout();
	auto *title = new QLabel(QStringLiteral("Stream ended"), card);
	title->setObjectName(QStringLiteral("kitEndedTitle"));
	top->addWidget(title, 1);
	endedTime_ = new QLabel(card);
	endedTime_->setObjectName(QStringLiteral("kitEndedSub"));
	top->addWidget(endedTime_, 0, Qt::AlignRight);
	v->addLayout(top);
	// Only numbers that exist: the time on air (the Kit's own clock) and the entries in In-Flight's session
	// log. "Problems", "frames dropped" and "reconnects" are not counted anywhere yet, so they are not shown.
	endedLog_ = new QLabel(card);
	endedLog_->setObjectName(QStringLiteral("kitEndedSub"));
	v->addWidget(endedLog_);
	auto *fresh = new QPushButton(QStringLiteral("Start fresh for the next stream"), card);
	fresh->setObjectName(QStringLiteral("kitPrimaryBtn"));
	fresh->setCursor(Qt::PointingHandCursor);
	connect(fresh, &QPushButton::clicked, this, &KitDock::doReset);
	v->addWidget(fresh);
	auto *note = new QLabel(QStringLiteral("Clears the session log and the Pre-Flight checks. Your settings stay."), card);
	note->setObjectName(QStringLiteral("kitEndedSub"));
	note->setWordWrap(true);
	v->addWidget(note);
	return card;
}

void KitDock::buildBody(QVBoxLayout *root)
{
	scroll_ = new QScrollArea(this);
	scroll_->setWidgetResizable(true);
	scroll_->setFrameShape(QFrame::NoFrame);
	scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	content_ = new QWidget(scroll_);
	content_->setObjectName(QStringLiteral("kitContent"));
	auto *cl = new QVBoxLayout(content_);
	cl->setContentsMargins(0, 0, 0, 0);
	cl->setSpacing(0);

	// A section = [header][body]. The Multistream header is always there and collapses its body (remembered);
	// the Pre-Flight / In-Flight headers only show in Show-all mode, where they collapse too (not remembered).
	auto makeSection = [&](Section &s, const QString &title, QWidget *body, QWidget *above = nullptr) {
		s.wrap = new QWidget(content_);
		auto *v = new QVBoxLayout(s.wrap);
		v->setContentsMargins(0, 0, 0, 0);
		v->setSpacing(0);
		s.head = new KitSectionHeader(s.wrap);
		s.head->setTitle(title);
		v->addWidget(s.head);
		if (above)
			v->addWidget(above);
		s.body = body;
		v->addWidget(body); // reparents the panel (created hidden, parented to OBS's main window)
		body->show();
		cl->addWidget(s.wrap);
	};
	if (panels_.multistream) {
		makeSection(msSection_, QStringLiteral("Multistream"), panels_.multistream);
		msSection_.collapsed = settings_.multistreamCollapsed;
		connect(msSection_.head, &QAbstractButton::clicked, this,
			[this] { setMultistreamCollapsed(!settings_.multistreamCollapsed, true); });
	}
	if (panels_.preflight) {
		makeSection(pfSection_, QStringLiteral("Pre-Flight"), panels_.preflight);
		connect(pfSection_.head, &QAbstractButton::clicked, this, [this] {
			pfSection_.collapsed = !pfSection_.collapsed;
			refreshSections();
		});
	}
	if (panels_.inflight) {
		endedCard_ = buildEndedCard(content_);
		endedCard_->setVisible(false);
		auto *wrapAbove = new QWidget(content_); // gives the card a margin around it
		auto *av = new QVBoxLayout(wrapAbove);
		av->setContentsMargins(12, 12, 12, 12);
		av->setSpacing(0);
		av->addWidget(endedCard_);
		wrapAbove->setVisible(false);
		endedHolder_ = wrapAbove;
		makeSection(ifSection_, QStringLiteral("In-Flight"), panels_.inflight, wrapAbove);
		connect(ifSection_.head, &QAbstractButton::clicked, this, [this] {
			ifSection_.collapsed = !ifSection_.collapsed;
			refreshSections();
		});
	}
	// The Levels section: In-Flight's Levels panel, placed here (under the stage page) so Pre-Flight can show
	// it too. Its own header (arrow + "Levels", dB | LUFS) is the section header; collapsed by default and the
	// open state persists through In-Flight's own settings (levels_open).
	if (panels_.inflight && panels_.inflight->levelsPanel()) {
		LevelsPanel *lp = panels_.inflight->levelsPanel();
		levelsWrap_ = new QWidget(content_);
		auto *lv = new QVBoxLayout(levelsWrap_);
		lv->setContentsMargins(8, 4, 8, 8);
		lv->setSpacing(0);
		lv->addWidget(lp); // reparents the panel (created hidden, a child of the In-Flight dock)
		lp->show();
		// The panel's "?" opens the manual's Levels section; this dock knows where the manual is.
		lp->setHelpOpener([this](const QString &anchor) { openManualAt(anchor); });
		cl->addWidget(levelsWrap_);
		connect(lp, &LevelsPanel::openChanged, this, [this](bool) {
			if (!closing_)
				refreshSections();
		});
	}
	cl->addStretch(1);
	scroll_->setWidget(content_);
	root->addWidget(scroll_, 1);
	if (levelsWrap_) {
		// The Levels gate follows what the user can see: scrolling moves the section in and out of the viewport; a
		// resize of the viewport or the content, or a layout pass (a row grows, a section opens), moves it too.
		connect(scroll_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int) { updateLevelsGate(); });
		scroll_->viewport()->installEventFilter(this);
		content_->installEventFilter(this);
		levelsWrap_->installEventFilter(this);
	}
}

// ---- theme --------------------------------------------------------------------------------------------------------

// Every colour is mixed from the palette's window and text (the lesson of "Dock text or controls invisible on
// the user's OBS theme"); geranium is only the accent of the active stage and the primary button.
void KitDock::applyTheme()
{
	if (!header_ || inTheme_)
		return;
	inTheme_ = true;
	const ui::Theme t = ui::Theme::from(palette());
	auto c = [](const QColor &col) { return ui::Theme::css(col); };
	const QColor geranium = t.geranium;
	const QColor iconBg = mixc(t.window, t.text, t.dark ? 0.16 : 0.10);
	const QColor headerBg = mixc(t.window, t.text, t.dark ? 0.06 : 0.04);
	const QString css =
		QStringLiteral(
			"#kitHeader { background:%1; border-bottom:1px solid %2; }"
			"#kitStageBtn { border:0; border-radius:6px; padding:6px 10px; background:transparent; color:%3; font-weight:600; font-size:12px; }"
			"#kitStageBtn:hover { background:%4; }"
			"#kitStageBtn:checked { background:%5; color:#ffffff; font-weight:700; }"
			"#kitStageBtn:disabled { color:%6; }"
			"#kitStageBtn:checked:disabled { background:%7; color:#ffffff; }"
			"#kitStageArrow { color:%6; }"
			"#kitIconBtn { border:0; border-radius:6px; background:%8; color:%9; font-weight:700; font-size:13px; }"
			"#kitIconBtn:hover { background:%4; }"
			"#kitResetBtn { border:1px solid %10; border-radius:6px; padding:5px 12px; background:transparent; color:%3; font-size:12px; }"
			"#kitResetBtn:hover { background:%4; }"
			"#kitResetBtn:disabled { color:%6; border-color:%2; }"
			"#kitLive { color:%11; font-weight:700; font-size:12px; }"
			"#kitUpdateBanner { background:%1; border-bottom:1px solid %2; padding:6px 12px; color:%9; }"
			"#kitEndedCard { background:%1; border:1px solid %10; border-radius:10px; }"
			"#kitEndedTitle { font-weight:700; font-size:16px; color:%9; }"
			"#kitEndedSub { color:%3; font-size:12px; }"
			"#kitPrimaryBtn { border:0; border-radius:6px; padding:10px; background:%5; color:#ffffff; font-weight:700; }"
			"#kitPrimaryBtn:hover { background:%12; }")
			.arg(c(headerBg), c(t.border), c(t.secondary), c(t.hover), c(geranium), c(t.muted),
			     c(mixc(geranium, t.window, 0.5)), c(iconBg), c(t.text), c(mixc(t.window, t.text, 0.32)),
			     c(t.error), c(mixc(geranium, QColor(Qt::white), 0.12)));
	// Setting a style sheet sends a PaletteChange to this widget: only when the colours really changed, and
	// never re-entered, or applyTheme would call itself until the stack ends.
	if (css != lastCss_) {
		lastCss_ = css;
		setStyleSheet(css);
	}
	const qreal dpr = devicePixelRatioF();
	gearBtn_->setIcon(gearIcon(t.text, dpr));
	gearBtn_->setIconSize(QSize(16, 16));
	for (KitSectionHeader *h : {msSection_.head, pfSection_.head, ifSection_.head})
		if (h)
			h->update();
	inTheme_ = false;
}

void KitDock::changeEvent(QEvent *e)
{
	QWidget::changeEvent(e);
	if (e->type() == QEvent::PaletteChange && !closing_)
		applyTheme();
}

// ---- view ---------------------------------------------------------------------------------------------------------

KitDock::ViewKey KitDock::viewKey() const
{
	ViewKey k;
	k.stage = machine_.stage();
	k.peeking = machine_.peeking();
	k.active = machine_.sessionActive();
	k.ended = machine_.ended();
	k.resetAllowed = machine_.resetAllowed();
	k.showAll = settings_.showAll;
	return k;
}

void KitDock::refreshView()
{
	if (closing_)
		return;
	shown_ = viewKey();
	refreshStrip();
	refreshSections();
	refreshLive();
	resetBtn_->setEnabled(machine_.resetAllowed());
	resetBtn_->setToolTip(machine_.resetAllowed()
				      ? QStringLiteral("Start fresh: clear the checks and the session log")
				      : QStringLiteral("You can reset after the stream ends"));
	if (panels_.preflight)
		panels_.preflight->setDoneVisible(panels_.inflight && !machine_.sessionActive());
}

void KitDock::refreshStrip()
{
	for (const StageButton &sb : stageButtons_) {
		const QSignalBlocker block(sb.button);
		sb.button->setChecked(sb.stage == machine_.stage());
		sb.button->setEnabled(!settings_.showAll); // dimmed: the stage is ignored in Show all
	}
	for (QLabel *a : stageArrows_)
		a->setEnabled(!settings_.showAll);
}

void KitDock::refreshSections()
{
	const bool all = settings_.showAll;
	const Stage st = machine_.stage();
	if (msSection_.wrap) {
		const bool open = !settings_.multistreamCollapsed;
		msSection_.head->setExpanded(open);
		msSection_.body->setVisible(open);
	}
	if (pfSection_.wrap) {
		pfSection_.wrap->setVisible(all || st == Stage::PreFlight);
		pfSection_.head->setVisible(all);
		pfSection_.head->setExpanded(!all || !pfSection_.collapsed);
		pfSection_.body->setVisible(!all || !pfSection_.collapsed);
	}
	if (ifSection_.wrap) {
		const bool pageShown = all || st == Stage::InFlight;
		ifSection_.wrap->setVisible(pageShown);
		ifSection_.head->setVisible(all);
		ifSection_.head->setExpanded(!all || !ifSection_.collapsed);
		const bool bodyShown = !all || !ifSection_.collapsed;
		ifSection_.body->setVisible(bodyShown);
		(void)bodyShown;
		const bool showEnded = machine_.ended() || testPreview_;
		endedCard_->setVisible(showEnded);
		endedHolder_->setVisible(showEnded);
		if (showEnded && panels_.inflight) {
			const int n = panels_.inflight->sessionLogCount();
			endedLog_->setText(n == 1 ? QStringLiteral("1 entry in the session log")
						  : QStringLiteral("%1 entries in the session log").arg(n));
		}
	}
	// The Levels meters run only while the Levels section is on screen (Pre-Flight or In-Flight stage, or Show
	// all) AND expanded; the gate also needs the Kit dock itself visible (it asks the dock, we only know the
	// section). Recomputed here on every page / Show-all / expand change.
	const bool levelsShown = levelsSectionShown();
	if (levelsWrap_)
		levelsWrap_->setVisible(levelsShown);
	updateLevelsGate();
	if (panels_.preflight)
		panels_.preflight->setLevelsAvailable(levelsWrap_ != nullptr);
}

// Is any part of the Levels section inside the scroll viewport? (Geometry of a section that is not shown is stale, so
// callers test levelsSectionShown() first.) A zero-sized section counts as not seen.
bool KitDock::levelsInViewport() const
{
	if (!levelsWrap_ || !scroll_)
		return false;
	// Test copy only: switches the viewport rule off so the harness can show its scroll checks fail without it.
	if (testHooksEnabled() && qEnvironmentVariableIsSet("RWS_KIT_TEST_GATE_IGNORES_VIEWPORT"))
		return true;
	const QRect view = scroll_->viewport()->rect();
	const QRect sec(levelsWrap_->mapTo(scroll_->viewport(), QPoint(0, 0)), levelsWrap_->size());
	return view.intersects(sec);
}

void KitDock::updateLevelsGate()
{
	if (closing_ || !panels_.inflight)
		return;
	LevelsPanel *lp = panels_.inflight->levelsPanel();
	panels_.inflight->setShownByHost(levelsSectionShown() && lp && lp->isOpen() && levelsInViewport());
}

void KitDock::scheduleLevelsGate()
{
	if (levelsGatePending_ || closing_)
		return;
	levelsGatePending_ = true;
	// Deferred: an event filter sees a Resize / LayoutRequest BEFORE the layout has moved the children, so the
	// geometry is read one event-loop turn later.
	QTimer::singleShot(0, this, [this] {
		levelsGatePending_ = false;
		updateLevelsGate();
	});
}

bool KitDock::eventFilter(QObject *o, QEvent *e)
{
	if (!closing_ && levelsWrap_) {
		const QEvent::Type t = e->type();
		if ((o == scroll_->viewport() && t == QEvent::Resize) ||
		    (o == content_ && (t == QEvent::Resize || t == QEvent::LayoutRequest)) ||
		    (o == levelsWrap_ && (t == QEvent::Move || t == QEvent::Resize || t == QEvent::Show || t == QEvent::Hide)))
			scheduleLevelsGate();
	}
	return QWidget::eventFilter(o, e);
}

bool KitDock::levelsSectionShown() const
{
	if (!levelsWrap_ || testLevelsHidden_)
		return false;
	if (settings_.showAll)
		return true;
	const Stage st = machine_.stage();
	if (st == Stage::InFlight)
		return true;
	if (st == Stage::PreFlight) {
		// In the Pre-Flight stage the Levels BUTTON in Pre-Flight's row is the way in; a collapsed "Levels" header right
		// under it would say the same thing twice. Open, the section shows (its header collapses it again).
		LevelsPanel *lp = panels_.inflight ? panels_.inflight->levelsPanel() : nullptr;
		return lp && lp->isOpen();
	}
	return false;
}

void KitDock::logLevels(const char *tag)
{
	LevelsPanel *lp = panels_.inflight ? panels_.inflight->levelsPanel() : nullptr;
	obs_log(LOG_INFO, "kit dock levels: %s stage=%s shown=%d open=%d hostShown=%d gate=%d running=%d button=%d", tag,
		stageName(machine_.stage()).toUtf8().constData(), levelsSectionShown() ? 1 : 0, lp && lp->isOpen() ? 1 : 0,
		panels_.inflight && panels_.inflight->levelsHostShown() ? 1 : 0,
		panels_.inflight && panels_.inflight->levelsGateVisible() ? 1 : 0,
		panels_.inflight && panels_.inflight->levelsRunning() ? 1 : 0,
		panels_.preflight && panels_.preflight->levelsButtonVisible() ? 1 : 0);
}

void KitDock::refreshLive()
{
	const bool active = machine_.sessionActive() || testPreview_;
	liveLabel_->setVisible(active);
	if (active)
		liveLabel_->setText(QString(QChar(0x25CF)) + QStringLiteral("  LIVE ") +
				    clockText(testPreview_ && !machine_.sessionActive() ? 42 * 60 * 1000 + 10 * 1000
											: nowMs() - sessionStartMs_));
}

void KitDock::setMultistreamCollapsed(bool collapsed, bool save)
{
	settings_.multistreamCollapsed = collapsed;
	refreshSections();
	if (save)
		saveSettings();
}

void KitDock::setShowAll(bool on, bool save)
{
	settings_.showAll = on;
	{
		const QSignalBlocker block(showAllBox_);
		showAllBox_->setChecked(on);
	}
	refreshView();
	if (save)
		saveSettings();
}

bool KitDock::saveSettings()
{
	const QString root = QString::fromUtf8(kitpaths::pluginConfigRoot().c_str());
	if (!kitsettings::save(root, settings_)) {
		obs_log(LOG_WARNING, "kit dock: could not save the Kit settings");
		return false;
	}
	return true;
}

// first_run_done goes to disk and is read back; only a verified marker counts as "placement settled".
bool KitDock::persistFirstRunDone()
{
	const QString root = QString::fromUtf8(kitpaths::pluginConfigRoot().c_str());
	firstRunPersisted_ = kitsettings::saveFirstRunDone(root, settings_); // settings_.firstRunDone is true afterwards either way
	if (firstRunPersisted_)
		obs_log(LOG_INFO, "kit dock: first-run marker saved and read back");
	else
		obs_log(LOG_WARNING,
			"kit dock: could not save the first-run marker (first_run_done); a later launch will not force the dock into place if OBS has a saved place for it");
	return firstRunPersisted_;
}

// Does OBS's saved window layout (user.ini, [BasicWindow] DockState) already hold this dock? (Read at load, before OBS
// restores it; the state is the base64 of QMainWindow::saveState.)
bool KitDock::savedLayoutHasDock() const
{
	// Test copy only: pretend OBS has no saved place for the dock (the harness shows its guard check fails without the guard).
	if (testHooksEnabled() && qEnvironmentVariableIsSet("RWS_KIT_TEST_IGNORE_SAVED_LAYOUT"))
		return false;
	config_t *cfg = obs_frontend_get_user_config();
	const char *state = cfg ? config_get_string(cfg, "BasicWindow", "DockState") : nullptr;
	return state && kitsettings::dockStateHasDock(QByteArray(state), QString::fromLatin1(kDockId));
}

// ---- live wiring and the stage machine -----------------------------------------------------------------------------

void KitDock::onLiveReport(inflight::LiveState live, quint64 sessionId)
{
	if (closing_)
		return;
	const qint64 now = nowMs();
	kitstage::Live l = kitstage::Live::Unknown;
	if (live == inflight::LiveState::Live)
		l = kitstage::Live::Live;
	else if (live == inflight::LiveState::Off)
		l = kitstage::Live::Off;
	const bool wasActive = machine_.sessionActive();
	machine_.onLive(l, sessionId, now);
	if (l == kitstage::Live::Live)
		lastLiveMs_ = now;
	if (machine_.sessionsStarted() != startedSeen_) {
		startedSeen_ = machine_.sessionsStarted();
		sessionStartMs_ = now;
		lastLiveMs_ = now;
	}
	if (!wasActive && machine_.sessionActive())
		obs_log(LOG_INFO, "kit dock: session started (stage %s)", stageName(machine_.stage()).toUtf8().constData());
	if (wasActive && !machine_.sessionActive())
		obs_log(LOG_INFO, "kit dock: session ended (on air %s)", clockText(lastLiveMs_ - sessionStartMs_).toUtf8().constData());
	if (machine_.ended() && !wasEnded_)
		endedTime_->setText(clockText(lastLiveMs_ - sessionStartMs_) + QStringLiteral(" on air"));
	wasEnded_ = machine_.ended();
	if (viewKey() != shown_)
		refreshView();
}

void KitDock::onTick()
{
	if (closing_)
		return;
	const qint64 now = nowMs();
	const bool wasActive = machine_.sessionActive();
	machine_.onTick(now); // the 10 s Off confirmation fires from here
	if (wasActive && !machine_.sessionActive())
		obs_log(LOG_INFO, "kit dock: session ended (on air %s)", clockText(lastLiveMs_ - sessionStartMs_).toUtf8().constData());
	if (machine_.ended() && !wasEnded_)
		endedTime_->setText(clockText(lastLiveMs_ - sessionStartMs_) + QStringLiteral(" on air"));
	wasEnded_ = machine_.ended();
	// The shared check_updates can be switched off from any tool's own settings dialog: while the update notice is
	// up, look at the shared preference every 3 s (one tiny file read) and drop the notice when it turned off.
	if (updateBanner_->isVisible() && ++bannerTicks_ % 3 == 0) {
		if (!updatecheck::loadSharedCheckUpdates(QString::fromUtf8(kitpaths::pluginConfigRoot().c_str())))
			updateBanner_->setVisible(false);
	}
	updateLevelsGate(); // the 1 Hz self-check: whatever event was missed, the gate is right within a second
	if (viewKey() != shown_)
		refreshView();
	else if (machine_.sessionActive())
		refreshLive(); // the elapsed time
	else if (machine_.ended() && panels_.inflight)
		refreshSections(); // the log count can still grow after the end
}

void KitDock::doReset()
{
	if (closing_)
		return;
	if (!machine_.reset(nowMs()))
		return; // refused: a session is active (the button is disabled then; a stale click lands here)
	if (panels_.preflight)
		panels_.preflight->resetForNewStream();
	if (panels_.inflight)
		panels_.inflight->resetSession();
	wasEnded_ = false;
	obs_log(LOG_INFO, "kit dock: reset (stage %s)", stageName(machine_.stage()).toUtf8().constData());
	refreshView();
}

void KitDock::doDone()
{
	if (closing_)
		return;
	if (machine_.done(nowMs()))
		obs_log(LOG_INFO, "kit dock: done (stage %s)", stageName(machine_.stage()).toUtf8().constData());
	refreshView();
}

// ---- title mark and taskbar alert (In-Flight raises them; the dock owns the title and the one alert) -----------------

void KitDock::setTitleMark(bool on)
{
	titleMarked_ = on;
	if (QDockWidget *d = hostDock()) {
		const QString base = QString::fromLatin1(kTitle);
		d->setWindowTitle(on ? QString(QChar(0x25CF)) + QLatin1Char(' ') + base : base); // U+25CF BLACK CIRCLE
	}
}

void KitDock::onAttentionChanged(bool needsAttention)
{
	if (!closing_)
		setTitleMark(needsAttention);
}

void KitDock::onTaskbarAlert()
{
	if (closing_)
		return;
	// One taskbar flash per new red. A timeout of 0 would flash until OBS is activated, which on a streamer who
	// never alt-tabs is "forever"; 3 s is a bounded, single flash burst.
	if (QWidget *main = static_cast<QWidget *>(obs_frontend_get_main_window()))
		QApplication::alert(main, 3000);
}

// ---- settings, manual, updates ------------------------------------------------------------------------------------

void KitDock::openSettings()
{
	if (closing_)
		return;
	const QString root = QString::fromUtf8(kitpaths::pluginConfigRoot().c_str());
	const bool updatesNow = updatecheck::loadSharedCheckUpdates(root);
	// On the heap, held by a QPointer and rejected by shutdown(): a dialog on the stack whose parent is the OBS
	// main window would be destroyed twice if OBS tears the main window down while it is open.
	auto *dlg = new KitSettingsDialog(settings_, updatesNow, [this] { return toolOffBlocked(); },
					  static_cast<QWidget *>(obs_frontend_get_main_window()));
	settingsDlg_ = dlg;
	const int rc = dlg->exec();
	if (!settingsDlg_)
		return; // destroyed under us (OBS is going down)
	kitsettings::Settings n = dlg->settings();
	const bool wantUpdates = dlg->checkUpdates();
	delete dlg;
	if (rc != QDialog::Accepted || closing_)
		return;
	if (toolOffBlocked()) {
		// Never switch a tool off mid-stream (the dialog refuses it too, on toggle and on Save; this is the
		// third lock).
		n.multistream = n.multistream || settings_.multistream;
		n.preflight = n.preflight || settings_.preflight;
		n.inflight = n.inflight || settings_.inflight;
	}
	settings_.multistream = n.multistream;
	settings_.preflight = n.preflight;
	settings_.inflight = n.inflight;
	settings_.autoSwitchOnLive = n.autoSwitchOnLive;
	machine_.setOptions(optionsFor(settings_));
	saveSettings();
	if (wantUpdates != updatesNow) {
		updatecheck::saveSharedCheckUpdates(root, wantUpdates);
		if (!wantUpdates)
			updateBanner_->setVisible(false);
	}
	obs_log(LOG_INFO, "kit dock: settings saved (tools ms=%d pf=%d if=%d, auto-switch=%d)", settings_.multistream ? 1 : 0,
		settings_.preflight ? 1 : 0, settings_.inflight ? 1 : 0, settings_.autoSwitchOnLive ? 1 : 0);
}

bool KitDock::toolOffBlocked() const
{
	return machine_.sessionActive() || obs_frontend_streaming_active() ||
	       (panels_.multistream && panels_.multistream->anyOutputActive());
}

// Opens a URL, except in a test run (testbed OBS + RWS_KIT_TEST): there it only logs what it would have opened.
static void openUrlOrLog(const QUrl &url)
{
	if (testHooksEnabled() && qEnvironmentVariableIsSet("RWS_KIT_TEST")) {
		obs_log(LOG_INFO, "kit dock: manual would open %s", url.toString(QUrl::FullyEncoded).toUtf8().constData());
		return;
	}
	QDesktopServices::openUrl(url);
}

void KitDock::openManual()
{
	openManualAt(QString());
}

void KitDock::openManualAt(const QString &anchor)
{
	// The manual ships in the plugin's data folder (data/manual); without it the Kit page opens (or, for a
	// section, the web manual at that section).
	QStringList candidates;
	if (obs_module_t *mod = obs_get_module("rws-stream-kit")) {
		if (const char *bin = obs_get_module_binary_path(mod))
			candidates << QFileInfo(QString::fromUtf8(bin)).absolutePath() + QStringLiteral("/manual/index.html");
		if (const char *data = obs_get_module_data_path(mod))
			candidates << QString::fromUtf8(data) + QStringLiteral("/manual/index.html");
	}
	for (const QString &c : candidates) {
		if (QFileInfo::exists(c)) {
			// Windows drops the #fragment when it opens a file URL, so a section opens through its own
			// small page (manual/<anchor>.html) that forwards to index.html#<anchor>.
			if (!anchor.isEmpty()) {
				const QString stub = QFileInfo(c).absolutePath() + QLatin1Char('/') + anchor + QStringLiteral(".html");
				if (QFileInfo::exists(stub)) {
					openUrlOrLog(QUrl::fromLocalFile(stub));
					return;
				}
			}
			QUrl u = QUrl::fromLocalFile(c);
			if (!anchor.isEmpty())
				u.setFragment(anchor);
			openUrlOrLog(u);
			return;
		}
	}
	if (!anchor.isEmpty()) {
		openUrlOrLog(QUrl(QString::fromLatin1(kManualWebUrl) + QLatin1Char('#') + anchor));
		return;
	}
	openUrlOrLog(QUrl(QString::fromLatin1(kKitPageUrl)));
}

void KitDock::showUpdate(const updatecheck::Manifest &m)
{
	if (closing_)
		return;
	const QString root = QString::fromUtf8(kitpaths::pluginConfigRoot().c_str());
	if (!updatecheck::loadSharedCheckUpdates(root))
		return; // switched off since the check started
	pendingUpdate_ = m;
	obs_log(LOG_INFO, "Stream Kit update available: %s (installed kit %s, plugin %s)", m.latest.toUtf8().constData(),
		updatecheck::kKitVersion, PLUGIN_VERSION);
	const QString obsVersion = QString::fromUtf8(obs_get_version_string());
	QString text;
	if (!m.minObs.isEmpty() && updatecheck::compareVersions(obsVersion, m.minObs) < 0) {
		// Don't invite an upgrade that would break this OBS.
		text = QStringLiteral("Red Warden Stream Kit %1 is out. It needs OBS %2 or newer (you have %3), so "
				      "update OBS first. <a href=\"download\">Details</a> &middot; "
				      "<a href=\"dismiss\">Not now</a>")
			       .arg(m.latest.toHtmlEscaped(), m.minObs.toHtmlEscaped(), obsVersion.toHtmlEscaped());
	} else {
		text = QStringLiteral("Red Warden Stream Kit %1 is available. "
				      "<a href=\"download\">Download</a> &middot; <a href=\"dismiss\">Not now</a>")
			       .arg(m.latest.toHtmlEscaped());
		if (!m.notes.isEmpty())
			text += QStringLiteral("<br>") + m.notes.toHtmlEscaped();
	}
	updateBanner_->setText(text);
	updateBanner_->setVisible(true);
}

// ---- first run and frontend events ------------------------------------------------------------------------------

// OBS creates a new dock hidden and floating. The first time only (the flag lives in the Kit settings), put it
// in the main window's right dock area and show it; after that the saved layout rules.
//
// Why the dock is muted at registration instead of docked there (OBS 31.1.1 sources): on a first run there is no
// saved DockState, so OBSBasic::OBSInit calls on_resetDocks_triggered(true) (OBSBasic.cpp:1154, 1158), which does
// `setVisible(true); setFloating(true); move(); setVisible(false)` on EVERY extra dock (OBSBasic_Docks.cpp:98-106).
// Whatever place we gave the dock before that is undone, and for the length of that setVisible(true) (the first
// show of a big widget tree: polish, layout) it is a visible floating top-level window, ahead of FINISHED_LOADING
// (OBSBasic.cpp:1298). A 250 ms window watch caught it in about 1 cycle in 25. Qt::WA_DontShowOnScreen makes
// show() render offscreen only (no native window is mapped), so nothing can flash; dockFirstRun() clears it
// just before docking the dock for real.
void KitDock::prepareFirstRun()
{
	if (closing_ || settings_.firstRunDone)
		return;
	const bool saved = savedLayoutHasDock();
	if (!kitsettings::shouldForceDock(settings_.firstRunDone, saved)) {
		// The marker says "never placed", but OBS already has a place for this dock: the marker was lost (a failed
		// save last time), not the dock new. The user's layout wins; try to write the marker again.
		skipFirstRun_ = true;
		obs_log(LOG_WARNING,
			"kit dock: first_run_done is not set, but OBS has a saved layout for the dock: not forcing it into place");
		persistFirstRunDone();
		return;
	}
	QDockWidget *d = hostDock();
	if (!d)
		return;
	d->setAttribute(Qt::WA_DontShowOnScreen, true);
	obs_log(LOG_INFO, "kit dock: first run, muted (offscreen) until it is docked");
}

void KitDock::dockFirstRun()
{
	QDockWidget *d = hostDock();
	auto *main = qobject_cast<QMainWindow *>(static_cast<QWidget *>(obs_frontend_get_main_window()));
	if (d)
		d->setAttribute(Qt::WA_DontShowOnScreen, false); // while hidden: see prepareFirstRun()
	if (!d || !main) {
		obs_log(LOG_WARNING, "kit dock: first run, but no dock or main window to dock into");
		return;
	}
	d->hide();
	if (d->isFloating())
		d->setFloating(false);
	main->addDockWidget(Qt::RightDockWidgetArea, d);
	d->show();
	persistFirstRunDone(); // verified on disk, or logged; the in-memory marker is set either way
	obs_log(LOG_INFO, "kit dock: first run, docked right");
	obs_log(LOG_INFO, "kit dock: dock state floating=%d visible=%d", d->isFloating() ? 1 : 0, d->isVisible() ? 1 : 0);
	// Did OBS's own restore / layout pass undo it afterwards? (The harness asserts this settled state too.)
	QTimer::singleShot(2000, this, [this] {
		if (QDockWidget *dd = closing_ ? nullptr : hostDock())
			obs_log(LOG_INFO, "kit dock: dock state settled floating=%d visible=%d marker=%d", dd->isFloating() ? 1 : 0,
				dd->isVisible() ? 1 : 0, firstRunPersisted_ ? 1 : 0);
	});
}

void KitDock::handleFrontendEvent(obs_frontend_event event)
{
	if (event == OBS_FRONTEND_EVENT_EXIT) {
		shutdown();
		return;
	}
	if (closing_)
		return;
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		// OBS has restored its saved dock layout by the time it sends this event, so the first-run docking can
		// happen right here: no delay in which the new floating dock could show as a window of its own.
		if (!settings_.firstRunDone && !skipFirstRun_)
			dockFirstRun();
		refreshView(); // the dock is registered by now: the Levels gate can read it
		QTimer::singleShot(1500, this, [this] {
			if (!closing_)
				refreshView();
		});
	}
}

// ---- test hooks ---------------------------------------------------------------------------------------------------

// "kit-dock-<what>.png", or with RWS_KIT_TEST_SHOT_PREFIX in place of "kit-dock" (the seeded-config run).
static QString shotName(const std::string &what)
{
	QString prefix = qEnvironmentVariable("RWS_KIT_TEST_SHOT_PREFIX");
	if (prefix.isEmpty())
		prefix = QStringLiteral("kit-dock");
	return prefix + QLatin1Char('-') + QString::fromStdString(what) + QStringLiteral(".png");
}

// RWS_KIT_TEST_SHOT_DPR=2 (the opt-in showcase run, plugin\tools\shots-showcase.ps1): the shot is rendered at that device
// pixel ratio, so a 360 px wide dock becomes a 720 px wide, crisp file. Unset (every release harness): exactly QWidget::grab().
static QPixmap grabShot(QWidget *w)
{
	bool ok = false;
	const double dpr = qEnvironmentVariable("RWS_KIT_TEST_SHOT_DPR").toDouble(&ok);
	if (!ok || dpr < 1.0 || dpr > 4.0)
		return w->grab();
	QPixmap px(QSize(qRound(w->width() * dpr), qRound(w->height() * dpr)).expandedTo(QSize(1, 1)));
	px.setDevicePixelRatio(dpr);
	px.fill(Qt::transparent);
	w->render(&px);
	return px;
}

bool KitDock::testPump()
{
	if (closing_)
		return false;
	QPointer<KitDock> self(this);
	QApplication::processEvents();
	return self && !closing_;
}

// false = a pump ran OBS's exit (this dock may be gone): the caller must not touch `this` again.
bool KitDock::saveShot(const QString &name, QWidget *target)
{
	const QString dir = qEnvironmentVariable("RWS_KIT_TEST_SHOT_DIR");
	QDir().mkpath(dir);
	// The WHOLE dock as one grab at a realistic dock size: 360 wide, 900 tall (taller only when the content is, so
	// nothing is cut off). The dock is pinned to that size for the grab and forced through a layout pass first;
	// grabbing the pieces of a never-laid-out hidden widget is what made the header look narrower than the body.
	const int w = 360;
	const QSize oldSize = size();
	setFixedSize(w, 900);
	if (!testPump())
		return false;
	layout()->activate();
	content_->layout()->activate();
	// Grow until the scroll area has nothing left to scroll, so the grab shows everything.
	for (int i = 0; i < 12 && scroll_->verticalScrollBar()->maximum() > 0; ++i) {
		setFixedSize(w, height() + scroll_->verticalScrollBar()->maximum());
		if (!testPump())
			return false;
		layout()->activate();
		content_->layout()->activate();
	}
	obs_log(LOG_INFO, "kit dock shot %s: minimum widths content=%d multistream=%d preflight=%d inflight=%d levels=%d",
		name.toUtf8().constData(), content_->minimumSizeHint().width(),
		panels_.multistream ? panels_.multistream->minimumSizeHint().width() : -1,
		panels_.preflight ? panels_.preflight->minimumSizeHint().width() : -1,
		panels_.inflight ? panels_.inflight->minimumSizeHint().width() : -1,
		panels_.inflight && panels_.inflight->levelsPanel() ? panels_.inflight->levelsPanel()->minimumSizeHint().width() : -1);
	// The first grab makes Qt lay the whole (hidden, offscreen) dock out and send its pending resizes; what that settles
	// (a destination row that grew, a list that must now be taller) reaches the parent layouts as queued events, so they
	// are delivered and the dock is laid out again before the grab that is kept.
	grab();
	if (!testPump())
		return false;
	layout()->activate();
	content_->layout()->activate();
	if (panels_.multistream)
		panels_.multistream->testLogListGeometry(name.toUtf8().constData());
	const QPixmap out = grabShot(target ? target : this);
	if (panels_.multistream)
		panels_.multistream->testLogListGeometry((name + QStringLiteral(" (after the grab)")).toUtf8().constData());
	setMinimumSize(0, 0);
	setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
	if (oldSize.isValid())
		resize(oldSize);
	if (!out.isNull() && out.save(QDir(dir).filePath(name), "PNG"))
		obs_log(LOG_INFO, "kit dock shot saved %s (%dx%d)", name.toUtf8().constData(), out.width(), out.height());
	else
		obs_log(LOG_WARNING, "kit dock shot failed %s", name.toUtf8().constData());
	return true;
}

// Test hook: the Levels panel in LUFS mode, with demo numbers, at the narrow dock widths (360 and 300 px of dock, 8 px
// margins either side as in the dock, so the panel gets width - 16), Reference on and True peak on; once with Reference
// off, once on a dark palette. Parentless widgets grabbed offscreen: nothing is shown. Logs "levels-state ..." lines,
// the panel's minimum width (it must fit) and, for the first panel, a click on "?" (which only logs, in a test run).
void KitDock::runLevelsLabelShots()
{
	const QString dir = qEnvironmentVariable("RWS_KIT_TEST_SHOT_DIR");
	QDir().mkpath(dir);
	auto tr = [](const char *key, const char *fallback) {
		const char *t = obs_module_text(key);
		return (t && qstrcmp(t, key) != 0) ? QString::fromUtf8(t) : QString::fromUtf8(fallback);
	};
	struct Case {
		int dockW;
		bool ref;
		bool dark;
		const char *what;
	};
	const Case cases[] = {{360, true, false, "levels-lufs-labels-360"},
			      {300, true, false, "levels-lufs-labels-300"},
			      {360, false, false, "levels-lufs-labels-360-refoff"},
			      {360, true, true, "levels-lufs-labels-360-dark"}};
	bool first = true;
	for (const Case &c : cases) {
		auto *backdrop = new QWidget(nullptr);
		backdrop->setAttribute(Qt::WA_DontShowOnScreen);
		backdrop->setAutoFillBackground(true);
		if (c.dark) {
			QPalette pal = backdrop->palette();
			pal.setColor(QPalette::Window, QColor(0x1e, 0x1e, 0x1e));
			pal.setColor(QPalette::WindowText, QColor(0xe0, 0xe0, 0xe0));
			pal.setColor(QPalette::Text, QColor(0xe0, 0xe0, 0xe0));
			backdrop->setPalette(pal);
		}
		auto *bl = new QVBoxLayout(backdrop);
		bl->setContentsMargins(8, 8, 8, 8);
		auto *lp = new LevelsPanel(tr, backdrop);
		lp->setDemoMode(true);
		lp->setHelpLogOnly(true);
		lp->setTruePeak(true, false);
		lp->setLufsRef(c.ref, -16.0, false);
		lp->setMode(inflight::LevelsMode::Lufs, false);
		lp->setSources(QStringLiteral("demo-mic"), QStringLiteral("demo-desktop"),
			       {QStringLiteral("demo-silent"), QStringLiteral("demo-warm")});
		lp->setOpen(true, false);
		if (first) // the real dock's opener, so the URL logged is the one a click in the dock would open
			lp->setHelpOpener([this](const QString &anchor) { openManualAt(anchor); });
		const double inf = std::numeric_limits<double>::infinity();
		lp->demoSetLufs(QStringLiteral("demo-mic"), QStringLiteral("Mic/Aux"), true, -18.4, true, -19.2, -1.2);
		lp->demoSetLufs(QStringLiteral("demo-desktop"), QStringLiteral("Desktop Audio"), true, -27.0, true, -25.3, -9.8);
		lp->demoSetLufs(QStringLiteral("demo-silent"), QStringLiteral("Alerts"), true, -inf, true, -inf, -inf);
		lp->demoSetLufs(QStringLiteral("demo-warm"), QStringLiteral("Capture card (just started)"), true, -31.5, false, -inf,
				std::numeric_limits<double>::quiet_NaN());
		bl->addWidget(lp);
		bl->activate();
		backdrop->resize(c.dockW, 100);
		bl->activate();
		const int h = bl->totalHeightForWidth(c.dockW);
		backdrop->resize(c.dockW, h > 0 ? h : backdrop->sizeHint().height());
		bl->activate();
		const int need = lp->minimumSizeHint().width() + 16;
		obs_log(LOG_INFO, "kit dock levels labels %s: dock width %d, panel minimum width %d, fits=%d", c.what, c.dockW, need,
			need <= c.dockW ? 1 : 0);
		const QString name = shotName(c.what);
		const QPixmap px = backdrop->grab();
		if (!px.isNull() && px.save(QDir(dir).filePath(name), "PNG"))
			obs_log(LOG_INFO, "kit dock shot saved %s (%dx%d)", name.toUtf8().constData(), px.width(), px.height());
		else
			obs_log(LOG_WARNING, "kit dock shot failed %s", name.toUtf8().constData());
		lp->logTestState(c.what);
		if (first) {
			lp->testClickHelp(); // logs "kit dock: manual would open <url>"
			first = false;
		}
		delete backdrop;
	}
}

// Test hook: log the Levels state. scrollToLevels first brings the section into the scroll viewport (the layout is
// forced through a pass, then scrolled and the gate recomputed at once), so the steps that test "Levels is open and
// on screen" do not depend on how tall this OBS window happens to be. The scroll steps below do NOT use it: they
// rely on the events (valueChanged, resize, layout) alone.
bool KitDock::logLevelsStep(const char *tag, bool scrollToLevels)
{
	if (scrollToLevels && levelsWrap_) {
		if (!testPump())
			return false; // OBS is exiting: the caller must not touch `this` again
		layout()->activate();
		content_->layout()->activate();
		scroll_->ensureWidgetVisible(levelsWrap_, 0, 0);
		updateLevelsGate();
	}
	logLevels(tag);
	return true;
}

// Showcase command file (RWS_KIT_TEST + RWS_KIT_TEST_SHOWCASE + RWS_KIT_TEST_SHOT_DIR, testbed OBS only): the harness writes
// <shotdir>\showcase-cmd.txt (key=value lines) and the dock applies it offscreen, then deletes it. Keys:
//   stage=pre|in      show that stage's page (a peek, nothing is saved)
//   showall=0|1       the "Show all tools" box
//   tick=a|b          tick those Pre-Flight checklist items
//   levels=0|1        the Levels section open / closed (nothing is saved); open scrolls it into view so its meters run
//   name=<file>       take a shot, saved as <shotdir>\<file>.png, of target=dock (default) | levels | settings-tools
// Lines without name= only set state, so the harness can let the meters run before it asks for the shot.
void KitDock::pollShowcase()
{
	if (showcaseBusy_ || closing_)
		return;
	const QString dir = qEnvironmentVariable("RWS_KIT_TEST_SHOT_DIR");
	const QString path = QDir(dir).filePath(QStringLiteral("showcase-cmd.txt"));
	QFile f(path);
	if (!f.exists() || !f.open(QIODevice::ReadOnly))
		return;
	const QString text = QString::fromUtf8(f.readAll());
	f.close();
	if (!QFile::remove(path))
		return;
	showcaseBusy_ = true;
	QMap<QString, QString> kv;
	for (const QString &line : text.split(QLatin1Char('\n'))) {
		const int eq = line.indexOf(QLatin1Char('='));
		if (eq > 0)
			kv.insert(line.left(eq).trimmed(), line.mid(eq + 1).trimmed());
	}
	QPointer<KitDock> self(this);
	if (kv.contains(QStringLiteral("stage"))) {
		machine_.peek(kv.value(QStringLiteral("stage")) == QLatin1String("in") ? Stage::InFlight : Stage::PreFlight);
		refreshView();
	}
	if (kv.contains(QStringLiteral("showall")))
		setShowAll(kv.value(QStringLiteral("showall")) == QLatin1String("1"), false);
	if (kv.contains(QStringLiteral("levels")) && panels_.inflight && panels_.inflight->levelsPanel()) {
		const bool open = kv.value(QStringLiteral("levels")) == QLatin1String("1");
		panels_.inflight->levelsPanel()->setOpen(open, false);
		refreshView();
		if (open)
			logLevelsStep("showcase-levels", true);
	}
	if (kv.contains(QStringLiteral("tick")) && panels_.preflight) {
		// Pre-Flight's own checklist boxes, by their text ("Water bottle|Phone on silent"): ticked the way a click does.
		const QStringList want = kv.value(QStringLiteral("tick")).split(QLatin1Char('|'));
		for (QCheckBox *b : panels_.preflight->findChildren<QCheckBox *>())
			if (want.contains(b->text()))
				b->setChecked(true);
	}
	if (!self || closing_)
		return;
	const QString name = kv.value(QStringLiteral("name"));
	if (!name.isEmpty()) {
		const QString target = kv.value(QStringLiteral("target"), QStringLiteral("dock"));
		if (target == QLatin1String("settings-tools")) {
			const QString shotDir = dir;
			KitSettingsDialog dlg(settings_, true, [] { return false; }, nullptr);
			dlg.setAttribute(Qt::WA_DontShowOnScreen);
			dlg.setWindowModality(Qt::NonModal);
			dlg.show();
			for (int i = 0; i < dlg.tabCount(); ++i) {
				if (dlg.tabKey(i) != QLatin1String("tools"))
					continue;
				dlg.showTab(i);
				if (!testPump())
					return;
				const QPixmap px = grabShot(&dlg);
				if (!px.isNull() && px.save(QDir(shotDir).filePath(name + QStringLiteral(".png")), "PNG"))
					obs_log(LOG_INFO, "kit dock shot saved %s.png (%dx%d)", name.toUtf8().constData(), px.width(), px.height());
				else
					obs_log(LOG_WARNING, "kit dock shot failed %s", name.toUtf8().constData());
			}
			dlg.close();
		} else {
			QWidget *w = (target == QLatin1String("levels")) ? levelsWrap_ : nullptr;
			if (!saveShot(name + QStringLiteral(".png"), w))
				return; // OBS is exiting
		}
	}
	showcaseBusy_ = false;
}

void KitDock::installTestHooks()
{
	// Opt-in showcase run (plugin\tools\shots-showcase.ps1): the harness drops command files, the dock obeys.
	if (testHooksEnabled() && qEnvironmentVariableIsSet("RWS_KIT_TEST") && qEnvironmentVariableIsSet("RWS_KIT_TEST_SHOWCASE") &&
	    !qEnvironmentVariableIsEmpty("RWS_KIT_TEST_SHOT_DIR")) {
		auto *t = new QTimer(this);
		t->setInterval(500);
		connect(t, &QTimer::timeout, this, [this] {
			if (!closing_)
				pollShowcase();
		});
		t->start();
		obs_log(LOG_INFO, "kit dock: showcase command file is watched");
	}
	if (!testHooksEnabled() || !qEnvironmentVariableIsSet("RWS_KIT_TEST") ||
	    qEnvironmentVariableIsEmpty("RWS_KIT_TEST_SHOT_DIR"))
		return;
	QTimer::singleShot(12000, this, [this] { runShots(); });
}

// Offscreen only: QWidget::grab() renders without showing anything. The states are previews of the view
// (testPreview_ forces the LIVE label and the "Stream ended" card); the stage machine is not touched except
// for peeks, which are undone.
void KitDock::runShots()
{
	if (closing_)
		return;
	const QString dir = qEnvironmentVariable("RWS_KIT_TEST_SHOT_DIR");
	QDir().mkpath(dir);
	auto step = [this](int delayMs, std::function<void()> fn) {
		QTimer::singleShot(delayMs, this, [this, fn] {
			if (!closing_)
				fn();
		});
	};
	// 1) before going live (Pre-Flight page)
	if (!saveShot(shotName("before-live")))
		return; // a pump ran OBS's exit: the step() timers below would use a dead `this`
	// 2) Show all
	step(500, [this] { setShowAll(true, false); });
	step(1200, [this] {
		if (!saveShot(shotName("show-all")))
			return;
		setShowAll(false, false);
	});
	// 3) In-Flight page, then the same with the "Stream ended" card and the LIVE label previewed
	step(1900, [this] {
		machine_.peek(Stage::InFlight);
		refreshView();
	});
	step(2600, [this] {
		if (!saveShot(shotName("in-flight")))
			return;
		testPreview_ = true;
		refreshView();
	});
	step(3300, [this] {
		if (!saveShot(shotName("ended-live-preview")))
			return;
		testPreview_ = false;
		machine_.peek(Stage::PreFlight);
		refreshView();
	});
	// 4) Multistream collapsed
	step(4000, [this] {
		setMultistreamCollapsed(true, false);
	});
	step(4700, [this] {
		if (!saveShot(shotName("ms-collapsed")))
			return;
		setMultistreamCollapsed(false, false);
	});
	// 4b) Levels reachable in the Pre-Flight stage, and the gate follows the section (log lines + one shot).
	// setOpen(.., false): nothing is saved; the state is put back at the end.
	if (panels_.inflight && panels_.inflight->levelsPanel()) {
		LevelsPanel *lp = panels_.inflight->levelsPanel();
		step(5000, [this, lp] {
			machine_.peek(Stage::PreFlight);
			lp->setOpen(true, false);
			refreshView();
			logLevelsStep("preflight-open", true);
		});
		step(5400, [this] { saveShot(shotName("preflight-levels")); });
		step(5800, [this, lp] {
			lp->setOpen(false, false);
			refreshView();
			logLevels("preflight-collapsed");
		});
		step(6100, [this, lp] {
			lp->setOpen(true, false);
			machine_.peek(Stage::InFlight);
			refreshView();
			logLevelsStep("inflight-open", true);
		});
		// 4b2) The collapsed Levels header is hidden in the Pre-Flight stage only (the button is the way in), and kept in
		// the In-Flight stage and in Show all.
		step(6300, [this, lp] {
			lp->setOpen(false, false);
			machine_.peek(Stage::InFlight);
			refreshView();
			logLevels("inflight-collapsed");
		});
		step(6340, [this, lp] {
			const bool was = settings_.showAll;
			settings_.showAll = true;
			refreshView();
			logLevels("showall-collapsed");
			settings_.showAll = was;
			lp->setOpen(true, false);
			machine_.peek(Stage::InFlight);
			refreshView();
		});
		step(6400, [this, lp] {
			testLevelsHidden_ = true;
			refreshView();
			logLevels("section-hidden");
			testLevelsHidden_ = false;
			lp->setOpen(false, false);
			machine_.peek(Stage::PreFlight);
			refreshView();
			logLevels("restored");
		});
		// 4c) Levels scrolled out of the Kit dock's scroll viewport (the viewport is squeezed to 100 px, the content
		// scrolled to the top: Levels, the last section, is below the fold) must not meter; scrolled back, it must.
		// Nothing here calls the gate: it follows the resize and scroll events by itself.
		step(6500, [this, lp] {
			machine_.peek(Stage::PreFlight);
			lp->setOpen(true, false);
			refreshView();
			if (!logLevelsStep("scroll-start", true))
				return;
			scroll_->setMaximumHeight(100);
		});
		step(6800, [this] { scroll_->verticalScrollBar()->setValue(0); });
		step(7100, [this] { logLevels("scrolled-out"); });
		step(7300, [this] { scroll_->verticalScrollBar()->setValue(scroll_->verticalScrollBar()->maximum()); });
		step(7600, [this] { logLevels("scrolled-in"); });
		step(7800, [this, lp] {
			scroll_->setMaximumHeight(QWIDGETSIZE_MAX);
			lp->setOpen(false, false);
			refreshView();
			logLevels("scroll-restored");
		});
	}
	// 4d) The LUFS labels ("Now" / "3s avg"), the Reference value and the "?" link on parentless demo panels.
	step(7900, [this] { runLevelsLabelShots(); });
	// 5) the settings dialog, one grab per tab (never shown on screen)
	step(8300, [this] {
		const QString shotDir = qEnvironmentVariable("RWS_KIT_TEST_SHOT_DIR");
		KitSettingsDialog dlg(settings_, true, [] { return false; }, nullptr);
		dlg.setAttribute(Qt::WA_DontShowOnScreen);
		dlg.setWindowModality(Qt::NonModal);
		dlg.show();
		for (int i = 0; i < dlg.tabCount(); ++i) {
			dlg.showTab(i);
			if (!testPump())
				break;
			const QString name = shotName("settings-" + dlg.tabKey(i).toStdString());
			const QPixmap px = grabShot(&dlg);
			if (!px.isNull() && px.save(QDir(shotDir).filePath(name), "PNG"))
				obs_log(LOG_INFO, "kit dock shot saved %s (%dx%d)", name.toUtf8().constData(), px.width(), px.height());
			else
				obs_log(LOG_WARNING, "kit dock shot failed %s", name.toUtf8().constData());
		}
		dlg.close();
		obs_log(LOG_INFO, "kit dock shots done");
	});
}

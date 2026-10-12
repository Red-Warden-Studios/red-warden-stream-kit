/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "inflight-dock.hpp"
#include "inflight-import.hpp"
#include "kit-paths.hpp"
#include "update-manifest.hpp"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/bmem.h>

#include "inflight-ui-logic.hpp"
#include "dock-visibility-gate.hpp"
#include "levels-panel.hpp"
#include "lights-panel.hpp"
#include "settings-dialog.hpp"

#include <limits>

#include <QApplication>
#include <QElapsedTimer>
#include <QMainWindow>
#include <QThread>
#include <QTabBar>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSpinBox>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>

namespace {
QString T(const char *key, const char *fallback)
{
	const char *t = obs_module_text(key);
	return (t && qstrcmp(t, key) != 0) ? QString::fromUtf8(t) : QString::fromUtf8(fallback);
}

// This plugin's own config folder (.../plugin_config/rws-inflight/).
QString ownConfigDir()
{
	return QString::fromUtf8(kitpaths::toolDir(kitpaths::kInflight).c_str());
}

// The folder holding every plugin's config folder (.../plugin_config), for the shared Stream Kit preference.
QString kitConfigRoot()
{
	return QString::fromUtf8(kitpaths::pluginConfigRoot().c_str());
}

inflight::TrFn uiTr()
{
	return [](const char *key, const char *fallback) { return T(key, fallback); };
}

QFrame *makeCard(QWidget *parent, const char *name)
{
	auto *f = new QFrame(parent);
	f->setObjectName(QString::fromLatin1(name));
	// The same translucent grey wash as the lights panel's details card (palette(alternate-base) is
	// light under OBS's dark theme while the text is light too).
	f->setStyleSheet(QStringLiteral("#%1 { background: rgba(128, 128, 128, 40); border: 1px solid rgba(128, 128, 128, 110);"
					" border-radius: 4px; }")
				 .arg(QString::fromLatin1(name)));
	return f;
}

struct SetupCardParts {
	QFrame *card = nullptr;
	QPushButton *copy = nullptr;
	QPushButton *hand = nullptr;
	QPushButton *later = nullptr;
	QLabel *note = nullptr;
};

// The setup card's widgets, unwired; the dock connects the buttons (the test hook builds a spare one).
SetupCardParts buildSetupCard(QWidget *parent)
{
	SetupCardParts p;
	p.card = makeCard(parent, "ifSetup");
	auto *sl = new QVBoxLayout(p.card);
	sl->setContentsMargins(10, 10, 10, 10);
	auto *text = new QLabel(T("InFlight.SetupIntro",
				  "In-Flight watches your stream while you are live. Choose your "
				  "microphone and scenes to get started."),
				p.card);
	text->setWordWrap(true);
	sl->addWidget(text);

	p.copy = new QPushButton(T("InFlight.SetupCopy", "Copy my choices from Pre-Flight"), p.card);
	sl->addWidget(p.copy, 0, Qt::AlignLeft);
	p.hand = new QPushButton(T("InFlight.SetupHand", "Choose by hand..."), p.card);
	sl->addWidget(p.hand, 0, Qt::AlignLeft);
	p.later = new QPushButton(T("InFlight.SetupLater", "Set up later"), p.card);
	sl->addWidget(p.later, 0, Qt::AlignLeft);

	p.note = new QLabel(p.card);
	p.note->setWordWrap(true);
	p.note->setVisible(false);
	sl->addWidget(p.note);
	return p;
}
} // namespace

InflightDock::InflightDock(QWidget *parent) : QWidget(parent)
{
	settings = inflight::Settings::load();
	// The shared Stream Kit preference, held for the update checker's off switch.
	checkUpdates = updatecheck::loadSharedCheckUpdates(kitConfigRoot());

	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(6, 6, 6, 6);
	root->setSpacing(6);

	uiTest = qEnvironmentVariableIsSet("RWS_INFLIGHT_TEST") && qEnvironmentVariableIsSet("RWS_INFLIGHT_TEST_SHOT_DIR");

	netTest = qEnvironmentVariableIsSet("RWS_INFLIGHT_TEST") && qEnvironmentVariableIsSet("RWS_INFLIGHT_TEST_NET");
	levelsTest = qEnvironmentVariableIsSet("RWS_INFLIGHT_TEST") && qEnvironmentVariableIsSet("RWS_INFLIGHT_TEST_LEVELS");
	levelsGateTest = qEnvironmentVariableIsSet("RWS_INFLIGHT_TEST") && qEnvironmentVariableIsSet("RWS_INFLIGHT_TEST_LEVELS_GATE");

	panel = new LightsPanel(uiTr(), this);
	root->addWidget(panel);

	// The Levels panel is configured here but placed by the host (the Stream Kit dock's Levels section, under the
	// stage page, shown in Pre-Flight too). Hidden until the host takes it.
	levels = new LevelsPanel(uiTr(), this);
	levels->hide();
	levels->setSources(settings.micUuid, settings.desktopUuid, settings.levelsSources);
	levels->setTruePeak(settings.levelsTruePeak, false);
	levels->setLufsRef(settings.levelsLufsRefOn, settings.levelsLufsRef, false);
	levels->setMode(inflight::levelsModeFromKey(settings.levelsMode.toUtf8().constData()), false);
	levels->setOpen(settings.levelsOpen, false);
	// Visible = the containing QDockWidget says so (visibilityChanged: tabbed away, closed) and the window
	// holding it is not minimised. Show/hide events are only the fallback inside the gate.
	gate_ = new DockVisibilityGate(this, this);
	connect(gate_, &DockVisibilityGate::changed, levels, &LevelsPanel::setDockVisible);
	// Self-correcting: QDockWidget::visibilityChanged is not guaranteed to reach us (a dock already shown or
	// floating before we connected, or restored by OBS), so the panel's 1 Hz beat (while open) re-reads the live state.
	// The beat also pushes the gate's verdict into the panel, so a verdict the panel never heard (computed
	// before the connection, or its signal lost) still arrives.
	// The same recheck runs at once when the panel is opened, on this dock's Show and right after the gate is
	// built, so the meters start without waiting for the beat.
	connect(levels, &LevelsPanel::recheckRequested, this, [this] { syncLevelsGate(); });
	syncLevelsGate(); // whatever the gate said while it was constructed was never heard by the panel
	connect(levels, &LevelsPanel::openChanged, this, [this](bool open) {
		settings.levelsOpen = open;
		if (!settings.save())
			obs_log(LOG_WARNING, "could not save settings");
	});
	connect(levels, &LevelsPanel::extrasChanged, this, [this](const QStringList &extras) {
		settings.levelsSources = extras;
		if (!settings.save())
			obs_log(LOG_WARNING, "could not save settings");
	});
	connect(levels, &LevelsPanel::modeChanged, this, [this](bool lufs) {
		settings.levelsMode = lufs ? QStringLiteral("lufs") : QStringLiteral("db");
		if (!settings.save())
			obs_log(LOG_WARNING, "could not save settings");
	});
	connect(levels, &LevelsPanel::truePeakChanged, this, [this](bool on) {
		settings.levelsTruePeak = on;
		if (!settings.save())
			obs_log(LOG_WARNING, "could not save settings");
	});
	connect(levels, &LevelsPanel::lufsRefChanged, this, [this](bool on, double lufs) {
		settings.levelsLufsRefOn = on;
		settings.levelsLufsRef = lufs;
		if (!settings.save())
			obs_log(LOG_WARNING, "could not save settings");
	});

	{
		const SetupCardParts parts = buildSetupCard(this);
		setupCard = parts.card;
		copyBtn = parts.copy;
		setupNote = parts.note;
		connect(parts.copy, &QPushButton::clicked, this, [this] { copyFromPreflight(); });
		connect(parts.hand, &QPushButton::clicked, this, [this] { openSettings(); });
		connect(parts.later, &QPushButton::clicked, this, [this] {
			setupDeferred = true;
			applySetupVisibility();
		});
	}
	// The one-line note shown after the automatic copy from Pre-Flight completed the setup.
	preflightNote = makeCard(this, "ifPreflightNote");
	{
		auto *nl = new QHBoxLayout(preflightNote);
		nl->setContentsMargins(10, 6, 6, 6);
		auto *text = new QLabel(T("InFlight.PreflightNote", "Using your audio choices from Pre-Flight."),
					preflightNote);
		text->setWordWrap(true);
		nl->addWidget(text, 1);
		auto *change = new QPushButton(T("InFlight.PreflightNoteChange", "Change"), preflightNote);
		change->setObjectName(QStringLiteral("ifPreflightNoteChange"));
		change->setFlat(true);
		change->setCursor(Qt::PointingHandCursor);
		change->setStyleSheet(QStringLiteral(
			"QPushButton { border: none; background: transparent; color: palette(link); text-decoration: underline; padding: 0 4px; }"));
		nl->addWidget(change, 0);
		auto *hide = new QPushButton(QStringLiteral("x"), preflightNote);
		hide->setObjectName(QStringLiteral("ifPreflightNoteHide"));
		hide->setFlat(true);
		hide->setFixedWidth(24);
		// The dock's button style pads a button by more than 22 px, which left no room for the "x" (an empty square in the
		// screenshot). No padding and no frame: the x sits on the note's own background, like the Change link beside it.
		hide->setStyleSheet(QStringLiteral("QPushButton { border: none; background: transparent; padding: 0; }"));
		hide->setCursor(Qt::PointingHandCursor);
		hide->setAccessibleName(T("InFlight.PreflightNoteHide", "Hide this note"));
		hide->setToolTip(T("InFlight.PreflightNoteHide", "Hide this note"));
		nl->addWidget(hide, 0);
		preflightNote->setVisible(false);
		connect(change, &QPushButton::clicked, this, [this] { dismissPreflightNote(true); });
		connect(hide, &QPushButton::clicked, this, [this] { dismissPreflightNote(false); });
	}
	root->addWidget(preflightNote);
	root->addWidget(setupCard);
	root->addStretch(1);

	applySetupVisibility();

	controller = new InflightController(this);
	connect(controller, &InflightController::snapshotReady, this,
		[this](const inflight::Snapshot &snap, inflight::LiveState live, qint64 nowMs) {
			onSnapshot(snap, live, nowMs);
		});
	connect(panel, &LightsPanel::acknowledgeRequested, controller, &InflightController::acknowledge);
	connect(panel, &LightsPanel::snoozeRequested, controller, &InflightController::snooze);
	connect(panel, &LightsPanel::turnOffRequested, controller, &InflightController::turnOffOutput);
	connect(panel, &LightsPanel::settingsRequested, this, [this] { openSettings(); });
	connect(panel, &LightsPanel::newRed, this, [this] { onNewRed(); });

	// Not set up and Pre-Flight already has choices: take them without a click (also on every Show, see showEvent).
	autoCopyFromPreflight();
}

InflightDock::~InflightDock()
{
	shutdown();
}

void InflightDock::syncLevelsGate()
{
	if (closing || !gate_ || !levels)
		return;
	gate_->rebind();
	levels->setDockVisible(gate_->visible());
}

void InflightDock::resetSession()
{
	if (closing)
		return;
	if (controller)
		controller->forgetIncidentMarks(); // may tick once; its log lines are cleared right after
	if (panel)
		panel->clearLog();
}

bool InflightDock::levelsHostShown() const
{
	return gate_ && gate_->hostShown();
}

bool InflightDock::levelsGateVisible() const
{
	return gate_ && gate_->visible();
}

bool InflightDock::levelsRunning() const
{
	return levels && levels->running();
}

void InflightDock::setShownByHost(bool shown)
{
	if (closing || !gate_)
		return;
	gate_->setHostShown(shown);
	syncLevelsGate();
}

int InflightDock::sessionLogCount() const
{
	return panel ? panel->logCount() : 0;
}

void InflightDock::showEvent(QShowEvent *e)
{
	QWidget::showEvent(e);
	syncLevelsGate();
	autoCopyFromPreflight(); // Pre-Flight may have been set up since the dock was built
}

LevelsPanel *InflightDock::levelsPanel() const
{
	return levels.data();
}

bool InflightDock::testPump()
{
	if (closing)
		return false;
	QPointer<InflightDock> self(this);
	QApplication::processEvents();
	return self && !closing;
}

// Idempotent: EXIT runs it, the destructor calls it again (then a no-op). Nothing in here runs the event loop (the
// controller's probe stop is a plain bounded wait), so no deletion can run in the middle of it; the QPointer checks
// after the one call that can run foreign code (a dialog's reject() fires signals) are belt and braces.
// Order: the Levels capture first (audio callbacks off, loudness worker joined, volmeters detached: nothing may call
// into a widget or libobs after this), then the controller (timer, audio watch, Multistream source), then the lights.
void InflightDock::shutdown()
{
	if (shutDown_)
		return;
	shutDown_ = true;
	closing = true;
	QPointer<InflightDock> self(this);
	if (dialog_)
		dialog_->reject(); // an open settings dialog ends here; settingsFinished sees `closing`
	if (!self)
		return;
	if (levels)
		levels->shutdown(); // detaches every meter, removes capture callbacks, joins the worker, stops the timers (a no-op after SCRIPTING_SHUTDOWN)
	if (!self)
		return;
	if (controller)
		controller->shutdown();
	if (!self)
		return;
	if (panel)
		panel->shutdown();
	if (!self)
		return;
	updates.stop();
}

// OBS emits OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN in OBSBasic::closeEvent BEFORE ClearSceneData() (OBSBasic.cpp:1704 vs
// :1710), while EXIT comes after it. Everything that holds a source reference or an audio callback (the Levels capture,
// the audio watch) lets go here so no capture runs through the scene clean-up. The Levels shutdown is terminal and
// idempotent, and the controller keeps its watch detached; EXIT's shutdown() then finds nothing left to release.
void InflightDock::releaseSources()
{
	QPointer<InflightDock> self(this);
	if (levels)
		levels->shutdown();
	if (!self)
		return;
	if (controller)
		controller->releaseSources();
}

void InflightDock::handleFrontendEvent(obs_frontend_event event)
{
	if (event == OBS_FRONTEND_EVENT_EXIT) {
		shutdown();
		return;
	}
	if (event == OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN) {
		releaseSources();
		return;
	}
	if (closing || !controller)
		return;
	// The meters must not outlive the sources they were attached to: let go before a scene collection or
	// profile switch tears them down, take them again afterwards (if the panel is still open and visible).
	if (levels) {
		if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING || event == OBS_FRONTEND_EVENT_PROFILE_CHANGING)
			levels->setSuspended(true);
		else if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED || event == OBS_FRONTEND_EVENT_PROFILE_CHANGED)
			levels->setSuspended(false);
	}
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		if (levels) {
			syncLevelsGate(); // the dock is registered by now
			obs_log(LOG_INFO, "levels: gate dock=%d minimized=%d visible=%d", gate_->hasDock() ? 1 : 0,
				gate_->windowMinimized() ? 1 : 0, gate_->visible() ? 1 : 0);
			// OBS restores the dock layout around now: look again a second later (the 1 Hz beat covers the rest).
			QTimer::singleShot(1000, this, [this] { syncLevelsGate(); });
			if (levelsGateTest)
				QTimer::singleShot(2500, this, [this] { runGateTest(); });
			if (levelsTest) {
				// Test hook (RWS_INFLIGHT_TEST + RWS_INFLIGHT_TEST_LEVELS): open the panel without a click and count the
				// dock as visible even if the hidden testbed OBS says it is not. The natural answer is logged.
				obs_log(LOG_INFO, "levels: test gate natural_visible=%d override=1", gate_->visible() ? 1 : 0);
				// Pre-gate label check: open the panel while suspended (the meters cannot attach), log the
				// labels the rows show (a real source must read its name, never "(missing source)"), and only a
				// second later let the meters run.
				levels->setSuspended(true);
				levels->setOpen(true, false);
				obs_log(LOG_INFO, "levels: pre-gate label mic=%s desktop=%s running=%d",
					levels->rowLabel(settings.micUuid).toUtf8().constData(),
					levels->rowLabel(settings.desktopUuid).toUtf8().constData(), levels->running() ? 1 : 0);
				QTimer::singleShot(1000, this, [this] {
					if (closing || !levels)
						return;
					levels->setGateOverride(true);
					levels->armTestLog();
					levels->setSuspended(false);
					if (qEnvironmentVariableIsSet("RWS_INFLIGHT_TEST_LUFS")) {
						// Test hook (RWS_INFLIGHT_TEST_LUFS): after the dB checks have logged (3 s and 10 s after
						// the meters start), switch the real dock's Levels to LUFS with true peak on. The panel
						// logs the "lufs:" lines 5 s after that. Nothing is saved (notify = false).
						QTimer::singleShot(11500, this, [this] {
							if (closing || !levels)
								return;
							levels->setTruePeak(true, false);
							levels->armLufsTestLog();
							levels->setMode(inflight::LevelsMode::Lufs, false);
						});
					}
				});
			}
		}
		controller->start(settings);
		if (uiTest)
			QTimer::singleShot(6000, this, [this] { runUiShots(); });
	} else
		controller->handleFrontendEvent(event);
}

void InflightDock::onSnapshot(const inflight::Snapshot &snap, inflight::LiveState live, qint64 nowMs)
{
	if (closing)
		return;
	// The panel emits newRed() (taskbar flash) from inside setSnapshot. The title mark follows the
	// snapshot on every tick, both ways: it clears on acknowledge/snooze/recovery and comes back when a
	// snooze expires on a still-red incident.
	panel->setSnapshot(snap, live, nowMs);
	if (netTest)
		runNetTest(snap);
	const bool need = inflight::needsAttention(snap, nowMs);
	if (need != titleMarked)
		setTitleMark(need);
	if (live == inflight::LiveState::Live) {
		if (!lastDefiniteLive_)
			++sessionId_; // Live after a confirmed Off (or the very first report) starts a session
		lastDefiniteLive_ = true;
	} else if (live == inflight::LiveState::Off) {
		lastDefiniteLive_ = false;
	}
	emit liveReport(live, sessionId_);
	if (uiTest) {
		lastSnap = snap;
		lastLive = live;
		lastNow = nowMs;
		haveSnap = true;
	}
}

void InflightDock::setOutputBridge(InflightController::OutputBridge bridge)
{
	if (controller)
		controller->setOutputBridge(std::move(bridge));
}

void InflightDock::setMultistreamSource(inflight::MultistreamLive::Source src)
{
	if (controller)
		controller->setMultistreamSource(std::move(src));
}

// Test hook. The Network light must name a bad Multistream destination for a few ticks in a row (so the
// harness sees the red verdict in the log first); then its details are opened (the real panel, the real
// button), the host is told to take its offscreen screenshot, and 2.5 s later the real "Turn off <name>"
// button is pressed once. Nothing is shown on screen: the panel is just a widget.
void InflightDock::runNetTest(const inflight::Snapshot &snap)
{
	using namespace inflight;
	const LightResult &n = snap.at(Light::Network);
	const bool badDest = (n.state == LightState::Red || n.state == LightState::Amber) && n.incidentId != 0 && n.net.valid &&
			     !n.net.outputId.isEmpty();
	if (!badDest) {
		netBadTicks = 0;
		return;
	}
	if (++netBadTicks < 3 || netClickScheduled)
		return;
	netClickScheduled = true;
	panel->showDetails(Light::Network);
	obs_log(LOG_INFO, "TEST net: details open, button visible=%d label=\"%s\"", panel->turnOffVisible() ? 1 : 0,
		panel->turnOffLabel().toUtf8().constData());
	// The state of the light as the panel shows it at the moment of the grab (the harness asserts this, never a colour).
	{
		const LightState shown = panel->shownState(Light::Network);
		const char letter = shown == LightState::Red ? 'R' : shown == LightState::Amber ? 'A' : shown == LightState::Green ? 'G'
				    : shown == LightState::Unknown ? 'U' : 'g';
		obs_log(LOG_INFO, "TEST net: light shown at grab=%c reason=%s pct30=%.1f out=%s", letter,
			n.reason == Reason::NetworkDrops ? "drops" : "other", n.value, n.net.outputName.toUtf8().constData());
	}
	emit testNetRed();
	QTimer::singleShot(2500, this, [this] {
		if (closing)
			return;
		const bool clicked = panel->testClickTurnOff();
		obs_log(LOG_INFO, "TEST net: Turn off button clicked=%d", clicked ? 1 : 0);
	});
}

void InflightDock::setTitleMark(bool on)
{
	// The dock title is the host's (the Kit dock puts the mark in front of its own title).
	titleMarked = on;
	emit attentionChanged(on);
}

void InflightDock::onNewRed()
{
	if (closing)
		return;
	if (settings.chime)
		QApplication::beep(); // the only sound path: the system beep, once per new red
	// One taskbar flash per new red, raised by the host (the Kit dock owns the one alert).
	emit taskbarAlertRequested();
}

void InflightDock::runUiShots()
{
	if (closing)
		return;
	using namespace inflight;
	const QString dir = qEnvironmentVariable("RWS_INFLIGHT_TEST_SHOT_DIR");
	QDir().mkpath(dir);

	auto save = [&dir](LightsPanel *p, const char *name) {
		const QPixmap px = p->grab();
		if (!px.isNull() && px.save(QDir(dir).filePath(QString::fromLatin1(name)), "PNG"))
			obs_log(LOG_INFO, "ui-shot saved %s (%dx%d)", name, px.width(), px.height());
		else
			obs_log(LOG_WARNING, "ui-shot failed %s", name);
	};
	// A second, parentless panel that is never shown: it renders offscreen only.
	auto make = []() {
		auto *p = new LightsPanel(uiTr(), nullptr);
		p->setAttribute(Qt::WA_DontShowOnScreen);
		p->resize(360, 560);
		return p;
	};

	// a) the controller's latest real snapshot
	{
		LightsPanel *p = make();
		if (haveSnap) {
			p->setSnapshot(lastSnap, lastLive, lastNow);
			save(p, "ui-live.png");
		} else {
			obs_log(LOG_WARNING, "ui-shot failed %s", "ui-live.png");
		}
		delete p;
	}

	// b) a synthetic snapshot with one light of each state
	{
		LightsPanel *p = make();
		const qint64 now = 2000000;
		auto setLight = [](Snapshot &s, Light l, LightState st, Reason r, double value, qint64 sinceAgoMs,
				   quint64 id) {
			LightResult &x = s.lights[static_cast<size_t>(l)];
			x.state = st;
			x.reason = r;
			x.value = value;
			x.sinceMs = 2000000 - sinceAgoMs;
			x.incidentId = id;
		};
		auto ev = [](IncidentEventKind k, Light l, quint64 id, Reason r, qint64 agoMs) {
			IncidentEvent e;
			e.kind = k;
			e.light = l;
			e.incidentId = id;
			e.reason = r;
			e.atMs = 2000000 - agoMs;
			return e;
		};
		// An earlier snapshot: a scene incident that has since ended, so its log lines read properly.
		Snapshot a;
		setLight(a, Light::Scene, LightState::Amber, Reason::SceneOverLimit, 30, 30000, 9);
		a.events.push_back(ev(IncidentEventKind::Started, Light::Scene, 9, Reason::SceneOverLimit, 1500000));
		p->setSnapshot(a, LiveState::Live, now);

		Snapshot s;
		for (LightResult &l : s.lights) {
			l.state = LightState::Grey;
			l.reason = Reason::OffAir;
		}
		setLight(s, Light::Mic, LightState::Red, Reason::MicMuted, 75, 15000, 11);
		setLight(s, Light::Desktop, LightState::Amber, Reason::DesktopDigitalSilence, 130, 130000, 12);
		setLight(s, Light::Scene, LightState::Green, Reason::Ok, 0, 0, 0);
		setLight(s, Light::Recording, LightState::Red, Reason::RecordingStopped, 0, 600000, 13);
		s.lights[static_cast<size_t>(Light::Recording)].acknowledged = true;
		setLight(s, Light::Network, LightState::Red, Reason::NetworkDrops, 3.4, 240000, 14);
		s.lights[static_cast<size_t>(Light::Network)].snoozedUntilMs = now + 400000;
		setLight(s, Light::Pc, LightState::Unknown, Reason::PcWindowFilling, -1, 5000, 0);
		setLight(s, Light::Stream, LightState::Grey, Reason::MainStreamOff, 0, 0, 0);
		s.worst = LightState::Red;
		s.events.push_back(ev(IncidentEventKind::Ended, Light::Scene, 9, Reason::SceneOverLimit, 1200000));
		s.events.push_back(ev(IncidentEventKind::Started, Light::Recording, 13, Reason::RecordingStopped, 600000));
		s.events.push_back(ev(IncidentEventKind::Acknowledged, Light::Recording, 13, Reason::RecordingStopped, 540000));
		s.events.push_back(ev(IncidentEventKind::Started, Light::Network, 14, Reason::NetworkDrops, 240000));
		s.events.push_back(ev(IncidentEventKind::Escalated, Light::Network, 14, Reason::NetworkDrops, 200000));
		s.events.push_back(ev(IncidentEventKind::Started, Light::Desktop, 12, Reason::DesktopDigitalSilence, 130000));
		s.events.push_back(ev(IncidentEventKind::Started, Light::Mic, 11, Reason::MicMuted, 75000));
		// An incident that started and ended inside one snapshot: its lines use the value-free label.
		s.events.insert(s.events.begin() + 1, ev(IncidentEventKind::Started, Light::Pc, 20, Reason::PcEncoderLag, 900000));
		s.events.insert(s.events.begin() + 2, ev(IncidentEventKind::Ended, Light::Pc, 20, Reason::PcEncoderLag, 880000));
		p->setSnapshot(s, LiveState::Live, now);
		p->toggleDetails(Light::Mic);
		p->setLogExpanded(true);
		save(p, "ui-demo.png");
		delete p;
	}

	// c) the Levels panel, demo numbers: mic, desktop, one silent extra, one missing extra. Parentless and
	// never shown: no meters, no timer (demo mode). The backdrop gives it the dock's own background and margin.
	{
		auto *backdrop = new QWidget(nullptr);
		backdrop->setAttribute(Qt::WA_DontShowOnScreen);
		backdrop->setAutoFillBackground(true);
		auto *bl = new QVBoxLayout(backdrop);
		bl->setContentsMargins(6, 6, 6, 6);
		auto *lp = new LevelsPanel(uiTr(), backdrop);
		lp->setDemoMode(true);
		lp->setSources(QStringLiteral("demo-mic"), QStringLiteral("demo-desktop"),
			       {QStringLiteral("demo-silent"), QStringLiteral("demo-nodata"), QStringLiteral("demo-gone")});
		lp->setOpen(true, false);
		lp->demoSet(QStringLiteral("demo-mic"), QStringLiteral("Mic (Shure SM7B through the interface)"), -18.2f, -24.6f);
		lp->demoSet(QStringLiteral("demo-desktop"), QStringLiteral("Game audio"), -30.1f, -36.0f);
		lp->demoSet(QStringLiteral("demo-silent"), QStringLiteral("Alerts"), -std::numeric_limits<float>::infinity(),
			    -std::numeric_limits<float>::infinity());
		lp->demoSetNoData(QStringLiteral("demo-nodata"), QStringLiteral("Capture card (no signal)"));
		bl->addWidget(lp);
		const int w = 360;
		bl->activate();
		backdrop->resize(w, bl->totalHeightForWidth(w) > 0 ? bl->totalHeightForWidth(w) : backdrop->sizeHint().height());
		const QPixmap px = backdrop->grab();
		if (!px.isNull() && px.save(QDir(dir).filePath(QStringLiteral("levels-demo.png")), "PNG"))
			obs_log(LOG_INFO, "ui-shot saved %s (%dx%d)", "levels-demo.png", px.width(), px.height());
		else
			obs_log(LOG_WARNING, "ui-shot failed %s", "levels-demo.png");
		delete backdrop;
	}
	// d) the same panel in LUFS mode with true peak on and the optional reference tick on (-16 LUFS, a demo value:
	// the real default is off): a loud mic, a quieter desktop, silence, a source whose short-term window is not
	// full yet ("--"), a missing source.
	{
		auto *backdrop = new QWidget(nullptr);
		backdrop->setAttribute(Qt::WA_DontShowOnScreen);
		backdrop->setAutoFillBackground(true);
		auto *bl = new QVBoxLayout(backdrop);
		bl->setContentsMargins(6, 6, 6, 6);
		auto *lp = new LevelsPanel(uiTr(), backdrop);
		lp->setDemoMode(true);
		lp->setTruePeak(true, false);
		lp->setLufsRef(true, -16.0, false);
		lp->setMode(inflight::LevelsMode::Lufs, false);
		lp->setSources(QStringLiteral("demo-mic"), QStringLiteral("demo-desktop"),
			       {QStringLiteral("demo-silent"), QStringLiteral("demo-warm"), QStringLiteral("demo-gone")});
		lp->setOpen(true, false);
		const double inf = std::numeric_limits<double>::infinity();
		lp->demoSetLufs(QStringLiteral("demo-mic"), QStringLiteral("Mic/Aux"), true, -18.4, true, -19.2, -1.2);
		lp->demoSetLufs(QStringLiteral("demo-desktop"), QStringLiteral("Desktop Audio"), true, -27.0, true, -25.3, -9.8);
		lp->demoSetLufs(QStringLiteral("demo-silent"), QStringLiteral("Alerts"), true, -inf, true, -inf, -inf);
		lp->demoSetLufs(QStringLiteral("demo-warm"), QStringLiteral("Capture card (just started)"), true, -31.5, false,
				-inf, std::numeric_limits<double>::quiet_NaN());
		bl->addWidget(lp);
		const int w = 360;
		bl->activate();
		backdrop->resize(w, bl->totalHeightForWidth(w) > 0 ? bl->totalHeightForWidth(w) : backdrop->sizeHint().height());
		const QPixmap px = backdrop->grab();
		if (!px.isNull() && px.save(QDir(dir).filePath(QStringLiteral("levels-lufs-demo.png")), "PNG"))
			obs_log(LOG_INFO, "ui-shot saved %s (%dx%d)", "levels-lufs-demo.png", px.width(), px.height());
		else
			obs_log(LOG_WARNING, "ui-shot failed %s", "levels-lufs-demo.png");
		// The labels, the "?" link (log only) and the Reference label, which exists only while Reference is on.
		lp->setHelpLogOnly(true);
		lp->logTestState("lufs-ref-on");
		lp->testClickHelp();
		lp->setLufsRef(false, -16.0, false);
		lp->logTestState("lufs-ref-off");
		delete backdrop;
	}
	if (!runSettingsShots())
		return; // a pump ran OBS's exit and `this` may be gone: runApplyTests() would read freed members
	if (qEnvironmentVariableIsSet("RWS_INFLIGHT_TEST_APPLY"))
		runApplyTests();
}

// Test hook (RWS_INFLIGHT_TEST + RWS_INFLIGHT_TEST_LEVELS_GATE): the visibility gate against real Qt dock
// behaviour, in a parentless QMainWindow that is shown with WA_DontShowOnScreen (no native window, nothing
// on screen). Two tabified docks; one holds an open LevelsPanel on the fixture sources, so meters really
// attach. Each step is checked after processEvents: attached meters (2 when visible, 0 when not) and the
// timer. Every step and every visibilityChanged signal is logged, pass or fail.
void InflightDock::runGateTest()
{
	if (closing)
		return;
	// Copied up front: a pump below can run OBS's exit, after which `this` may be gone. Once testPump() says so
	// this function frees the fixtures it owns itself (they are not children of the dock) and RETURNS at once:
	// nothing after a failed pump may touch `this` (uiTr(), settings, ...).
	const QString micUuid = settings.micUuid;
	const QString desktopUuid = settings.desktopUuid;
	auto *win = new QMainWindow(nullptr);
	win->setAttribute(Qt::WA_DontShowOnScreen);
	win->resize(500, 400);
	auto *host = new QWidget();
	auto *hl = new QVBoxLayout(host);
	auto *lp = new LevelsPanel(uiTr(), host);
	hl->addWidget(lp);
	lp->setSources(micUuid, desktopUuid, QStringList());
	auto *dockA = new QDockWidget(QStringLiteral("GateA"), win);
	dockA->setWidget(host);
	auto *dockB = new QDockWidget(QStringLiteral("GateB"), win);
	dockB->setWidget(new QLabel(QStringLiteral("other"), dockB));
	win->addDockWidget(Qt::RightDockWidgetArea, dockA);
	win->addDockWidget(Qt::RightDockWidgetArea, dockB);
	win->tabifyDockWidget(dockA, dockB);
	auto *gate = new DockVisibilityGate(host, win);
	connect(gate, &DockVisibilityGate::changed, lp, &LevelsPanel::setDockVisible);
	connect(lp, &LevelsPanel::recheckRequested, gate, [gate, lp] {
		gate->rebind();
		lp->setDockVisible(gate->visible());
	});
	connect(dockA, &QDockWidget::visibilityChanged, win,
		[](bool v) { obs_log(LOG_INFO, "levels-gate-test signal dockA visibilityChanged(%d)", v ? 1 : 0); });
	lp->setOpen(true, false);
	win->show();
	auto abortRun = [&]() {
		obs_log(LOG_INFO, "levels-gate-test aborted: OBS is exiting");
		lp->shutdown();
		delete win; // the panel's destructor detaches whatever is left
	};
	if (!testPump()) {
		abortRun();
		return;
	}

	QString firstFail;
	auto selectTab = [win](QDockWidget *d) {
		const auto bars = win->findChildren<QTabBar *>();
		for (QTabBar *tb : bars) {
			for (int i = 0; i < tb->count(); ++i) {
				if (tb->tabText(i) == d->windowTitle()) {
					tb->setCurrentIndex(i);
					return true;
				}
			}
		}
		return false;
	};
	// false = the pump says OBS is exiting: the caller must abortRun() and return.
	auto check = [&](const char *step, bool expectRunning) {
		if (!testPump())
			return false;
		const int att = lp->attachedMeters();
		const bool timer = lp->timerActive();
		const bool ok = expectRunning ? (att == 2 && timer) : (att == 0 && !timer);
		obs_log(LOG_INFO,
			"levels-gate-test step %s: attached=%d timer=%d gate=%d dockHasDock=%d minimized=%d dockA.isVisible=%d expect=%s -> %s",
			step, att, timer ? 1 : 0, gate->visible() ? 1 : 0, gate->hasDock() ? 1 : 0, gate->windowMinimized() ? 1 : 0,
			dockA->isVisible() ? 1 : 0, expectRunning ? "running" : "stopped", ok ? "ok" : "MISMATCH");
		if (!ok && firstFail.isEmpty())
			firstFail = QString::fromLatin1(step);
		return true;
	};

	if (!selectTab(dockA)) {
		obs_log(LOG_INFO, "levels-gate-test: no tab bar found (tabs=%d)", static_cast<int>(win->findChildren<QTabBar *>().size()));
		if (firstFail.isEmpty())
			firstFail = QStringLiteral("no-tab-bar");
	}
	if (!check("start", true)) {
		abortRun();
		return;
	}
	selectTab(dockB);
	if (!check("tab-away", false)) {
		abortRun();
		return;
	}
	selectTab(dockA);
	if (!check("tab-back", true)) {
		abortRun();
		return;
	}
	dockA->close();
	if (!check("close", false)) {
		abortRun();
		return;
	}
	dockA->show();
	selectTab(dockA);
	if (!check("reopen", true)) {
		abortRun();
		return;
	}
	win->setWindowState(Qt::WindowMinimized);
	if (!check("minimize", false)) {
		abortRun();
		return;
	}
	win->setWindowState(Qt::WindowNoState);
	if (!check("restore", true)) {
		abortRun();
		return;
	}
	obs_log(LOG_INFO, "levels-gate-test tabs: dockA tabMatch=%s (data = the tab's quintptr data matched the dock)",
		gate->tabMatch().toUtf8().constData());

	// ---- Floating docks (the field case). Each fixture is a parentless QMainWindow with ONE floating
	// QDockWidget; the dock is its own window, so WA_DontShowOnScreen is set on it too (nothing on screen).
	// Meters are real (the fixture sources), 2 attached when running.
	struct Fx {
		QMainWindow *win = nullptr;
		QDockWidget *dock = nullptr;
		QWidget *host = nullptr;
		LevelsPanel *lp = nullptr;
		DockVisibilityGate *gate = nullptr;
	};
	auto makeFx = [&](const char *title, bool shownFirst) {
		Fx f;
		f.win = new QMainWindow(nullptr);
		f.win->setAttribute(Qt::WA_DontShowOnScreen);
		f.win->resize(500, 400);
		f.dock = new QDockWidget(QString::fromLatin1(title), f.win);
		f.dock->setAttribute(Qt::WA_DontShowOnScreen);
		f.win->addDockWidget(Qt::RightDockWidgetArea, f.dock);
		f.dock->setFloating(true);
		if (!shownFirst)
			f.dock->hide();
		f.win->show();
		if (shownFirst)
			f.dock->show();
		if (!testPump()) { // the dock is shown (or hidden) and floating BEFORE panel and gate exist
			delete f.win; // exit is under way: free this fixture and let the caller bail out (no uiTr() below)
			return Fx();
		}
		f.host = new QWidget();
		auto *l = new QVBoxLayout(f.host);
		f.lp = new LevelsPanel(uiTr(), f.host);
		l->addWidget(f.lp);
		f.lp->setSources(micUuid, desktopUuid, QStringList());
		f.dock->setWidget(f.host);
		f.gate = new DockVisibilityGate(f.host, f.win);
		connect(f.gate, &DockVisibilityGate::changed, f.lp, &LevelsPanel::setDockVisible);
		// Connected AFTER the gate was built, as in the dock: whatever the gate said at construction was never
		// heard by the panel (the field case). Only the 1 Hz beat can bring it over.
		connect(f.lp, &LevelsPanel::recheckRequested, f.gate, [g = f.gate, p = f.lp] {
			g->rebind();
			p->setDockVisible(g->visible());
		});
		f.lp->setOpen(true, false);
		return f;
	};
	auto dropFx = [](Fx &f) {
		f.lp->shutdown();
		delete f.win; // takes the floating dock, the host, the panel and the gate with it
		f = Fx();
	};
	// Wait up to `ms` (real time, events pumped) for the expected state; logs how long it took. false = the pump says
	// OBS is exiting: the caller drops its fixture, abortRun()s and returns (see `bail`).
	auto waitStep = [&](const char *step, const Fx &f, bool expectRunning, int ms) {
		QElapsedTimer et;
		et.start();
		bool ok = false;
		int att = 0;
		bool tm = false;
		do {
			if (!testPump())
				return false;
			att = f.lp->attachedMeters();
			tm = f.lp->timerActive();
			ok = expectRunning ? (att == 2 && tm) : (att == 0 && !tm);
			if (ok)
				break;
			QThread::msleep(25);
		} while (et.elapsed() < ms);
		obs_log(LOG_INFO,
			"levels-gate-test step %s: attached=%d timer=%d gate=%d dock.isVisible=%d floating=%d minimized=%d tabbed=%d after=%lldms expect=%s -> %s",
			step, att, tm ? 1 : 0, f.gate->visible() ? 1 : 0, f.dock->isVisible() ? 1 : 0, f.dock->isFloating() ? 1 : 0,
			f.gate->windowMinimized() ? 1 : 0, f.gate->state().tabbed ? 1 : 0, static_cast<long long>(et.elapsed()),
			expectRunning ? "running" : "stopped", ok ? "ok" : "MISMATCH");
		if (!ok && firstFail.isEmpty())
			firstFail = QString::fromLatin1(step);
		return true;
	};
	// A failed pump: free the fixture and the base window, then the caller returns.
	auto bail = [&](Fx &f) {
		if (f.win)
			dropFx(f);
		abortRun();
	};

	// (a1) already shown and floating before the panel / gate exist: meters attach within ~1.5 s.
	{
		Fx f = makeFx("GateFloatShown", true);
		if (!f.win) {
			abortRun();
			return;
		}
		if (!waitStep("float-shown-before-gate", f, true, 1500)) {
			bail(f);
			return;
		}
		// (b) floating dock minimised -> stop; restored -> run.
		f.dock->setWindowState(Qt::WindowMinimized);
		if (!waitStep("float-minimize", f, false, 1500)) {
			bail(f);
			return;
		}
		f.dock->setWindowState(Qt::WindowNoState);
		if (!waitStep("float-restore", f, true, 1500)) {
			bail(f);
			return;
		}
		f.dock->hide();
		if (!waitStep("float-hide", f, false, 1500)) {
			bail(f);
			return;
		}
		f.dock->show();
		if (!waitStep("float-show", f, true, 1500)) {
			bail(f);
			return;
		}
		dropFx(f);
	}

	// (a2) the same, with every notification lost: the gate is bound to a hidden floating dock, then the dock
	// is shown with no visibilityChanged and no Show event reaching the gate (filters removed, signal blocked).
	// Only the 1 Hz recheck can notice. Reproduces "visibilityChanged never reached us".
	{
		Fx f = makeFx("GateFloatLost", false);
		if (!f.win) {
			abortRun();
			return;
		}
		if (!waitStep("float-lost-premise-stopped", f, false, 0)) {
			bail(f);
			return;
		}
		f.dock->removeEventFilter(f.gate);
		f.host->removeEventFilter(f.gate);
		f.dock->blockSignals(true);
		f.dock->show();
		const bool alive = testPump();
		f.dock->blockSignals(false);
		if (!alive) {
			bail(f);
			return;
		}
		obs_log(LOG_INFO, "levels-gate-test float-lost: dock.isVisible=%d gate.visible=%d (gate must still be stale here)",
			f.dock->isVisible() ? 1 : 0, f.gate->visible() ? 1 : 0);
		if (!waitStep("float-lost-signal-1hz", f, true, 1500)) {
			bail(f);
			return;
		}
		dropFx(f);
	}

	if (firstFail.isEmpty())
		obs_log(LOG_INFO, "levels-gate-test ok");
	else
		obs_log(LOG_WARNING, "levels-gate-test FAIL %s", firstFail.toUtf8().constData());
	lp->shutdown();
	delete win; // the panel's destructor detaches whatever is left
}

bool InflightDock::runSettingsShots()
{
	if (closing)
		return true;
	const QString dir = qEnvironmentVariable("RWS_INFLIGHT_TEST_SHOT_DIR");
	QDir().mkpath(dir);
	auto save = [&dir](QWidget *w, const QString &name) {
		const QPixmap px = w->grab();
		if (!px.isNull() && px.save(QDir(dir).filePath(name), "PNG"))
			obs_log(LOG_INFO, "settings-shot saved %s (%dx%d)", name.toUtf8().constData(), px.width(),
				px.height());
		else
			obs_log(LOG_WARNING, "settings-shot failed %s", name.toUtf8().constData());
	};
	// A dialog is shown only with WA_DontShowOnScreen (rendered offscreen, no native window) and is
	// made non-modal first so it cannot block anything. Parentless, closed and deleted after the grabs.
	auto offscreen = [](inflight::Settings s) {
		auto *d = new InflightSettingsDialog(s, nullptr);
		d->setAttribute(Qt::WA_DontShowOnScreen);
		d->setWindowModality(Qt::NonModal);
		d->show();
		return d;
	};

	// Every pump below can run OBS's exit; testPump() says so, and then nothing here touches `this` again and the
	// function returns false so the CALLER does not touch it either.
	bool dead = false;
	// a) the real settings, one grab per tab
	{
		InflightSettingsDialog *d = offscreen(settings);
		for (int i = 0; i < d->tabCount() && !dead; ++i) {
			d->showTab(i);
			if (!testPump()) {
				dead = true;
				break;
			}
			save(d, QStringLiteral("settings-%1.png").arg(d->tabKey(i)));
			if (d->tabKey(i) == QStringLiteral("advanced")) {
				d->scrollCurrentTabToEnd();
				if (!testPump()) {
					dead = true;
					break;
				}
				save(d, QStringLiteral("settings-advanced-end.png"));
			}
		}
		d->close();
		d->deleteLater();
	}
	if (dead)
		return false;

	// b) values that need adjusting: OK shows the note and keeps the dialog open
	{
		inflight::Settings bad = settings;
		bad.micMutedAmberS = 100;
		bad.micMutedRedS = 102;
		bad.dropAmberPct = 3.0;
		bad.dropRedPct = 2.0;
		inflight::HoldingScene dup;
		dup.sceneUuid = bad.holdingScenes.isEmpty() ? QStringLiteral("x") : bad.holdingScenes[0].sceneUuid;
		dup.label = QStringLiteral("Be Right Back");
		dup.limitSeconds = 600;
		bad.holdingScenes.push_back(dup);
		InflightSettingsDialog *d = offscreen(bad);
		// The dialog adjusts the out-of-range values when it opens and shows the note itself.
		d->showTab(3);
		if (!testPump())
			dead = true;
		else
			save(d, QStringLiteral("settings-note.png"));
		d->close();
		d->deleteLater();
	}
	if (dead)
		return false;

	// c) a spare, never-shown copy of the setup card
	{
		// The card sits on the dock's own background in real use, so the spare gets a themed
		// backdrop and the dock's 6 px margin; the card's wash alone would render over white.
		auto *backdrop = new QWidget(nullptr);
		backdrop->setAttribute(Qt::WA_DontShowOnScreen);
		backdrop->setAutoFillBackground(true);
		auto *bl = new QVBoxLayout(backdrop);
		bl->setContentsMargins(6, 6, 6, 6);
		SetupCardParts parts = buildSetupCard(backdrop);
		bl->addWidget(parts.card);
		parts.note->setText(T("InFlight.NeedMic", "Copied what was found. Still needed: your microphone."));
		parts.note->setVisible(true);
		const int w = 340;
		bl->activate();
		backdrop->resize(w, bl->totalHeightForWidth(w));
		save(backdrop, QStringLiteral("settings-setupcard.png"));
		delete backdrop;
	}
	return true;
}

// Test hook (RWS_INFLIGHT_TEST + RWS_INFLIGHT_TEST_APPLY): drives the REAL openSettings/settingsFinished
// path offscreen (WA_DontShowOnScreen, open() instead of exec()), pressing the real OK / Cancel buttons.
void InflightDock::runApplyTests()
{
	if (closing)
		return;
	const int kNewAmber = 30; // what the apply test sets and then expects on disk and in the controller
	const QString file = QDir(ownConfigDir()).filePath(QStringLiteral("settings.json"));
	auto readFile = [&file]() {
		QFile f(file);
		return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
	};
	auto verdict = [](const char *name, const QString &why) {
		if (why.isEmpty())
			obs_log(LOG_INFO, "%s ok", name);
		else
			obs_log(LOG_WARNING, "%s FAIL %s", name, why.toUtf8().constData());
	};
	// Opens the dialog the way the dock does, finds the named widgets and the OK/Cancel buttons.
	struct Handles {
		InflightSettingsDialog *d = nullptr;
		QSpinBox *amber = nullptr;
		QCheckBox *net = nullptr;
		QPushButton *ok = nullptr;
		QPushButton *cancel = nullptr;
	};
	auto open = [this]() {
		Handles h;
		h.d = openSettings(true);
		if (!h.d)
			return h;
		h.amber = h.d->findChild<QSpinBox *>(QStringLiteral("micMutedAmber"));
		h.net = h.d->findChild<QCheckBox *>(QStringLiteral("lightNetwork"));
		if (auto *bb = h.d->findChild<QDialogButtonBox *>()) {
			h.ok = bb->button(QDialogButtonBox::Ok);
			h.cancel = bb->button(QDialogButtonBox::Cancel);
		}
		return h;
	};

	// ---- OK: values reach the file (unknown keys kept) and the controller
	{
		QString why;
		Handles h = open();
		if (!h.d || !h.amber || !h.net || !h.ok) {
			why = QStringLiteral("dialog or widgets not found");
		} else {
			h.amber->setValue(kNewAmber);
			h.net->setChecked(false);
			h.ok->click(); // the normal accept path: finished -> settingsFinished applies and saves
			const QJsonObject o = QJsonDocument::fromJson(readFile()).object();
			if (o.value(QStringLiteral("mic_muted_amber_s")).toInt() != kNewAmber)
				why += QStringLiteral("file amber=%1 ").arg(o.value(QStringLiteral("mic_muted_amber_s")).toInt());
			if (!o.value(QStringLiteral("light_network")).isBool() || o.value(QStringLiteral("light_network")).toBool())
				why += QStringLiteral("file network light still on ");
			const QJsonArray hs = o.value(QStringLiteral("holding_scenes")).toArray();
			if (hs.isEmpty() || hs.at(0).toObject().value(QStringLiteral("future_key")).toString() !=
						    QStringLiteral("keep-me"))
				why += QStringLiteral("holding scene future_key lost ");
			if (settings.micMutedAmberS != kNewAmber || settings.lightNetwork)
				why += QStringLiteral("dock settings not updated ");
			if (!controller || controller->currentSettings().micMutedAmberS != kNewAmber ||
			    controller->currentSettings().lightNetwork)
				why += QStringLiteral("controller did not receive it ");
			if (controller)
				obs_log(LOG_INFO, "apply-test controller amber=%d network=%d", controller->currentSettings().micMutedAmberS,
					controller->currentSettings().lightNetwork ? 1 : 0);
			if (dialog_)
				why += QStringLiteral("dialog still held ");
		}
		verdict("apply-test", why.trimmed());
	}

	// ---- Levels round trip: the file was saved through the real OK path above. Fixture (test-ui.ps1):
	// levels_open true, levels_sources [{uuid: Tone, future_key: keep}, {uuid: missing-uuid}].
	{
		QString why;
		const QJsonObject o = QJsonDocument::fromJson(readFile()).object();
		if (!o.value(QStringLiteral("levels_open")).toBool(false))
			why += QStringLiteral("levels_open lost ");
		const QJsonArray ls = o.value(QStringLiteral("levels_sources")).toArray();
		QStringList seen;
		bool futureKept = false;
		bool missingListed = false;
		for (const QJsonValue &v : ls) {
			const QString u = v.toObject().value(QStringLiteral("uuid")).toString();
			if (seen.contains(u))
				why += QStringLiteral("duplicate %1 ").arg(u);
			seen << u;
			if (u == QStringLiteral("missing-uuid"))
				missingListed = true;
			if (v.toObject().value(QStringLiteral("future_key")).toString() == QStringLiteral("keep"))
				futureKept = true;
		}
		if (!futureKept)
			why += QStringLiteral("Tone entry's future_key lost ");
		if (!missingListed)
			why += QStringLiteral("missing-uuid entry dropped ");
		if (ls.size() != 2)
			why += QStringLiteral("entries=%1 ").arg(ls.size());
		if (!settings.levelsOpen || settings.levelsSources.size() != 2)
			why += QStringLiteral("dock settings lost the levels fields ");
		// Levels keys (fixture: lufs, true peak on, reference on at -18.5): kept by the merge-preserving save.
		if (o.value(QStringLiteral("levels_mode")).toString() != QStringLiteral("lufs"))
			why += QStringLiteral("levels_mode lost ");
		if (!o.value(QStringLiteral("levels_true_peak")).toBool(false))
			why += QStringLiteral("levels_true_peak lost ");
		if (!o.value(QStringLiteral("levels_lufs_ref_on")).toBool(false) ||
		    qAbs(o.value(QStringLiteral("levels_lufs_ref")).toDouble(0.0) + 18.5) > 1e-9)
			why += QStringLiteral("levels_lufs_ref lost ");
		if (settings.levelsMode != QStringLiteral("lufs") || !settings.levelsTruePeak || !settings.levelsLufsRefOn ||
		    qAbs(settings.levelsLufsRef + 18.5) > 1e-9)
			why += QStringLiteral("dock settings lost the lufs fields ");
		if (!levels || levels->mode() != inflight::LevelsMode::Lufs || !levels->truePeak())
			why += QStringLiteral("panel did not start in the saved mode ");
		verdict("levels-roundtrip", why.trimmed());
	}

	// ---- Cancel: nothing is written or applied
	{
		QString why;
		const QByteArray before = readFile();
		Handles h = open();
		if (!h.d || !h.amber || !h.cancel) {
			why = QStringLiteral("dialog or widgets not found");
		} else {
			h.amber->setValue(45);
			h.cancel->click();
			if (readFile() != before)
				why += QStringLiteral("file changed ");
			if (settings.micMutedAmberS != kNewAmber ||
			    (controller && controller->currentSettings().micMutedAmberS != kNewAmber))
				why += QStringLiteral("settings changed ");
			if (dialog_)
				why += QStringLiteral("dialog still held ");
		}
		verdict("cancel-test", why.trimmed());
	}
}

InflightSettingsDialog *InflightDock::openSettings(bool testOpenOnly)
{
	if (closing || dialog_)
		return nullptr;
	inflight::Settings cur = settings;
	// The Kit-wide preference may have been changed by another tool since we loaded it.
	cur.updateChecks = updatecheck::loadSharedCheckUpdates(kitConfigRoot());
	dialogOpenedUpdates = cur.updateChecks;
	// On the heap, parented to the dock, held by a QPointer; deleted after it finishes.
	auto *dlg = new InflightSettingsDialog(cur, this);
	dialog_ = dlg;
	connect(dlg, &QDialog::finished, this, [this, dlg](int rc) { settingsFinished(dlg, rc); });
	if (testOpenOnly) {
		dlg->setAttribute(Qt::WA_DontShowOnScreen); // test hook: rendered offscreen, never visible
		dlg->open();
		return dlg;
	}
	dlg->exec(); // the finished signal has applied the result by the time this returns
	return nullptr;
}

void InflightDock::settingsFinished(InflightSettingsDialog *dlg, int rc)
{
	const bool accepted = rc == QDialog::Accepted;
	inflight::Settings out;
	if (accepted && dlg)
		out = dlg->result();
	if (dlg)
		dlg->deleteLater();
	dialog_ = nullptr;
	if (closing || !accepted)
		return;

	// Setup counts as done once a mic is chosen, or the Mic light is off on purpose.
	out.setupDone = !out.micUuid.isEmpty() || !out.lightMic;
	settings = out;
	if (levels)
		levels->setSources(settings.micUuid, settings.desktopUuid, settings.levelsSources);
	// The shared Kit preference is written only when this dialog changed it.
	if (settings.updateChecks != dialogOpenedUpdates)
		updatecheck::saveSharedCheckUpdates(kitConfigRoot(), settings.updateChecks);
	checkUpdates = updatecheck::loadSharedCheckUpdates(kitConfigRoot());
	if (!checkUpdates)
		updates.stop();
	if (!settings.save())
		obs_log(LOG_WARNING, "could not save settings");
	if (controller)
		controller->setSettings(settings);
	if (settings.setupDone) {
		setupNote->setVisible(false);
	} else {
		setupDeferred = false; // the card comes back until a mic is chosen
		setupNote->setText(T("InFlight.StillNeedMic", "Still needed: your microphone."));
		setupNote->setVisible(true);
	}
	applySetupVisibility();
	obs_log(LOG_INFO, "settings saved");
}

QString InflightDock::preflightFile() const
{
	return inflight::preflightSettingsPath(ownConfigDir());
}

void InflightDock::applySetupVisibility()
{
	setupCard->setVisible(!settings.setupDone && !setupDeferred);
	const QString path = preflightFile();
	const bool exists = !path.isEmpty() && QFileInfo::exists(path);
	copyBtn->setEnabled(exists);
	copyBtn->setToolTip(exists ? QString()
				   : T("InFlight.SetupCopyNone", "Pre-Flight has no saved choices on this computer."));
	applyPreflightNote();
}

void InflightDock::applyPreflightNote()
{
	if (preflightNote)
		preflightNote->setVisible(autoCopied_ && settings.setupDone && !settings.preflightNoteSeen);
}

void InflightDock::dismissPreflightNote(bool openSettingsAfter)
{
	if (closing)
		return;
	settings.preflightNoteSeen = true;
	if (!settings.save())
		obs_log(LOG_WARNING, "could not save settings");
	applyPreflightNote();
	if (openSettingsAfter)
		openSettings();
}

bool InflightDock::importPreflightChoices(QString *failNote)
{
	constexpr qint64 kMaxFileBytes = 256 * 1024;
	QByteArray bytes;
	QFile file(preflightFile());
	if (file.size() > kMaxFileBytes) {
		if (failNote)
			*failNote = T("InFlight.FileTooLarge", "Pre-Flight's settings file is too large to read.");
		return false;
	}
	if (file.open(QIODevice::ReadOnly))
		bytes = file.readAll();
	const inflight::ImportResult r = inflight::importFromPreflightJson(bytes);
	if (!r.anythingFound()) {
		if (failNote)
			*failNote = T("InFlight.NothingFound", "No choices to copy were found in Pre-Flight.");
		return false;
	}
	inflight::applyImport(r, settings);
	if (!settings.save())
		obs_log(LOG_WARNING, "could not save settings");
	if (levels)
		levels->setSources(settings.micUuid, settings.desktopUuid, settings.levelsSources);
	if (controller)
		controller->setSettings(settings);
	return true;
}

void InflightDock::copyFromPreflight()
{
	if (closing)
		return;
	QString failNote;
	if (!importPreflightChoices(&failNote)) {
		setupNote->setText(failNote);
		setupNote->setVisible(true);
		return;
	}
	if (!settings.setupDone) {
		setupNote->setText(T("InFlight.NeedMic", "Copied what was found. Still needed: your microphone."));
		setupNote->setVisible(true);
	} else {
		setupNote->setVisible(false);
	}
	applySetupVisibility();
}

void InflightDock::autoCopyFromPreflight()
{
	// Never over the user's own choices: only when In-Flight has none saved at all (not a partial setup), and never
	// while OBS is streaming (a later show, after the stream, tries again).
	if (closing || settings.setupDone)
		return;
	if (!settings.micUuid.isEmpty() || !settings.desktopUuid.isEmpty() || settings.recordIntent)
		return;
	if (obs_frontend_streaming_active())
		return;
	const QString path = preflightFile();
	const QFileInfo fi(path);
	if (path.isEmpty() || !fi.exists())
		return;
	// One attempt per state of Pre-Flight's file: a file with no usable choices is not re-read and re-saved on every Show.
	const QString stamp = QString::number(fi.size()) + QLatin1Char('/') + QString::number(fi.lastModified().toMSecsSinceEpoch());
	if (stamp == autoCopyStamp_)
		return;
	autoCopyStamp_ = stamp;
	if (!importPreflightChoices(nullptr))
		return;
	const bool done = settings.setupDone;
	obs_log(LOG_INFO, "inflight: auto-copied choices from Pre-Flight (setup %s)", done ? "complete" : "incomplete");
	autoCopied_ = done;
	if (done) {
		setupNote->setVisible(false);
	} else {
		setupDeferred = false; // the card is shown, as after the Copy button
		setupNote->setText(T("InFlight.NeedMic", "Copied what was found. Still needed: your microphone."));
		setupNote->setVisible(true);
	}
	applySetupVisibility();
}

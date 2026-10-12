/*
Red Warden Multistream
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "multistream-dock.hpp"
#include "kit-paths.hpp"
#include "kit-cost.hpp"
#include "fit-scroll-area.hpp"
#include "bandwidth-dialog.hpp"
#include "credentials.hpp"
#include "edit-dialog.hpp"
#include "import-dialog.hpp"
#include "obs-audio.hpp"
#include "platforms.hpp"
#include "report-dialog.hpp"
#include "upload-test.hpp"
#include "connection-budget.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <QDateTime>
#include <QDesktopServices>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QDockWidget>
#include <QEvent>
#include <QFrame>
#include <QInputDialog>
#include <QLocale>
#include <QMenu>
#include <util/platform.h>
#include <QRandomGenerator>
#include <algorithm>
#include <cmath>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QScopeGuard>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace {

QString mbps(double kbps)
{
	return QString::number(kbps / 1000.0, 'f', 1) + QStringLiteral(" Mbps");
}

constexpr qint64 kMainWaitTimeoutMs = 20000;

double jitter()
{
	return QRandomGenerator::global()->generateDouble();
}

// Quick reconnect after a live connection drops: 5 s, 10 s, 20 s, 40 s, then
// every 60 s, 10 tries (~7 minutes), +/-20 % jitter. Same shape as the libobs
// reconnect it replaces, and 1 connect + 10 reconnects fits the 12-per-10-minute
// budget by itself. The budget still has the final say (scheduleRetry/startOne).
constexpr int kQuickReconnects = 10;
int64_t quickReconnectDelayMs(int attempt, double jitter01)
{
	const int64_t base = std::min<int64_t>(5000LL << std::min(attempt, 4), 60000);
	return (int64_t)(double(base) * (0.8 + 0.4 * jitter01));
}

QString mmss(int64_t ms)
{
	const int64_t secs = (ms + 999) / 1000;
	return QStringLiteral("%1:%2").arg(secs / 60).arg(secs % 60, 2, 10, QLatin1Char('0'));
}

// The test harnesses (tools\test-*.ps1) drive a portable OBS copy under
// .testbed\ through RWS_MULTISTREAM_* environment variables. Those hooks take
// screenshots and can save imported destinations, so they only switch on for an
// OBS running from a .testbed folder: an environment variable alone can never
// change a real user's profile.
bool testHooksEnabled()
{
	static const bool on =
		QCoreApplication::applicationDirPath().contains(QStringLiteral("/.testbed/"), Qt::CaseInsensitive);
	return on;
}
QString testEnv(const char *name)
{
	return testHooksEnabled() ? qEnvironmentVariable(name) : QString();
}

// OBS's main stream is connected AND its encoders are producing packets.
bool mainStreamProducing()
{
	obs_output_t *main = obs_frontend_get_streaming_output();
	if (!main)
		return false;
	const bool ok = obs_output_active(main) && obs_output_get_total_frames(main) > 0;
	obs_output_release(main);
	return ok;
}

} // namespace

// ---- MultistreamDock -----------------------------------------------------------

// The folder holding every plugin's config folder (.../plugin_config), for the shared Stream Kit preference.
static QString kitConfigRoot()
{
	return QString::fromUtf8(kitpaths::pluginConfigRoot().c_str());
}

MultistreamDock::MultistreamDock(QWidget *parent) : QWidget(parent)
{
	setObjectName(QStringLiteral("rwsMultistreamDock"));
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(0, 0, 0, 8);
	root->setSpacing(0);

	// Status header: the product logo and a headline that says what is live.
	auto *header = new QWidget(this);
	auto *hv = new QVBoxLayout(header);
	hv->setContentsMargins(10, 10, 10, 10);
	hv->setSpacing(8);
	auto *hrow = new QHBoxLayout();
	hrow->setSpacing(10);
	auto *logo = new QLabel(header);
	const int logoPx = 32;
	QPixmap pm(QStringLiteral(":/rws-multistream/logo-128.png"));
	pm.setDevicePixelRatio(128.0 / logoPx); // crisp on high-DPI screens
	logo->setPixmap(pm);
	logo->setFixedSize(logoPx, logoPx);
	logo->setToolTip(QStringLiteral("Red Warden Multistream by Red Warden Studios"));
	hrow->addWidget(logo, 0, Qt::AlignTop);
	auto *texts = new QVBoxLayout();
	texts->setSpacing(1);
	headline = new QLabel(header);
	headline->setTextFormat(Qt::PlainText);
	headline->setWordWrap(true);
	subline = new QLabel(header);
	subline->setTextFormat(Qt::PlainText);
	subline->setWordWrap(true);
	texts->addWidget(headline);
	texts->addWidget(subline);
	hrow->addLayout(texts, 1);
	menuButton = new QToolButton(header);
	menuButton->setText(QStringLiteral("\u22EF"));
	menuButton->setToolTip(QStringLiteral("More"));
	menuButton->setAccessibleName(QStringLiteral("More options"));
	menuButton->setCursor(Qt::PointingHandCursor);
	menuButton->setPopupMode(QToolButton::InstantPopup);
	auto *menu = new QMenu(menuButton);
	QAction *testAct = menu->addAction(QStringLiteral("Test my upload speed"));
	QAction *setAct = menu->addAction(QStringLiteral("Enter my upload speed..."));
	QAction *forgetAct = menu->addAction(QStringLiteral("Forget my upload speed"));
	menu->addSeparator();
	QAction *importAct = menu->addAction(QStringLiteral("Import from obs-multi-rtmp or Aitum..."));
	menu->addSeparator();
	QAction *reportAct = menu->addAction(QStringLiteral("Last stream report"));
	// ("Tell me when an update is available" moved to the Stream Kit settings, Updates tab.)
	connect(reportAct, &QAction::triggered, this, &MultistreamDock::showReport);
	connect(importAct, &QAction::triggered, this, &MultistreamDock::importDestinations);
	connect(testAct, &QAction::triggered, this, &MultistreamDock::startUploadTest);
	connect(setAct, &QAction::triggered, this, &MultistreamDock::enterUploadSpeed);
	connect(forgetAct, &QAction::triggered, this, [this] {
		const bool keepUpdates = globals.checkUpdates; // forget the speed, not the other settings
		globals = GlobalSettings();
		globals.checkUpdates = keepUpdates;
		saveGlobalSettings(globals);
		refreshSummary();
	});
	connect(menu, &QMenu::aboutToShow, this, [this, testAct, forgetAct, reportAct] {
		reportAct->setEnabled(report.hasResult());
		const bool live = obs_frontend_streaming_active();
		testAct->setEnabled(!live && !uploadTesting);
		testAct->setText(live ? QStringLiteral("Test my upload speed (stop streaming first)")
				      : QStringLiteral("Test my upload speed"));
		forgetAct->setEnabled(globals.uploadMbps > 0);
	});
	menuButton->setMenu(menu);
	hrow->addWidget(menuButton, 0, Qt::AlignTop);
	hv->addLayout(hrow);
	strip = new ui::BeaconStrip(header);
	hv->addWidget(strip);
	note = new QLabel(header);
	note->setWordWrap(true);
	note->setTextFormat(Qt::RichText);
	note->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
	connect(note, &QLabel::linkActivated, this, [this](const QString &link) {
		if (link == QLatin1String("test"))
			startUploadTest();
		else if (link == QLatin1String("enter"))
			enterUploadSpeed();
		else if (link == QLatin1String("import"))
			importDestinations();
	});
	hv->addWidget(note);
	recap = new QLabel(header);
	recap->setWordWrap(true);
	recap->setTextFormat(Qt::RichText);
	recap->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
	recap->setVisible(false);
	connect(recap, &QLabel::linkActivated, this, [this](const QString &) { showReport(); });
	hv->addWidget(recap);
	root->addWidget(header);
	limits = parseServiceLimits(obsServicesJson());
	{
		const std::string dir = kitpaths::toolFile(kitpaths::kMultistream, "reports");
		if (!dir.empty())
			reportsDir = QDir::cleanPath(QString::fromUtf8(dir.c_str()));
	}
	globals = loadGlobalSettings();

	auto *scroll = new FitScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	auto *listHost = new QWidget(scroll);
	listHost->setObjectName(QStringLiteral("msList"));
	list = new QVBoxLayout(listHost);
	list->setContentsMargins(0, 0, 0, 0);
	list->setSpacing(0);
	list->addStretch(1);
	scroll->setFitWidget(listHost);
	root->addWidget(scroll);

	auto *footer = new QHBoxLayout();
	footer->setContentsMargins(10, 8, 10, 0);
	addButton = new QPushButton(QStringLiteral("Add destination"), this);
	addButton->setCursor(Qt::PointingHandCursor);
	connect(addButton, &QPushButton::clicked, this, &MultistreamDock::addDestination);
	footer->addWidget(addButton);
	root->addLayout(footer);

	theme = ui::Theme::from(palette());

	snapshotDir = testEnv("RWS_MULTISTREAM_SNAPSHOT");
	if (!snapshotDir.isEmpty()) {
		auto *shots = new QTimer(this);
		shots->setInterval(4000);
		connect(shots, &QTimer::timeout, this, [this, shots] {
			QPointer<MultistreamDock> self(this);
			takeSnapshot(); // pumps events: OBS may shut down inside it
			if (!self || closing) {
				if (self)
					shots->stop();
				return;
			}
			if (snapshotCount >= 8)
				shots->stop();
		});
		shots->start();
	}

	statsTimer.setInterval(1000);
	connect(&statsTimer, &QTimer::timeout, this, [this] {
		kitcost::Scope costScope(kitcost::MultistreamStats); // test-only timing, see kit-cost.hpp
		for (auto &[id, r] : runners)
			r->poll();
		pollMainStream();
		if (report.active()) {
			const int64_t now = QDateTime::currentMSecsSinceEpoch();
			const int64_t dt = lastTrafficMs > 0 ? now - lastTrafficMs : 1000;
			lastTrafficMs = now;
			for (const auto &d : cfg.destinations) {
				if (auto it = runners.find(d.id); it != runners.end())
					report.traffic(d.id, it->second->kbps(), dt, it->second->droppedFrames(),
						       it->second->totalFrames());
				updateReport(d.id);
			}
		}
		refreshAll();
	});
	statsTimer.start();

	mainReadyTimer.setInterval(250);
	connect(&mainReadyTimer, &QTimer::timeout, this, &MultistreamDock::startWhenMainReady);

	// Global hotkeys (OBS > Settings > Hotkeys). A Stream Deck "Hotkey" key
	// bound to the same keys works too.
	allOnHotkey = obs_hotkey_register_frontend("rws_multistream.all_on",
						   "Red Warden Multistream: turn all destinations on", onHotkey, this);
	allOffHotkey = obs_hotkey_register_frontend("rws_multistream.all_off",
						    "Red Warden Multistream: turn all destinations off", onHotkey, this);
	if (const std::string pathStr = kitpaths::toolFile(kitpaths::kMultistream, "hotkeys.json"); !pathStr.empty()) {
		const char *path = pathStr.c_str();
		if (obs_data_t *root = obs_data_create_from_json_file_safe(path, "bak")) {
			for (auto [key, hk] :
			     {std::pair<const char *, obs_hotkey_id>{"all_on", allOnHotkey}, {"all_off", allOffHotkey}}) {
				obs_data_array_t *arr = obs_data_get_array(root, key);
				obs_hotkey_load(hk, arr);
				obs_data_array_release(arr);
			}
			obs_data_release(root);
		}
	}

	reload();

	// The update look is scheduled by the Kit dock (kit-dock.cpp), not here. The shared preference is still
	// read once at startup: the first read migrates the pre-merge per-plugin values into the shared file,
	// and globals.checkUpdates mirrors it into this tool's own settings.json as before.
	globals.checkUpdates = updatecheck::loadSharedCheckUpdates(kitConfigRoot());

	// Test copy only: run the Twitch bandwidth test on the named destination
	// (the harness points it at a local receiver) and photograph the result.
	const QString bwTest = testEnv("RWS_MULTISTREAM_TEST_BWTEST");
	if (!bwTest.isEmpty()) {
		QTimer::singleShot(5000, this, [this, bwTest] {
			for (const auto &d : cfg.destinations) {
				if (d.name != bwTest)
					continue;
				auto *dlg = new BandwidthDialog(d, creds::loadKey(d.id), this);
				dlg->setAttribute(Qt::WA_DeleteOnClose);
				connect(dlg, &BandwidthDialog::done, this, [this, dlg](const QString &verdict) {
					obs_log(LOG_INFO, "bandwidth test verdict: %s", verdict.toUtf8().constData());
					if (!snapshotDir.isEmpty()) {
						QDir().mkpath(snapshotDir);
						QPointer<BandwidthDialog> alive(dlg);
						QCoreApplication::processEvents();
						if (!alive || closing) // OBS may have shut down inside processEvents()
							return;
						dlg->grab().save(QDir(snapshotDir).filePath(QStringLiteral("bandwidth.png")));
					}
				});
				dlg->show();
				return;
			}
			obs_log(LOG_WARNING, "bandwidth test: no destination named %s", bwTest.toUtf8().constData());
		});
	}
}

void MultistreamDock::testLogListGeometry(const char *tag)
{
	for (QScrollArea *sa : findChildren<QScrollArea *>())
		if (auto *fs = dynamic_cast<FitScrollArea *>(sa))
			obs_log(LOG_INFO, "list geometry %s: %s", tag, fs->diagnose().toUtf8().constData());
}

MultistreamDock::~MultistreamDock()
{
	shutdown();
}

// ---- config ---------------------------------------------------------------

void MultistreamDock::reload()
{
	stopAll(true);
	runners.clear();
	unregisterHotkeys(); // bindings were saved with the profile they belong to
	cfg = loadConfig();
	for (const auto &d : cfg.destinations)
		runner(d.id);
	syncHotkeys();
	importableCount = 0;
	if (cfg.destinations.empty()) {
		auto seen = existingServerKeys();
		for (const auto &c : findImportCandidates())
			if (c.skipReason.isEmpty() && seen.insert(c.server.trimmed() + QLatin1Char('\n') + c.key).second)
				importableCount++;
	}
	rebuild();
}

void MultistreamDock::save()
{
	captureHotkeys();
	saveConfig(cfg);

	// "All on" / "All off" bindings are global, not per profile.
	const std::string dir = kitpaths::toolDir(kitpaths::kMultistream);
	if (!dir.empty())
		os_mkdirs(dir.c_str());
	const std::string pathStr = kitpaths::toolFile(kitpaths::kMultistream, "hotkeys.json");
	if (!pathStr.empty()) {
		const char *path = pathStr.c_str();
		obs_data_t *root = obs_data_create();
		for (auto [key, hk] : {std::pair<const char *, obs_hotkey_id>{"all_on", allOnHotkey}, {"all_off", allOffHotkey}}) {
			if (hk == OBS_INVALID_HOTKEY_ID)
				continue;
			obs_data_array_t *arr = obs_hotkey_save(hk);
			obs_data_set_array(root, key, arr);
			obs_data_array_release(arr);
		}
		obs_data_save_json_safe(root, path, "tmp", "bak");
		obs_data_release(root);
	}
}

Destination *MultistreamDock::find(const QString &id)
{
	for (auto &d : cfg.destinations)
		if (d.id == id)
			return &d;
	return nullptr;
}

OutputRunner *MultistreamDock::runner(const QString &id)
{
	auto it = runners.find(id);
	if (it != runners.end())
		return it->second.get();
	auto r = std::make_unique<OutputRunner>();
	OutputRunner *raw = r.get();
	RetryState &rs = retries[id];
	if (!rs.timer) {
		rs.timer = new QTimer(this);
		rs.timer->setSingleShot(true);
		connect(rs.timer, &QTimer::timeout, this, [this, id] {
			auto &st = retries[id];
			st.heldByBudget = false;
			st.retryAtMs = 0;
			const Destination *d = find(id);
			if (d && d->enabled && obs_frontend_streaming_active() && !closing)
				startOne(id);
		});
	}
	connect(raw, &OutputRunner::changed, this, [this, id] { onRunnerChanged(id); });
	connect(raw, &OutputRunner::attempted, this,
		[this, id] { connectionBudget(id).recordAttempt(QDateTime::currentMSecsSinceEpoch()); });
	connect(raw, &OutputRunner::stalled, this, [this, raw, id] {
		const int64_t now = QDateTime::currentMSecsSinceEpoch();
		auto &st = retries[id];
		connectionBudget(id).recordStall(now);
		report.stall(id, now);
		raw->shutdown();
		if (connectionBudget(id).unstable(now)) {
			obs_log(LOG_WARNING, "[%s] unstable: 3 stalls in 10 minutes, slowing retries",
				find(id) ? find(id)->name.toUtf8().constData() : "?");
			scheduleRetry(id, connectionBudget(id).autoRetryDelayMs(st.autoFailures++, jitter(), now), false);
		} else {
			startOne(id); // still goes through the budget
		}
	});
	runners[id] = std::move(r);
	return raw;
}

void MultistreamDock::onRunnerChanged(const QString &id)
{
	auto it = runners.find(id);
	if (it != runners.end() && !closing) {
		OutputRunner *r = it->second.get();
		auto &st = retries[id];
		const auto now = r->state();
		const auto prev = st.lastState;
		const bool entered = now != prev;
		st.lastState = now;
		if (entered && now == OutputRunner::State::Live) {
			st.autoFailures = 0; // healthy again
			st.recovering = false;
			st.dropAttempts = 0;
			cancelRetry(id);
		}
		if (entered && now == OutputRunner::State::Error && !r->lastStopRetryable()) {
			st.recovering = false; // rejected key, bad URL, ...: never retried
			st.dropAttempts = 0;
		}
		// A network error while the user is live: reconnect quickly after a drop,
		// then keep trying gently. Every attempt goes through startOne() and so
		// through the connection budget.
		const Destination *d = find(id);
		if (entered && now == OutputRunner::State::Error && r->lastStopRetryable() && d && d->enabled &&
		    obs_frontend_streaming_active()) {
			const int64_t t = QDateTime::currentMSecsSinceEpoch();
			if (prev == OutputRunner::State::Live)
				st.recovering = true;
			if (st.recovering && st.dropAttempts < kQuickReconnects) {
				scheduleRetry(id, quickReconnectDelayMs(st.dropAttempts++, jitter()), false);
			} else {
				st.recovering = false;
				st.dropAttempts = 0;
				scheduleRetry(id, connectionBudget(id).autoRetryDelayMs(st.autoFailures++, jitter(), t), false);
			}
		}
	}
	updateReport(id);
	refreshCard(id);
	refreshSummary();
	notify(id);
}

void MultistreamDock::scheduleRetry(const QString &id, int64_t delayMs, bool heldByBudget)
{
	auto &st = retries[id];
	if (!st.timer)
		return;
	const int64_t now = QDateTime::currentMSecsSinceEpoch();
	// The budget always wins over the schedule.
	const int64_t budgetWait = connectionBudget(id).budgetWaitMs(now);
	if (budgetWait > delayMs) {
		delayMs = budgetWait;
		heldByBudget = true;
	}
	st.heldByBudget = heldByBudget;
	st.retryAtMs = now + delayMs;
	st.timer->start((int)qMin<int64_t>(delayMs, INT32_MAX));
	if (const Destination *d = find(id))
		obs_log(LOG_INFO, "[%s] next connection attempt in %lld s%s", d->name.toUtf8().constData(),
			(long long)(delayMs / 1000), heldByBudget ? " (connection budget)" : "");
	refreshCard(id);
}

void MultistreamDock::cancelRetry(const QString &id)
{
	auto it = retries.find(id);
	if (it == retries.end())
		return;
	if (it->second.timer)
		it->second.timer->stop();
	it->second.retryAtMs = 0;
	it->second.heldByBudget = false;
}

// ---- UI -------------------------------------------------------------------

void MultistreamDock::rebuild()
{
	for (auto &[id, row] : rows)
		delete row;
	rows.clear();

	int index = 0;
	for (const auto &d : cfg.destinations) {
		auto *row = new ui::DestinationRow();
		const QString id = d.id;
		connect(row, &ui::DestinationRow::toggled, this, [this, id](bool on) { setOn(id, on); });
		connect(row, &ui::DestinationRow::editClicked, this, [this, id] { editDestination(id); });
		connect(row, &ui::DestinationRow::retryClicked, this, [this, id] { startOne(id); });
		row->applyTheme(theme);
		list->insertWidget(index++, row);
		rows[id] = row;
	}
	refreshAll();
}

void MultistreamDock::applyTheme()
{
	theme = ui::Theme::from(palette());
	headline->setStyleSheet(QStringLiteral("font-size:15px; font-weight:600; color:%1;").arg(ui::Theme::css(theme.text)));
	subline->setStyleSheet(QStringLiteral("font-size:12px; color:%1;").arg(ui::Theme::css(theme.secondary)));
	addButton->setStyleSheet(
		QStringLiteral("QPushButton { color:%1; background:transparent; border:1px solid %2; border-radius:4px;"
			       " padding:6px 10px; font-size:13px; font-weight:600; }"
			       "QPushButton:hover { background:%3; }"
			       "QPushButton:focus { border:1px solid %4; }")
			.arg(ui::Theme::css(theme.text), ui::Theme::css(theme.geranium), ui::Theme::css(theme.hover),
			     ui::Theme::css(palette().color(QPalette::Highlight))));
	menuButton->setStyleSheet(QStringLiteral("QToolButton { color:%1; background:transparent; border:none; font-size:18px;"
						 " padding:0px 4px; }"
						 "QToolButton::menu-indicator { image:none; width:0px; }"
						 "QToolButton:hover { background:%2; border-radius:4px; }")
					  .arg(ui::Theme::css(theme.secondary), ui::Theme::css(theme.hover)));
	for (auto &[id, row] : rows)
		row->applyTheme(theme);
	strip->update();
}

void MultistreamDock::changeEvent(QEvent *e)
{
	QWidget::changeEvent(e);
	if (e->type() == QEvent::PaletteChange && headline && !closing)
		applyTheme();
}

void MultistreamDock::pollMainStream()
{
	obs_output_t *main = obs_frontend_get_streaming_output();
	const bool active = main && obs_output_active(main);
	const uint64_t bytes = active ? obs_output_get_total_bytes(main) : 0;
	if (main)
		obs_output_release(main);
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	if (!active || bytes < mainLastBytes || mainLastMs == 0) {
		mainKbps = 0.0;
	} else if (now > mainLastMs) {
		mainKbps = double(bytes - mainLastBytes) * 8.0 / double(now - mainLastMs);
	}
	mainLastBytes = bytes;
	mainLastMs = active ? now : 0;
}

namespace {
// What OBS's own stream is called in the breakdown: its platform if known.
QString mainStreamLabel()
{
	QString label = QStringLiteral("OBS's own stream");
	if (obs_service_t *svc = obs_frontend_get_streaming_service()) {
		obs_data_t *st = obs_service_get_settings(svc);
		const QString name = QString::fromUtf8(obs_data_get_string(st, "service"));
		obs_data_release(st);
		if (!name.isEmpty())
			label = QStringLiteral("OBS's own stream (%1)").arg(name);
	}
	return label;
}
} // namespace

ui::RowView MultistreamDock::rowView(const Destination &d)
{
	using ui::Status;
	const QString id = d.id;
	ui::RowView v;
	const auto &p = platformById(d.platform);
	v.name = d.name;
	v.badge = p.badge;
	v.badgeColor = p.color;
	v.platformLabel = p.label;
	v.enabled = d.enabled;

	OutputRunner *r = runners.count(id) ? runners[id].get() : nullptr;
	const bool obsLive = obs_frontend_streaming_active();
	const auto state = r ? r->state() : OutputRunner::State::Idle;
	const RetryState *rs = retries.count(id) ? &retries.at(id) : nullptr;
	const int64_t nowMs = QDateTime::currentMSecsSinceEpoch();
	const bool retryPending = rs && rs->timer && rs->timer->isActive() &&
				  (state == OutputRunner::State::Idle || state == OutputRunner::State::Error);

	if (retryPending) {
		const QString left = mmss(qMax<int64_t>(0, rs->retryAtMs - nowMs));
		v.status = Status::Busy;
		if (rs->heldByBudget) {
			v.statusText = QStringLiteral("Paused");
			v.statusDetail = QStringLiteral("Protecting your %1 account from too many connections. Next try in %2.")
						 .arg(p.label, left);
		} else if (rs->recovering) {
			v.statusText = QStringLiteral("Reconnecting");
			v.statusDetail = QStringLiteral("Connection dropped. Attempt %1 of %2 in %3")
						 .arg(rs->dropAttempts)
						 .arg(kQuickReconnects)
						 .arg(left);
		} else {
			v.statusText = QStringLiteral("Retrying in %1").arg(left);
			if (state == OutputRunner::State::Error && r)
				v.statusDetail = r->detail();
			v.showRetry = true;
		}
		if (connectionBudget(id).unstable(nowMs))
			v.tip = QStringLiteral("This connection keeps dropping, so retries are spaced further apart.");
	} else {
		switch (state) {
		case OutputRunner::State::Live:
			if (r->awaitingFirstFrame()) {
				v.status = Status::Busy;
				v.statusText = QStringLiteral("Connected");
				v.statusDetail = QStringLiteral("Waiting for a keyframe from OBS");
			} else {
				v.status = Status::Live;
				v.statusText = QStringLiteral("LIVE");
				v.statusDetail = mbps(r->kbps());
				if (const int dropped = r->droppedFrames(); dropped > 0)
					v.statusDetail += QStringLiteral(", %1 frames dropped").arg(dropped);
				if (const auto fig = dropFigures_.find(id); fig != dropFigures_.end())
					v.dropBadge = QStringLiteral("%1% DROPPED").arg(fig->second, 0, 'f', 1);
			}
			break;
		case OutputRunner::State::Starting:
			v.status = Status::Busy;
			if (rs && rs->recovering) {
				v.statusText = QStringLiteral("Reconnecting");
				v.statusDetail =
					QStringLiteral("Attempt %1 of %2").arg(rs->dropAttempts).arg(kQuickReconnects);
			} else {
				v.statusText = QStringLiteral("Connecting");
			}
			break;
		case OutputRunner::State::Reconnecting:
			v.status = Status::Busy;
			v.statusText = QStringLiteral("Reconnecting");
			v.statusDetail = r->detail();
			break;
		case OutputRunner::State::Stopping:
			v.status = Status::Busy;
			v.statusText = QStringLiteral("Stopping");
			break;
		case OutputRunner::State::Error:
			v.status = Status::Problem;
			v.statusText = QStringLiteral("Not live");
			v.statusDetail = r->detail();
			// Retrying cannot fix a bad URL, key or format: point at Edit instead.
			v.showRetry = d.enabled && obsLive && r->lastStopRetryable();
			if (!r->lastStopRetryable())
				v.tip = QStringLiteral("Check this destination's settings with Edit.");
			break;
		case OutputRunner::State::Idle:
			if (waitingForMain.count(id)) {
				v.status = Status::Busy;
				v.statusText = QStringLiteral("Waiting");
				v.statusDetail = QStringLiteral("Joins as soon as OBS's stream is sending video");
			} else if (!d.enabled) {
				v.status = Status::Off;
				v.statusText = QStringLiteral("Off");
			} else if (!obsLive) {
				v.status = Status::Ready;
				v.statusText = QStringLiteral("Ready");
				v.statusDetail = QStringLiteral("Goes live with Start Streaming");
			} else {
				v.status = Status::Problem;
				v.statusText = QStringLiteral("Not live");
				v.showRetry = true;
			}
			break;
		}
	}

	// Audio actually being sent when live; otherwise what it will resolve to.
	int audio = (r && r->busy()) ? r->audioBitrateKbps() : 0;
	if (audio <= 0)
		audio = (!d.enc.shared && d.enc.audioBitrate > 0) ? d.enc.audioBitrate : obsAudio.bitrate;
	if (d.enc.shared) {
		v.meta = QStringLiteral("Shared encoder, %1 kbps audio").arg(audio);
	} else {
		v.meta = QStringLiteral("Own encoder, %1 kbps video").arg(d.enc.videoBitrate);
		if (d.enc.height > 0)
			v.meta += QStringLiteral(" at %1p").arg(d.enc.height);
		v.meta += QStringLiteral(", %1 kbps audio").arg(audio);
	}
	if (v.tip.isEmpty()) {
		// Platform limits: the serious ones first, then gentle notes.
		auto issues = limitIssues(d);
		std::stable_sort(issues.begin(), issues.end(),
				 [](const LimitIssue &a, const LimitIssue &b) { return a.serious && !b.serious; });
		if (!issues.empty()) {
			v.tip = issues.front().text;
			v.tipIsNote = !issues.front().serious;
		}
	}
	if (v.tip.isEmpty() && r && r->usesSharedEncoder() && r->busy()) {
		const int k = r->sharedKeyintSec();
		if (k == 0 || k > 4)
			v.tip = QStringLiteral("Set Keyframe Interval to 2 s in OBS Settings > Output > Streaming. "
					       "Platforms expect it, and shared destinations join faster.");
	}
	return v;
}

void MultistreamDock::refreshCard(const QString &id)
{
	if (closing)
		return; // OBS's frontend is gone; nothing left to show
	auto it = rows.find(id);
	const Destination *d = find(id);
	if (it == rows.end() || !d)
		return;
	it->second->setView(rowView(*d));
	it->second->setToolTip(QUrl(d->server).host());
}

void MultistreamDock::refreshAll()
{
	if (closing)
		return;
	obsAudio = obsStreamAudio(); // cheap: reads the in-memory profile config
	for (const auto &d : cfg.destinations)
		refreshCard(d.id);
	refreshSummary();
}

void MultistreamDock::refreshSummary()
{
	if (closing)
		return;
	using ui::Status;
	int on = 0, live = 0, busy = 0, trouble = 0;
	double kbps = 0.0;
	QStringList breakdown;
	std::vector<ui::BeaconStrip::Segment> segs;
	for (const auto &d : cfg.destinations) {
		const ui::RowView v = rowView(d);
		segs.push_back({v.status, d.name});
		if (d.enabled)
			on++;
		if (v.status == Status::Live) {
			live++;
			if (auto it = runners.find(d.id); it != runners.end()) {
				kbps += it->second->kbps();
				breakdown << QStringLiteral("%1: %2").arg(d.name, mbps(it->second->kbps()));
			}
		} else if (v.status == Status::Busy) {
			busy++;
		} else if (v.status == Status::Problem) {
			trouble++;
		}
	}
	strip->setSegments(std::move(segs));

	const int total = (int)cfg.destinations.size();
	QString head, sub;
	if (total == 0) {
		head = QStringLiteral("Send this stream to more platforms");
		sub = QStringLiteral("Add YouTube, Kick, TikTok and others. Each one goes live when you press Start Streaming.");
	} else if (!obs_frontend_streaming_active()) {
		if (on == 0) {
			head = QStringLiteral("All destinations are off");
			sub = QStringLiteral("Switch one on to send your stream there too.");
		} else {
			head = on == 1 ? QStringLiteral("Ready for 1 platform") : QStringLiteral("Ready for %1 platforms").arg(on);
			sub = QStringLiteral("They go live when you press Start Streaming.");
		}
	} else if (live == 0 && busy > 0 && trouble == 0) {
		head = QStringLiteral("Connecting");
		sub = QStringLiteral("Going live on %1 of %2").arg(busy).arg(on);
	} else {
		head = QStringLiteral("Live on %1 of %2").arg(live).arg(on);
		QStringList parts;
		// Total = OBS's own stream + every destination. Each platform gets
		// its own full copy, so this is what the internet connection carries.
		const double totalKbps = kbps + mainKbps;
		if (totalKbps >= 100)
			parts << (globals.uploadMbps > 0
					  ? QStringLiteral("Uploading %1 of %2 Mbps").arg(QString::number(totalKbps / 1000.0, 'f', 1))
							  .arg(QString::number(globals.uploadMbps, 'f', 0))
					  : QStringLiteral("Uploading %1 in total").arg(mbps(totalKbps)));
		if (busy)
			parts << QStringLiteral("%1 connecting").arg(busy);
		if (trouble)
			parts << (trouble == 1 ? QStringLiteral("1 needs attention")
					       : QStringLiteral("%1 need attention").arg(trouble));
		sub = parts.join(QStringLiteral(", "));
	}
	headline->setText(head);
	subline->setText(sub);
	{
		// The Kit dock's section header: short, "of" = how many are on (as the headline above counts).
		QString s;
		if (total == 0)
			s = QStringLiteral("No destinations yet");
		else if (!obs_frontend_streaming_active())
			s = on == 0 ? QStringLiteral("All off") : QStringLiteral("Ready on %1 of %2").arg(on).arg(total);
		else if (live == 0 && busy > 0 && trouble == 0)
			s = QStringLiteral("Connecting %1 of %2").arg(busy).arg(on);
		else
			s = QStringLiteral("Live on %1 of %2").arg(live).arg(on);
		if (s != summaryText) {
			summaryText = s;
			emit summaryChanged(summaryText);
		}
	}

	// ---- upload estimate and warnings ----
	auto fmt = [](double kbpsValue) {
		const double m = kbpsValue / 1000.0;
		return m < 10 ? QString::number(m, 'f', 1) : QString::number(m, 'f', 0);
	};
	QString noteText;
	QColor noteColor = theme.secondary;
	const bool liveNow = obs_frontend_streaming_active();
	const double capKbps = globals.uploadMbps * 1000.0;
	const double usingKbps = liveNow ? (kbps + mainKbps) : plannedUploadKbps();
	if (total == 0 && importableCount > 0) {
		noteText = (importableCount == 1
				    ? QStringLiteral("Found 1 destination saved in another multistream plugin. ")
				    : QStringLiteral("Found %1 destinations saved in another multistream plugin. ").arg(importableCount)) +
			   QStringLiteral("<a href=\"import\">Import them</a>");
	} else if (uploadTesting) {
		noteText = QStringLiteral("Testing your upload speed... %1 s, about %2 Mbps so far")
				   .arg((int)uploadProgressSeconds)
				   .arg(QString::number(uploadProgressMbps, 'f', 0));
	} else if (total > 0 && (on > 0 || liveNow)) {
		if (!liveNow) {
			noteText = capKbps > 0 ? QStringLiteral("Needs about %1 Mbps of your %2 Mbps upload.")
							 .arg(fmt(usingKbps), QString::number(globals.uploadMbps, 'f', 0))
					       : QStringLiteral("Needs about %1 Mbps of upload. "
								"<a href=\"test\">Test my upload speed</a> to check it fits.")
							 .arg(fmt(usingKbps));
		}
		if (capKbps > 0 && usingKbps > 0) {
			const double ratio = usingKbps / capKbps;
			if (ratio > 0.9) {
				noteColor = theme.error;
				noteText += (noteText.isEmpty() ? QString() : QStringLiteral(" ")) +
					    QStringLiteral("That's more than your connection can reliably carry. "
							   "Turn a platform off or lower its bitrate.");
			} else if (ratio > 0.75) {
				noteColor = theme.ember;
				noteText += (noteText.isEmpty() ? QString() : QStringLiteral(" ")) +
					    QStringLiteral("That's %1% of your upload. Streams start dropping frames "
							   "above about 75%, so keep some headroom.")
						    .arg((int)std::lround(ratio * 100));
			}
		}
	}
	note->setText(noteText);
	note->setVisible(!noteText.isEmpty());
	note->setStyleSheet(QStringLiteral("font-size:12px; color:%1;").arg(ui::Theme::css(noteColor)));

	// After a stream: one line on how it went, with the full report a click away.
	const bool showRecap = !liveNow && report.hasResult() && !report.destinations().empty();
	if (showRecap) {
		recap->setText(report.headline().toHtmlEscaped() + QStringLiteral(" <a href=\"report\">See report</a>"));
		recap->setStyleSheet(QStringLiteral("font-size:12px; color:%1;")
					     .arg(ui::Theme::css(report.hadTrouble() ? theme.ember : theme.secondary)));
	}
	recap->setVisible(showRecap);
	if (globals.uploadMbps > 0) {
		const QString how = globals.uploadSource == QLatin1String("measured") ? QStringLiteral("Tested")
										      : QStringLiteral("Entered");
		note->setToolTip(QStringLiteral("Your upload: %1 Mbps (%2 %3)")
					 .arg(QString::number(globals.uploadMbps, 'f', 0), how.toLower(),
					      QDateTime::fromString(globals.uploadWhen, Qt::ISODate).toString(QStringLiteral("MMM d, h:mm AP"))));
	} else {
		note->setToolTip(QString());
	}
	if (obs_frontend_streaming_active() && (kbps + mainKbps) >= 100) {
		breakdown.prepend(QStringLiteral("%1: %2").arg(mainStreamLabel(), mbps(mainKbps)));
		subline->setToolTip(QStringLiteral("Upload by stream\n") + breakdown.join('\n') +
				    QStringLiteral("\n\nEvery platform receives its own full copy of the stream."));
	} else {
		subline->setToolTip(QString());
	}
	if (headline->styleSheet().isEmpty())
		applyTheme();
}

void MultistreamDock::takeSnapshot()
{
	if (snapshotDir.isEmpty() || closing)
		return;
	// processEvents() below can run OBS's whole shutdown re-entrantly (the
	// harness closes OBS on a timer) and delete this dock under us: check after
	// every pump; without it the plugin can exit with heap corruption.
	QPointer<MultistreamDock> self(this);
	QDir().mkpath(snapshotDir);
	refreshAll(); // headline and rows from the same moment
	// Show the real dock, floating, so OBS lays it out exactly as a user sees
	// it (test copy of OBS only: this path runs only with the env var set).
	// Shots alternate a roomy and a narrow width.
	const int width = (snapshotCount % 2 == 0) ? 380 : 250;
	if (auto *dw = qobject_cast<QDockWidget *>(parentWidget())) {
		dw->setFloating(true);
		dw->show();
		dw->resize(width, 640);
	} else {
		resize(width, 640);
	}
	for (int i = 0; i < 3; i++) {
		QCoreApplication::processEvents();
		if (!self || closing)
			return;
		if (layout())
			layout()->activate();
	}
	grab().save(QDir(snapshotDir).filePath(QStringLiteral("dock-%1-w%2.png").arg(++snapshotCount).arg(width)));
	if (snapshotCount == 1 && !cfg.destinations.empty()) {
		EditDialog dlg(cfg.destinations.front(), false, true, this);
		dlg.resize(dlg.sizeHint());
		if (dlg.layout())
			dlg.layout()->activate();
		dlg.grab().save(QDir(snapshotDir).filePath(QStringLiteral("dialog.png")));
		// Also the dialogs that show extras: Twitch (bandwidth test) and any
		// destination with platform-limit advice.
		for (const auto &d : cfg.destinations) {
			if (&d == &cfg.destinations.front() || (d.platform != QLatin1String("twitch") && limitIssues(d).empty()))
				continue;
			EditDialog extra(d, false, creds::hasKey(d.id), this);
			extra.resize(extra.sizeHint());
			if (extra.layout())
				extra.layout()->activate();
			extra.grab().save(QDir(snapshotDir).filePath(QStringLiteral("dialog-%1.png").arg(d.platform)));
		}
	}
	if (snapshotCount == 1 && cfg.destinations.empty()) {
		auto candidates = findImportCandidates();
		if (!candidates.empty()) {
			ImportDialog dlg(std::move(candidates), existingServerKeys(), this);
			dlg.resize(dlg.sizeHint().expandedTo(QSize(480, 360)));
			if (dlg.layout())
				dlg.layout()->activate();
			dlg.grab().save(QDir(snapshotDir).filePath(QStringLiteral("import.png")));
			// Test copy only: accept the dialog's default selection so the
			// harness can check what an import produces.
			if (!testEnv("RWS_MULTISTREAM_TEST_IMPORT").isEmpty())
				addImported(dlg.selected());
		}
	}
}

// ---- behaviour --------------------------------------------------------------

void MultistreamDock::setOn(const QString &id, bool on)
{
	Destination *d = find(id);
	if (!d)
		return;
	d->enabled = on;
	save();
	if (on && obs_frontend_streaming_active())
		startOne(id);
	else if (!on)
		stopOne(id);
	updateReport(id);
	refreshCard(id);
	refreshSummary();
	notify(id);
}

void MultistreamDock::startOne(const QString &id, bool skipMainWait)
{
	Destination *d = find(id);
	if (!d || closing)
		return;
	if (!skipMainWait && d->enc.shared && obs_frontend_streaming_active() && !mainStreamProducing()) {
		if (waitingForMain.empty())
			mainWaitStartedMs = QDateTime::currentMSecsSinceEpoch();
		waitingForMain.insert(id);
		mainReadyTimer.start();
		refreshCard(id);
		refreshSummary();
		return;
	}
	waitingForMain.erase(id);
	OutputRunner *r = runner(id);
	if (r->busy())
		return;
	// Nothing connects without the budget's say-so: automatic retries,
	// stall restarts and the Retry button all come through here.
	const int64_t wait = connectionBudget(id).budgetWaitMs(QDateTime::currentMSecsSinceEpoch());
	if (wait > 0) {
		scheduleRetry(id, wait, true);
		return;
	}
	cancelRetry(id);
	QString err;
	r->start(*d, creds::loadKey(id), &err);
	refreshCard(id);
	refreshSummary();
}

void MultistreamDock::startWhenMainReady()
{
	if (waitingForMain.empty() || closing) {
		mainReadyTimer.stop();
		return;
	}
	if (!obs_frontend_streaming_active()) {
		// OBS stopped (or failed) before its stream got going.
		waitingForMain.clear();
		mainReadyTimer.stop();
		refreshAll();
		return;
	}
	bool timedOut = false;
	if (!mainStreamProducing()) {
		if (QDateTime::currentMSecsSinceEpoch() - mainWaitStartedMs < kMainWaitTimeoutMs)
			return;
		obs_log(LOG_WARNING, "OBS's stream never started producing frames; starting shared destinations anyway");
		timedOut = true;
	}
	mainReadyTimer.stop();
	const std::set<QString> ids = std::move(waitingForMain);
	waitingForMain.clear();
	for (const QString &id : ids) {
		const Destination *d = find(id);
		if (d && d->enabled)
			startOne(id, timedOut); // without skipping, it would just re-queue itself
	}
}

void MultistreamDock::stopOne(const QString &id)
{
	waitingForMain.erase(id);
	cancelRetry(id);
	retries[id].autoFailures = 0;
	retries[id].recovering = false;
	retries[id].dropAttempts = 0;
	auto it = runners.find(id);
	if (it != runners.end())
		it->second->stop();
}

void MultistreamDock::stopAll(bool immediate)
{
	waitingForMain.clear();
	mainReadyTimer.stop();
	// Stop Streaming cancels every pending retry. The budget history is kept,
	// so stopping and restarting cannot be used to reset it.
	for (auto &[id, st] : retries) {
		if (st.timer)
			st.timer->stop();
		st.retryAtMs = 0;
		st.heldByBudget = false;
		st.autoFailures = 0;
		st.recovering = false;
		st.dropAttempts = 0;
	}
	for (auto &[id, r] : runners) {
		if (immediate)
			r->shutdown();
		else
			r->stop();
	}
}

void MultistreamDock::onStreamingStarted()
{
	report.begin(QDateTime::currentMSecsSinceEpoch(),
		     QLocale().toString(QDateTime::currentDateTime(), QStringLiteral("dddd, MMM d, h:mm AP")));
	reportStamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HHmmss"));
	lastTrafficMs = 0;
	for (const auto &d : cfg.destinations)
		report.declare(d.id, d.name, d.platform);
	for (const auto &d : cfg.destinations)
		if (d.enabled)
			startOne(d.id);
	for (const auto &d : cfg.destinations)
		updateReport(d.id);
	refreshAll();
}

void MultistreamDock::onStreamingStopping()
{
	finishReport(); // before the destinations stop, so stopping is not counted as switching off
	stopAll(false);
	refreshAll();
}

void MultistreamDock::onProfileChanging()
{
	save(); // keep this profile's hotkey bindings
	stopAll(true);
}

void MultistreamDock::onProfileChanged()
{
	reload();
}

void MultistreamDock::shutdown()
{
	if (closing)
		return;
	finishReport(); // OBS closed mid-stream
	closing = true;
	// No event loop runs in this function (nothing here pumps events), so the only code that can run foreign
	// handlers is a dialog's reject(); `self` is re-checked after it.
	QPointer<MultistreamDock> self(this);
	// Open edit / import / report dialogs and message boxes end here, innermost first (their callers see `closing`).
	{
		const std::vector<QPointer<QDialog>> open = modals_;
		for (auto it = open.rbegin(); it != open.rend(); ++it)
			if (*it)
				(*it)->reject();
	}
	if (!self)
		return;
	statsTimer.stop();
	uploadCancel = true;
	if (uploadThread && uploadThread->joinable())
		uploadThread->join();
	uploadThread.reset();
	save(); // persists hotkey bindings changed in OBS's settings
	stopAll(true);
	runners.clear();
	unregisterHotkeys();
	for (obs_hotkey_id *hk : {&allOnHotkey, &allOffHotkey}) {
		if (*hk != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_unregister(*hk);
		*hk = OBS_INVALID_HOTKEY_ID;
	}
}

void MultistreamDock::addDestination()
{
	Destination d;
	d.id = newDestinationId();
	d.platform = QStringLiteral("youtube");
	QPointer<EditDialog> dlg(new EditDialog(d, true, false, this));
	const auto cleanup = qScopeGuard([&] { delete dlg.data(); });
	if (execModal(dlg) != QDialog::Accepted)
		return;
	Destination nd = dlg->destination();
	if (!creds::storeKey(nd.id, dlg->newKey())) {
		messageBox(QMessageBox::Warning, QStringLiteral("Red Warden Multistream"),
			   QStringLiteral("Windows Credential Manager would not save the stream key. "
					  "The destination was not added."));
		return;
	}
	nd.enabled = true;
	cfg.destinations.push_back(nd);
	syncHotkeys();
	save();
	runner(nd.id);
	rebuild();
	if (obs_frontend_streaming_active())
		startOne(nd.id);
}

void MultistreamDock::editDestination(const QString &id)
{
	Destination *d = find(id);
	if (!d)
		return;
	QPointer<EditDialog> dlg(new EditDialog(*d, false, creds::hasKey(id), this));
	const auto cleanup = qScopeGuard([&] { delete dlg.data(); });
	if (execModal(dlg) != QDialog::Accepted)
		return;

	OutputRunner *r = runner(id);
	if (dlg->removeRequested()) {
		r->shutdown();
		runners.erase(id);
		if (auto rit = retries.find(id); rit != retries.end()) {
			delete rit->second.timer;
			retries.erase(rit);
		}
		creds::deleteKey(id);
		cfg.destinations.erase(std::remove_if(cfg.destinations.begin(), cfg.destinations.end(),
						      [&](const Destination &x) { return x.id == id; }),
				       cfg.destinations.end());
		syncHotkeys();
		save();
		rebuild();
		return;
	}

	const Destination before = *d;
	Destination after = dlg->destination();
	after.enabled = before.enabled;
	const QString key = dlg->newKey();
	if (!key.isEmpty() && !creds::storeKey(id, key))
		messageBox(QMessageBox::Warning, QStringLiteral("Red Warden Multistream"),
			   QStringLiteral("Windows Credential Manager would not save the new stream key. "
					  "The old key is still in use."));
	*d = after;
	if (before.name != after.name)
		syncHotkeys(); // the hotkey's label carries the name; bindings are kept
	save();
	rebuild();

	const bool connectionChanged = !key.isEmpty() || before.server != after.server ||
				       before.enc.shared != after.enc.shared || before.enc.encoderId != after.enc.encoderId ||
				       before.enc.videoBitrate != after.enc.videoBitrate ||
				       before.enc.audioBitrate != after.enc.audioBitrate ||
				       before.enc.audioTrack != after.enc.audioTrack || before.enc.width != after.enc.width ||
				       before.enc.height != after.enc.height;
	if (connectionChanged && r->busy()) {
		const auto answer = messageBox(
			QMessageBox::Question, QStringLiteral("Red Warden Multistream"),
			QStringLiteral("\"%1\" is live. Reconnect it now with the new settings?\n"
				       "Viewers on that platform will see a short interruption.")
				.arg(after.name),
			QMessageBox::Yes | QMessageBox::No);
		if (answer == QMessageBox::Yes) {
			r->shutdown();
			startOne(id);
		}
	}
}

// ---- upload capacity ------------------------------------------------------------

double MultistreamDock::plannedUploadKbps()
{
	bool any = false;
	const double mainVideo = obsStreamVideoBitrate();
	const double mainAudio = obsAudio.bitrate;
	double sum = mainVideo + mainAudio; // OBS's own stream always goes out
	for (const auto &d : cfg.destinations) {
		if (!d.enabled)
			continue;
		any = true;
		if (d.enc.shared)
			sum += mainVideo + mainAudio; // its own full copy of the stream
		else
			sum += d.enc.videoBitrate + (d.enc.audioBitrate > 0 ? d.enc.audioBitrate : mainAudio);
	}
	return any ? sum * 1.05 : 0.0; // ~5% for RTMP/TCP overhead
}

void MultistreamDock::startUploadTest()
{
	if (uploadTesting || closing)
		return;
	if (obs_frontend_streaming_active()) {
		messageBox(QMessageBox::Information, QStringLiteral("Red Warden Multistream"),
			   QStringLiteral("Stop streaming before testing your upload speed. The test uses "
					  "your whole connection and would make your stream drop frames."));
		return;
	}
	const auto answer = messageBox(
		QMessageBox::Question, QStringLiteral("Test my upload speed"),
		QStringLiteral("This uploads test data to Cloudflare's public speed test (speed.cloudflare.com) for "
			       "about 10 seconds, the same way speed-test websites do. Nothing about you or your stream "
			       "is sent.\n\nStart the test?"),
		QMessageBox::Yes | QMessageBox::No);
	if (answer != QMessageBox::Yes)
		return;
	if (uploadThread && uploadThread->joinable())
		uploadThread->join();
	uploadTesting = true;
	uploadCancel = false;
	uploadProgressSeconds = 0.0;
	uploadProgressMbps = 0.0;
	refreshSummary();
	QPointer<MultistreamDock> self(this);
	uploadThread = std::make_unique<std::thread>([self, this] {
		uploadtest::Options opt;
		const auto res = uploadtest::run(opt, &uploadCancel, [self, this](double t, double mbps) {
			QMetaObject::invokeMethod(
				this,
				[self, this, t, mbps] {
					if (!self)
						return;
					uploadProgressSeconds = t;
					uploadProgressMbps = mbps;
					refreshSummary();
				},
				Qt::QueuedConnection);
		});
		QMetaObject::invokeMethod(
			this,
			[self, this, res] {
				if (self)
					finishUploadTest(res.ok, res.mbps, QString::fromStdString(res.error));
			},
			Qt::QueuedConnection);
	});
}

void MultistreamDock::finishUploadTest(bool ok, double mbpsValue, const QString &error)
{
	uploadTesting = false;
	if (uploadThread && uploadThread->joinable())
		uploadThread->join();
	uploadThread.reset();
	if (closing)
		return;
	if (ok) {
		globals.uploadMbps = mbpsValue;
		globals.uploadSource = QStringLiteral("measured");
		globals.uploadWhen = QDateTime::currentDateTime().toString(Qt::ISODate);
		saveGlobalSettings(globals);
		obs_log(LOG_INFO, "upload test: %.1f Mbps", mbpsValue);
	} else if (error != QLatin1String("Cancelled")) {
		messageBox(QMessageBox::Warning, QStringLiteral("Red Warden Multistream"),
			   QStringLiteral("The upload test didn't finish: %1\n\nYou can enter your upload speed "
					  "by hand instead, from any speed-test website.")
				   .arg(error));
	}
	refreshSummary();
}

void MultistreamDock::enterUploadSpeed()
{
	// A heap, tracked QInputDialog (the static getDouble is an exec loop shutdown could not end).
	QPointer<QInputDialog> dlg(new QInputDialog(this));
	const auto cleanup = qScopeGuard([&] { delete dlg.data(); });
	dlg->setWindowTitle(QStringLiteral("Enter my upload speed"));
	dlg->setLabelText(QStringLiteral("Your upload speed in Mbps (from any speed-test website):"));
	dlg->setInputMode(QInputDialog::DoubleInput);
	dlg->setDoubleDecimals(1);
	dlg->setDoubleRange(0.5, 100000.0);
	dlg->setDoubleValue(globals.uploadMbps > 0 ? globals.uploadMbps : 20.0);
	if (execModal(dlg) != QDialog::Accepted)
		return;
	const double v = dlg->doubleValue();
	globals.uploadMbps = v;
	globals.uploadSource = QStringLiteral("manual");
	globals.uploadWhen = QDateTime::currentDateTime().toString(Qt::ISODate);
	saveGlobalSettings(globals);
	refreshSummary();
}

// ---- import ---------------------------------------------------------------------

std::set<QString> MultistreamDock::existingServerKeys()
{
	std::set<QString> out;
	for (const auto &d : cfg.destinations)
		out.insert(d.server.trimmed() + QLatin1Char('\n') + creds::loadKey(d.id));
	return out;
}

std::vector<ImportCandidate> MultistreamDock::findImportCandidates()
{
	std::vector<ImportCandidate> out;
	auto readFile = [](const QString &path) {
		QFile f(path);
		return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
	};
	// obs-multi-rtmp: a file in the current profile folder.
	if (char *profile = obs_frontend_get_current_profile_path()) {
		const QByteArray json = readFile(QString::fromUtf8(profile) + "/obs-multi-rtmp.json");
		bfree(profile);
		if (!json.isEmpty())
			for (auto &c : parseMultiRtmp(json))
				out.push_back(std::move(c));
	}
	// Aitum: plugin_config/aitum-multistream/config.json, keyed by profile name.
	if (const std::string root = kitpaths::pluginConfigRoot(); !root.empty()) {
		const QDir dir(QString::fromUtf8(root.c_str())); // .../plugin_config/
		const QByteArray json = readFile(dir.filePath(QStringLiteral("aitum-multistream/config.json")));
		if (!json.isEmpty()) {
			char *name = obs_frontend_get_current_profile();
			const QString profileName = name ? QString::fromUtf8(name) : QString();
			bfree(name);
			for (auto &c : parseAitum(json, profileName))
				out.push_back(std::move(c));
		}
	}
	return out;
}

void MultistreamDock::importDestinations()
{
	auto candidates = findImportCandidates();
	if (candidates.empty()) {
		messageBox(QMessageBox::Information, QStringLiteral("Import destinations"),
			   QStringLiteral("Nothing to import: no obs-multi-rtmp or Aitum Multistream "
					  "destinations are saved for this OBS profile."));
		return;
	}
	QPointer<ImportDialog> dlg(new ImportDialog(std::move(candidates), existingServerKeys(), this));
	const auto cleanup = qScopeGuard([&] { delete dlg.data(); });
	if (execModal(dlg) != QDialog::Accepted)
		return;
	addImported(dlg->selected());
}

void MultistreamDock::addImported(const std::vector<ImportCandidate> &chosen)
{
	int added = 0;
	for (const auto &c : chosen) {
		Destination d;
		d.id = newDestinationId();
		d.name = c.name;
		d.platform = c.platform;
		d.server = c.server.trimmed();
		d.enabled = false; // nothing goes live until the user turns it on
		d.enc.shared = c.shared;
		if (!c.shared) {
			// The other plugin may name an encoder this OBS no longer has
			// (older NVENC ids, a removed plugin); fall back to the default.
			const QByteArray encId = c.encoderId.toUtf8();
			const char *codec = encId.isEmpty() ? nullptr : obs_get_encoder_codec(encId.constData());
			d.enc.encoderId = (codec && obs_get_encoder_type(encId.constData()) == OBS_ENCODER_VIDEO)
						  ? c.encoderId
						  : QString();
			if (c.videoBitrate > 0)
				d.enc.videoBitrate = c.videoBitrate;
			d.enc.audioBitrate = c.audioBitrate;
			d.enc.audioTrack = c.audioTrack;
			d.enc.width = c.width;
			d.enc.height = c.height;
		}
		if (!c.key.isEmpty() && !creds::storeKey(d.id, c.key))
			continue;
		cfg.destinations.push_back(d);
		runner(d.id);
		added++;
	}
	obs_log(LOG_INFO, "imported %d destination(s)", added);
	importableCount = 0;
	syncHotkeys();
	save();
	rebuild();
}

// ---- external control -------------------------------------------------------

namespace {
QString hotkeyName(const QString &id)
{
	return QStringLiteral("rws_multistream.toggle.") + id;
}
} // namespace

void MultistreamDock::onHotkey(void *data, obs_hotkey_id id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	auto *self = static_cast<MultistreamDock *>(data);
	// Hotkeys fire on libobs' hotkey thread; act on the UI thread.
	QMetaObject::invokeMethod(
		self,
		[self, id] {
			if (self->closing)
				return;
			if (id == self->allOnHotkey)
				self->setAll(true);
			else if (id == self->allOffHotkey)
				self->setAll(false);
			else if (auto it = self->hotkeyDest.find(id); it != self->hotkeyDest.end())
				self->setEnabledByKey(it->second, 2);
		},
		Qt::QueuedConnection);
}

void MultistreamDock::captureHotkeys()
{
	for (const auto &[hk, destId] : hotkeyDest) {
		Destination *d = find(destId);
		if (!d)
			continue;
		obs_data_array_t *arr = obs_hotkey_save(hk);
		obs_data_t *wrap = obs_data_create();
		obs_data_set_array(wrap, "b", arr);
		d->hotkeyJson = QString::fromUtf8(obs_data_get_json(wrap));
		obs_data_release(wrap);
		obs_data_array_release(arr);
	}
}

void MultistreamDock::unregisterHotkeys()
{
	for (const auto &[hk, destId] : hotkeyDest)
		obs_hotkey_unregister(hk);
	hotkeyDest.clear();
}

void MultistreamDock::syncHotkeys()
{
	captureHotkeys();
	unregisterHotkeys();
	for (const auto &d : cfg.destinations) {
		const QByteArray name = hotkeyName(d.id).toUtf8();
		const QByteArray desc = QStringLiteral("Red Warden Multistream: toggle %1").arg(d.name).toUtf8();
		const obs_hotkey_id hk = obs_hotkey_register_frontend(name.constData(), desc.constData(), onHotkey, this);
		if (hk == OBS_INVALID_HOTKEY_ID)
			continue;
		if (!d.hotkeyJson.isEmpty()) {
			if (obs_data_t *wrap = obs_data_create_from_json(d.hotkeyJson.toUtf8().constData())) {
				obs_data_array_t *arr = obs_data_get_array(wrap, "b");
				obs_hotkey_load(hk, arr);
				obs_data_array_release(arr);
				obs_data_release(wrap);
			}
		}
		hotkeyDest[hk] = d.id;
	}
}

MultistreamDock::DestinationInfo MultistreamDock::info(const Destination &d)
{
	DestinationInfo i;
	i.id = d.id;
	i.name = d.name;
	i.platform = d.platform;
	i.enabled = d.enabled;
	auto rit = runners.find(d.id);
	OutputRunner *r = rit != runners.end() ? rit->second.get() : nullptr;
	auto sit = retries.find(d.id);
	const RetryState *rs = sit != retries.end() ? &sit->second : nullptr;
	if (waitingForMain.count(d.id)) {
		i.state = QStringLiteral("waiting");
	} else if (rs && rs->timer && rs->timer->isActive()) {
		i.state = rs->heldByBudget  ? QStringLiteral("paused")
			  : rs->recovering ? QStringLiteral("reconnecting")
					   : QStringLiteral("retrying");
	} else if (r) {
		switch (r->state()) {
		case OutputRunner::State::Live:
			i.state = r->awaitingFirstFrame() ? QStringLiteral("connecting") : QStringLiteral("live");
			i.kbps = r->kbps();
			break;
		case OutputRunner::State::Starting:
			i.state = (rs && rs->recovering) ? QStringLiteral("reconnecting") : QStringLiteral("connecting");
			break;
		case OutputRunner::State::Reconnecting:
			i.state = QStringLiteral("reconnecting");
			break;
		case OutputRunner::State::Stopping:
			i.state = QStringLiteral("stopping");
			break;
		case OutputRunner::State::Error:
			i.state = QStringLiteral("error");
			break;
		case OutputRunner::State::Idle:
			i.state = !d.enabled ? QStringLiteral("off")
					     : (obs_frontend_streaming_active() ? QStringLiteral("idle") : QStringLiteral("ready"));
			break;
		}
	} else {
		i.state = d.enabled ? QStringLiteral("ready") : QStringLiteral("off");
	}
	return i;
}

void MultistreamDock::notify(const QString &id)
{
	if (!stateListener || closing)
		return;
	// Emit on a later turn of the event loop: a websocket request may be
	// blocked waiting on this thread right now, and emitting an event from
	// inside it could deadlock obs-websocket.
	QMetaObject::invokeMethod(
		this,
		[this, id] {
			if (!stateListener || closing)
				return;
			if (const Destination *d = find(id))
				stateListener(info(*d));
		},
		Qt::QueuedConnection);
}

std::vector<MultistreamDock::DestinationInfo> MultistreamDock::snapshot()
{
	std::vector<DestinationInfo> out;
	for (const auto &d : cfg.destinations)
		out.push_back(info(d));
	return out;
}

std::optional<std::vector<MultistreamDock::DestinationInfo>> MultistreamDock::destinationsNow()
{
	if (closing)
		return std::nullopt;
	return snapshot();
}

std::vector<MultistreamDock::OutputSnapshot> MultistreamDock::outputSnapshots()
{
	std::vector<OutputSnapshot> out;
	if (closing)
		return out;
	for (const auto &d : cfg.destinations) {
		OutputSnapshot s;
		s.id = d.id;
		s.name = d.name;
		// A destination switched off is Off even while its output is still winding down (Stopping): the user's
		// decision comes first, so the Network light drops it at the very next tick.
		if (d.enabled) {
			const QString st = info(d).state;
			if (st == QStringLiteral("live"))
				s.state = OutputSnapshot::State::Live;
			else if (st == QStringLiteral("reconnecting"))
				s.state = OutputSnapshot::State::Reconnecting;
			else if (st == QStringLiteral("off") || st == QStringLiteral("ready"))
				s.state = OutputSnapshot::State::Off;
			else
				s.state = OutputSnapshot::State::Other; // connecting, retrying, paused, waiting, failed
		}
		auto rit = runners.find(d.id);
		if (s.state != OutputSnapshot::State::Off && rit != runners.end() && rit->second) {
			const OutputRunner *r = rit->second.get();
			s.dropped = (uint64_t)qMax(0, r->droppedFrames());
			s.total = (uint64_t)qMax(0, r->totalFrames());
			s.serial = r->outputSerial();
		}
		out.push_back(std::move(s));
	}
	return out;
}

void MultistreamDock::setDropFigures(const std::map<QString, double> &pctById)
{
	if (closing || pctById == dropFigures_)
		return;
	dropFigures_ = pctById;
	QString what;
	for (const auto &[id, pct] : pctById)
		if (const Destination *d = find(id))
			what += QStringLiteral("%1%2=%3").arg(what.isEmpty() ? QString() : QStringLiteral(", "), d->name).arg(pct, 0, 'f', 1);
	obs_log(LOG_INFO, "drop badge: %s", what.isEmpty() ? "none" : what.toUtf8().constData());
	refreshAll();
}

bool MultistreamDock::setEnabledByKey(const QString &key, int mode, DestinationInfo *out)
{
	Destination *match = find(key);
	if (!match)
		for (auto &d : cfg.destinations)
			if (d.name.compare(key, Qt::CaseInsensitive) == 0) {
				match = &d;
				break;
			}
	if (!match)
		return false;
	const bool on = mode == 2 ? !match->enabled : mode == 1;
	const QString id = match->id; // setOn may rebuild nothing, but keep a copy
	setOn(id, on);
	if (out)
		if (const Destination *d = find(id))
			*out = info(*d);
	return true;
}

void MultistreamDock::setAll(bool on)
{
	std::vector<QString> ids;
	for (const auto &d : cfg.destinations)
		ids.push_back(d.id);
	for (const QString &id : ids)
		setOn(id, on);
}

// ---- end-of-stream report ------------------------------------------------------

SessionReport::Phase MultistreamDock::phaseFor(const Destination &d)
{
	using P = SessionReport::Phase;
	if (!d.enabled)
		return P::Off;
	if (waitingForMain.count(d.id))
		return P::Connecting;
	auto it = runners.find(d.id);
	if (it == runners.end())
		return P::Down;
	auto rit = retries.find(d.id);
	const bool recovering = rit != retries.end() && rit->second.recovering;
	switch (it->second->state()) {
	case OutputRunner::State::Live:
		return P::Live;
	case OutputRunner::State::Starting:
		return recovering ? P::Reconnecting : P::Connecting;
	case OutputRunner::State::Error:
	case OutputRunner::State::Idle:
		return recovering ? P::Reconnecting : P::Down;
	case OutputRunner::State::Reconnecting:
		return P::Reconnecting;
	case OutputRunner::State::Stopping:
		return P::Off;
	default:
		return P::Down; // failed, or waiting for its next retry
	}
}

void MultistreamDock::updateReport(const QString &id)
{
	if (!report.active())
		return;
	const Destination *d = find(id);
	if (!d)
		return;
	QString error;
	if (auto it = runners.find(id); it != runners.end() && it->second->state() == OutputRunner::State::Error)
		error = it->second->detail();
	report.update(id, d->name, d->platform, phaseFor(*d), QDateTime::currentMSecsSinceEpoch(), error);
}

void MultistreamDock::finishReport()
{
	if (!report.active())
		return;
	for (const auto &d : cfg.destinations)
		updateReport(d.id);
	report.end(QDateTime::currentMSecsSinceEpoch());
	if (report.destinations().empty())
		return; // nothing was multistreamed: no file, no recap
	obs_log(LOG_INFO, "stream report: %s", report.headline().toUtf8().constData());
	if (!reportsDir.isEmpty() && QDir().mkpath(reportsDir)) {
		QFile f(QDir(reportsDir).filePath(QStringLiteral("stream-%1.txt").arg(reportStamp)));
		if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
			f.write(report.text().toUtf8());
		// Keep the last 50.
		QDir dir(reportsDir);
		const QStringList files =
			dir.entryList({QStringLiteral("stream-*.txt")}, QDir::Files, QDir::Name | QDir::Reversed);
		for (int i = 50; i < files.size(); i++)
			dir.remove(files[i]);
	}
	refreshSummary();
	if (!snapshotDir.isEmpty()) {
		// Test copy only: photograph the recap and the report once the
		// destinations have finished stopping.
		QTimer::singleShot(4000, this, [this] {
			if (closing)
				return;
			if (auto *dw = qobject_cast<QDockWidget *>(parentWidget()))
				dw->resize(380, 640);
			refreshAll();
			QPointer<MultistreamDock> self(this);
			QCoreApplication::processEvents();
			if (!self || closing) // OBS may have shut down inside processEvents()
				return;
			grab().save(QDir(snapshotDir).filePath(QStringLiteral("after-stream-w380.png")));
			ReportDialog dlg(report, reportsDir, this);
			if (dlg.layout())
				dlg.layout()->activate();
			dlg.grab().save(QDir(snapshotDir).filePath(QStringLiteral("report.png")));
		});
	}
}

void MultistreamDock::showReport()
{
	if (!report.hasResult())
		return;
	QPointer<ReportDialog> dlg(new ReportDialog(report, reportsDir, this));
	const auto cleanup = qScopeGuard([&] { delete dlg.data(); });
	execModal(dlg);
}

// Heap + QPointer + rejected by shutdown(), like Pre-Flight's dialogs: a dialog on the stack whose parent is
// (inside) the OBS main window would be destroyed twice if OBS tears the window down while it is open. Returns
// the dialog result, or -1 when the dialog was destroyed under us (the caller must then return at once).
int MultistreamDock::execModal(QDialog *dlg)
{
	if (!dlg || closing)
		return -1;
	QPointer<QDialog> me(dlg);
	modals_.push_back(me);
	dlg->exec();
	const bool alive = !me.isNull();
	const int rc = alive ? dlg->result() : -1;
	modals_.erase(std::remove_if(modals_.begin(), modals_.end(),
				     [&](const QPointer<QDialog> &p) { return p.isNull() || p == me; }),
		      modals_.end());
	return (!alive || closing) ? -1 : rc;
}

QMessageBox::StandardButton MultistreamDock::messageBox(QMessageBox::Icon icon, const QString &title,
							const QString &text, QMessageBox::StandardButtons buttons)
{
	if (closing)
		return QMessageBox::NoButton;
	QPointer<QMessageBox> box(new QMessageBox(icon, title, text, buttons, this));
	const int rc = execModal(box);
	QMessageBox::StandardButton clicked = QMessageBox::NoButton;
	if (rc >= 0 && box)
		clicked = box->standardButton(box->clickedButton());
	delete box.data();
	return clicked;
}

// ---- platform limits -------------------------------------------------------------

std::vector<LimitIssue> MultistreamDock::limitIssues(const Destination &d)
{
	auto it = limits.find(d.platform);
	if (it == limits.end())
		return {};
	StreamFacts f;
	f.shared = d.enc.shared;
	obs_video_info ovi = {};
	const bool haveVideo = obs_get_video_info(&ovi);
	if (haveVideo && ovi.fps_den > 0)
		f.fps = (double)ovi.fps_num / (double)ovi.fps_den;
	const OutputRunner *r = nullptr;
	if (auto rit = runners.find(d.id); rit != runners.end())
		r = rit->second.get();
	if (d.enc.shared) {
		f.videoKbps = obsStreamVideoBitrate();
		f.audioKbps = (r && r->busy() && r->audioBitrateKbps() > 0) ? r->audioBitrateKbps() : obsAudio.bitrate;
		// While live, the encoder's own setting is the truth.
		f.keyintSec = (r && r->usesSharedEncoder() && r->busy() && r->sharedKeyintSec() >= 0) ? r->sharedKeyintSec()
													: obsStreamKeyintSec();
		f.height = haveVideo ? (int)ovi.output_height : 0;
	} else {
		f.videoKbps = d.enc.videoBitrate;
		f.audioKbps = d.enc.audioBitrate > 0 ? d.enc.audioBitrate : obsAudio.bitrate;
		f.keyintSec = 2; // separate encoders always use 2 s
		f.height = d.enc.height > 0 ? d.enc.height : (haveVideo ? (int)ovi.output_height : 0);
	}
	return checkLimits(d.platform, platformById(d.platform).label, it->second, f);
}

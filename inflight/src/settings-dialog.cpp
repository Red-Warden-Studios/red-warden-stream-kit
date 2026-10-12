/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "settings-dialog.hpp"

#include "inflight-settings-check.hpp"
#include "inflight-ui-logic.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPair>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

using namespace inflight;

namespace {
QString T(const char *key, const char *fallback)
{
	const char *t = obs_module_text(key);
	return (t && qstrcmp(t, key) != 0) ? QString::fromUtf8(t) : QString::fromUtf8(fallback);
}

TrFn uiTr()
{
	return [](const char *key, const char *fallback) { return T(key, fallback); };
}

const char *const kTabKeys[4] = {"watch", "scenes", "lights", "advanced"};

// The seven lights, in the order of the Light enum, and the Settings flag each one switches.
struct LightFlag {
	Light light;
	bool Settings::*flag;
};
const LightFlag kLightFlags[7] = {
	{Light::Mic, &Settings::lightMic},       {Light::Desktop, &Settings::lightDesktop},
	{Light::Scene, &Settings::lightScene},   {Light::Recording, &Settings::lightRecording},
	{Light::Network, &Settings::lightNetwork}, {Light::Pc, &Settings::lightPc},
	{Light::Stream, &Settings::lightStream},
};

struct NameList {
	QStringList names;
	QStringList uuids;
};

// obs_enum_sources hands out borrowed pointers: nothing is retained here, so nothing to release.
bool enumAudio(void *param, obs_source_t *src)
{
	auto *out = static_cast<QVector<QPair<QString, QString>> *>(param);
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

NameList audioSources()
{
	QVector<QPair<QString, QString>> v;
	obs_enum_sources(enumAudio, &v);
	std::sort(v.begin(), v.end(), [](const QPair<QString, QString> &a, const QPair<QString, QString> &b) {
		return a.first.compare(b.first, Qt::CaseInsensitive) < 0;
	});
	NameList r;
	for (const auto &p : v) {
		r.names << p.first;
		r.uuids << p.second;
	}
	return r;
}

// obs_frontend_get_scenes returns strong refs in the list; obs_frontend_source_list_free releases them.
NameList sceneList()
{
	NameList r;
	struct obs_frontend_source_list list = {};
	obs_frontend_get_scenes(&list);
	for (size_t i = 0; i < list.sources.num; ++i) {
		obs_source_t *s = list.sources.array[i];
		const char *n = obs_source_get_name(s);
		const char *u = obs_source_get_uuid(s);
		if (n && u) {
			r.names << QString::fromUtf8(n);
			r.uuids << QString::fromUtf8(u);
		}
	}
	obs_frontend_source_list_free(&list);
	return r;
}

// Item 0 is the "nothing chosen" entry (empty UUID). A saved UUID that is no longer in the list is
// added as a visible "(missing ...)" entry and selected, never silently dropped.
void fillCombo(QComboBox *combo, const QStringList &names, const QStringList &uuids, const QString &saved,
	       const QString &firstLabel, const QString &missingLabel)
{
	combo->clear();
	combo->addItem(firstLabel, QString());
	int select = 0;
	for (int i = 0; i < names.size(); ++i) {
		combo->addItem(names[i], uuids[i]);
		if (!saved.isEmpty() && uuids[i] == saved)
			select = combo->count() - 1;
	}
	if (!saved.isEmpty() && select == 0) {
		combo->addItem(missingLabel, saved);
		select = combo->count() - 1;
	}
	combo->setCurrentIndex(select);
}

// A minutes spin box that remembers the exact seconds it was loaded with, so opening and closing the
// dialog never rounds a value the user did not touch.
void setMinutes(QSpinBox *sb, int seconds)
{
	const int m = std::min(std::max((seconds + 30) / 60, sb->minimum()), sb->maximum());
	sb->setValue(m);
	sb->setProperty("origMin", m);
	sb->setProperty("origSec", seconds);
}

int getSeconds(const QSpinBox *sb)
{
	if (sb->property("origMin").isValid() && sb->value() == sb->property("origMin").toInt())
		return sb->property("origSec").toInt();
	return sb->value() * 60;
}

QSpinBox *secondsSpin(QWidget *parent, int lo, int hi)
{
	auto *sb = new QSpinBox(parent);
	sb->setRange(lo, hi);
	sb->setSuffix(QStringLiteral(" s"));
	return sb;
}

QLabel *hintLabel(const QString &text, QWidget *parent)
{
	auto *l = new QLabel(text, parent);
	l->setWordWrap(true);
	QFont f = l->font();
	f.setItalic(true);
	l->setFont(f);
	return l;
}
} // namespace

InflightSettingsDialog::InflightSettingsDialog(const Settings &current, QWidget *parent)
	: QDialog(parent), base_(current)
{
	{
		const NameList a = audioSources();
		audioNames_ = a.names;
		audioUuids_ = a.uuids;
		const NameList s = sceneList();
		sceneNames_ = s.names;
		sceneUuids_ = s.uuids;
	}
	// Nothing is adjusted silently: values that are out of range in the saved settings are fixed
	// BEFORE the widgets are filled, and the note says so. Valid values are never touched.
	Settings s = current;
	const QStringList notes = normalizeSettings(s, uiTr());
	base_ = s;
	build();
	loadFrom(s);
	if (!notes.isEmpty())
		showNote(T("InFlight.Settings.AdjustedOpen",
			   "Some saved values were out of range and have been adjusted. Check them, then press OK."),
			 notes);
}

void InflightSettingsDialog::showNote(const QString &title, const QStringList &notes)
{
	QString text = title;
	for (const QString &n : notes)
		text += QStringLiteral("\n- ") + n;
	note_->setText(text);
	note_->setVisible(true);
}

InflightSettingsDialog::~InflightSettingsDialog() = default;

int InflightSettingsDialog::tabCount() const
{
	return tabs_ ? tabs_->count() : 0;
}

QString InflightSettingsDialog::tabKey(int i) const
{
	return (i >= 0 && i < 4) ? QString::fromLatin1(kTabKeys[i]) : QString();
}

void InflightSettingsDialog::showTab(int i)
{
	if (tabs_ && i >= 0 && i < tabs_->count())
		tabs_->setCurrentIndex(i);
}

void InflightSettingsDialog::scrollCurrentTabToEnd()
{
	if (auto *sa = qobject_cast<QScrollArea *>(tabs_ ? tabs_->currentWidget() : nullptr))
		if (QScrollBar *bar = sa->verticalScrollBar())
			bar->setValue(bar->maximum());
}

void InflightSettingsDialog::build()
{
	setWindowTitle(T("InFlight.Settings.Title", "In-Flight settings"));
	setModal(true);
	resize(600, 620); // fits a 1280x720 screen; the Advanced tab scrolls if it still needs more
	const TrFn tr = uiTr();

	auto *root = new QVBoxLayout(this);
	tabs_ = new QTabWidget(this);
	root->addWidget(tabs_, 1);

	// ---- What to watch
	{
		auto *page = new QWidget(tabs_);
		page->setObjectName(QString::fromLatin1(kTabKeys[0]));
		auto *lay = new QVBoxLayout(page);
		auto *form = new QFormLayout();
		mic_ = new QComboBox(page);
		desktop_ = new QComboBox(page);
		for (QComboBox *c : {mic_, desktop_}) {
			c->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
			c->setMinimumContentsLength(24);
			c->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		}
		form->addRow(T("InFlight.Settings.Mic", "Microphone"), mic_);
		form->addRow(T("InFlight.Settings.Desktop", "Desktop / game audio"), desktop_);
		lay->addLayout(form);
		lay->addWidget(hintLabel(T("InFlight.Settings.WatchHint",
					   "Pick the audio sources In-Flight should listen to. They are remembered "
					   "even if you rename them."),
					 page));
		lay->addSpacing(8);
		record_ = new QCheckBox(T("InFlight.Settings.Record", "I record my streams"), page);
		lay->addWidget(record_);
		auto *graceRow = new QHBoxLayout();
		graceRow->setContentsMargins(24, 0, 0, 0);
		graceRow->addWidget(new QLabel(T("InFlight.Settings.RecordGraceA", "Recording must start within"), page));
		recordGrace_ = secondsSpin(page, 10, 600);
		graceRow->addWidget(recordGrace_);
		graceRow->addWidget(new QLabel(T("InFlight.Settings.RecordGraceB", "of going live"), page));
		graceRow->addStretch(1);
		lay->addLayout(graceRow);
		lay->addStretch(1);
		tabs_->addTab(page, T("InFlight.Settings.Tab.Watch", "What to watch"));
		connect(record_, &QCheckBox::toggled, this, [this] { refreshEnabled(); });
	}

	// ---- Holding scenes
	{
		auto *page = new QWidget(tabs_);
		page->setObjectName(QString::fromLatin1(kTabKeys[1]));
		auto *lay = new QVBoxLayout(page);
		auto *intro = new QLabel(T("InFlight.Settings.ScenesIntro",
					   "Holding scenes are scenes that should only be on air for a while, like "
					   "Starting Soon or Be Right Back. The Scene light warns you when one stays "
					   "up too long."),
					 page);
		intro->setWordWrap(true);
		lay->addWidget(intro);
		auto *scroll = new QScrollArea(page);
		scroll->setWidgetResizable(true);
		scroll->setFrameShape(QFrame::NoFrame);
		rowsHost_ = new QWidget(scroll);
		rowsLay_ = new QVBoxLayout(rowsHost_);
		rowsLay_->setContentsMargins(0, 0, 0, 0);
		rowsLay_->setSpacing(4);
		rowsEmpty_ = hintLabel(T("InFlight.Settings.ScenesNone", "No holding scenes yet."), rowsHost_);
		rowsLay_->addWidget(rowsEmpty_);
		rowsLay_->addStretch(1);
		scroll->setWidget(rowsHost_);
		lay->addWidget(scroll, 1);
		auto *btns = new QHBoxLayout();
		auto *add = new QPushButton(T("InFlight.Settings.AddScene", "Add a scene"), page);
		btns->addWidget(add);
		btns->addWidget(hintLabel(T("InFlight.Settings.RemoveHint", "Use the x on a row to remove it."), page), 1);
		lay->addLayout(btns);
		tabs_->addTab(page, T("InFlight.Settings.Tab.Scenes", "Holding scenes"));
		connect(add, &QPushButton::clicked, this, [this] { addSceneRow(QString(), QString(), 300); });
	}

	// ---- Lights
	{
		auto *page = new QWidget(tabs_);
		page->setObjectName(QString::fromLatin1(kTabKeys[2]));
		auto *lay = new QVBoxLayout(page);
		auto *intro = new QLabel(T("InFlight.Settings.LightsIntro", "Choose which lights In-Flight shows and judges."), page);
		intro->setWordWrap(true);
		lay->addWidget(intro);
		for (int i = 0; i < 7; ++i) {
			lights_[i] = new QCheckBox(lightName(kLightFlags[i].light, tr), page);
			lay->addWidget(lights_[i]);
		}
		lay->addStretch(1);
		tabs_->addTab(page, T("InFlight.Settings.Tab.Lights", "Lights"));
	}

	// ---- Advanced
	{
		auto *scroll = new QScrollArea(tabs_);
		scroll->setObjectName(QString::fromLatin1(kTabKeys[3]));
		scroll->setWidgetResizable(true);
		scroll->setFrameShape(QFrame::NoFrame);
		auto *page = new QWidget(scroll);
		auto *lay = new QVBoxLayout(page);

		auto *micBox = new QGroupBox(T("InFlight.Settings.Group.Mic", "Microphone"), page);
		auto *micForm = new QFormLayout(micBox);
		micAmber_ = secondsSpin(micBox, 5, 600);
		micRed_ = secondsSpin(micBox, 10, 1800);
		micForm->addRow(T("InFlight.Settings.MicAmber", "Warn when muted for"), micAmber_);
		micForm->addRow(T("InFlight.Settings.MicRed", "Fail when muted for"), micRed_);
		heard_ = new QCheckBox(T("InFlight.Settings.Heard", "Warn when my mic is very quiet"), micBox);
		micForm->addRow(heard_);
		heardSecs_ = secondsSpin(micBox, 30, 1800);
		heardDb_ = new QSpinBox(micBox);
		heardDb_->setRange(-80, -20);
		heardDb_->setSuffix(QStringLiteral(" dBFS"));
		micForm->addRow(T("InFlight.Settings.HeardSecs", "Quiet for"), heardSecs_);
		micForm->addRow(T("InFlight.Settings.HeardDb", "Quieter than"), heardDb_);
		micForm->addRow(hintLabel(T("InFlight.Settings.HeardHint",
					    "Off by default. It only arms after your mic has been heard this session, "
					    "and never while a holding scene is on air."),
					  micBox));
		lay->addWidget(micBox);

		auto *deskBox = new QGroupBox(T("InFlight.Settings.Group.Desktop", "Desktop audio"), page);
		auto *deskForm = new QFormLayout(deskBox);
		desktopSilence_ = secondsSpin(deskBox, 30, 1800);
		deskForm->addRow(T("InFlight.Settings.DesktopSilence", "Warn after complete silence for"), desktopSilence_);
		lay->addWidget(deskBox);

		auto *sceneBox = new QGroupBox(T("InFlight.Settings.Group.Scene", "Holding scenes"), page);
		auto *sceneForm = new QFormLayout(sceneBox);
		sceneExtra_ = new QSpinBox(sceneBox);
		sceneExtra_->setRange(1, 60);
		sceneExtra_->setSuffix(QStringLiteral(" min"));
		sceneForm->addRow(T("InFlight.Settings.SceneExtra", "Turn red this long after the limit"), sceneExtra_);
		lay->addWidget(sceneBox);

		auto *netBox = new QGroupBox(T("InFlight.Settings.Group.Net", "Network and PC (dropped or late frames)"), page);
		auto *netForm = new QFormLayout(netBox);
		dropAmber_ = new QDoubleSpinBox(netBox);
		dropAmber_->setRange(0.1, 10.0);
		dropAmber_->setDecimals(1);
		dropAmber_->setSingleStep(0.1);
		dropAmber_->setSuffix(QStringLiteral(" %"));
		dropRed_ = new QDoubleSpinBox(netBox);
		dropRed_->setRange(0.2, 50.0);
		dropRed_->setDecimals(1);
		dropRed_->setSingleStep(0.1);
		dropRed_->setSuffix(QStringLiteral(" %"));
		netForm->addRow(T("InFlight.Settings.DropAmber", "Warn at"), dropAmber_);
		netForm->addRow(T("InFlight.Settings.DropRed", "Fail at"), dropRed_);
		lay->addWidget(netBox);

		chime_ = new QCheckBox(T("InFlight.Settings.Chime", "Play a sound on a new red light"), page);
		lay->addWidget(chime_);
		lay->addWidget(hintLabel(T("InFlight.Settings.ChimeHint", "Plays the system beep once. Off by default."),
					 page));
		updates_ = new QCheckBox(T("InFlight.Settings.Updates", "Check for Stream Kit updates"), page);
		lay->addWidget(updates_);
		lay->addWidget(hintLabel(T("InFlight.Settings.UpdatesHint",
					   "This setting is shared by every Red Warden Stream Kit tool."),
					 page));
		lay->addStretch(1);
		scroll->setWidget(page);
		tabs_->addTab(scroll, T("InFlight.Settings.Tab.Advanced", "Advanced"));
		connect(heard_, &QCheckBox::toggled, this, [this] { refreshEnabled(); });
	}

	// Object names, so the apply-path test hook can drive the widgets.
	mic_->setObjectName(QStringLiteral("mic"));
	desktop_->setObjectName(QStringLiteral("desktop"));
	record_->setObjectName(QStringLiteral("record"));
	recordGrace_->setObjectName(QStringLiteral("recordGrace"));
	micAmber_->setObjectName(QStringLiteral("micMutedAmber"));
	micRed_->setObjectName(QStringLiteral("micMutedRed"));
	heard_->setObjectName(QStringLiteral("heard"));
	desktopSilence_->setObjectName(QStringLiteral("desktopSilence"));
	sceneExtra_->setObjectName(QStringLiteral("sceneExtra"));
	dropAmber_->setObjectName(QStringLiteral("dropAmber"));
	dropRed_->setObjectName(QStringLiteral("dropRed"));
	chime_->setObjectName(QStringLiteral("chime"));
	updates_->setObjectName(QStringLiteral("updates"));
	{
		const char *const names[7] = {"lightMic",     "lightDesktop", "lightScene", "lightRecording",
					      "lightNetwork", "lightPc",      "lightStream"};
		for (int i = 0; i < 7; ++i)
			lights_[i]->setObjectName(QString::fromLatin1(names[i]));
	}

	note_ = new QLabel(this);
	note_->setObjectName(QStringLiteral("ifSettingsNote"));
	note_->setWordWrap(true);
	note_->setStyleSheet(QStringLiteral("#ifSettingsNote { background: rgba(242, 166, 59, 45);"
					    " border: 1px solid rgba(242, 166, 59, 150); border-radius: 4px; padding: 6px; }"));
	note_->setVisible(false);
	root->addWidget(note_);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	connect(buttons, &QDialogButtonBox::accepted, this, &InflightSettingsDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	root->addWidget(buttons);
}

void InflightSettingsDialog::refreshEnabled()
{
	recordGrace_->setEnabled(record_->isChecked());
	heardSecs_->setEnabled(heard_->isChecked());
	heardDb_->setEnabled(heard_->isChecked());
}

void InflightSettingsDialog::addSceneRow(const QString &uuid, const QString &label, int limitSeconds)
{
	auto *row = new QWidget(rowsHost_);
	row->setObjectName(QStringLiteral("holdRow"));
	auto *h = new QHBoxLayout(row);
	h->setContentsMargins(0, 0, 0, 0);
	h->setSpacing(6);

	auto *scene = new QComboBox(row);
	scene->setObjectName(QStringLiteral("scene"));
	scene->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	scene->setMinimumContentsLength(12);
	fillCombo(scene, sceneNames_, sceneUuids_, uuid, T("InFlight.Settings.ChooseScene", "(choose a scene)"),
		  T("InFlight.Settings.MissingScene", "(missing scene)"));
	h->addWidget(scene, 3);

	auto *name = new QLineEdit(label, row);
	name->setObjectName(QStringLiteral("label"));
	name->setPlaceholderText(T("InFlight.Settings.LabelHint", "Label, e.g. Starting Soon"));
	h->addWidget(name, 3);

	auto *mins = new QSpinBox(row);
	mins->setObjectName(QStringLiteral("mins"));
	mins->setRange(1, 120);
	mins->setSuffix(QStringLiteral(" min"));
	setMinutes(mins, limitSeconds);
	h->addWidget(mins, 0);

	auto *rm = new QToolButton(row);
	rm->setText(QString(QChar(0x00D7)));
	rm->setToolTip(T("InFlight.Settings.RemoveScene", "Remove this scene"));
	rm->setAccessibleName(T("InFlight.Settings.RemoveScene", "Remove this scene"));
	h->addWidget(rm, 0);
	connect(rm, &QToolButton::clicked, this, [this, row] {
		rowsLay_->removeWidget(row);
		row->setParent(nullptr);
		row->deleteLater();
		rowsEmpty_->setVisible(rowsHost_->findChildren<QWidget *>(QStringLiteral("holdRow"), Qt::FindDirectChildrenOnly).isEmpty());
	});

	// Above the empty-text label and the closing stretch.
	rowsLay_->insertWidget(rowsLay_->count() - 2, row);
	rowsEmpty_->setVisible(false);
}

void InflightSettingsDialog::loadFrom(const Settings &s)
{
	fillCombo(mic_, audioNames_, audioUuids_, s.micUuid, T("InFlight.Settings.None", "None"),
		  T("InFlight.Settings.MissingSource", "(missing source)"));
	fillCombo(desktop_, audioNames_, audioUuids_, s.desktopUuid, T("InFlight.Settings.None", "None"),
		  T("InFlight.Settings.MissingSource", "(missing source)"));
	record_->setChecked(s.recordIntent);
	recordGrace_->setValue(s.recordStartGraceS);

	for (QWidget *w : rowsHost_->findChildren<QWidget *>(QStringLiteral("holdRow"), Qt::FindDirectChildrenOnly)) {
		rowsLay_->removeWidget(w);
		w->setParent(nullptr);
		delete w;
	}
	for (const HoldingScene &hs : s.holdingScenes)
		addSceneRow(hs.sceneUuid, hs.label, hs.limitSeconds);
	rowsEmpty_->setVisible(s.holdingScenes.isEmpty());

	for (int i = 0; i < 7; ++i)
		lights_[i]->setChecked(s.*(kLightFlags[i].flag));

	micAmber_->setValue(s.micMutedAmberS);
	micRed_->setValue(s.micMutedRedS);
	heard_->setChecked(s.heardYouEnabled);
	heardSecs_->setValue(s.heardYouSilenceS);
	heardDb_->setValue(s.heardYouDbfs);
	desktopSilence_->setValue(s.desktopSilenceS);
	setMinutes(sceneExtra_, s.sceneRedExtraS);
	dropAmber_->setValue(s.dropAmberPct);
	dropRed_->setValue(s.dropRedPct);
	chime_->setChecked(s.chime);
	updates_->setChecked(s.updateChecks);
	refreshEnabled();
}

Settings InflightSettingsDialog::collect() const
{
	Settings s = base_; // chime and anything the dialog does not show stay as they were
	s.micUuid = mic_->currentData().toString();
	s.desktopUuid = desktop_->currentData().toString();
	s.recordIntent = record_->isChecked();
	s.recordStartGraceS = recordGrace_->value();

	s.holdingScenes.clear();
	for (QWidget *row : rowsHost_->findChildren<QWidget *>(QStringLiteral("holdRow"), Qt::FindDirectChildrenOnly)) {
		HoldingScene h;
		if (auto *c = row->findChild<QComboBox *>(QStringLiteral("scene")))
			h.sceneUuid = c->currentData().toString();
		if (auto *e = row->findChild<QLineEdit *>(QStringLiteral("label")))
			h.label = e->text();
		if (auto *m = row->findChild<QSpinBox *>(QStringLiteral("mins")))
			h.limitSeconds = getSeconds(m);
		s.holdingScenes.push_back(h);
	}

	for (int i = 0; i < 7; ++i)
		s.*(kLightFlags[i].flag) = lights_[i]->isChecked();

	s.micMutedAmberS = micAmber_->value();
	s.micMutedRedS = micRed_->value();
	s.heardYouEnabled = heard_->isChecked();
	s.heardYouSilenceS = heardSecs_->value();
	s.heardYouDbfs = heardDb_->value();
	s.desktopSilenceS = desktopSilence_->value();
	s.sceneRedExtraS = getSeconds(sceneExtra_);
	s.dropAmberPct = dropAmber_->value();
	s.dropRedPct = dropRed_->value();
	s.chime = chime_->isChecked();
	s.updateChecks = updates_->isChecked();
	return s;
}

Settings InflightSettingsDialog::result() const
{
	Settings s = collect();
	normalizeSettings(s);
	return s;
}

void InflightSettingsDialog::accept()
{
	Settings s = collect();
	const QStringList notes = normalizeSettings(s, uiTr());
	if (!notes.isEmpty() && !noted_) {
		noted_ = true;
		loadFrom(s); // show the adjusted values
		showNote(T("InFlight.Settings.AdjustedTitle", "I adjusted a few values. Check them, then press OK again."),
			 notes);
		return;
	}
	QDialog::accept();
}

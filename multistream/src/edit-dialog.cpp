/*
Red Warden Multistream
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "edit-dialog.hpp"
#include "bandwidth-dialog.hpp"
#include "credentials.hpp"
#include "dock-ui.hpp"
#include "obs-audio.hpp"
#include "platforms.hpp"
#include "server-url.hpp"

#include <obs.h>

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QSize>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QStringList>
#include <QUrl>
#include <QVBoxLayout>

namespace {
struct Res {
	const char *label;
	int w, h;
};
const Res kResolutions[] = {
	{"Same as OBS output", 0, 0},
	{"1920 x 1080", 1920, 1080},
	{"1280 x 720", 1280, 720},
	{"854 x 480", 854, 480},
};

void fillEncoders(QComboBox *combo, const QString &current)
{
	const char *id = nullptr;
	int preferred = -1;
	for (size_t i = 0; obs_enum_encoder_types(i, &id); i++) {
		if (obs_get_encoder_type(id) != OBS_ENCODER_VIDEO)
			continue;
		const uint32_t caps = obs_get_encoder_caps(id);
		if (caps & (OBS_ENCODER_CAP_DEPRECATED | OBS_ENCODER_CAP_INTERNAL))
			continue;
		const QString codec = QString::fromUtf8(obs_get_encoder_codec(id));
		if (codec != "h264" && codec != "hevc" && codec != "av1")
			continue;
		const QString name = QString::fromUtf8(obs_encoder_get_display_name(id));
		combo->addItem(QStringLiteral("%1  (%2)").arg(name, codec.toUpper()), QString::fromUtf8(id));
		if (preferred < 0 && QString::fromUtf8(id) == "obs_nvenc_h264_tex")
			preferred = combo->count() - 1;
	}
	int idx = combo->findData(current);
	if (idx < 0)
		idx = preferred >= 0 ? preferred : combo->findData(QStringLiteral("obs_x264"));
	if (idx >= 0)
		combo->setCurrentIndex(idx);
}
} // namespace

EditDialog::EditDialog(const Destination &d, bool isNew, bool hasSavedKey, QWidget *parent)
	: QDialog(parent), base(d)
{
	setWindowTitle(isNew ? QStringLiteral("Add destination") : QStringLiteral("Edit destination"));
	setMinimumWidth(460);
	// Hints use the theme's text colour, softened: legible on any OBS theme.
	const ui::Theme theme = ui::Theme::from(palette());
	const QString hintCss = QStringLiteral("color:%1; font-size:11px;").arg(ui::Theme::css(theme.secondary));
	setWindowIcon(QIcon(QStringLiteral(":/rws-multistream/logo-64.png")));

	auto *root = new QVBoxLayout(this);

	// --- Where ---------------------------------------------------------
	auto *where = new QGroupBox(QStringLiteral("Where"), this);
	auto *form = new QFormLayout(where);

	platformCombo = new QComboBox(where);
	for (const auto &p : platformPresets())
		platformCombo->addItem(p.label, p.id);
	platformCombo->setCurrentIndex(qMax(0, platformCombo->findData(d.platform.isEmpty() ? QStringLiteral("youtube") : d.platform)));
	form->addRow(QStringLiteral("Platform"), platformCombo);

	nameEdit = new QLineEdit(d.name, where);
	nameEdit->setPlaceholderText(QStringLiteral("e.g. YouTube main channel"));
	form->addRow(QStringLiteral("Name"), nameEdit);

	serverEdit = new QLineEdit(d.server, where);
	serverEdit->setPlaceholderText(QStringLiteral("rtmp:// or rtmps:// ingest URL"));
	form->addRow(QStringLiteral("Server URL"), serverEdit);

	keyEdit = new QLineEdit(where);
	keyEdit->setEchoMode(QLineEdit::Password);
	keyEdit->setPlaceholderText(hasSavedKey ? QStringLiteral("Saved in Windows Credential Manager. Leave blank to keep it.")
						: QStringLiteral("Paste your stream key"));
	showKey = new QCheckBox(QStringLiteral("Show"), where);
	auto *keyRow = new QHBoxLayout();
	keyRow->addWidget(keyEdit, 1);
	keyRow->addWidget(showKey);
	form->addRow(QStringLiteral("Stream key"), keyRow);

	hintLabel = new QLabel(where);
	hintLabel->setWordWrap(true);
	hintLabel->setStyleSheet(hintCss);
	form->addRow(QString(), hintLabel);

	// Twitch only: its bandwidth-test mode never goes on air.
	bandwidthButton = new QPushButton(QStringLiteral("Test bandwidth without going live"), where);
	bandwidthButton->setToolTip(QStringLiteral("Uses Twitch's bandwidth-test mode: Twitch receives the stream for "
						   "about 20 seconds but never shows it."));
	connect(bandwidthButton, &QPushButton::clicked, this, [this] {
		QString key = newKey();
		if (key.isEmpty())
			key = creds::loadKey(base.id);
		QPointer<BandwidthDialog> dlg(new BandwidthDialog(destination(), key, this));
		execNested(dlg);
		delete dlg.data();
	});
	auto *bwRow = new QHBoxLayout();
	bwRow->addWidget(bandwidthButton);
	bwRow->addStretch(1);
	form->addRow(QString(), bwRow);
	root->addWidget(where);

	// --- Encoding --------------------------------------------------------
	auto *encBox = new QGroupBox(QStringLiteral("Encoding"), this);
	auto *encLayout = new QVBoxLayout(encBox);
	const ObsStreamAudio oa = obsStreamAudio();
	sharedRadio = new QRadioButton(QStringLiteral("Share OBS's stream encoder (recommended: no extra GPU/CPU load)"), encBox);
	ownRadio = new QRadioButton(QStringLiteral("Separate encoder for this destination"), encBox);
	auto *group = new QButtonGroup(this);
	group->addButton(sharedRadio);
	group->addButton(ownRadio);
	(d.enc.shared ? sharedRadio : ownRadio)->setChecked(true);
	encLayout->addWidget(sharedRadio);
	auto *sharedNote = new QLabel(QStringLiteral("Sends exactly what OBS streams: OBS's video bitrate, and audio from "
						     "Track %1 at %2 kbps (%3).")
					      .arg(oa.track + 1)
					      .arg(oa.bitrate)
					      .arg(encoderLabel(oa.encoderId)),
				      encBox);
	sharedNote->setWordWrap(true);
	sharedNote->setContentsMargins(22, 0, 0, 4);
	sharedNote->setStyleSheet(hintCss);
	encLayout->addWidget(sharedNote);
	encLayout->addWidget(ownRadio);

	ownBox = new QWidget(encBox);
	auto *ownForm = new QFormLayout(ownBox);
	ownForm->setContentsMargins(22, 0, 0, 0);
	encoderCombo = new QComboBox(ownBox);
	fillEncoders(encoderCombo, d.enc.encoderId);
	ownForm->addRow(QStringLiteral("Encoder"), encoderCombo);
	videoBitrate = new QSpinBox(ownBox);
	videoBitrate->setRange(500, 51000);
	videoBitrate->setSingleStep(500);
	videoBitrate->setSuffix(QStringLiteral(" kbps"));
	videoBitrate->setValue(d.enc.videoBitrate);
	ownForm->addRow(QStringLiteral("Video bitrate"), videoBitrate);
	resCombo = new QComboBox(ownBox);
	for (const auto &r : kResolutions)
		resCombo->addItem(QString::fromUtf8(r.label), QSize(r.w, r.h));
	resCombo->setCurrentIndex(qMax(0, resCombo->findData(QSize(d.enc.width, d.enc.height))));
	ownForm->addRow(QStringLiteral("Resolution"), resCombo);
	// Audio defaults to whatever OBS's own stream uses. AAC stereo tops out
	// at 320 kbps in OBS's encoders.
	audioBitrate = new QComboBox(ownBox);
	audioBitrate->addItem(QStringLiteral("Match OBS (%1 kbps)").arg(oa.bitrate), 0);
	for (int k : {128, 160, 192, 224, 256, 320})
		audioBitrate->addItem(QStringLiteral("%1 kbps").arg(k), k);
	{
		int i = audioBitrate->findData(d.enc.audioBitrate);
		if (i < 0 && d.enc.audioBitrate > 0) { // a custom value from an older config
			audioBitrate->addItem(QStringLiteral("%1 kbps").arg(d.enc.audioBitrate), d.enc.audioBitrate);
			i = audioBitrate->count() - 1;
		}
		audioBitrate->setCurrentIndex(qMax(0, i));
	}
	ownForm->addRow(QStringLiteral("Audio bitrate"), audioBitrate);
	trackCombo = new QComboBox(ownBox);
	trackCombo->addItem(QStringLiteral("Match OBS (Track %1)").arg(oa.track + 1), -1);
	for (int i = 1; i <= 6; i++)
		trackCombo->addItem(QStringLiteral("Track %1").arg(i), i - 1);
	trackCombo->setCurrentIndex(qMax(0, trackCombo->findData(d.enc.audioTrack < 0 ? -1 : qBound(0, d.enc.audioTrack, 5))));
	ownForm->addRow(QStringLiteral("Audio track"), trackCombo);
	auto *audioNote = new QLabel(QStringLiteral("Audio is encoded with %1, the same AAC encoder OBS uses.")
					     .arg(encoderLabel(oa.encoderId)),
				     ownBox);
	audioNote->setWordWrap(true);
	audioNote->setStyleSheet(hintCss);
	ownForm->addRow(audioNote);
	auto *ownNote = new QLabel(QStringLiteral("A separate encoder adds GPU/CPU load for the whole stream. Use it when "
						  "this platform needs a different bitrate or resolution."),
				   ownBox);
	ownNote->setWordWrap(true);
	ownNote->setStyleSheet(hintCss);
	ownForm->addRow(ownNote);
	encLayout->addWidget(ownBox);
	root->addWidget(encBox);

	limitsLabel = new QLabel(this);
	limitsLabel->setWordWrap(true);
	limitsLabel->setVisible(false);
	root->addWidget(limitsLabel);
	limits = parseServiceLimits(obsServicesJson());
	obsAudioKbps = oa.bitrate;
	emberColor = theme.ember;
	noteColor = theme.secondary;

	auto *hotkeyNote = new QLabel(QStringLiteral("Hotkey: OBS > Settings > Hotkeys > \"Red Warden Multistream: toggle %1\". "
						     "A Stream Deck Hotkey key bound to the same keys works too.")
					      .arg(d.name.isEmpty() ? QStringLiteral("<name>") : d.name),
				      this);
	hotkeyNote->setWordWrap(true);
	hotkeyNote->setStyleSheet(hintCss);
	root->addWidget(hotkeyNote);

	errorLabel = new QLabel(this);
	errorLabel->setStyleSheet(QStringLiteral("color:%1;").arg(ui::Theme::css(theme.error)));
	errorLabel->setWordWrap(true);
	root->addWidget(errorLabel);

	// --- Buttons ---------------------------------------------------------
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
	okButton = buttons->button(QDialogButtonBox::Save);
	if (!isNew) {
		auto *remove = buttons->addButton(QStringLiteral("Remove"), QDialogButtonBox::DestructiveRole);
		connect(remove, &QPushButton::clicked, this, [this] {
			QPointer<QMessageBox> box(new QMessageBox(
				QMessageBox::Question, QStringLiteral("Remove destination"),
				QStringLiteral("Remove \"%1\" and delete its saved stream key?").arg(nameEdit->text()),
				QMessageBox::Yes | QMessageBox::No, this));
			const int rc = execNested(box);
			const bool yes = rc >= 0 && box && box->standardButton(box->clickedButton()) == QMessageBox::Yes;
			delete box.data();
			if (yes) {
				removeFlag = true;
				accept();
			}
		});
	}
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	root->addWidget(buttons);

	connect(platformCombo, &QComboBox::currentIndexChanged, this, &EditDialog::onPlatformChanged);
	connect(sharedRadio, &QRadioButton::toggled, this, &EditDialog::onEncoderModeChanged);
	connect(showKey, &QCheckBox::toggled, this,
		[this](bool on) { keyEdit->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password); });
	connect(nameEdit, &QLineEdit::textChanged, this, &EditDialog::validate);
	connect(serverEdit, &QLineEdit::textChanged, this, &EditDialog::validate);
	connect(keyEdit, &QLineEdit::textChanged, this, &EditDialog::validate);

	// New destinations need a key before they can be saved.
	setProperty("needsKey", isNew || !hasSavedKey);

	if (isNew && d.server.isEmpty()) {
		const auto &p = platformById(platformCombo->currentData().toString());
		serverEdit->setText(p.defaultServer);
		if (nameEdit->text().isEmpty())
			nameEdit->setText(p.label);
	}
	hintLabel->setText(platformById(platformCombo->currentData().toString()).hint);
	connect(videoBitrate, &QSpinBox::valueChanged, this, &EditDialog::updateLimits);
	connect(audioBitrate, &QComboBox::currentIndexChanged, this, &EditDialog::updateLimits);
	connect(resCombo, &QComboBox::currentIndexChanged, this, &EditDialog::updateLimits);
	onEncoderModeChanged();
	validate();
}

void EditDialog::updateLimits()
{
	const QString pid = platformCombo->currentData().toString();
	auto it = limits.find(pid);
	std::vector<LimitIssue> issues;
	if (it != limits.end()) {
		StreamFacts f;
		f.shared = sharedRadio->isChecked();
		obs_video_info ovi = {};
		const bool haveVideo = obs_get_video_info(&ovi);
		if (haveVideo && ovi.fps_den > 0)
			f.fps = (double)ovi.fps_num / (double)ovi.fps_den;
		if (f.shared) {
			f.videoKbps = obsStreamVideoBitrate();
			f.audioKbps = obsAudioKbps;
			f.keyintSec = obsStreamKeyintSec();
			f.height = haveVideo ? (int)ovi.output_height : 0;
		} else {
			f.videoKbps = videoBitrate->value();
			const int a = audioBitrate->currentData().toInt();
			f.audioKbps = a > 0 ? a : obsAudioKbps;
			f.keyintSec = 2;
			const int h = resCombo->currentData().toSize().height();
			f.height = h > 0 ? h : (haveVideo ? (int)ovi.output_height : 0);
		}
		issues = checkLimits(pid, platformById(pid).label, it->second, f);
	}
	QStringList lines;
	bool serious = false;
	for (const auto &i : issues) {
		lines << i.text;
		serious |= i.serious;
	}
	limitsLabel->setText(lines.join(QLatin1Char('\n')));
	limitsLabel->setStyleSheet(QStringLiteral("color:%1; font-size:11px;").arg(ui::Theme::css(serious ? emberColor : noteColor)));
	limitsLabel->setVisible(!lines.isEmpty());
}

void EditDialog::onPlatformChanged()
{
	const auto &p = platformById(platformCombo->currentData().toString());
	const auto &prev = platformById(base.platform);

	// Only overwrite fields the user has not customised.
	const QString server = serverEdit->text().trimmed();
	if (server.isEmpty() || server == prev.defaultServer)
		serverEdit->setText(p.defaultServer);
	const QString name = nameEdit->text().trimmed();
	if (name.isEmpty() || name == prev.label)
		nameEdit->setText(p.label);

	hintLabel->setText(p.hint);
	base.platform = p.id;
	validate();
	updateLimits();
}

void EditDialog::onEncoderModeChanged()
{
	ownBox->setEnabled(ownRadio->isChecked());
	updateLimits();
}

void EditDialog::validate()
{
	QString err;
	const QString server = serverEdit->text().trimmed();
	if (nameEdit->text().trimmed().isEmpty())
		err = QStringLiteral("Give this destination a name.");
	else if (server.isEmpty())
		err = QStringLiteral("Paste the server URL from the platform's streaming dashboard.");
	else if (!server.startsWith("rtmp://", Qt::CaseInsensitive) && !server.startsWith("rtmps://", Qt::CaseInsensitive))
		err = QStringLiteral("The server URL should start with rtmp:// or rtmps://");
	else if (QUrl(server).host().isEmpty())
		err = QStringLiteral("The server URL is missing a host name (rtmp://host/app).");
	// OBS writes the server URL into its log, so a key or password in it would
	// leak there. Keys belong in the key field (kept in Credential Manager).
	else if (!QUrl(server).userInfo().isEmpty())
		err = QStringLiteral("Remove the username/password from the server URL. Only the server goes here.");
	else if (const QString k = keyEdit->text().trimmed().isEmpty() ? creds::loadKey(base.id)
								      : keyEdit->text().trimmed();
		 serverUrlContainsKey(server, k))
		err = QStringLiteral("Your stream key is in the server URL. Put only the server here and the key below.");
	else if (property("needsKey").toBool() && keyEdit->text().trimmed().isEmpty())
		err = QStringLiteral("Paste your stream key.");
	errorLabel->setText(err);
	okButton->setEnabled(err.isEmpty());
	const bool twitch = platformCombo->currentData().toString() == QLatin1String("twitch");
	bandwidthButton->setVisible(twitch);
	bandwidthButton->setEnabled(twitch && err.isEmpty() &&
				    (!keyEdit->text().trimmed().isEmpty() || creds::hasKey(base.id)));
}

void EditDialog::reject()
{
	if (nested_)
		nested_->reject(); // the dialog on top of this one ends first (OBS exit, or the user closing this one)
	QDialog::reject();
}

int EditDialog::execNested(QDialog *dlg)
{
	if (!dlg)
		return -1;
	QPointer<QDialog> me(dlg);
	nested_ = me;
	dlg->exec();
	if (!me)
		return -1; // destroyed under us
	nested_.clear();
	return dlg->result();
}

Destination EditDialog::destination() const
{
	Destination d = base;
	d.name = nameEdit->text().trimmed();
	d.platform = platformCombo->currentData().toString();
	d.server = serverEdit->text().trimmed();
	d.enc.shared = sharedRadio->isChecked();
	d.enc.encoderId = encoderCombo->currentData().toString();
	d.enc.videoBitrate = videoBitrate->value();
	d.enc.audioBitrate = audioBitrate->currentData().toInt();
	d.enc.audioTrack = trackCombo->currentData().toInt();
	const QSize r = resCombo->currentData().toSize();
	d.enc.width = r.width();
	d.enc.height = r.height();
	return d;
}

QString EditDialog::newKey() const
{
	return keyEdit->text().trimmed();
}

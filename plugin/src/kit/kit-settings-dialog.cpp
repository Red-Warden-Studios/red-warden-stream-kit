/*
Red Warden Stream Kit
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "kit-settings-dialog.hpp"

#include "dock-ui.hpp"
#include "update-manifest.hpp"

#include <plugin-support.h>

#include <QCheckBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <utility>

namespace {
const char *kTabKeys[] = {"tools", "updates", "about"};

QColor mixc(const QColor &a, const QColor &b, double t)
{
	const float f = (float)t;
	return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * f, a.greenF() + (b.greenF() - a.greenF()) * f,
				a.blueF() + (b.blueF() - a.blueF()) * f);
}

// One tool row: a card with the name, one line about it and the switch on the right.
QFrame *toolRow(QWidget *parent, const QString &name, const QString &about, QCheckBox *box)
{
	auto *f = new QFrame(parent);
	f->setObjectName(QStringLiteral("kitToolRow"));
	auto *h = new QHBoxLayout(f);
	h->setContentsMargins(12, 10, 12, 10);
	h->setSpacing(12);
	auto *texts = new QVBoxLayout();
	texts->setSpacing(1);
	auto *n = new QLabel(name, f);
	QFont bold = n->font();
	bold.setBold(true);
	n->setFont(bold);
	auto *a = new QLabel(about, f);
	a->setObjectName(QStringLiteral("kitToolAbout"));
	a->setWordWrap(true);
	texts->addWidget(n);
	texts->addWidget(a);
	h->addLayout(texts, 1);
	box->setParent(f);
	box->setAccessibleName(name);
	h->addWidget(box, 0, Qt::AlignVCenter);
	return f;
}
} // namespace

KitSettingsDialog::KitSettingsDialog(const kitsettings::Settings &settings, bool checkUpdates,
				     std::function<bool()> toolOffBlocked, QWidget *parent)
	: QDialog(parent), initial_(settings), toolOffBlocked_(std::move(toolOffBlocked)), checkUpdatesInitial_(checkUpdates)
{
	setWindowTitle(QStringLiteral("Red Warden Stream Kit - Settings"));
	setMinimumWidth(400);
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(12, 12, 12, 12);
	root->setSpacing(10);
	tabs_ = new QTabWidget(this);
	tabs_->addTab(buildToolsTab(), QStringLiteral("Tools"));
	tabs_->addTab(buildUpdatesTab(), QStringLiteral("Updates"));
	tabs_->addTab(buildAboutTab(), QStringLiteral("About"));
	root->addWidget(tabs_, 1);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
	connect(buttons, &QDialogButtonBox::accepted, this, &KitSettingsDialog::trySave);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	if (QPushButton *save = buttons->button(QDialogButtonBox::Save)) {
		const ui::Theme t = ui::Theme::from(palette());
		save->setStyleSheet(QStringLiteral("QPushButton { background:%1; color:#ffffff; border:0; border-radius:6px;"
						   " padding:8px 16px; font-weight:700; }"
						   "QPushButton:hover { background:%2; }")
					    .arg(ui::Theme::css(t.geranium), ui::Theme::css(mixc(t.geranium, Qt::white, 0.12))));
		save->setDefault(true);
	}
	root->addWidget(buttons);

	const ui::Theme t = ui::Theme::from(palette());
	setStyleSheet(QStringLiteral("#kitToolRow { background:%1; border-radius:8px; }"
				     "#kitToolAbout { color:%2; font-size:12px; }"
				     "#kitNote { color:%2; }")
			      .arg(ui::Theme::css(mixc(t.window, t.text, t.dark ? 0.08 : 0.05)), ui::Theme::css(t.secondary)));
}

QWidget *KitSettingsDialog::buildToolsTab()
{
	auto *page = new QWidget(this);
	auto *v = new QVBoxLayout(page);
	v->setContentsMargins(12, 14, 12, 12);
	v->setSpacing(8);
	auto *intro = new QLabel(QStringLiteral("Turn off a tool you don't use. It disappears from the dock and does no "
						"work at all. Its settings are kept."),
				 page);
	intro->setObjectName(QStringLiteral("kitNote"));
	intro->setWordWrap(true);
	v->addWidget(intro);
	auto *restart = new QLabel(QStringLiteral("Takes effect the next time OBS starts."), page);
	restart->setObjectName(QStringLiteral("kitNote"));
	restart->setWordWrap(true);
	v->addWidget(restart);

	ms_ = new QCheckBox(page);
	ms_->setChecked(initial_.multistream);
	pf_ = new QCheckBox(page);
	pf_->setChecked(initial_.preflight);
	inflight_ = new QCheckBox(page);
	inflight_->setChecked(initial_.inflight);
	v->addWidget(toolRow(page, QStringLiteral("Multistream"), QStringLiteral("Stream to more places at once"), ms_));
	v->addWidget(toolRow(page, QStringLiteral("Pre-Flight"), QStringLiteral("Checks before you go live"), pf_));
	v->addWidget(toolRow(page, QStringLiteral("In-Flight"), QStringLiteral("Watches while you're live, plus Levels"),
			     inflight_));
	for (QCheckBox *b : {ms_, pf_, inflight_})
		connect(b, &QCheckBox::toggled, this, [this, b](bool on) { toolToggled(b, on); });

	toolsNote_ = new QLabel(page);
	toolsNote_->setObjectName(QStringLiteral("kitNote"));
	toolsNote_->setWordWrap(true);
	toolsNote_->setVisible(false);
	v->addWidget(toolsNote_);

	auto *dockTitle = new QLabel(QStringLiteral("Dock"), page);
	QFont bold = dockTitle->font();
	bold.setBold(true);
	dockTitle->setFont(bold);
	v->addSpacing(6);
	v->addWidget(dockTitle);
	autoSwitch_ = new QCheckBox(QStringLiteral("Switch to In-Flight when I go live"), page);
	autoSwitch_->setChecked(initial_.autoSwitchOnLive);
	v->addWidget(autoSwitch_);
	v->addStretch(1);
	return page;
}

// Switching a tool off is refused while OBS is streaming, a Multistream output is active or a Kit session is
// active: stopping a tool's work in the middle of a stream is exactly what the Kit must not do. (Switching one on
// is always fine.) The same check runs again on Save (the state may have changed while the dialog was open).
void KitSettingsDialog::toolToggled(QCheckBox *box, bool on)
{
	if (on) {
		toolsNote_->setVisible(false);
		return;
	}
	if (toolOffBlocked_ && toolOffBlocked_()) {
		const QSignalBlocker block(box);
		box->setChecked(true);
		toolsNote_->setText(QStringLiteral("You can switch a tool off after the stream ends."));
		toolsNote_->setVisible(true);
	}
}

void KitSettingsDialog::trySave()
{
	if (toolOffBlocked_ && toolOffBlocked_()) {
		bool refused = false;
		const std::pair<QCheckBox *, bool> tools[] = {{ms_, initial_.multistream}, {pf_, initial_.preflight}, {inflight_, initial_.inflight}};
		for (const auto &t : tools) {
			if (t.second && !t.first->isChecked()) {
				const QSignalBlocker block(t.first);
				t.first->setChecked(true);
				refused = true;
			}
		}
		if (refused) {
			tabs_->setCurrentIndex(0);
			toolsNote_->setText(QStringLiteral("You can switch a tool off after the stream ends."));
			toolsNote_->setVisible(true);
			return; // stays open: nothing was saved
		}
	}
	accept();
}

QWidget *KitSettingsDialog::buildUpdatesTab()
{
	auto *page = new QWidget(this);
	auto *v = new QVBoxLayout(page);
	v->setContentsMargins(12, 14, 12, 12);
	v->setSpacing(10);
	updates_ = new QCheckBox(QStringLiteral("Tell me when an update is available"), page);
	updates_->setChecked(checkUpdatesInitial_);
	v->addWidget(updates_);
	auto *about = new QLabel(QStringLiteral("Once per OBS start the Kit looks for a newer version and shows a one-line "
						"notice with a download link. It never downloads or installs anything by "
						"itself, and sends nothing but a plain request that names the version."),
				 page);
	about->setObjectName(QStringLiteral("kitNote"));
	about->setWordWrap(true);
	v->addWidget(about);
	auto *ver = new QLabel(QStringLiteral("Installed: Red Warden Stream Kit %1").arg(QString::fromLatin1(updatecheck::kKitVersion)), page);
	ver->setObjectName(QStringLiteral("kitNote"));
	v->addWidget(ver);
	v->addStretch(1);
	return page;
}

QWidget *KitSettingsDialog::buildAboutTab()
{
	auto *page = new QWidget(this);
	auto *v = new QVBoxLayout(page);
	v->setContentsMargins(12, 14, 12, 12);
	v->setSpacing(8);
	auto *title = new QLabel(QStringLiteral("Red Warden Stream Kit"), page);
	QFont big = title->font();
	big.setBold(true);
	big.setPointSizeF(big.pointSizeF() + 3);
	title->setFont(big);
	v->addWidget(title);
	auto *ver = new QLabel(QStringLiteral("Version %1").arg(QString::fromLatin1(PLUGIN_VERSION)), page);
	ver->setObjectName(QStringLiteral("kitNote"));
	v->addWidget(ver);
	auto *what = new QLabel(QStringLiteral("Multistream, Pre-Flight and In-Flight for OBS, in one dock. By Red Warden Studios."), page);
	what->setWordWrap(true);
	v->addWidget(what);
	auto *link = new QLabel(QStringLiteral("<a href=\"https://redwardenstudios.com/division/bastion/stream-kit/\">"
					       "redwardenstudios.com/division/bastion/stream-kit</a>"),
				page);
	link->setTextFormat(Qt::RichText);
	link->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
	connect(link, &QLabel::linkActivated, this, [](const QString &url) { QDesktopServices::openUrl(QUrl(url)); });
	v->addWidget(link);
	v->addStretch(1);
	return page;
}

kitsettings::Settings KitSettingsDialog::settings() const
{
	kitsettings::Settings s = initial_;
	s.multistream = ms_->isChecked();
	s.preflight = pf_->isChecked();
	s.inflight = inflight_->isChecked();
	s.autoSwitchOnLive = autoSwitch_->isChecked();
	return s;
}

bool KitSettingsDialog::checkUpdates() const
{
	return updates_->isChecked();
}

int KitSettingsDialog::tabCount() const
{
	return tabs_->count();
}

void KitSettingsDialog::showTab(int index)
{
	tabs_->setCurrentIndex(index);
}

QString KitSettingsDialog::tabKey(int index) const
{
	return (index >= 0 && index < 3) ? QString::fromLatin1(kTabKeys[index]) : QString();
}

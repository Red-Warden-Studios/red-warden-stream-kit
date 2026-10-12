/*
Red Warden Multistream
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "dock-ui.hpp"
#include "platforms.hpp"

#include <QEnterEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QResizeEvent>
#include <QToolButton>
#include <QVBoxLayout>

namespace ui {

namespace {
QColor mix(const QColor &a, const QColor &b, double t)
{
	const float f = (float)t;
	return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * f, a.greenF() + (b.greenF() - a.greenF()) * f,
				a.blueF() + (b.blueF() - a.blueF()) * f);
}

QColor statusColor(const Theme &t, Status s)
{
	switch (s) {
	case Status::Live:
		return t.geranium;
	case Status::Busy:
		return t.ember;
	case Status::Problem:
		return t.error;
	case Status::Ready:
		return mix(t.window, t.text, 0.38);
	case Status::Off:
	default:
		return mix(t.window, t.text, t.dark ? 0.16 : 0.13);
	}
}

constexpr int kIndent = 34; // badge width + spacing: second-line text aligns with the name
} // namespace

Theme Theme::from(const QPalette &p)
{
	Theme t;
	t.window = p.color(QPalette::Window);
	t.text = p.color(QPalette::WindowText);
	t.dark = t.window.lightnessF() < 0.5;
	t.surface = mix(t.window, t.text, t.dark ? 0.05 : 0.04);
	t.hover = mix(t.window, t.text, t.dark ? 0.08 : 0.06);
	t.border = mix(t.window, t.text, 0.14);
	t.secondary = mix(t.window, t.text, 0.68);
	t.muted = mix(t.window, t.text, 0.48);
	if (!t.dark) { // keep signal colours readable as text on light themes
		t.ember = QColor(0xA8, 0x5F, 0x00);
		t.error = QColor(0xC6, 0x28, 0x28);
	}
	return t;
}

QString Theme::css(const QColor &c)
{
	return QStringLiteral("rgb(%1,%2,%3)").arg(c.red()).arg(c.green()).arg(c.blue());
}

// ---- ToggleSwitch -----------------------------------------------------------

ToggleSwitch::ToggleSwitch(QWidget *parent) : QAbstractButton(parent)
{
	setCheckable(true);
	setCursor(Qt::PointingHandCursor);
	setFocusPolicy(Qt::StrongFocus);
	setFixedSize(sizeHint());
}

QSize ToggleSwitch::sizeHint() const
{
	return {36, 20};
}

void ToggleSwitch::paintEvent(QPaintEvent *)
{
	const Theme t = Theme::from(palette());
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	const QRectF track = QRectF(rect()).adjusted(1, 2, -1, -2);
	QColor fill = isChecked() ? t.geranium : mix(t.window, t.text, t.dark ? 0.28 : 0.22);
	if (!isEnabled())
		fill.setAlphaF(0.45f);
	p.setPen(Qt::NoPen);
	p.setBrush(fill);
	p.drawRoundedRect(track, track.height() / 2, track.height() / 2);
	if (hasFocus()) {
		p.setPen(QPen(palette().color(QPalette::Highlight), 1.5));
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 1.5, -0.5, -1.5), track.height() / 2 + 1, track.height() / 2 + 1);
	}
	const qreal d = track.height() - 4;
	const qreal x = isChecked() ? track.right() - d - 2 : track.left() + 2;
	p.setPen(Qt::NoPen);
	p.setBrush(isChecked() ? QColor(Qt::white) : mix(t.window, t.text, 0.85));
	p.drawEllipse(QRectF(x, track.top() + 2, d, d));
}

// ---- BeaconStrip --------------------------------------------------------------

BeaconStrip::BeaconStrip(QWidget *parent) : QWidget(parent)
{
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void BeaconStrip::setSegments(std::vector<Segment> segs)
{
	segments = std::move(segs);
	QStringList lines;
	for (const auto &s : segments) {
		const char *word = s.status == Status::Live      ? "live"
				   : s.status == Status::Busy    ? "connecting"
				   : s.status == Status::Problem ? "needs attention"
				   : s.status == Status::Ready   ? "on"
								 : "off";
		lines << QStringLiteral("%1: %2").arg(s.name, QString::fromLatin1(word));
	}
	setToolTip(lines.join('\n'));
	setVisible(!segments.empty());
	update();
}

QSize BeaconStrip::sizeHint() const
{
	return {120, 6};
}

QSize BeaconStrip::minimumSizeHint() const
{
	return {24, 6};
}

void BeaconStrip::paintEvent(QPaintEvent *)
{
	if (segments.empty())
		return;
	const Theme t = Theme::from(palette());
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(Qt::NoPen);
	const qreal gap = 3;
	const qreal n = (qreal)segments.size();
	const qreal w = (width() - gap * (n - 1)) / n;
	for (size_t i = 0; i < segments.size(); i++) {
		const QRectF r(i * (w + gap), 0, w, height());
		const QColor c = statusColor(t, segments[i].status);
		if (segments[i].status == Status::Problem) {
			// Hollow, so "needs attention" never reads as "live" by colour alone.
			p.setPen(QPen(c, 1.5));
			p.setBrush(Qt::NoBrush);
			p.drawRoundedRect(r.adjusted(0.75, 0.75, -0.75, -0.75), 3, 3);
			p.setPen(Qt::NoPen);
		} else {
			p.setBrush(c);
			p.drawRoundedRect(r, 3, 3);
		}
	}
}

// ---- DestinationRow -----------------------------------------------------------

DestinationRow::DestinationRow(QWidget *parent) : QFrame(parent)
{
	setObjectName(QStringLiteral("msRow"));
	setAttribute(Qt::WA_StyledBackground, true);

	auto *v = new QVBoxLayout(this);
	v->setContentsMargins(9, 8, 6, 8);
	v->setSpacing(3);

	auto *top = new QHBoxLayout();
	top->setSpacing(6);
	badge = new QLabel(this);
	badge->setAlignment(Qt::AlignCenter);
	badge->setFixedSize(kIndent - 8, 17);
	top->addWidget(badge);
	name = new QLabel(this);
	name->setTextFormat(Qt::PlainText);
	// Long names shorten with an ellipsis instead of pushing the switch away.
	name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	name->setMinimumWidth(24);
	top->addWidget(name, 1);
	retry = new QToolButton(this);
	retry->setText(QStringLiteral("Retry"));
	retry->setToolTip(QStringLiteral("Try to connect now (still within the connection limit)"));
	retry->setCursor(Qt::PointingHandCursor);
	retry->setVisible(false);
	connect(retry, &QToolButton::clicked, this, &DestinationRow::retryClicked);
	top->addWidget(retry);
	edit = new QToolButton(this);
	edit->setText(QStringLiteral("Edit"));
	edit->setCursor(Qt::PointingHandCursor);
	connect(edit, &QToolButton::clicked, this, &DestinationRow::editClicked);
	top->addWidget(edit);
	toggle = new ToggleSwitch(this);
	connect(toggle, &ToggleSwitch::clicked, this, &DestinationRow::toggled);
	top->addWidget(toggle);
	v->addLayout(top);

	auto *status = new QHBoxLayout();
	status->setContentsMargins(kIndent, 0, 0, 0);
	status->setSpacing(6);
	pill = new QLabel(this);
	pill->setTextFormat(Qt::PlainText);
	status->addWidget(pill, 0, Qt::AlignTop);
	// Next to LIVE, small and amber: only while the Network light judges this destination's frames bad.
	dropPill = new QLabel(this);
	dropPill->setTextFormat(Qt::PlainText);
	dropPill->setVisible(false);
	status->addWidget(dropPill, 0, Qt::AlignTop);
	detail = new QLabel(this);
	detail->setTextFormat(Qt::PlainText);
	detail->setWordWrap(true);
	status->addWidget(detail, 1);
	v->addLayout(status);

	meta = new QLabel(this);
	meta->setTextFormat(Qt::PlainText);
	meta->setWordWrap(true);
	meta->setContentsMargins(kIndent, 0, 0, 0);
	v->addWidget(meta);

	tip = new QLabel(this);
	tip->setTextFormat(Qt::PlainText);
	tip->setWordWrap(true);
	tip->setContentsMargins(kIndent, 2, 0, 0);
	tip->setVisible(false);
	v->addWidget(tip);

	theme = Theme::from(palette());
}

void DestinationRow::applyTheme(const Theme &t)
{
	theme = t;
	restyle();
}

void DestinationRow::setView(const RowView &rv)
{
	view = rv;
	elideName();
	name->setToolTip(rv.name);
	badge->setText(rv.badge);
	badge->setToolTip(rv.platformLabel);
	if (toggle->isChecked() != rv.enabled)
		toggle->setChecked(rv.enabled);
	toggle->setAccessibleName(QStringLiteral("%1 on or off").arg(rv.name));
	toggle->setToolTip(rv.enabled ? QStringLiteral("On: goes live whenever OBS is streaming. Click to turn off.")
				      : QStringLiteral("Off: skipped. Click to turn on."));
	pill->setText(rv.statusText);
	dropPill->setText(rv.dropBadge);
	dropPill->setVisible(!rv.dropBadge.isEmpty());
	dropPill->setToolTip(QStringLiteral("In-Flight's Network light: this destination is dropping frames (last 30 s)."));
	detail->setText(rv.statusDetail);
	detail->setVisible(!rv.statusDetail.isEmpty());
	meta->setText(rv.meta);
	tip->setText(rv.tip);
	tip->setVisible(!rv.tip.isEmpty());
	retry->setVisible(rv.showRetry);
	restyle();
}

void DestinationRow::elideName()
{
	const int w = name->width() > 10 ? name->width() : 200;
	name->setText(name->fontMetrics().elidedText(view.name, Qt::ElideRight, w));
}

void DestinationRow::resizeEvent(QResizeEvent *e)
{
	QFrame::resizeEvent(e);
	elideName();
}

void DestinationRow::enterEvent(QEnterEvent *e)
{
	hovered = true;
	restyle();
	QFrame::enterEvent(e);
}

void DestinationRow::leaveEvent(QEvent *e)
{
	hovered = false;
	restyle();
	QFrame::leaveEvent(e);
}

void DestinationRow::restyle()
{
	const Theme &t = theme;
	const bool lit = view.status == Status::Live || view.status == Status::Busy || view.status == Status::Problem;
	const QColor edge = lit ? statusColor(t, view.status) : QColor(0, 0, 0, 0);
	setStyleSheet(QStringLiteral("#msRow { background:%1; border:none; border-left:3px solid %2;"
				     " border-bottom:1px solid %3; }")
			      .arg(hovered ? Theme::css(t.hover) : QStringLiteral("transparent"),
				   edge.alpha() ? Theme::css(edge) : QStringLiteral("transparent"), Theme::css(t.border)));

	badge->setStyleSheet(QStringLiteral("background:%1; color:%2; border-radius:3px; font-weight:700; font-size:10px;")
				     .arg(Theme::css(view.badgeColor), Theme::css(badgeTextColor(view.badgeColor))));
	name->setStyleSheet(QStringLiteral("font-size:13px; font-weight:600; color:%1;")
				    .arg(Theme::css(view.enabled ? t.text : t.secondary)));

	switch (view.status) {
	case Status::Live:
		pill->setStyleSheet(QStringLiteral("background:%1; color:white; border-radius:3px; padding:0px 5px;"
						   " font-size:10px; font-weight:700;")
					    .arg(Theme::css(t.geranium)));
		break;
	case Status::Busy:
		pill->setStyleSheet(QStringLiteral("color:%1; font-size:12px; font-weight:600;").arg(Theme::css(t.ember)));
		break;
	case Status::Problem:
		pill->setStyleSheet(QStringLiteral("color:%1; font-size:12px; font-weight:600;").arg(Theme::css(t.error)));
		break;
	case Status::Ready:
		pill->setStyleSheet(QStringLiteral("color:%1; font-size:12px;").arg(Theme::css(t.text)));
		break;
	case Status::Off:
		pill->setStyleSheet(QStringLiteral("color:%1; font-size:12px;").arg(Theme::css(t.muted)));
		break;
	}
	// Amber with dark ink in both themes (white on ember is unreadable); the text carries the meaning, not the colour.
	dropPill->setStyleSheet(QStringLiteral("background:%1; color:#202020; border-radius:3px; padding:0px 5px;"
					       " font-size:10px; font-weight:700;")
					.arg(Theme::css(t.ember)));
	const QColor detailColor = view.status == Status::Problem ? t.error : t.secondary;
	detail->setStyleSheet(QStringLiteral("color:%1; font-size:12px;").arg(Theme::css(detailColor)));
	meta->setStyleSheet(QStringLiteral("color:%1; font-size:11px;").arg(Theme::css(t.muted)));
	tip->setStyleSheet(QStringLiteral("color:%1; font-size:11px;").arg(Theme::css(view.tipIsNote ? t.secondary : t.ember)));

	const QString btn = QStringLiteral(
		"QToolButton { color:%1; border:1px solid transparent; border-radius:4px; padding:1px 4px; font-size:12px;"
		" background:transparent; }"
		"QToolButton:hover { color:%2; background:%3; }"
		"QToolButton:focus { border:1px solid %4; }");
	const QString focus = Theme::css(palette().color(QPalette::Highlight));
	edit->setStyleSheet(btn.arg(Theme::css(t.secondary), Theme::css(t.text), Theme::css(t.border), focus));
	retry->setStyleSheet(btn.arg(Theme::css(t.ember), Theme::css(t.ember), Theme::css(t.border), focus));
}

} // namespace ui

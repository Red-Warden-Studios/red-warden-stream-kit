/*
Red Warden Multistream
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

// The visual pieces of the dock. Every colour is derived from the active OBS
// theme's palette at paint/refresh time, mixed toward text or background, so
// the dock stays legible on dark and light themes alike. Brand colours are
// used only as signals: geranium = live / on, ember = in progress, error red.

#include <QAbstractButton>
#include <QColor>
#include <QFrame>
#include <QPalette>
#include <QString>
#include <QWidget>

#include <vector>

class QLabel;
class QPushButton;
class QToolButton;

namespace ui {

struct Theme {
	QColor window, surface, text, secondary, muted, border, hover;
	QColor geranium{0xC0, 0x39, 0x2B};
	QColor ember{0xF2, 0xA6, 0x3B};
	QColor error{0xFF, 0x6B, 0x5E};
	bool dark = true;
	static Theme from(const QPalette &p);
	static QString css(const QColor &c); // rgba(...) for stylesheets
};

enum class Status { Off, Ready, Busy, Live, Problem };

// Everything a destination row shows. Built by the dock from live state.
struct RowView {
	QString name;
	QString badge;
	QColor badgeColor;
	QString platformLabel;
	bool enabled = false;
	Status status = Status::Off;
	QString statusText; // e.g. "LIVE" / "Retrying in 0:42"
	QString statusDetail; // e.g. "6.1 Mbps" or the error
	QString dropBadge;    // e.g. "3.1% DROPPED": the Network light (In-Flight) is judging this destination bad
	QString meta;         // e.g. "Shared encoder, 256 kbps audio"
	QString tip;          // optional advice line
	bool tipIsNote = false; // good to know, not a warning: shown quietly
	bool showRetry = false;
};

class ToggleSwitch : public QAbstractButton {
	Q_OBJECT
public:
	explicit ToggleSwitch(QWidget *parent = nullptr);
	QSize sizeHint() const override;

protected:
	void paintEvent(QPaintEvent *) override;
};

// One segment per destination, lit by state: the at-a-glance status.
class BeaconStrip : public QWidget {
	Q_OBJECT
public:
	struct Segment {
		Status status;
		QString name;
	};
	explicit BeaconStrip(QWidget *parent = nullptr);
	void setSegments(std::vector<Segment> segs);
	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

protected:
	void paintEvent(QPaintEvent *) override;

private:
	std::vector<Segment> segments;
};

class DestinationRow : public QFrame {
	Q_OBJECT
public:
	explicit DestinationRow(QWidget *parent = nullptr);
	void setView(const RowView &v);
	void applyTheme(const Theme &t);

signals:
	void toggled(bool on);
	void editClicked();
	void retryClicked();

protected:
	void enterEvent(QEnterEvent *) override;
	void leaveEvent(QEvent *) override;
	void resizeEvent(QResizeEvent *) override;

private:
	void restyle();
	void elideName();

	QLabel *badge;
	QLabel *name;
	ToggleSwitch *toggle;
	QLabel *pill;
	QLabel *dropPill;
	QLabel *detail;
	QLabel *meta;
	QLabel *tip;
	QToolButton *edit;
	QToolButton *retry;
	RowView view;
	Theme theme;
	bool hovered = false;
};

} // namespace ui

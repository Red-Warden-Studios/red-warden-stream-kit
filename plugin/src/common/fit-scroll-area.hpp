// Red Warden Stream Kit - a scroll area that asks for exactly the height of its content. ASCII only.
//
// The tool panels (Multistream's destination list, Pre-Flight's checklist) were written to fill a dock of their own, so
// they hold a QScrollArea. Hosted in the Stream Kit dock - which scrolls as a whole - that inner area would otherwise
// show a default-sized empty block when it has no rows, and a tiny nested scroller when it has many. This one reports
// the content's height for the width it is given (wrapping labels included), so: no rows = no block, many rows = the
// Kit dock scrolls, never a scroller inside a scroller.
//
// It must keep asking, not ask once. A row that grows (LIVE + "x% DROPPED" + "n frames dropped" wrapping onto a second
// line) changed the content's height without the parent layouts ever re-reading this area's height for the dock's real width,
// and the next row's last line was cut off (kit-dock-net-red.png). Two parts:
//  1. the size policy carries the height-for-width flag (setFitWidget), so every parent layout asks heightForWidth(its
//     actual width) instead of sizing the area from sizeHint(), which is computed at QScrollArea's own preferred width;
//  2. a layout request anywhere inside the content (a row's label changed, a badge appeared) and every resize of the content
//     or of the area is followed, one event-loop turn later (when the layouts have settled), by announcing the new height
//     to the WHOLE chain of parent layouts at once. Qt passes a size change up one parent per layout request, and a layout
//     request only travels through widgets that are visible: in a dock that is hidden (a tab behind another, a collapsed
//     page, the offscreen test grab) the chain stopped at the first row and the area kept its old height. A row has its own
//     layout, so the request never reached the content either: every descendant is watched, new ones as they are added.
#pragma once

#include <QCoreApplication>
#include <QEvent>
#include <QLayout>
#include <QScrollArea>
#include <QString>
#include <QTimer>

class FitScrollArea : public QScrollArea {
public:
	using QScrollArea::QScrollArea;

	void setFitWidget(QWidget *w)
	{
		setWidget(w);
		watchTree(w);
		setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		// The height-for-width FLAG matters: QWidgetItem::hasHeightForWidth() asks the size policy, not the virtual
		// below, for a widget without a QLayout of its own. Without the flag the parent layouts never ask for
		// heightForWidth(actual width) and size this area from sizeHint() (computed at QScrollArea's own preferred width),
		// so a row that wraps at the dock's real width was not counted and the next row's last line was cut off.
		QSizePolicy sp(QSizePolicy::Preferred, QSizePolicy::Minimum);
		sp.setHeightForWidth(!oldBehaviourForTest());
		setSizePolicy(sp);
	}
	// True when the area is too short for its content at its current width (the bug this class exists to prevent).
	bool tooShort() const { return widget() && height() < heightForWidth(width()); }
	bool hasHeightForWidth() const override { return widget() != nullptr; }
	int heightForWidth(int w) const override
	{
		QWidget *c = widget();
		if (!c)
			return QScrollArea::heightForWidth(w);
		const int inner = qMax(0, w - 2 * frameWidth());
		const int h = c->layout() ? c->layout()->totalHeightForWidth(inner) : c->sizeHint().height();
		return h + 2 * frameWidth();
	}
	QSize sizeHint() const override
	{
		const int w = QScrollArea::sizeHint().width();
		return QSize(w, heightForWidth(w));
	}
	QSize minimumSizeHint() const override { return QSize(0, sizeHint().height()); }

	// One line of geometry numbers for the logs (what the area asks for versus what it got), for the test hooks.
	QString diagnose() const
	{
		QWidget *c = widget();
		if (!c)
			return QStringLiteral("no content");
		QString kids;
		if (QLayout *l = c->layout())
			for (int i = 0; i < l->count(); ++i)
				if (QWidget *w = l->itemAt(i)->widget())
					kids += QStringLiteral(" [%1x%2 need %3]")
							.arg(w->width())
							.arg(w->height())
							.arg(w->layout() ? w->layout()->totalHeightForWidth(w->width()) : w->heightForWidth(w->width()));
		return QStringLiteral("area=%1x%2 hint.h=%3 hfw(width)=%4 viewport=%5x%6 content=%7x%8 contentHfw(contentWidth)=%9 rows:%10")
			.arg(width())
			.arg(height())
			.arg(sizeHint().height())
			.arg(heightForWidth(width()))
			.arg(viewport()->width())
			.arg(viewport()->height())
			.arg(c->width())
			.arg(c->height())
			.arg(c->layout() ? c->layout()->totalHeightForWidth(c->width()) : -1)
			.arg(kids);
	}

protected:
	bool eventFilter(QObject *o, QEvent *e) override
	{
		const QEvent::Type t = e->type();
		if (o == widget() && t == QEvent::LayoutRequest)
			updateGeometry();
		// (Every event of every widget below the content passes through here: the type is tested first, and only the few that
		// can change a height are looked at.)
		if (!oldBehaviourForTest()) {
			if (t == QEvent::ChildAdded) {
				watchTree(static_cast<QChildEvent *>(e)->child());
				scheduleRefit(true);
			} else if (t == QEvent::LayoutRequest || t == QEvent::ChildRemoved || t == QEvent::Show || t == QEvent::Hide) {
				scheduleRefit(true);
			} else if (t == QEvent::Resize && o == widget()) {
				scheduleRefit(false);
			}
		}
		return QScrollArea::eventFilter(o, e);
	}
	void resizeEvent(QResizeEvent *e) override
	{
		QScrollArea::resizeEvent(e);
		if (!oldBehaviourForTest())
			scheduleRefit(false);
	}

private:
	// Every widget below the content reports its layout requests to this filter.
	void watchTree(QObject *o)
	{
		if (!o || !o->isWidgetType())
			return;
		o->installEventFilter(this); // installing again only moves the filter to the front
		for (QObject *c : o->children())
			watchTree(c);
	}
	// Tells every parent layout, now, that this area's height may have changed (Qt would do it one parent per event).
	void invalidateParents()
	{
		for (QWidget *p = parentWidget(); p && !p->isWindow(); p = p->parentWidget()) {
			if (p->layout())
				p->layout()->invalidate();
			if (p->inherits("QDockWidget"))
				break; // OBS's own dock machinery is not ours to poke
		}
	}
	// Deferred: when the filter sees a layout request or a resize, the children have not been moved yet.
	// contentChanged: something inside asked for a layout, so the height is announced whatever it is now; a plain resize only
	// announces when the area ended up shorter than its content needs.
	void scheduleRefit(bool contentChanged)
	{
		if (!widget())
			return;
		changedSinceRefit_ = changedSinceRefit_ || contentChanged;
		if (refitPending_)
			return;
		refitPending_ = true;
		QTimer::singleShot(0, this, [this] {
			refitPending_ = false;
			const bool announce = changedSinceRefit_ || tooShort();
			changedSinceRefit_ = false;
			if (announce) {
				invalidateParents();
				updateGeometry(); // the parents ask again and give the area the height the content needs
			}
		});
	}
	// Test copy only (an OBS running from a .testbed folder): RWS_KIT_TEST_FIT_OLD_BEHAVIOUR puts both parts above back to
	// how this class was before (no height-for-width flag, no re-check), so the harness can show that its "the list is as
	// tall as its rows need" check fails without the fix.
	static bool oldBehaviourForTest()
	{
		static const bool off = qEnvironmentVariableIsSet("RWS_KIT_TEST_FIT_OLD_BEHAVIOUR") &&
					QCoreApplication::applicationDirPath().contains(QStringLiteral("/.testbed/"), Qt::CaseInsensitive);
		return off;
	}
	bool refitPending_ = false;
	bool changedSinceRefit_ = false;
};

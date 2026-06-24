// Lyra — drag-to-dock controller.  See dockdragcontroller.h.
#include "dockdragcontroller.h"

#include <QApplication>
#include <QCursor>
#include <QDockWidget>
#include <QEvent>
#include <QMainWindow>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPainter>
#include <QStatusBar>
#include <QToolBar>
#include <QToolButton>
#include <QWidget>

namespace lyra::ui {

// ── translucent cyan drop-zone overlay ──────────────────────────────────────
// A mouse-transparent child of the main window, raised above the docks during
// a drag, painting the candidate drop rectangle.  It never receives input —
// the controller drives it from the drag's mouse-move events.
class DropOverlay : public QWidget {
public:
    explicit DropOverlay(QWidget *parent) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setAttribute(Qt::WA_TranslucentBackground, true);
        hide();
    }
    void setHighlight(const QRect &r) {
        if (r == rect_) return;
        rect_ = r;
        update();
    }
    void clearHighlight() {
        if (rect_.isNull()) return;
        rect_ = QRect();
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        if (rect_.isNull()) return;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QColor cyan(0x00, 0xe5, 0xff);
        QColor fill = cyan;
        fill.setAlpha(64);
        p.fillRect(rect_, fill);
        p.setPen(QPen(cyan, 2.0));
        p.drawRect(rect_.adjusted(1, 1, -1, -1));
    }

private:
    QRect rect_;
};

// ── controller ──────────────────────────────────────────────────────────────
DockDragController::DockDragController(
    QMainWindow *win, const QHash<QString, QDockWidget *> *docks,
    QObject *parent)
    : QObject(parent), win_(win), docks_(docks) {}

DockDragController::~DockDragController() = default;

QDockWidget *DockDragController::dockOf(QWidget *w) const {
    for (QWidget *p = w; p; p = p->parentWidget())
        if (auto *d = qobject_cast<QDockWidget *>(p)) return d;
    return nullptr;
}

QRect DockDragController::dockRegionLocal() const {
    QRect r = win_->rect();   // client area, win_-local
    int top = 0, bottom = 0;
    if (auto *mb = win_->menuBar(); mb && mb->isVisible())
        top += mb->height();
    // Top-docked toolbars sit just under the menu bar.
    const auto bars = win_->findChildren<QToolBar *>();
    for (auto *tb : bars) {
        if (!tb || !tb->isVisible() || tb->isFloating()) continue;
        if (tb->mapTo(win_, QPoint(0, 0)).y() <= top + 4)
            top += tb->height();
    }
    if (auto *sb = win_->statusBar(); sb && sb->isVisible())
        bottom += sb->height();
    return r.adjusted(0, top, 0, -bottom);
}

DockDragController::Zone
DockDragController::hitTest(const QPoint &globalPos) const {
    Zone z;
    const QRect region = dockRegionLocal();
    const QPoint p = win_->mapFromGlobal(globalPos);
    if (!region.contains(p))
        return z;   // off the dock region → float

    // Edge bands first so the far edges stay reachable past the panels.
    const int bandW = qMin(48, region.width() / 6);
    const int bandH = qMin(48, region.height() / 6);
    const int w3 = region.width() / 3;
    const int h3 = region.height() / 3;
    if (p.x() - region.left() < bandW) {
        z.type = ZoneType::Edge;
        z.edge = Qt::LeftDockWidgetArea;
        z.highlight = QRect(region.left(), region.top(), w3, region.height());
        return z;
    }
    if (region.right() - p.x() < bandW) {
        z.type = ZoneType::Edge;
        z.edge = Qt::RightDockWidgetArea;
        z.highlight = QRect(region.right() - w3, region.top(), w3, region.height());
        return z;
    }
    if (p.y() - region.top() < bandH) {
        z.type = ZoneType::Edge;
        z.edge = Qt::TopDockWidgetArea;
        z.highlight = QRect(region.left(), region.top(), region.width(), h3);
        return z;
    }
    if (region.bottom() - p.y() < bandH) {
        z.type = ZoneType::Edge;
        z.edge = Qt::BottomDockWidgetArea;
        z.highlight = QRect(region.left(), region.bottom() - h3, region.width(), h3);
        return z;
    }

    // Interior → split/tabify relative to the docked neighbor under the cursor.
    for (QDockWidget *t : *docks_) {
        if (!t || t == dragDock_) continue;
        if (!t->isVisible() || t->isFloating()) continue;
        const QRect tr(t->mapTo(win_, QPoint(0, 0)), t->size());
        if (!tr.contains(p)) continue;
        const int lx = p.x() - tr.left();
        const int ly = p.y() - tr.top();
        z.neighbor = t;
        if (lx < tr.width() / 3) {
            z.type = ZoneType::Split; z.orient = Qt::Horizontal; z.before = true;
            z.highlight = QRect(tr.left(), tr.top(), tr.width() / 2, tr.height());
        } else if (lx > 2 * tr.width() / 3) {
            z.type = ZoneType::Split; z.orient = Qt::Horizontal; z.before = false;
            z.highlight = QRect(tr.center().x(), tr.top(), tr.width() / 2, tr.height());
        } else if (ly < tr.height() / 3) {
            z.type = ZoneType::Split; z.orient = Qt::Vertical; z.before = true;
            z.highlight = QRect(tr.left(), tr.top(), tr.width(), tr.height() / 2);
        } else if (ly > 2 * tr.height() / 3) {
            z.type = ZoneType::Split; z.orient = Qt::Vertical; z.before = false;
            z.highlight = QRect(tr.left(), tr.center().y(), tr.width(), tr.height() / 2);
        } else {
            z.type = ZoneType::Tabify;
            z.highlight = tr;
        }
        return z;
    }
    return z;   // inside the region but over no neighbor → float
}

void DockDragController::commit(const Zone &z) {
    QDockWidget *d = dragDock_;
    if (!d) return;
    switch (z.type) {
    case ZoneType::Edge:
        win_->addDockWidget(z.edge, d);   // re-docks (un-floats) automatically
        break;
    case ZoneType::Tabify:
        if (z.neighbor) win_->tabifyDockWidget(z.neighbor, d);
        break;
    case ZoneType::Split:
        if (z.neighbor) {
            // splitDockWidget(first, second, orient) puts `second` AFTER
            // `first`.  For a "before" drop, split once (dragged after the
            // neighbor) then split again with the neighbor as `second` so it
            // lands after the dragged dock → dragged ends up before it.
            win_->splitDockWidget(z.neighbor, d, z.orient);
            if (z.before) win_->splitDockWidget(d, z.neighbor, z.orient);
        }
        break;
    case ZoneType::None:
        // Dropped off the dock region → leave floating.  A dock that was
        // already floating followed the cursor during the drag; a docked one
        // tears out here, under the cursor.
        if (!dragWasFloating_) {
            d->setFloating(true);
            d->move(QCursor::pos() - QPoint(40, 10));
            d->activateWindow();
        }
        break;
    }
    d->show();
    d->raise();
    emit layoutChanged();
}

void DockDragController::endDrag() {
    if (overlay_) {
        overlay_->hide();
        overlay_->clearHighlight();
    }
    state_ = State::Idle;
    dragDock_ = nullptr;
}

bool DockDragController::eventFilter(QObject *obj, QEvent *event) {
    auto *bar = qobject_cast<QWidget *>(obj);
    if (!bar || bar->objectName() != QStringLiteral("dockTitleBar"))
        return QObject::eventFilter(obj, event);

    switch (event->type()) {
    case QEvent::MouseButtonDblClick: {
        // Standard QDockWidget gesture, lost when we replaced the title bar.
        if (QDockWidget *d = dockOf(bar)) {
            if (d->features() & QDockWidget::DockWidgetFloatable) {
                d->setFloating(!d->isFloating());
                if (d->isFloating()) { d->raise(); d->activateWindow(); }
            }
            return true;
        }
        break;
    }
    case QEvent::MouseButtonPress: {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() != Qt::LeftButton) break;
        QDockWidget *d = dockOf(bar);
        if (!d) break;
        // Locked panel → not draggable (applyPanelLock clears Movable).
        if (!(d->features() & QDockWidget::DockWidgetMovable)) break;
        // Never start a drag on a title-bar button (?/float/close).
        if (qobject_cast<QToolButton *>(bar->childAt(me->position().toPoint())))
            break;
        dragDock_        = d;
        pressGlobal_     = me->globalPosition().toPoint();
        dragWasFloating_ = d->isFloating();
        floatGrabOffset_ = pressGlobal_ - d->frameGeometry().topLeft();
        state_           = State::Armed;
        return true;
    }
    case QEvent::MouseMove: {
        if (state_ == State::Idle || !dragDock_) break;
        auto *me = static_cast<QMouseEvent *>(event);
        const QPoint g = me->globalPosition().toPoint();
        if (state_ == State::Armed) {
            if ((g - pressGlobal_).manhattanLength()
                < QApplication::startDragDistance())
                return true;   // not past threshold yet
            state_ = State::Dragging;
            if (!overlay_) overlay_ = new DropOverlay(win_);
            overlay_->setGeometry(QRect(QPoint(0, 0), win_->size()));
            overlay_->clearHighlight();
            overlay_->show();
            overlay_->raise();
        }
        // An already-floating dock free-follows the cursor (preserves the old
        // floating-drag feel); a docked one stays put and only the overlay
        // tracks (no mid-drag reparent → no lost mouse grab).
        if (dragWasFloating_)
            dragDock_->move(g - floatGrabOffset_);
        const Zone z = hitTest(g);
        if (z.type == ZoneType::None) overlay_->clearHighlight();
        else                          overlay_->setHighlight(z.highlight);
        return true;
    }
    case QEvent::MouseButtonRelease: {
        if (state_ == State::Armed) {   // pressed but never dragged → click
            state_ = State::Idle;
            dragDock_ = nullptr;
            return true;
        }
        if (state_ != State::Dragging) break;
        auto *me = static_cast<QMouseEvent *>(event);
        commit(hitTest(me->globalPosition().toPoint()));
        endDrag();
        return true;
    }
    default:
        break;
    }
    return QObject::eventFilter(obj, event);
}

} // namespace lyra::ui

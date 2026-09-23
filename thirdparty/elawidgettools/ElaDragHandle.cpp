#include "ElaDragHandle.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDrag>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QWidget>

ElaDragHandle::ElaDragHandle(QWidget* widget, QObject* owner)
    : QObject(widget), handle(widget), source(owner)
{
    widget->setProperty("elaDragManaged", true);
    qApp->installEventFilter(this);
}

void ElaDragHandle::setMimeDataFactory(std::function<QMimeData*()> value) { factory = std::move(value); }

bool ElaDragHandle::eventFilter(QObject* watched, QEvent* event)
{
    if (!handle || !source)
        return false;
    if (down && event->type() == QEvent::KeyPress
        && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        down = false;
        movedEnough = false;
        emit cancelled();
        return true;
    }
    if (watched != handle || !handle->isEnabled())
        return false;
    if (event->type() == QEvent::MouseButtonPress) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() != Qt::LeftButton)
            return false;
        start = mouse->globalPosition().toPoint();
        down = true;
        movedEnough = false;
        emit pressed(start);
        return true;
    }
    if (event->type() == QEvent::MouseMove && down) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (!(mouse->buttons() & Qt::LeftButton)) {
            down = false;
            emit cancelled();
            return false;
        }
        const QPoint position = mouse->globalPosition().toPoint();
        movedEnough |= (position - start).manhattanLength() >= QApplication::startDragDistance();
        if (!movedEnough)
            return true;
        if (!factory) {
            emit moved(position);
            return true;
        }
        down = false;
        QPointer<ElaDragHandle> guard(this);
        QMimeData* mime = factory();
        if (!mime)
            return true;
        QPointer<QDrag> drag(new QDrag(source));
        drag->setMimeData(mime);
        emit dragStarted();
        if (!guard || !drag)
            return true;
        const bool accepted = drag->exec(Qt::MoveAction) == Qt::MoveAction;
        if (drag)
            drag->deleteLater();
        if (guard)
            emit dragFinished(accepted);
        return true;
    }
    if (event->type() == QEvent::MouseButtonRelease && down) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() != Qt::LeftButton)
            return false;
        down = false;
        const QPoint position = mouse->globalPosition().toPoint();
        const bool dragged = movedEnough || (position - start).manhattanLength() >= QApplication::startDragDistance();
        movedEnough = false;
        emit released(position, dragged);
        return true;
    }
    if (down && (event->type() == QEvent::Hide || event->type() == QEvent::UngrabMouse)) {
        down = false;
        movedEnough = false;
        emit cancelled();
    }
    return false;
}

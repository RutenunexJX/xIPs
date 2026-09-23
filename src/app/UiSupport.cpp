#include "UiSupport.h"
#include "ElaMenu.h"
#include "ElaScrollBar.h"
#include "ElaText.h"
#include "ElaToolTip.h"
#include <QAbstractItemView>
#include <QApplication>
#include <QHelpEvent>
#include <QPointer>
#include <QScreen>
#include <QTimer>
#include <QWheelEvent>
#include <memory>

namespace xips
{
namespace
{
class WheelRouter final : public QObject
{
    QPointer<ElaScrollBar> horizontal;
    QPointer<ElaScrollBar> vertical;
  public:
    explicit WheelRouter(QAbstractScrollArea *area) : QObject(area),
        horizontal(qobject_cast<ElaScrollBar *>(area->horizontalScrollBar())),
        vertical(qobject_cast<ElaScrollBar *>(area->verticalScrollBar()))
    {
        area->installEventFilter(this);
        area->viewport()->installEventFilter(this);
    }
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::KeyPress || event->type() == QEvent::MouseButtonPress ||
            event->type() == QEvent::Hide || event->type() == QEvent::Resize ||
            event->type() == QEvent::FocusOut)
            for (const auto &bar : {horizontal, vertical})
                if (bar)
                    bar->stopSmoothWheel();
        if (event->type() != QEvent::Wheel)
            return false;
        auto *wheel = static_cast<QWheelEvent *>(event);
        const auto pixels = wheel->pixelDelta();
        if (pixels.isNull())
            return false;
        const auto bar = qAbs(pixels.x()) > qAbs(pixels.y()) || wheel->modifiers().testFlag(Qt::ShiftModifier)
            ? horizontal : vertical;
        if (!bar)
            return false;
        wheel->ignore();
        QApplication::sendEvent(bar.data(), wheel);
        return wheel->isAccepted();
    }
};
class ToolTip final : public QObject
{
    QWidget *owner;
    std::unique_ptr<ElaToolTip> tip;
    ElaText *text;
    QTimer dismiss;
  public:
    explicit ToolTip(QWidget *widget) : QObject(widget), owner(widget),
        tip(std::make_unique<ElaToolTip>()), text(new ElaText(tip.get()))
    {
        tip->setObjectName("xipsToolTip");
        tip->setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint);
        tip->setAttribute(Qt::WA_ShowWithoutActivating);
        tip->setFocusPolicy(Qt::NoFocus);
        text->setTextFormat(Qt::PlainText);
        text->setWordWrap(true);
        text->setTextPixelSize(13);
        tip->setCustomWidget(text);
        owner->installEventFilter(this);
        owner->window()->installEventFilter(this);
        if (auto *area = qobject_cast<QAbstractScrollArea *>(owner))
            area->viewport()->installEventFilter(this);
        dismiss.setSingleShot(true);
        connect(&dismiss, &QTimer::timeout, tip.get(), &QWidget::hide);
    }
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::ToolTip)
        {
            const auto *help = static_cast<QHelpEvent *>(event);
            QString value = owner->toolTip();
            if (const auto *view = qobject_cast<QAbstractItemView *>(owner))
                value = view->indexAt(help->pos()).data(Qt::ToolTipRole).toString();
            if (value.isEmpty())
            {
                tip->hide();
                return true;
            }
            const auto *screen = QGuiApplication::screenAt(help->globalPos());
            const QRect bounds = (screen ? screen : owner->screen())->availableGeometry().adjusted(4, 4, -4, -4);
            text->setMaximumWidth(qMin(480, qMax(80, bounds.width() - 40)));
            text->setText(value);
            tip->adjustSize();
            const QPoint requested = help->globalPos() + QPoint(12, 18);
            tip->move(qBound(bounds.left(), requested.x(), qMax(bounds.left(), bounds.right() - tip->width())),
                      qBound(bounds.top(), requested.y(), qMax(bounds.top(), bounds.bottom() - tip->height())));
            tip->show();
            dismiss.start(8000);
            return true;
        }
        if (event->type() == QEvent::Leave || event->type() == QEvent::Hide ||
            event->type() == QEvent::MouseButtonPress || event->type() == QEvent::KeyPress ||
            event->type() == QEvent::Wheel || event->type() == QEvent::WindowDeactivate ||
            event->type() == QEvent::Resize)
        {
            dismiss.stop();
            tip->hide();
        }
        return false;
    }
};
class MenuOwner final : public QObject
{
    ElaMenu *menu;
  public:
    MenuOwner(ElaMenu *value, QWidget *owner) : QObject(value), menu(value)
    {
        owner->installEventFilter(this);
        owner->window()->installEventFilter(this);
        connect(owner, &QObject::destroyed, menu, &QWidget::close);
    }
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::Hide || event->type() == QEvent::Close)
        {
            menu->finishPopupAnimation();
            menu->close();
        }
        return false;
    }
};
}
void enableSmoothScrolling(QAbstractScrollArea *area)
{
    if (area->property("xipsSmoothScrolling").toBool())
        return;
    area->setProperty("xipsSmoothScrolling", true);
    new WheelRouter(area);
    for (auto *scroll : {area->horizontalScrollBar(), area->verticalScrollBar()})
        if (auto *bar = qobject_cast<ElaScrollBar *>(scroll))
        {
            bar->setIsAnimation(false);
            bar->setSmoothWheelEnabled(true);
            bar->setWheelAnimationDuration(160);
        }
}
void enableToolTip(QWidget *widget)
{
    if (!widget->property("xipsToolTipEnabled").toBool())
    {
        widget->setProperty("xipsToolTipEnabled", true);
        new ToolTip(widget);
    }
}
void prepareMenu(ElaMenu *menu, QWidget *owner)
{
    menu->setNativeMenuBehavior(true);
    new MenuOwner(menu, owner);
}
QAction *executeMenu(ElaMenu &menu, QWidget *owner, const QPoint &position)
{
    prepareMenu(&menu, owner);
    QPointer<QWidget> guard(owner);
    const auto action = menu.exec(position);
    return guard ? action : nullptr;
}
}

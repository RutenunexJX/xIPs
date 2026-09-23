#include "ElaScrollBar.h"

#include <QDebug>
#include <QApplication>
#include <QPainter>
#include <QPointer>
#include <QPropertyAnimation>
#include <QTimer>
#include <QWheelEvent>

#include "ElaMenu.h"
#include "ElaScrollBarStyle.h"
#include "private/ElaScrollBarPrivate.h"
Q_PROPERTY_CREATE_Q_CPP(ElaScrollBar, bool, IsAnimation)
Q_PROPERTY_CREATE_Q_CPP(ElaScrollBar, qreal, SpeedLimit)
ElaScrollBar::ElaScrollBar(QWidget* parent)
    : QScrollBar(parent), d_ptr(new ElaScrollBarPrivate())
{
    Q_D(ElaScrollBar);
    d->q_ptr = this;
    setSingleStep(1);
    setObjectName("ElaScrollBar");
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    d->_pSpeedLimit = 20;
    d->_pTargetMaximum = 0;
    d->_pIsAnimation = false;
    connect(this, &ElaScrollBar::rangeChanged, d, &ElaScrollBarPrivate::onRangeChanged);
    ElaScrollBarStyle* scrollBarStyle = new ElaScrollBarStyle(style());
    scrollBarStyle->setParent(this);
    scrollBarStyle->setScrollBar(this);
    setStyle(scrollBarStyle);
    d->_pWheelValue = 0;
    d->_slideSmoothAnimation = new QPropertyAnimation(this, "value", this);
    d->_slideSmoothAnimation->setEasingCurve(QEasingCurve::OutSine);
    d->_slideSmoothAnimation->setDuration(300);
    connect(d->_slideSmoothAnimation, &QPropertyAnimation::finished, this, [=]() {
        d->_scrollValue = value();
    });
    connect(d, &ElaScrollBarPrivate::pWheelValueChanged, this, [=]() {
        d->_writingWheelValue = true;
        setValue(qRound(d->getWheelValue()));
        d->_writingWheelValue = false;
    });
    connect(this, &QScrollBar::valueChanged, this, [=]() {
        if (d->_smoothWheelEnabled && !d->_writingWheelValue)
            stopSmoothWheel();
    });
    connect(this, &QScrollBar::rangeChanged, this, [=]() {
        if (d->_smoothWheelEnabled)
            stopSmoothWheel();
    });
    connect(this, &QScrollBar::actionTriggered, this, [=]() {
        if (d->_smoothWheelEnabled)
            stopSmoothWheel();
    });

    d->_expandTimer = new QTimer(this);
    connect(d->_expandTimer, &QTimer::timeout, this, [=]() {
        d->_expandTimer->stop();
        d->_isExpand = underMouse();
        scrollBarStyle->startExpandAnimation(d->_isExpand);
    });
}

ElaScrollBar::ElaScrollBar(Qt::Orientation orientation, QWidget* parent)
    : ElaScrollBar(parent)
{
    setOrientation(orientation);
}

ElaScrollBar::ElaScrollBar(QScrollBar* originScrollBar, QAbstractScrollArea* parent)
    : ElaScrollBar(parent)
{
    Q_D(ElaScrollBar);
    if (!originScrollBar || !parent)
    {
        qCritical() << "Invalid origin or parent!";
        return;
    }
    d->_originScrollArea = parent;
    Qt::Orientation orientation = originScrollBar->orientation();
    setOrientation(orientation);
    orientation == Qt::Horizontal ? parent->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff) : parent->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    parent->installEventFilter(this);

    d->_originScrollBar = originScrollBar;
    d->_initAllConfig();

    connect(d->_originScrollBar, &QScrollBar::valueChanged, this, [=](int value) {
        d->_handleScrollBarValueChanged(this, value);
    });
    connect(this, &QScrollBar::valueChanged, this, [=](int value) {
        d->_handleScrollBarValueChanged(d->_originScrollBar, value);
    });
    connect(d->_originScrollBar, &QScrollBar::rangeChanged, this, [=](int min, int max) {
        d->_handleScrollBarRangeChanged(min, max);
    });
}

ElaScrollBar::~ElaScrollBar()
{
    stopSmoothWheel();
    setStyle(nullptr);
}

void ElaScrollBar::setSmoothWheelEnabled(bool enabled)
{
    Q_D(ElaScrollBar);
    stopSmoothWheel();
    d->_smoothWheelEnabled = enabled;
    d->_slideSmoothAnimation->setTargetObject(nullptr);
    d->_slideSmoothAnimation->setPropertyName(enabled ? "pWheelValue" : "value");
    d->_slideSmoothAnimation->setTargetObject(enabled ? static_cast<QObject*>(d) : this);
}

bool ElaScrollBar::smoothWheelEnabled() const
{
    return d_ptr->_smoothWheelEnabled;
}

void ElaScrollBar::setWheelAnimationDuration(int duration)
{
    d_ptr->_slideSmoothAnimation->setDuration(qMax(0, duration));
}

void ElaScrollBar::stopSmoothWheel()
{
    Q_D(ElaScrollBar);
    d->_slideSmoothAnimation->stop();
    d->_scrollValue = value();
}

void ElaScrollBar::smoothWheelEvent(QWheelEvent* event)
{
    Q_D(ElaScrollBar);
    const bool horizontal = orientation() == Qt::Horizontal;
    const QPoint pixels = event->pixelDelta();
    const int pixelDelta = horizontal
        ? (pixels.x() ? pixels.x() : event->modifiers().testFlag(Qt::ShiftModifier) ? pixels.y() : 0)
        : pixels.y();
    if (!pixels.isNull()) {
        // Precision gestures already provide a motion curve; apply them directly.
        stopSmoothWheel();
        const int next = qBound(minimum(), value() - pixelDelta, maximum());
        event->setAccepted(next != value());
        setValue(next);
        return;
    }
    const QPoint angles = event->angleDelta();
    const int delta = horizontal ? (angles.x() ? angles.x() : angles.y()) : angles.y();
    if (!delta) {
        event->ignore();
        return;
    }
    if (event->modifiers().testFlag(Qt::ControlModifier)) {
        stopSmoothWheel();
        QScrollBar::wheelEvent(event);
        return;
    }
    const qreal distance = -delta / 120.0 * qMin(pageStep(), singleStep() * QApplication::wheelScrollLines());
    const bool running = d->_slideSmoothAnimation->state() == QAbstractAnimation::Running;
    if (!running || (d->_scrollValue - value()) * distance < 0)
        d->_scrollValue = value();
    const qreal target = qBound(qreal(minimum()), d->_scrollValue + distance, qreal(maximum()));
    if (qFuzzyCompare(target + 1, value() + 1)) {
        stopSmoothWheel();
        event->ignore();
        return;
    }
    d->_slideSmoothAnimation->stop();
    d->_scrollValue = target;
    d->_slideSmoothAnimation->setStartValue(qreal(value()));
    d->_slideSmoothAnimation->setEndValue(target);
    d->_slideSmoothAnimation->start();
    event->accept();
}

bool ElaScrollBar::event(QEvent* event)
{
    Q_D(ElaScrollBar);
    switch (event->type())
    {
    case QEvent::Hide:
    case QEvent::EnabledChange:
        if (d->_smoothWheelEnabled)
            stopSmoothWheel();
        break;
    case QEvent::Enter:
    {
        d->_expandTimer->stop();
        if (!d->_isExpand)
        {
            d->_expandTimer->start(350);
        }
        break;
    }
    case QEvent::Leave:
    {
        d->_expandTimer->stop();
        if (d->_isExpand)
        {
            d->_expandTimer->start(350);
        }
        break;
    }
    default:
    {
        break;
    }
    }
    return QScrollBar::event(event);
}

bool ElaScrollBar::eventFilter(QObject* watched, QEvent* event)
{
    Q_D(ElaScrollBar);
    switch (event->type())
    {
    case QEvent::Show:
    case QEvent::Resize:
    case QEvent::LayoutRequest:
    {
        d->_handleScrollBarGeometry();
        break;
    }
    default:
    {
        break;
    }
    }
    return QScrollBar::eventFilter(watched, event);
}

void ElaScrollBar::mousePressEvent(QMouseEvent* event)
{
    Q_D(ElaScrollBar);
    d->_slideSmoothAnimation->stop();
    QScrollBar::mousePressEvent(event);
    d->_scrollValue = value();
}

void ElaScrollBar::mouseReleaseEvent(QMouseEvent* event)
{
    Q_D(ElaScrollBar);
    d->_slideSmoothAnimation->stop();
    QScrollBar::mouseReleaseEvent(event);
    d->_scrollValue = value();
}

void ElaScrollBar::mouseMoveEvent(QMouseEvent* event)
{
    Q_D(ElaScrollBar);
    d->_slideSmoothAnimation->stop();
    QScrollBar::mouseMoveEvent(event);
    d->_scrollValue = value();
}

void ElaScrollBar::wheelEvent(QWheelEvent* event)
{
    Q_D(ElaScrollBar);
    if (d->_smoothWheelEnabled) {
        smoothWheelEvent(event);
        return;
    }
    if (!d->_pIsAnimation) {
        QScrollBar::wheelEvent(event);
        return;
    }
    int verticalDelta = event->angleDelta().y();
    if (d->_slideSmoothAnimation->state() == QAbstractAnimation::Stopped)
    {
        d->_scrollValue = value();
    }
    if (verticalDelta != 0)
    {
        if ((value() == minimum() && verticalDelta > 0) || (value() == maximum() && verticalDelta < 0))
        {
            QScrollBar::wheelEvent(event);
            return;
        }
        d->_scroll(event->modifiers(), verticalDelta);
    }
    else
    {
        int horizontalDelta = event->angleDelta().x();
        if ((value() == minimum() && horizontalDelta > 0) || (value() == maximum() && horizontalDelta < 0))
        {
            QScrollBar::wheelEvent(event);
            return;
        }
        d->_scroll(event->modifiers(), horizontalDelta);
    }
    event->accept();
}

void ElaScrollBar::contextMenuEvent(QContextMenuEvent* event)
{
    Q_D(ElaScrollBar);
    bool horiz = this->orientation() == Qt::Horizontal;
    QPointer<ElaMenu> menu = new ElaMenu(this);
    menu->setMenuItemHeight(27);
    // Scroll here
    QAction* actScrollHere = menu->addElaIconAction(ElaIconType::UpDownLeftRight, tr("滚动到此处"));
    menu->addSeparator();
    // Left edge Top
    QAction* actScrollTop = menu->addElaIconAction(horiz ? ElaIconType::ArrowLeftToLine : ElaIconType::ArrowUpToLine, horiz ? tr("左边缘") : tr("顶端"));
    // Right edge Bottom
    QAction* actScrollBottom = menu->addElaIconAction(horiz ? ElaIconType::ArrowRightToLine : ElaIconType::ArrowDownToLine, horiz ? tr("右边缘") : tr("底部"));
    menu->addSeparator();
    // Page left Page up
    QAction* actPageUp = menu->addElaIconAction(horiz ? ElaIconType::AnglesLeft : ElaIconType::AnglesUp, horiz ? tr("向左翻页") : tr("向上翻页"));
    //Page right Page down
    QAction* actPageDn = menu->addElaIconAction(horiz ? ElaIconType::AnglesRight : ElaIconType::AnglesDown, horiz ? tr("向右翻页") : tr("向下翻页"));
    menu->addSeparator();
    //Scroll left Scroll up
    QAction* actScrollUp = menu->addElaIconAction(horiz ? ElaIconType::AngleLeft : ElaIconType::AngleUp, horiz ? tr("向左滚动") : tr("向上滚动"));
    //Scroll right Scroll down
    QAction* actScrollDn = menu->addElaIconAction(horiz ? ElaIconType::AngleRight : ElaIconType::AngleDown, horiz ? tr("向右滚动") : tr("向下滚动"));
    QAction* actionSelected = menu->exec(event->globalPos());
    delete menu;
    if (!actionSelected)
    {
        return;
    }
    if (actionSelected == actScrollHere)
    {
        setValue(d->_pixelPosToRangeValue(horiz ? event->pos().x() : event->pos().y()));
    }
    else if (actionSelected == actScrollTop)
    {
        triggerAction(QAbstractSlider::SliderToMinimum);
    }
    else if (actionSelected == actScrollBottom)
    {
        triggerAction(QAbstractSlider::SliderToMaximum);
    }
    else if (actionSelected == actPageUp)
    {
        triggerAction(QAbstractSlider::SliderPageStepSub);
    }
    else if (actionSelected == actPageDn)
    {
        triggerAction(QAbstractSlider::SliderPageStepAdd);
    }
    else if (actionSelected == actScrollUp)
    {
        triggerAction(QAbstractSlider::SliderSingleStepSub);
    }
    else if (actionSelected == actScrollDn)
    {
        triggerAction(QAbstractSlider::SliderSingleStepAdd);
    }
}

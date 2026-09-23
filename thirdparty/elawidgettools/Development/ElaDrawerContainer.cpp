#include "ElaDrawerContainer.h"

#include "ElaTheme.h"

#include <QPainter>
#include <QPropertyAnimation>
#include <QApplication>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QScopedValueRollback>
ElaDrawerContainer::ElaDrawerContainer(QWidget* parent)
    : QWidget(parent)
{
    _pBorderRadius = 6;
    _pOpacity = 0;
    _pContainerPix = QPixmap();
    setObjectName("ElaDrawerContainer");
    setStyleSheet("#ElaDrawerContainer{background-color:transparent;}");

    _containerWidget = new QWidget(this);
    _containerWidget->setObjectName("ElaDrawerContainerWidget");
    _containerWidget->setStyleSheet("#ElaDrawerContainerWidget{background-color:transparent;}");
    _containerWidget->setVisible(false);

    _containerLayout = new QVBoxLayout(_containerWidget);
    _containerLayout->setContentsMargins(0, 0, 0, 0);
    _containerLayout->setSpacing(0);
    _containerLayout->setSizeConstraint(QLayout::SetNoConstraint);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setMaximumHeight(0);
    _animation = new QPropertyAnimation(this, "pOpacity", this);
    _animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(_animation, &QPropertyAnimation::valueChanged, this, [this] { update(); });
    connect(_animation, &QPropertyAnimation::finished, this, &ElaDrawerContainer::finishAnimation);

    _themeMode = eTheme->getThemeMode();
    connect(eTheme, &ElaTheme::themeModeChanged, this, [=](ElaThemeType::ThemeMode themeMode) {
        _themeMode = themeMode;
        update();
    });
}

ElaDrawerContainer::~ElaDrawerContainer()
{
    qApp->removeEventFilter(this);
    _animation->stop();
    for (const auto& widget : _drawerWidgetList)
        if (widget) disconnect(widget, nullptr, this, nullptr);
}

void ElaDrawerContainer::addWidget(QWidget* widget)
{
    if (!widget || _drawerWidgetList.contains(widget))
    {
        return;
    }
    finishAnimation();
    _containerLayout->addWidget(widget);
    _drawerWidgetList.append(widget);
    connect(widget, &QObject::destroyed, this, [this] {
        _drawerWidgetList.removeIf([](const auto& item) { return item.isNull(); });
        finishAnimation();
        updateGeometry();
    });
    updateGeometry();
}

void ElaDrawerContainer::removeWidget(QWidget* widget)
{
    if (!widget)
    {
        return;
    }
    finishAnimation();
    _containerLayout->removeWidget(widget);
    _drawerWidgetList.removeOne(widget);
    disconnect(widget, nullptr, this, nullptr);
    updateGeometry();
}

bool ElaDrawerContainer::isAnimating() const
{
    return _animation->state() == QAbstractAnimation::Running;
}

qint64 ElaDrawerContainer::snapshotBytes() const
{
    return qint64(_pContainerPix.width()) * _pContainerPix.height() * 4;
}

QSize ElaDrawerContainer::sizeHint() const
{
    const QSize content = _containerLayout->sizeHint();
    return _expanded ? content : QSize(0, 0);
}

QSize ElaDrawerContainer::minimumSizeHint() const
{
    const QSize content = _containerLayout->minimumSize();
    return _expanded ? content : QSize(0, 0);
}

void ElaDrawerContainer::setEdge(Qt::Edge edge)
{
    if (_edge == edge) return;
    finishAnimation();
    _edge = edge;
}

void ElaDrawerContainer::finishAnimation()
{
    if (_settling || _preparing) return;
    _settling = true;
    const bool wasAnimating = isAnimating() || !_pContainerPix.isNull();
    _animation->stop();
    qApp->removeEventFilter(this);
    _pContainerPix = QPixmap();
    _pOpacity = _expanded ? 1 : 0;
    _isShowBorder = _expanded;
    setMinimumHeight(0);
    setMaximumHeight(_expanded ? QWIDGETSIZE_MAX : 0);
    _containerWidget->setVisible(_expanded);
    _containerWidget->setGeometry(rect());
    updateGeometry();
    update();
    _settling = false;
    if (wasAnimating) Q_EMIT animationFinished(_expanded);
}

void ElaDrawerContainer::doDrawerAnimation(bool isExpand, bool animate)
{
    if (_expanded == isExpand) {
        if (!animate) finishAnimation();
        return;
    }
    _expanded = isExpand;
    _animation->stop();
    if (!animate || !isVisible() || _containerLayout->count() == 0) {
        finishAnimation();
        return;
    }
    QElapsedTimer preparation;
    preparation.start();
    {
        QScopedValueRollback<bool> guard(_preparing, true);
        if (_pContainerPix.isNull()) {
            setMinimumHeight(0);
            setMaximumHeight(QWIDGETSIZE_MAX);
            updateGeometry();
            if (parentWidget() && parentWidget()->layout()) parentWidget()->layout()->activate();
            if (height() <= 0) resize(width(), qMax(1, _containerLayout->sizeHint().height()));
            _containerWidget->setGeometry(rect());
            _containerWidget->show();
            _containerLayout->activate();
            const qreal scale = devicePixelRatioF();
            const qint64 bytes = qint64(qCeil(width() * scale)) * qCeil(height() * scale) * 4;
            if (bytes <= 32 * 1024 * 1024 && width() > 0) {
                _pContainerPix = _containerWidget->grab();
            }
        }
        _containerWidget->hide();
        _isShowBorder = false;
    }
    _preparationMs = preparation.nsecsElapsed() / 1e6;
    if (_pContainerPix.isNull()) {
        finishAnimation();
        return;
    }
    const qreal target = isExpand ? 1 : 0;
    _animation->setDuration(qMax(1, qRound(300 * qAbs(target - _pOpacity))));
    _animation->setStartValue(_pOpacity);
    _animation->setEndValue(target);
    qApp->installEventFilter(this);
    _animation->start();
}

void ElaDrawerContainer::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (!_preparing) finishAnimation();
    _containerWidget->setGeometry(rect());
}

void ElaDrawerContainer::hideEvent(QHideEvent* event)
{
    finishAnimation();
    QWidget::hideEvent(event);
}

bool ElaDrawerContainer::eventFilter(QObject* watched, QEvent* event)
{
    if (!_preparing && isAnimating()) {
        const auto type = event->type();
        auto* widget = qobject_cast<QWidget*>(watched);
        const bool contentInput = widget && (widget == this || widget == _containerWidget || _containerWidget->isAncestorOf(widget))
            && (type == QEvent::MouseButtonPress || type == QEvent::Wheel);
        if (type == QEvent::KeyPress || contentInput) {
            QPointer<ElaDrawerContainer> guard(this);
            finishAnimation();
            if (!guard) return false;
            if (widget == this && type == QEvent::MouseButtonPress && _expanded) {
                auto* mouse = static_cast<QMouseEvent*>(event);
                if (auto* target = childAt(mouse->position().toPoint())) {
                    const QPointF position = target->mapFromGlobal(mouse->globalPosition().toPoint());
                    QMouseEvent forwarded(mouse->type(), position, mouse->globalPosition(),
                                          mouse->button(), mouse->buttons(), mouse->modifiers());
                    QApplication::sendEvent(target, &forwarded);
                    return true;
                }
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ElaDrawerContainer::paintEvent(QPaintEvent* event)
{
    QPainter painter(this);
    painter.save();
    painter.setRenderHints(QPainter::Antialiasing);
    if (_isShowBorder)
    {
        // 背景绘制
        painter.setPen(ElaThemeColor(_themeMode, BasicBorder));
        painter.setBrush(ElaThemeColor(_themeMode, BasicBaseAlpha));
        QRect foregroundRect(1, 1 - 2 * _pBorderRadius, width() - 2, height() - 2 + 2 * _pBorderRadius);
        painter.drawRoundedRect(foregroundRect, _pBorderRadius, _pBorderRadius);
        // 分割线绘制
        int drawerHeight = 0;
        for (int i = 0; i < _drawerWidgetList.count() - 1; i++)
        {
            QWidget* drawerWidget = _drawerWidgetList[i];
            if (!drawerWidget) continue;
            drawerHeight += drawerWidget->height();
            painter.drawLine(0, drawerHeight, width(), drawerHeight);
        }
    }
    if (!_pContainerPix.isNull())
    {
        painter.setOpacity(_pOpacity);
        const qreal distance = 1 - _pOpacity;
        QPointF offset;
        if (_edge == Qt::TopEdge) offset.setY(-height() * distance);
        else if (_edge == Qt::BottomEdge) offset.setY(height() * distance);
        else if (_edge == Qt::LeftEdge) offset.setX(-width() * distance);
        else offset.setX(width() * distance);
        painter.drawPixmap(offset, _pContainerPix);
    }
    painter.restore();
}

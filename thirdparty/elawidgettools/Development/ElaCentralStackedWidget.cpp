#include "ElaCentralStackedWidget.h"

#include "ElaTheme.h"
#include <QApplication>
#include <QDebug>
#include <QGraphicsBlurEffect>
#include <QPainter>
#include <QPainterPath>
#include <QPropertyAnimation>
#include <QParallelAnimationGroup>
#include <QSequentialAnimationGroup>
#include <QTimer>
#include <cmath>
ElaCentralStackedWidget::ElaCentralStackedWidget(QWidget* parent)
    : QWidget(parent)
{
    _pPopupAnimationYOffset = 0;
    _pScaleAnimationRatio = 1;
    _pScaleAnimationPixOpacity = 1;
    _pFlipAnimationRatio = 1;
    _pBlurAnimationRadius = 0;
    _pLastTargetIndex = 0;

    setObjectName("ElaCentralStackedWidget");
    setStyleSheet("#ElaCentralStackedWidget{background-color:transparent;}");

    _containerStackedWidget = new QStackedWidget(this);
    _containerStackedWidget->setObjectName("ElaCentralStackedWidget");
    _containerStackedWidget->setStyleSheet("#ElaCentralStackedWidget{background-color:transparent;}");

    _blurEffect = new QGraphicsBlurEffect(_containerStackedWidget);
    _blurEffect->setBlurHints(QGraphicsBlurEffect::BlurHint::QualityHint);
    _blurEffect->setBlurRadius(0);
    _blurEffect->setEnabled(false);
    _containerStackedWidget->setGraphicsEffect(_blurEffect);

    _mainLayout = new QVBoxLayout(this);
    _mainLayout->setSpacing(0);
    _mainLayout->setContentsMargins(0, 0, 0, 0);
    _mainLayout->addWidget(_containerStackedWidget);

    _themeMode = eTheme->getThemeMode();
    connect(eTheme, &ElaTheme::themeModeChanged, this, &ElaCentralStackedWidget::onThemeModeChanged);
    qApp->installEventFilter(this);
    connect(_containerStackedWidget, &QStackedWidget::widgetRemoved, this, [this] { finishStackSwitch(); });
}

ElaCentralStackedWidget::~ElaCentralStackedWidget()
{
    qApp->removeEventFilter(this);
}

QStackedWidget* ElaCentralStackedWidget::getContainerStackedWidget() const
{
    return _containerStackedWidget;
}

void ElaCentralStackedWidget::setCustomWidget(QWidget* widget)
{
    if (!widget)
    {
        return;
    }
    if (_customWidget)
    {
        _mainLayout->removeWidget(_customWidget);
    }
    _mainLayout->insertWidget(0, widget);
    _customWidget = widget;
}

QWidget* ElaCentralStackedWidget::getCustomWidget() const
{
    return this->_customWidget;
}

void ElaCentralStackedWidget::onThemeModeChanged(ElaThemeType::ThemeMode themeMode)
{
    _themeMode = themeMode;
}

void ElaCentralStackedWidget::setIsTransparent(bool isTransparent)
{
    this->_isTransparent = isTransparent;
    update();
}

bool ElaCentralStackedWidget::getIsTransparent() const
{
    return _isTransparent;
}

void ElaCentralStackedWidget::setIsHasRadius(bool isHasRadius)
{
    this->_isHasRadius = isHasRadius;
    update();
}

void ElaCentralStackedWidget::finishStackSwitch()
{
    ++_switchGeneration;
    if (_switchAnimation) {
        _switchAnimation->stop();
        _switchAnimation->deleteLater();
        _switchAnimation.clear();
    }
    _targetStackPix = QPixmap();
    _currentStackPix = QPixmap();
    _blurEffect->setEnabled(false);
    _pLastTargetIndex = _containerStackedWidget->currentIndex();
    if (auto* current = _containerStackedWidget->currentWidget()) current->show();
    update();
}

bool ElaCentralStackedWidget::isStackSwitching() const
{
    return _switchAnimation && _switchAnimation->state() == QAbstractAnimation::Running;
}

bool ElaCentralStackedWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (isStackSwitching() && (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::KeyPress)) {
        auto* widget = qobject_cast<QWidget*>(watched);
        if (widget && (widget == this || isAncestorOf(widget))) finishStackSwitch();
    }
    return QWidget::eventFilter(watched, event);
}

void ElaCentralStackedWidget::resizeEvent(QResizeEvent* event)
{
    finishStackSwitch();
    QWidget::resizeEvent(event);
}

void ElaCentralStackedWidget::hideEvent(QHideEvent* event)
{
    finishStackSwitch();
    QWidget::hideEvent(event);
}

void ElaCentralStackedWidget::doWindowStackSwitch(ElaWindowType::StackSwitchMode mode, int index, bool isRouteBack)
{
    QPointer<QWidget> target = _containerStackedWidget->widget(index);
    if (!target || (_containerStackedWidget->currentIndex() == index && !isStackSwitching())) return;
    finishStackSwitch();
    const auto generation = _switchGeneration;
    _pLastTargetIndex = index;
    _stackSwitchMode = mode;
    if (mode == ElaWindowType::None || !isVisible()) {
        _containerStackedWidget->setCurrentIndex(index);
        return;
    }
    if (mode == ElaWindowType::Scale || mode == ElaWindowType::Flip) _getCurrentStackPix();
    const QPointer<ElaCentralStackedWidget> self(this);
    _containerStackedWidget->setCurrentIndex(index);
    if (!self || !target || generation != _switchGeneration) return;
    target->show();
    if (!self || !target || generation != _switchGeneration) return;
    if (mode != ElaWindowType::Blur) {
        _getTargetStackPix();
        if (!self || !target || generation != _switchGeneration) return;
        target->hide();
        if (!self || !target || generation != _switchGeneration) return;
    }
    auto animation = [this](const char* property, const QVariant& from, const QVariant& to,
                            int duration, QEasingCurve curve) {
        auto* result = new QPropertyAnimation(this, property);
        result->setStartValue(from);
        result->setEndValue(to);
        result->setDuration(duration);
        result->setEasingCurve(curve);
        connect(result, &QPropertyAnimation::valueChanged, this, [this] { update(); });
        return result;
    };
    switch (mode) {
    case ElaWindowType::Popup: {
        const int y = _containerStackedWidget->mapToParent(QPoint()).y();
        _switchAnimation = animation("pPopupAnimationYOffset", y + 80, y, 300, QEasingCurve::OutCubic);
        break;
    }
    case ElaWindowType::Scale: {
        _isDrawNewPix = false;
        auto* sequence = new QSequentialAnimationGroup(this);
        auto* outgoing = new QParallelAnimationGroup(sequence);
        outgoing->addAnimation(animation("pScaleAnimationRatio", 1.0, isRouteBack ? 0.85 : 1.15, 150, QEasingCurve::Linear));
        outgoing->addAnimation(animation("pScaleAnimationPixOpacity", 1.0, 0.0, 150, QEasingCurve::Linear));
        auto* incoming = new QParallelAnimationGroup(sequence);
        incoming->addAnimation(animation("pScaleAnimationRatio", isRouteBack ? 1.5 : 0.85, 1.0, 300, QEasingCurve::OutCubic));
        incoming->addAnimation(animation("pScaleAnimationPixOpacity", 0.0, 1.0, 300, QEasingCurve::Linear));
        connect(outgoing, &QAbstractAnimation::finished, this, [this] { _isDrawNewPix = true; });
        sequence->addAnimation(outgoing);
        sequence->addAnimation(incoming);
        _switchAnimation = sequence;
        break;
    }
    case ElaWindowType::Flip:
        _switchAnimation = animation("pFlipAnimationRatio", 0.0, isRouteBack ? -180.0 : 180.0, 650, QEasingCurve::InOutSine);
        break;
    case ElaWindowType::Blur:
        _blurEffect->setEnabled(true);
        _switchAnimation = animation("pBlurAnimationRadius", 40, 2, 350, QEasingCurve::InOutSine);
        connect(static_cast<QPropertyAnimation*>(_switchAnimation.data()), &QPropertyAnimation::valueChanged,
                this, [this] { _blurEffect->setBlurRadius(_pBlurAnimationRadius); });
        break;
    default:
        finishStackSwitch();
        return;
    }
    _switchAnimation->setParent(this);
    connect(_switchAnimation, &QAbstractAnimation::finished, this, [this, generation] {
        if (generation != _switchGeneration) return;
        finishStackSwitch();
    });
    _switchAnimation->start();
}

void ElaCentralStackedWidget::paintEvent(QPaintEvent* event)
{
    QRect targetRect = this->rect();
    targetRect.adjust(1, 1, 10, 10);
    QPainter painter(this);
    painter.save();
    painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    if (!_isTransparent)
    {
        painter.setPen(QPen(ElaThemeColor(_themeMode, BasicBaseLine), 1.5));
        painter.setBrush(ElaThemeColor(_themeMode, WindowCentralStackBase));
        if (_isHasRadius)
        {
            painter.drawRoundedRect(targetRect, 10, 10);
        }
        else
        {
            painter.drawRect(targetRect);
        }
    }
    // 切换动画
    if (!_targetStackPix.isNull())
    {
        QPoint centralStackPos = _containerStackedWidget->mapToParent(QPoint(0, 0));
        QRect centralStackRect = QRect(centralStackPos.x(), centralStackPos.y(), _containerStackedWidget->width(), _containerStackedWidget->height());
        QPainterPath clipPath;
        clipPath.addRoundedRect(centralStackRect, 10, 10);
        painter.setClipPath(clipPath);
        switch (_stackSwitchMode)
        {
        case ElaWindowType::None:
        {
            break;
        }
        case ElaWindowType::Popup:
        {
            painter.drawPixmap(QRect(0, _pPopupAnimationYOffset, width(), _containerStackedWidget->height()), _targetStackPix);
            break;
        }
        case ElaWindowType::Scale:
        {
            painter.setOpacity(_pScaleAnimationPixOpacity);
            painter.translate(_containerStackedWidget->rect().center());
            painter.scale(_pScaleAnimationRatio, _pScaleAnimationRatio);
            painter.translate(-_containerStackedWidget->rect().center());
            painter.drawPixmap(centralStackRect, _isDrawNewPix ? _targetStackPix : _currentStackPix);
            break;
        }
        case ElaWindowType::Flip:
        {
            QTransform transform;
            transform.translate(centralStackRect.center().x(), 0);
            if (abs(_pFlipAnimationRatio) >= 90)
            {
                transform.rotate(-180 + _pFlipAnimationRatio, Qt::YAxis);
            }
            else
            {
                transform.rotate(_pFlipAnimationRatio, Qt::YAxis);
            }
            transform.translate(-centralStackRect.center().x(), 0);
            painter.setTransform(transform);
            if (abs(_pFlipAnimationRatio) >= 90)
            {
                painter.drawPixmap(centralStackRect, _targetStackPix);
            }
            else
            {
                painter.drawPixmap(centralStackRect, _currentStackPix);
            }
            break;
        }
        case ElaWindowType::Blur:
        {
            break;
        }
        }
    }
    painter.restore();
}

void ElaCentralStackedWidget::_getCurrentStackPix()
{
    if (!_containerStackedWidget->currentWidget()) return;
    _targetStackPix = QPixmap();
    bool isTransparent = _isTransparent;
    _isTransparent = true;
    _containerStackedWidget->currentWidget()->setVisible(true);
    _currentStackPix = _containerStackedWidget->grab();
    _containerStackedWidget->currentWidget()->setVisible(false);
    _isTransparent = isTransparent;
}

void ElaCentralStackedWidget::_getTargetStackPix()
{
    _targetStackPix = QPixmap();
    bool isTransparent = _isTransparent;
    _isTransparent = true;
    _targetStackPix = _containerStackedWidget->grab();
    _isTransparent = isTransparent;
}

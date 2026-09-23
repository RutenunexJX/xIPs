#include "ElaComboBox.h"

#include "ElaComboBoxStyle.h"
#include "ElaScrollBar.h"
#include "ElaTheme.h"
#include "private/ElaComboBoxPrivate.h"
#include <QAbstractItemView>
#include <QApplication>
#include <QLayout>
#include <QLineEdit>
#include <QListView>
#include <QMouseEvent>
#include <QPropertyAnimation>
#include <QParallelAnimationGroup>
#include <QHideEvent>
#include <QResizeEvent>
Q_PROPERTY_CREATE_Q_CPP(ElaComboBox, int, BorderRadius)
ElaComboBox::ElaComboBox(QWidget* parent)
    : QComboBox(parent), d_ptr(new ElaComboBoxPrivate())
{
    Q_D(ElaComboBox);
    d->q_ptr = this;
    d->_pBorderRadius = 3;
    d->_themeMode = eTheme->getThemeMode();
    setObjectName("ElaComboBox");
    setFixedHeight(35);
    d->_comboBoxStyle = new ElaComboBoxStyle(style());
    d->_comboBoxStyle->setParent(qApp);
    setStyle(d->_comboBoxStyle);
    d->_popupAnimation = new QParallelAnimationGroup(this);
    connect(d->_popupAnimation, &QParallelAnimationGroup::finished, this, &ElaComboBox::finishPopupAnimation);
    d->_indicatorAnimation = new QParallelAnimationGroup(this);
    d->_rotation = new QPropertyAnimation(d->_comboBoxStyle, "pExpandIconRotate", d->_indicatorAnimation);
    d->_mark = new QPropertyAnimation(d->_comboBoxStyle, "pExpandMarkWidth", d->_indicatorAnimation);
    for (auto* animation : {d->_rotation, d->_mark}) {
        animation->setDuration(150);
        animation->setEasingCurve(QEasingCurve::InOutSine);
    }
    connect(d->_rotation, &QPropertyAnimation::valueChanged, this, [this] { update(); });

    //调用view 让container初始化
    setView(new QListView(this));
    QAbstractItemView* comboBoxView = this->view();
    comboBoxView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    ElaScrollBar* scrollBar = new ElaScrollBar(this);
    comboBoxView->setVerticalScrollBar(scrollBar);
    ElaScrollBar* floatVScrollBar = new ElaScrollBar(scrollBar, comboBoxView);
    floatVScrollBar->setIsAnimation(true);
    comboBoxView->setAutoScroll(false);
    comboBoxView->setSelectionMode(QAbstractItemView::NoSelection);
    comboBoxView->setObjectName("ElaComboBoxView");
    comboBoxView->setStyleSheet("#ElaComboBoxView{background-color:transparent;}");
    comboBoxView->setStyle(d->_comboBoxStyle);
    comboBoxView->installEventFilter(this);
    comboBoxView->viewport()->installEventFilter(this);
    QWidget* container = this->findChild<QFrame*>();
    if (container)
    {
        container->installEventFilter(this);
        container->setWindowFlags(Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
        container->setAttribute(Qt::WA_TranslucentBackground);
        container->setObjectName("ElaComboBoxContainer");
        container->setStyle(d->_comboBoxStyle);
        QLayout* layout = container->layout();
        while (layout->count())
        {
            delete layout->takeAt(0);
        }
        layout->addWidget(view());
        layout->setContentsMargins(6, 0, 6, 6);
#ifndef Q_OS_WIN
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        container->setStyleSheet("background-color:transparent;");
#endif
#endif
    }
    QComboBox::setMaxVisibleItems(5);
    connect(eTheme, &ElaTheme::themeModeChanged, d, &ElaComboBoxPrivate::onThemeChanged);
}

ElaComboBox::~ElaComboBox()
{
    Q_D(ElaComboBox);
    finishPopupAnimation();
    d->_indicatorAnimation->stop();
    // Keep the shared style alive through QComboBox's popup/view destruction.
    // Resetting popup styles here repolishes its native children mid-teardown.
    // The application owns the fallback lifetime if its event loop has stopped.
    d->_comboBoxStyle->deleteLater();
}

void ElaComboBox::setEditable(bool editable)
{
    Q_D(ElaComboBox);
    QComboBox::setEditable(editable);
    if (editable)
    {
        lineEdit()->setStyle(d->_comboBoxStyle);
        d->onThemeChanged(d->_themeMode);
    }
}

bool ElaComboBox::isPopupAnimating() const
{
    Q_D(const ElaComboBox);
    return d->_popupAnimation->state() == QAbstractAnimation::Running;
}

void ElaComboBox::finishPopupAnimation()
{
    Q_D(ElaComboBox);
    if (d->_settling || !d->_popup) return;
    d->_settling = true;
    d->_popupAnimation->stop();
    auto* layout = d->_popup->layout();
    d->_popup->setMinimumHeight(0);
    d->_popup->setMaximumHeight(QWIDGETSIZE_MAX);
    if (d->_popupHeight > 0) d->_popup->resize(d->_popup->width(), d->_popupHeight);
    view()->move(d->_viewPosition);
    if (layout->indexOf(view()) < 0) layout->addWidget(view());
    layout->activate();
    d->_settling = false;
}

void ElaComboBox::animateIndicator(bool expanded)
{
    Q_D(ElaComboBox);
    d->_indicatorAnimation->stop();
    d->_rotation->setStartValue(d->_comboBoxStyle->getExpandIconRotate());
    d->_rotation->setEndValue(expanded ? -180 : 0);
    d->_mark->setStartValue(d->_comboBoxStyle->getExpandMarkWidth());
    d->_mark->setEndValue(expanded ? qMax(0, width() / 2 - d->_pBorderRadius - 6) : 0);
    d->_indicatorAnimation->start();
}

void ElaComboBox::showPopup()
{
    Q_D(ElaComboBox);
    finishPopupAnimation();
    QPointer<ElaComboBox> alive(this);
    const bool oldEffects = qApp->isEffectEnabled(Qt::UI_AnimateCombo);
    qApp->setEffectEnabled(Qt::UI_AnimateCombo, false);
    QComboBox::showPopup();
    qApp->setEffectEnabled(Qt::UI_AnimateCombo, oldEffects);
    if (!alive || count() == 0 || !view()->isVisible()) return;
    d->_popup = view()->window();
    d->_popupHeight = d->_popup->height();
    d->_viewPosition = view()->pos();
    auto* layout = d->_popup->layout();
    if (!layout) return;
    layout->removeWidget(view());
    d->_popupAnimation->clear();
    auto* height = new QPropertyAnimation(d->_popup, "maximumHeight", d->_popupAnimation);
    connect(height, &QPropertyAnimation::valueChanged, this, [this](const QVariant& value) {
        Q_D(ElaComboBox);
        if (d->_popup) d->_popup->setFixedHeight(value.toInt());
    });
    height->setStartValue(1);
    height->setEndValue(d->_popupHeight);
    height->setDuration(180);
    height->setEasingCurve(QEasingCurve::OutCubic);
    auto* position = new QPropertyAnimation(view(), "pos", d->_popupAnimation);
    position->setStartValue(d->_viewPosition - QPoint(0, view()->height()));
    position->setEndValue(d->_viewPosition);
    position->setDuration(180);
    position->setEasingCurve(QEasingCurve::OutCubic);
    d->_popupAnimation->start();
    animateIndicator(true);
}

void ElaComboBox::hidePopup()
{
    finishPopupAnimation();
    QComboBox::hidePopup();
    animateIndicator(false);
}

bool ElaComboBox::eventFilter(QObject* watched, QEvent* event)
{
    Q_D(ElaComboBox);
    if (watched == d->_popup && event->type() == QEvent::Hide) {
        finishPopupAnimation();
        animateIndicator(false);
    } else if (isPopupAnimating() && (event->type() == QEvent::KeyPress
        || event->type() == QEvent::MouseButtonPress || event->type() == QEvent::Wheel)) {
        finishPopupAnimation();
    }
    return QComboBox::eventFilter(watched, event);
}

void ElaComboBox::hideEvent(QHideEvent* event)
{
    hidePopup();
    QComboBox::hideEvent(event);
}

void ElaComboBox::resizeEvent(QResizeEvent* event)
{
    if (isPopupAnimating()) finishPopupAnimation();
    QComboBox::resizeEvent(event);
}

void ElaComboBox::paintEvent(QPaintEvent* event)
{
    Q_D(ElaComboBox);
    if (lineEdit() && lineEdit()->palette().color(QPalette::Text) != ElaThemeColor(d->_themeMode, BasicText))
    {
        d->onThemeChanged(d->_themeMode);
    }
    QComboBox::paintEvent(event);
}

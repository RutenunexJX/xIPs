#include "ElaMenu.h"

#include <QApplication>
#include <QCloseEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPropertyAnimation>
#include <QPointer>
#include <QVBoxLayout>
#include <QWidgetAction>

#include "ElaMenuStyle.h"
#include "private/ElaMenuPrivate.h"
ElaMenu::ElaMenu(QWidget* parent)
    : QMenu(parent), d_ptr(new ElaMenuPrivate())
{
    Q_D(ElaMenu);
    d->q_ptr = this;
    setWindowFlags(Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setObjectName("ElaMenu");
    d->_menuStyle = new ElaMenuStyle(style());
    d->_menuStyle->setParent(qApp);
    connect(this, &QObject::destroyed, d->_menuStyle, &QObject::deleteLater);
    setStyle(d->_menuStyle);
    d->_pAnimationImagePosY = 0;
    d->_popupAnimation = new QPropertyAnimation(d, "pAnimationImagePosY", this);
    d->_popupAnimation->setEasingCurve(QEasingCurve::OutCubic);
    d->_popupAnimation->setDuration(160);
    connect(d->_popupAnimation, &QPropertyAnimation::valueChanged, this, [this] { update(); });
    connect(d->_popupAnimation, &QPropertyAnimation::finished, this, &ElaMenu::finishPopupAnimation);
}

ElaMenu::ElaMenu(const QString& title, QWidget* parent)
    : ElaMenu(parent)
{
    setTitle(title);
}

ElaMenu::~ElaMenu()
{
    Q_D(ElaMenu);
    d->_popupAnimation->stop();
}

void ElaMenu::setNativeMenuBehavior(bool enabled)
{
    Q_D(ElaMenu);
    finishPopupAnimation();
    _nativeMenuBehavior = enabled;
    d->_menuStyle->setNativeItemContent(enabled);
}

void ElaMenu::setMenuItemHeight(int menuItemHeight)
{
    Q_D(ElaMenu);
    d->_menuStyle->setMenuItemHeight(menuItemHeight);
}

int ElaMenu::getMenuItemHeight() const
{
    Q_D(const ElaMenu);
    return d->_menuStyle->getMenuItemHeight();
}

QAction* ElaMenu::addMenu(QMenu* menu)
{
    return QMenu::addMenu(menu);
}

ElaMenu* ElaMenu::addMenu(const QString& title)
{
    ElaMenu* menu = new ElaMenu(title, this);
    menu->setNativeMenuBehavior(_nativeMenuBehavior);
    QMenu::addAction(menu->menuAction());
    return menu;
}

ElaMenu* ElaMenu::addMenu(const QIcon& icon, const QString& title)
{
    ElaMenu* menu = new ElaMenu(title, this);
    menu->setNativeMenuBehavior(_nativeMenuBehavior);
    menu->setIcon(icon);
    QMenu::addAction(menu->menuAction());
    return menu;
}

ElaMenu* ElaMenu::addMenu(ElaIconType::IconName icon, const QString& title)
{
    ElaMenu* menu = new ElaMenu(title, this);
    menu->setNativeMenuBehavior(_nativeMenuBehavior);
    QMenu::addAction(menu->menuAction());
    menu->menuAction()->setProperty("ElaIconType", QChar(static_cast<char32_t>(icon)));
    return menu;
}

QAction* ElaMenu::addElaIconAction(ElaIconType::IconName icon, const QString& text)
{
    QAction* action = new QAction(text, this);
    action->setProperty("ElaIconType", QChar(static_cast<char32_t>(icon)));
    QMenu::addAction(action);
    return action;
}

QAction* ElaMenu::addElaIconAction(ElaIconType::IconName icon, const QString& text, const QKeySequence& shortcut)
{
    QAction* action = new QAction(text, this);
    action->setShortcut(shortcut);
    action->setProperty("ElaIconType", QChar(static_cast<char32_t>(icon)));
    QMenu::addAction(action);
    return action;
}

bool ElaMenu::isHasChildMenu() const
{
    QList<QAction*> actionList = this->actions();
    for (auto action: actionList)
    {
        if (action->isSeparator())
        {
            continue;
        }
        if (action->menu())
        {
            return true;
        }
    }
    return false;
}

bool ElaMenu::isHasIcon() const
{
    QList<QAction*> actionList = this->actions();
    for (auto action: actionList)
    {
        if (action->isSeparator())
        {
            continue;
        }
        QMenu* menu = action->menu();
        if (menu && (!menu->icon().isNull() || !menu->property("ElaIconType").toString().isEmpty()))
        {
            return true;
        }
        if (!action->icon().isNull() || !action->property("ElaIconType").toString().isEmpty())
        {
            return true;
        }
    }
    return false;
}

bool ElaMenu::isPopupAnimating() const
{
    Q_D(const ElaMenu);
    return d->_popupAnimation && d->_popupAnimation->state() == QAbstractAnimation::Running;
}

void ElaMenu::finishPopupAnimation()
{
    Q_D(ElaMenu);
    if (d->_popupAnimation) d->_popupAnimation->stop();
    d->_animationPix = QPixmap();
    d->_pAnimationImagePosY = 0;
    update();
}

bool ElaMenu::event(QEvent* event)
{
    Q_D(ElaMenu);
    if (!d->_capturing && isPopupAnimating()) {
        switch (event->type()) {
        case QEvent::KeyPress:
        case QEvent::MouseButtonPress:
        case QEvent::MouseMove:
        case QEvent::Wheel:
        case QEvent::Hide:
        case QEvent::Resize:
        case QEvent::ActionAdded:
        case QEvent::ActionRemoved:
        case QEvent::ActionChanged:
        case QEvent::PaletteChange:
        case QEvent::StyleChange:
        case QEvent::FontChange:
            finishPopupAnimation();
            break;
        default:
            break;
        }
    }
    return QMenu::event(event);
}

void ElaMenu::showEvent(QShowEvent* event)
{
    QMenu::showEvent(event);
    QPointer<ElaMenu> alive(this);
    Q_EMIT menuShow();
    if (!alive || !isVisible()) return;
    finishPopupAnimation();
    Q_D(ElaMenu);
    // Qt keeps popup placement and actionable item semantics. Ela animates its image.
    // Embedded editors must remain live instead of painting a duplicate snapshot.
    for (auto* action : actions())
        if (qobject_cast<QWidgetAction*>(action)) return;
    const qreal scale = devicePixelRatioF();
    if (qint64(qCeil(width() * scale)) * qCeil(height() * scale) * 4 > 8 * 1024 * 1024)
        return;
    d->_capturing = true;
    d->_animationPix = grab();
    d->_capturing = false;
    if (d->_animationPix.isNull()) return;
    const int distance = qMin(160, height());
    const bool fromAbove = pos().y() + d->_menuStyle->getMenuItemHeight() + 9 >= QCursor::pos().y();
    d->_popupAnimation->setStartValue(fromAbove ? -distance : distance);
    d->_popupAnimation->setEndValue(0);
    d->_popupAnimation->start();
}

void ElaMenu::paintEvent(QPaintEvent* event)
{
    Q_D(ElaMenu);
    if (!d->_animationPix.isNull() && !d->_capturing) {
        QPainter painter(this);
        painter.drawPixmap(QPoint(0, d->_pAnimationImagePosY), d->_animationPix);
    } else {
        QMenu::paintEvent(event);
    }
}

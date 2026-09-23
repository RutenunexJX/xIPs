#include "ElaDrawerArea.h"
#include "ElaDrawerAreaPrivate.h"
#include "ElaTheme.h"
ElaDrawerArea::ElaDrawerArea(QWidget* parent)
    : QWidget(parent), d_ptr(new ElaDrawerAreaPrivate())
{
    Q_D(ElaDrawerArea);
    d->q_ptr = this;
    setObjectName("ElaDrawerArea");
    setStyleSheet("#ElaDrawerArea{background-color:transparent;}");

    d->_drawerHeader = new ElaDrawerHeader(this);
    d->_drawerContainer = new ElaDrawerContainer(this);
    connect(d->_drawerHeader, &ElaDrawerHeader::drawerHeaderClicked, d, &ElaDrawerAreaPrivate::onDrawerHeaderClicked);
    connect(d->_drawerContainer, &ElaDrawerContainer::animationFinished,
            this, &ElaDrawerArea::drawerAnimationFinished);

    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);
    mainLayout->addWidget(d->_drawerHeader);
    mainLayout->addWidget(d->_drawerContainer);

    d->_themeMode = eTheme->getThemeMode();
    connect(eTheme, &ElaTheme::themeModeChanged, this, [=](ElaThemeType::ThemeMode themeMode) {
        d->_themeMode = themeMode;
    });
}

ElaDrawerArea::~ElaDrawerArea()
{
}

void ElaDrawerArea::setBorderRadius(int borderRadius)
{
    Q_D(ElaDrawerArea);
    d->_drawerHeader->setBorderRadius(borderRadius);
    d->_drawerContainer->setBorderRadius(borderRadius);
    Q_EMIT pBorderRadiusChanged();
}

int ElaDrawerArea::getBorderRadius() const
{
    Q_D(const ElaDrawerArea);
    return d->_drawerHeader->getBorderRadius();
}

void ElaDrawerArea::setHeaderHeight(int headerHeight)
{
    Q_D(ElaDrawerArea);
    d->_drawerHeader->setFixedHeight(headerHeight);
}

int ElaDrawerArea::getHeaderHeight() const
{
    Q_D(const ElaDrawerArea);
    return d->_drawerHeader->height();
}

void ElaDrawerArea::setDrawerHeader(QWidget* widget)
{
    Q_D(ElaDrawerArea);
    d->_drawerHeader->setHeaderWidget(widget);
}

void ElaDrawerArea::addDrawer(QWidget* widget)
{
    Q_D(ElaDrawerArea);
    d->_drawerContainer->addWidget(widget);
}

void ElaDrawerArea::removeDrawer(QWidget* widget)
{
    Q_D(ElaDrawerArea);
    d->_drawerContainer->removeWidget(widget);
}

void ElaDrawerArea::expand()
{
    setExpanded(true);
}

void ElaDrawerArea::collapse()
{
    setExpanded(false);
}

void ElaDrawerArea::setExpanded(bool expanded, bool animate)
{
    Q_D(ElaDrawerArea);
    const bool changed = d->_drawerContainer->isExpanded() != expanded;
    d->_drawerHeader->setIsExpand(expanded);
    d->_drawerHeader->doExpandOrCollapseAnimation(animate && isVisible());
    QPointer<ElaDrawerArea> guard(this);
    d->_drawerContainer->doDrawerAnimation(expanded, animate);
    if (guard && changed) Q_EMIT expandStateChanged(expanded);
}

void ElaDrawerArea::setDrawerHeaderVisible(bool visible)
{
    Q_D(ElaDrawerArea);
    d->_drawerHeader->setVisible(visible);
}

void ElaDrawerArea::setDrawerEdge(Qt::Edge edge)
{
    Q_D(ElaDrawerArea);
    d->_drawerContainer->setEdge(edge);
}

bool ElaDrawerArea::isDrawerAnimating() const
{
    Q_D(const ElaDrawerArea);
    return d->_drawerContainer->isAnimating();
}

void ElaDrawerArea::finishDrawerAnimation()
{
    Q_D(ElaDrawerArea);
    d->_drawerContainer->finishAnimation();
}

qint64 ElaDrawerArea::drawerSnapshotBytes() const
{
    Q_D(const ElaDrawerArea);
    return d->_drawerContainer->snapshotBytes();
}

double ElaDrawerArea::drawerPreparationMs() const
{
    Q_D(const ElaDrawerArea);
    return d->_drawerContainer->preparationMs();
}

qreal ElaDrawerArea::drawerProgress() const
{
    Q_D(const ElaDrawerArea);
    return d->_drawerContainer->getOpacity();
}

bool ElaDrawerArea::getIsExpand() const
{
    Q_D(const ElaDrawerArea);
    return d->_drawerHeader->getIsExpand();
}

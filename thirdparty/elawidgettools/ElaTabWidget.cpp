#include "ElaTabWidget.h"

#include "ElaTabBar.h"
#include "ElaTabWidgetPrivate.h"
#include "ElaTabWidgetHost.h"
#include <QDebug>
#include <QDrag>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
Q_PROPERTY_CREATE_Q_CPP(ElaTabWidget, bool, IsTabTransparent);
Q_PROPERTY_CREATE_Q_CPP(ElaTabWidget, bool, IsContainerAcceptDrops);
Q_PROPERTY_CREATE_Q_CPP(ElaTabWidget, QSize, FloatWidgetSize)
ElaTabWidget::ElaTabWidget(QWidget* parent)
    : QTabWidget(parent), d_ptr(new ElaTabWidgetPrivate())
{
    Q_D(ElaTabWidget);
    d->q_ptr = this;
    d->_pIsContainerAcceptDrops = false;
    d->_pIsTabTransparent = false;
    d->_pFloatWidgetSize = QSize(700, 500);
    setObjectName("ElaTabWidget");
    setAcceptDrops(true);
    d->_tabBar = new ElaTabBar(this);
    setTabBar(d->_tabBar);
    connect(d->_tabBar, &ElaTabBar::tabDragCreate, d, &ElaTabWidgetPrivate::onTabDragCreate);
    connect(d->_tabBar, &ElaTabBar::tabDragEnter, d, &ElaTabWidgetPrivate::onTabDragEnter);
    connect(d->_tabBar, &ElaTabBar::tabDragLeave, d, &ElaTabWidgetPrivate::onTabDragLeave);
    connect(d->_tabBar, &ElaTabBar::tabDragDrop, d, &ElaTabWidgetPrivate::onTabDragDrop);
    connect(d->_tabBar, &ElaTabBar::tabCloseRequested, d, &ElaTabWidgetPrivate::onTabCloseRequested);
    connect(this, &QTabWidget::currentChanged, this, [=](int index) {
        if (index < 0)
        {
            return;
        }
        Q_EMIT currentWidgetChanged(widget(index));
    });
}

ElaTabWidget::~ElaTabWidget()
{
    Q_D(ElaTabWidget);
    if (!_host)
        d->_clearAllTabWidgetList();
}

void ElaTabWidget::setTabSize(QSize tabSize)
{
    Q_D(ElaTabWidget);
    d->_tabBar->setTabSize(tabSize);
}

QSize ElaTabWidget::getTabSize() const
{
    Q_D(const ElaTabWidget);
    return d->_tabBar->getTabSize();
}

void ElaTabWidget::setTabPosition(TabPosition position)
{
    if (position == QTabWidget::North || position == QTabWidget::South)
    {
        QTabWidget::setTabPosition(position);
    }
}

void ElaTabWidget::paintEvent(QPaintEvent* event)
{
    Q_D(ElaTabWidget);
    if (!d->_pIsTabTransparent)
    {
        QTabWidget::paintEvent(event);
    }
}

void ElaTabWidget::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->property("DragType").toString() == "ElaTabBarDrag")
    {
        event->acceptProposedAction();
    }
    QTabWidget::dragEnterEvent(event);
}

void ElaTabWidget::dropEvent(QDropEvent* event)
{
    Q_D(ElaTabWidget);
    if (d->_pIsContainerAcceptDrops && event->mimeData()->property("ElaTabWidgetObject").value<ElaTabWidget*>() != this)
    {
        QMimeData* data = const_cast<QMimeData*>(event->mimeData());
        data->setProperty("TabDropIndex", count());
        d->onTabDragDrop(data);
        event->accept();
    }
    QTabWidget::dropEvent(event);
}

void ElaTabWidget::tabInserted(int index)
{
    Q_D(ElaTabWidget);
    QWidget* tabWidget = widget(index);
    if (!_host && !tabWidget->property("IsMetaWidget").toBool() && !tabWidget->property("ElaOriginTabWidget").isValid())
    {
        d->_allTabWidgetList.append(widget(index));
    }
    QTabWidget::tabInserted(index);
}

void ElaTabWidget::setHostedTabs(QObject* scope, SplitResolver split, ReturnTarget returnTarget)
{
    Q_D(ElaTabWidget);
    if (_host || !scope || count() != 0)
        return;
    // The stock handlers own/delete pages; a hosted document must never enter them.
    disconnect(d->_tabBar, nullptr, d, nullptr);
    d->_allTabWidgetList.clear();
    d->_tabBar->setNativeTabBehavior(true);
    d->_tabBar->setSmoothScrollEnabled(true);
    _host = new ElaTabWidgetHost(this, scope, std::move(split), std::move(returnTarget));
}

bool ElaTabWidget::hasHostedTabs() const { return _host != nullptr; }
bool ElaTabWidget::isHostedTabDragging() const { return _host && _host->isDragging(); }
bool ElaTabWidget::isFloatingTabWidget() const { return _host && _host->floatingWindow; }

void ElaTabWidget::setHostedTabBar(ElaTabBar* bar)
{
    Q_D(ElaTabWidget);
    if (!_host || !bar || count() != 0)
        return;
    d->_tabBar = bar;
    bar->setNativeTabBehavior(true);
    bar->setSmoothScrollEnabled(true);
    setTabBar(bar);
    bar->setAcceptDrops(true);
}

bool ElaTabWidget::transferHostedTab(QWidget* page, ElaTabWidget* target, int index)
{
    return _host && _host->transfer(page, target, index);
}

ElaTabWidget* ElaTabWidget::floatHostedTab(QWidget* page, const QPoint& globalPosition)
{
    return _host ? _host->detach(page, globalPosition) : nullptr;
}

void ElaTabWidget::syncFloatingVisibility()
{
    if (_host)
        _host->syncVisibility();
}

void ElaTabWidget::disposeEmptyFloatingWindow()
{
    if (_host && _host->floatingWindow && count() == 0) {
        _host->floatingWindow->hide();
        _host->floatingWindow->deleteLater();
    }
}

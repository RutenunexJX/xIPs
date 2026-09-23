#include "ElaTableView.h"

#include <QHeaderView>
#include <QApplication>
#include <QMouseEvent>

#include "ElaTableViewStyle.h"
#include "ElaScrollBar.h"
#include "ElaTableViewPrivate.h"
ElaTableView::ElaTableView(QWidget* parent)
    : QTableView(parent), d_ptr(new ElaTableViewPrivate())
{
    Q_D(ElaTableView);
    d->q_ptr = this;
    setMouseTracking(true);
    setObjectName("ElaTableView");
    setStyleSheet(
        "QTableView{background-color:transparent;}"
        "QHeaderView{background-color:transparent;border:0px;}");
    setShowGrid(false);
    setVerticalScrollBar(new ElaScrollBar(this));
    setHorizontalScrollBar(new ElaScrollBar(this));
    this->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    this->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    d->_tableViewStyle = new ElaTableViewStyle(style());
    d->_tableViewStyle->setParent(qApp);
    connect(this, &QObject::destroyed, d->_tableViewStyle, &QObject::deleteLater);
    setStyle(d->_tableViewStyle);
}

ElaTableView::~ElaTableView()
{
}

QStyle* ElaTableView::createStyle(QObject* owner)
{
    auto* viewStyle = new ElaTableViewStyle();
    viewStyle->setParent(owner);
    viewStyle->setNativeItemContent(true);
    return viewStyle;
}

void ElaTableView::setNativeItemContent(bool enabled)
{
    Q_D(ElaTableView);
    d->_tableViewStyle->setNativeItemContent(enabled);
    viewport()->update();
}

void ElaTableView::setHeaderMargin(int headerMargin)
{
    Q_D(ElaTableView);
    if (headerMargin >= 0)
    {
        d->_tableViewStyle->setHeaderMargin(headerMargin);
        doItemsLayout();
    }
}

int ElaTableView::getHeaderMargin() const
{
    Q_D(const ElaTableView);
    return d->_tableViewStyle->getHeaderMargin();
}

void ElaTableView::showEvent(QShowEvent* event)
{
    Q_EMIT tableViewShow();
    QTableView::showEvent(event);
}

void ElaTableView::hideEvent(QHideEvent* event)
{
    Q_EMIT tableViewHide();
    QTableView::hideEvent(event);
}

void ElaTableView::mouseMoveEvent(QMouseEvent* event)
{
    Q_D(ElaTableView);
    if (selectionBehavior() == QAbstractItemView::SelectRows)
    {
        d->_tableViewStyle->setCurrentHoverRow(indexAt(event->pos()).row());
        update();
    }
    QTableView::mouseMoveEvent(event);
}

void ElaTableView::leaveEvent(QEvent* event)
{
    Q_D(ElaTableView);
    if (selectionBehavior() == QAbstractItemView::SelectRows)
    {
        d->_tableViewStyle->setCurrentHoverRow(-1);
        update();
    }
    QTableView::leaveEvent(event);
}

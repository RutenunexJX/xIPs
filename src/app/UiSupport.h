#pragma once
#include <QPoint>
class QAbstractScrollArea;
class QWidget;
class ElaMenu;
class QAction;

namespace xips
{
void enableSmoothScrolling(QAbstractScrollArea *area);
void enableToolTip(QWidget *widget);
void prepareMenu(ElaMenu *menu, QWidget *owner);
QAction *executeMenu(ElaMenu &menu, QWidget *owner, const QPoint &position);
}

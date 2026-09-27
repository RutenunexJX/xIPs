#pragma once
#include <QPoint>
class QAbstractScrollArea;
class QWidget;
class ElaMenu;
class QAction;
class ElaPushButton;

namespace xips
{
void primaryButton(ElaPushButton *button);
void enableSmoothScrolling(QAbstractScrollArea *area);
void enableToolTip(QWidget *widget);
void prepareMenu(ElaMenu *menu, QWidget *owner);
QAction *executeMenu(ElaMenu &menu, QWidget *owner, const QPoint &position);
}

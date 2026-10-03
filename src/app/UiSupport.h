#pragma once
#include <QPoint>
#include <QIcon>
class QAbstractScrollArea;
class QAbstractItemDelegate;
class QObject;
class QWidget;
class ElaMenu;
class QAction;
class ElaPushButton;

namespace xips
{
enum class UiIcon { Add, Folder, Collect, Filter, Refresh, FolderPlus, Reference, Edit,
                    Archive, Open, Lock, File, Copy, Theme, Trash, Unlink, Receipt, Warning };
QIcon uiIcon(UiIcon icon, bool primary = false);
QAbstractItemDelegate *detailDelegate(bool revisions, QObject *parent);
void primaryButton(ElaPushButton *button);
void enableSmoothScrolling(QAbstractScrollArea *area);
void enableToolTip(QWidget *widget);
void keepDialogOnScreen(QWidget *dialog);
void prepareMenu(ElaMenu *menu, QWidget *owner);
QAction *executeMenu(ElaMenu &menu, QWidget *owner, const QPoint &position);
}

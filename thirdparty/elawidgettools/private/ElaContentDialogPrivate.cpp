#include "ElaContentDialogPrivate.h"

#include "ElaContentDialog.h"
#include "ElaMaskWidget.h"

#include <QApplication>
#include <QDebug>
#include <QScreen>
#include <QWindow>
ElaContentDialogPrivate::ElaContentDialogPrivate(QObject* parent)
    : QObject{parent}
{
}

ElaContentDialogPrivate::~ElaContentDialogPrivate()
{
}

void ElaContentDialogPrivate::_doCloseAnimation(bool isAccept)
{
    Q_Q(ElaContentDialog);
    isAccept ? q->accept() : q->reject();
}

void ElaContentDialogPrivate::_moveToCenter()
{
    Q_Q(ElaContentDialog);
    int width = q->width();
    int height = q->height();
    const QRect area = _maskWidget
        ? QRect(_maskWidget->mapToGlobal(QPoint()), _maskWidget->size())
        : q->screen()->availableGeometry();
    q->move(area.center() - QPoint(width / 2, height / 2));
}

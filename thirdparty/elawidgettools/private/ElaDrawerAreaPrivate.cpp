#include "ElaDrawerAreaPrivate.h"
#include "ElaDrawerArea.h"

ElaDrawerAreaPrivate::ElaDrawerAreaPrivate(QObject* parent)
    : QObject(parent)
{
}

ElaDrawerAreaPrivate::~ElaDrawerAreaPrivate()
{
}

void ElaDrawerAreaPrivate::onDrawerHeaderClicked(bool isExpand)
{
    Q_Q(ElaDrawerArea);
    q->setExpanded(isExpand);
}

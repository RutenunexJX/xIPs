#include "Branding.h"
#include <QResource>
static void initializeIconResources()
{
    Q_INIT_RESOURCE(xips);
}
QIcon xips::applicationIcon()
{
    static const bool initialized = []
    {
        initializeIconResources();
        return true;
    }();
    Q_UNUSED(initialized);
    return QIcon(QStringLiteral(":/xips/icons/xips.ico"));
}

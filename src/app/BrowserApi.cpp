#include "xips/BrowserApi.h"
#include "BrowserPanel.h"

extern "C" Q_DECL_EXPORT const char *xips_browser_abi_v1()
{
    static const QByteArray abi = xipsExpectedBrowserAbi();
    return abi.constData();
}

extern "C" Q_DECL_EXPORT QWidget *xips_create_browser_v1(QWidget *parent, QObject *host)
{
    return new xips::BrowserPanel(parent, host);
}

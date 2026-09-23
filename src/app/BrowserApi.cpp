#include "xips/BrowserApi.h"
#include "BrowserPanel.h"
#include "BuildCapabilities.h"

extern "C" Q_DECL_EXPORT const char *xips_browser_capabilities_v1()
{
    return xips::buildCapabilities;
}

extern "C" Q_DECL_EXPORT const char *xips_browser_abi_v1()
{
    static const QByteArray abi = xipsExpectedBrowserAbi();
    return abi.constData();
}

extern "C" Q_DECL_EXPORT QWidget *xips_create_browser_v1(QWidget *parent, QObject *host)
{
    return new xips::BrowserPanel(parent, host);
}

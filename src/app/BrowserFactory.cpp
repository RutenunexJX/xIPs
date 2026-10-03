#include "BrowserPanel.h"

// Private implementation export. The public v1 entry point is BrowserApi.cpp.
extern "C" Q_DECL_EXPORT QWidget *xips_create_browser_impl_v1(QWidget *parent, QObject *host)
{
    xips::initializeEla(true);
    return new xips::BrowserPanel(parent, host, true);
}

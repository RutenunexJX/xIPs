#ifndef ELAWORKSPACE_ELAWIDGETTOOLS_ELATREEVIEW_H_
#define ELAWORKSPACE_ELAWIDGETTOOLS_ELATREEVIEW_H_

#include <QTreeView>

#include "ElaWidgetToolsExport.h"
#include "ElaPropertyMacro.h"

class ElaTreeViewPrivate;
class ELA_EXPORT ElaTreeView : public QTreeView
{
    Q_OBJECT
    Q_Q_CREATE(ElaTreeView)
    Q_PROPERTY_CREATE_Q_H(int, ItemHeight)
    Q_PROPERTY_CREATE_Q_H(int, HeaderMargin)
public:
    explicit ElaTreeView(QWidget* parent = nullptr);
    ~ElaTreeView();
    // Reuse Ela rendering without replacing a host's QTreeWidget item model.
    static QStyle* createStyle(QObject* owner, int itemHeight = 28);
    void setNativeItemContent(bool enabled);
};

#endif // ELAWORKSPACE_ELAWIDGETTOOLS_ELATREEVIEW_H_

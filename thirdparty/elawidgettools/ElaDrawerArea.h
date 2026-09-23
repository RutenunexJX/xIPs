#ifndef ELAWORKSPACE_ELAWIDGETTOOLS_ELADRAWERAREA_H_
#define ELAWORKSPACE_ELAWIDGETTOOLS_ELADRAWERAREA_H_

#include "ElaWidgetToolsExport.h"
#include "ElaPropertyMacro.h"
#include <QWidget>

class ElaDrawerAreaPrivate;
class ELA_EXPORT ElaDrawerArea : public QWidget
{
    Q_OBJECT
    Q_Q_CREATE(ElaDrawerArea)
    Q_PROPERTY_CREATE_Q_H(int, BorderRadius)
    Q_PROPERTY_CREATE_Q_H(int, HeaderHeight)
public:
    explicit ElaDrawerArea(QWidget* parent = nullptr);
    ~ElaDrawerArea() override;

    void setDrawerHeader(QWidget* widget);

    void addDrawer(QWidget* widget);
    void removeDrawer(QWidget* widget);

    void expand();
    void collapse();

    void setExpanded(bool expanded, bool animate = true);
    void setDrawerHeaderVisible(bool visible);
    void setDrawerEdge(Qt::Edge edge);
    bool isDrawerAnimating() const;
    void finishDrawerAnimation();
    qint64 drawerSnapshotBytes() const;
    double drawerPreparationMs() const;
    qreal drawerProgress() const;

    bool getIsExpand() const;
Q_SIGNALS:
    Q_SIGNAL void expandStateChanged(bool isExpand);
    Q_SIGNAL void drawerAnimationFinished(bool isExpand);
};

#endif //ELAWORKSPACE_ELAWIDGETTOOLS_ELADRAWERAREA_H_

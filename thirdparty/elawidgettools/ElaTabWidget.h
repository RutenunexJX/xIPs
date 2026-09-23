#ifndef ELAWORKSPACE_ELAWIDGETTOOLS_ELATABWIDGET_H_
#define ELAWORKSPACE_ELAWIDGETTOOLS_ELATABWIDGET_H_

#include <QTabWidget>
#include <QDrag>
#include <functional>

#include "ElaPropertyMacro.h"
#include "ElaWidgetToolsExport.h"

class ElaTabWidgetPrivate;
class ElaTabWidgetHost;
class ELA_EXPORT ElaTabWidget : public QTabWidget
{
    Q_OBJECT
    Q_Q_CREATE(ElaTabWidget)
    Q_PROPERTY_CREATE_Q_H(bool, IsTabTransparent);
    Q_PROPERTY_CREATE_Q_H(bool, IsContainerAcceptDrops);
    Q_PROPERTY_CREATE_Q_H(QSize, TabSize)
    Q_PROPERTY_CREATE_Q_H(QSize, FloatWidgetSize)
public:
    explicit ElaTabWidget(QWidget* parent = nullptr);
    ~ElaTabWidget() override;
    void setTabPosition(TabPosition position);

    // Opt-in document hosting. Ela owns gestures, transfer and floating chrome;
    // the host owns document lifetime and resolves application split layouts.
    enum class DropArea { Center, Left, Right, Top, Bottom };
    using SplitResolver = std::function<ElaTabWidget*(ElaTabWidget*, DropArea)>;
    using ReturnTarget = std::function<ElaTabWidget*()>;
    void setHostedTabs(QObject* scope, SplitResolver split, ReturnTarget returnTarget);
    bool hasHostedTabs() const;
    bool isHostedTabDragging() const;
    bool isFloatingTabWidget() const;
    void setHostedTabBar(class ElaTabBar* bar);
    bool transferHostedTab(QWidget* page, ElaTabWidget* target, int index = -1);
    ElaTabWidget* floatHostedTab(QWidget* page, const QPoint& globalPosition);
    void syncFloatingVisibility();
    void disposeEmptyFloatingWindow();

Q_SIGNALS:
    Q_SIGNAL void currentWidgetChanged(QWidget* widget);
    Q_SIGNAL void floatingTabWidgetCreated(ElaTabWidget* group);
    Q_SIGNAL void hostedTabMoved(QWidget* page, ElaTabWidget* source, ElaTabWidget* target);
    Q_SIGNAL void hostedTabDragStarted(QDrag* drag);
    Q_SIGNAL void hostedTabDragFinished();

protected:
    void paintEvent(QPaintEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void tabInserted(int index) override;

private:
    friend class ElaCustomTabWidget;
    friend class ElaTabWidgetHost;
    friend class ElaHostedTabWindow;
    ElaTabWidgetHost* _host{nullptr};
};

#endif // ELAWORKSPACE_ELAWIDGETTOOLS_ELATABWIDGET_H_

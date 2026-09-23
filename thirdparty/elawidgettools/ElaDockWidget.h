#ifndef ELAWORKSPACE_ELAWIDGETTOOLS_ELADOCKWIDGET_H_
#define ELAWORKSPACE_ELAWIDGETTOOLS_ELADOCKWIDGET_H_

#include <QDockWidget>

#include "ElaWidgetToolsExport.h"
#include "ElaPropertyMacro.h"
class ElaDockWidgetPrivate;
class ELA_EXPORT ElaDockWidget : public QDockWidget
{
    Q_OBJECT
    Q_Q_CREATE(ElaDockWidget)
public:
    explicit ElaDockWidget(QWidget* parent = nullptr, Qt::WindowFlags flags = Qt::WindowFlags());
    explicit ElaDockWidget(const QString& title, QWidget* parent = nullptr, Qt::WindowFlags flags = Qt::WindowFlags());
    ~ElaDockWidget() override;
    // Resume a drag that started in an application's resource section header.
    void beginDockDrag(const QPoint& globalPosition);
    bool isDockDragging() const;
    void cancelDockDrag();
signals:
    void dockDragStarted();
    void dockDragMoved(const QPoint& globalPosition);
    void dockDragFinished(bool cancelled);

protected:
    virtual void paintEvent(QPaintEvent* event) override;
    virtual bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
#ifdef Q_OS_WIN
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    virtual bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
#else
    virtual bool nativeEvent(const QByteArray& eventType, void* message, long* result) override;
#endif
#endif
};

#endif // ELAWORKSPACE_ELAWIDGETTOOLS_ELADOCKWIDGET_H_

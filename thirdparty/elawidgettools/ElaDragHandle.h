#pragma once

#include "ElaWidgetToolsExport.h"
#include <QObject>
#include <QPointer>
#include <QPoint>
#include <functional>

class QWidget;
class QMimeData;

// Reusable Ela gesture controller for hosted panels. Without MIME data it
// reports a reorder/detach gesture; with a factory it owns the native drag loop.
class ELA_EXPORT ElaDragHandle final : public QObject {
    Q_OBJECT
public:
    ElaDragHandle(QWidget* handle, QObject* source);
    void setMimeDataFactory(std::function<QMimeData*()> factory);
signals:
    void pressed(const QPoint& globalPosition);
    void moved(const QPoint& globalPosition);
    void released(const QPoint& globalPosition, bool dragged);
    void cancelled();
    void dragStarted();
    void dragFinished(bool accepted);
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
private:
    QPointer<QWidget> handle;
    QPointer<QObject> source;
    std::function<QMimeData*()> factory;
    QPoint start;
    bool down{false};
    bool movedEnough{false};
};

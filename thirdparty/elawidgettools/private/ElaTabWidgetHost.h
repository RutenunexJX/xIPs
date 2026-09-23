#pragma once

#include "ElaTabWidget.h"
#include <QPointer>

class ElaHostedTabWindow;
class ElaTabDropIndicator;

class ElaTabWidgetHost final : public QObject
{
public:
    ElaTabWidgetHost(ElaTabWidget* group, QObject* scope,
                     ElaTabWidget::SplitResolver split, ElaTabWidget::ReturnTarget returnTarget);
    ~ElaTabWidgetHost() override;
    bool transfer(QWidget* page, ElaTabWidget* target, int index);
    ElaTabWidget* detach(QWidget* page, const QPoint& globalPosition);
    void syncVisibility();
    bool isDragging() const { return dragging; }
    QPointer<QWidget> floatingWindow;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    friend class ElaHostedTabWindow;
    QPointer<ElaTabWidget> group;
    QPointer<QObject> scope;
    ElaTabWidget::SplitResolver split;
    ElaTabWidget::ReturnTarget returnTarget;
    QPointer<QWidget> pressedPage;
    QPoint pressPosition;
    QPointer<QWidget> indicator;
    bool dragging{false};
    bool cancelled{false};

    void startDrag();
    bool belongsToGroup(QObject* watched) const;
    ElaTabWidget::DropArea areaAt(const QPoint& position) const;
    void showIndicator(ElaTabWidget::DropArea area);
    void clearIndicator();
};

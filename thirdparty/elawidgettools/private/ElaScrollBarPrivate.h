#ifndef ELAWORKSPACE_ELAWIDGETTOOLS_PRIVATE_ELASCROLLBARPRIVATE_H_
#define ELAWORKSPACE_ELAWIDGETTOOLS_PRIVATE_ELASCROLLBARPRIVATE_H_

#include <QAbstractScrollArea>
#include <QObject>
#include <QScrollBar>

#include "ElaWidgetToolsExport.h"
#include "ElaPropertyMacro.h"
class QTimer;
class QPropertyAnimation;
class ElaScrollBar;
class ElaScrollBarPrivate : public QObject
{
    Q_OBJECT
    Q_D_CREATE(ElaScrollBar)
    Q_PROPERTY_CREATE_D(bool, IsAnimation)
    Q_PROPERTY_CREATE_D(qreal, SpeedLimit)
    Q_PROPERTY_CREATE(int, TargetMaximum)
    Q_PROPERTY_CREATE(qreal, WheelValue)
public:
    explicit ElaScrollBarPrivate(QObject* parent = nullptr);
    ~ElaScrollBarPrivate();
    Q_SLOT void onRangeChanged(int min, int max);

private:
    QScrollBar* _originScrollBar{nullptr};
    QAbstractScrollArea* _originScrollArea{nullptr};
    QTimer* _expandTimer{nullptr};
    bool _isExpand{false};
    QPropertyAnimation* _slideSmoothAnimation{nullptr};
    qreal _scrollValue{-1};
    bool _smoothWheelEnabled{false};
    bool _writingWheelValue{false};
    void _scroll(Qt::KeyboardModifiers modifiers, int value);
    int _pixelPosToRangeValue(int pos) const;

    // 映射处理函数
    void _initAllConfig();
    void _handleScrollBarValueChanged(QScrollBar* scrollBar, int value);
    void _handleScrollBarRangeChanged(int min, int max);
    void _handleScrollBarGeometry();
};

#endif // ELAWORKSPACE_ELAWIDGETTOOLS_PRIVATE_ELASCROLLBARPRIVATE_H_

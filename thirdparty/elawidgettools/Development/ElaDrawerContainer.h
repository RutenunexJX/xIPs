#ifndef ELAWORKSPACE_ELAWIDGETTOOLS_DEVELOPERCOMPONENTS_ELADRAWERCONTAINER_H_
#define ELAWORKSPACE_ELAWIDGETTOOLS_DEVELOPERCOMPONENTS_ELADRAWERCONTAINER_H_

#include <QWidget>

#include "ElaWidgetToolsDef.h"
#include <QVBoxLayout>

#include <QGraphicsOpacityEffect>
#include <QPointer>
class ElaDrawerContainer : public QWidget
{
    Q_OBJECT
    Q_PRIVATE_CREATE(int, BorderRadius)
    Q_PRIVATE_REF_CREATE(QPixmap, ContainerPix)
    Q_PROPERTY_CREATE(qreal, Opacity)
public:
    explicit ElaDrawerContainer(QWidget* parent = nullptr);
    ~ElaDrawerContainer() override;

    void addWidget(QWidget* widget);
    void removeWidget(QWidget* widget);

    void doDrawerAnimation(bool isExpand, bool animate = true);
    void finishAnimation();
    bool isAnimating() const;
    bool isExpanded() const { return _expanded; }
    void setEdge(Qt::Edge edge);
    qint64 snapshotBytes() const;
    double preparationMs() const { return _preparationMs; }
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void animationFinished(bool expanded);

protected:
    virtual void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    bool _isShowBorder{true};
    ElaThemeType::ThemeMode _themeMode;
    QVBoxLayout* _containerLayout{nullptr};
    QWidget* _containerWidget{nullptr};
    QList<QPointer<QWidget>> _drawerWidgetList;
    class QPropertyAnimation* _animation{nullptr};
    Qt::Edge _edge{Qt::TopEdge};
    bool _expanded{false};
    bool _preparing{false};
    bool _settling{false};
    double _preparationMs{0};
};

#endif //ELAWORKSPACE_ELAWIDGETTOOLS_DEVELOPERCOMPONENTS_ELADRAWERCONTAINER_H_

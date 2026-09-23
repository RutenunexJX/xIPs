#ifndef ELAWORKSPACE_ELAWIDGETTOOLS_ELATABBAR_H_
#define ELAWORKSPACE_ELAWIDGETTOOLS_ELATABBAR_H_

#include <QDrag>
#include <QTabBar>

#include "ElaPropertyMacro.h"
#include "ElaWidgetToolsExport.h"
class ElaTabBarPrivate;
class ELA_EXPORT ElaTabBar : public QTabBar
{
    Q_OBJECT
    Q_Q_CREATE(ElaTabBar)
    Q_PROPERTY_CREATE_Q_H(QSize, TabSize)
    Q_PROPERTY(bool smoothScrollEnabled READ smoothScrollEnabled WRITE setSmoothScrollEnabled)
public:
    explicit ElaTabBar(QWidget* parent = nullptr);
    ~ElaTabBar() override;

    // Hosts with their own document controller retain Qt layout and input semantics.
    void setNativeTabBehavior(bool enabled);
    bool nativeTabBehavior() const;
    void setSmoothScrollEnabled(bool enabled);
    bool smoothScrollEnabled() const;
    void setTabText(int index, const QString& text);

Q_SIGNALS:
    Q_SIGNAL void tabBarTextChanged(int tabIndex, const QString& tabText);
    Q_SIGNAL void tabDragCreate(QMimeData* mimeData);
    Q_SIGNAL void tabDragEnter(QMimeData* mimeData);
    Q_SIGNAL void tabDragLeave(QMimeData* mimeData);
    Q_SIGNAL void tabDragDrop(QMimeData* mimeData);

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    void tabInserted(int index) override;
    void tabRemoved(int index) override;
    void resizeEvent(QResizeEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    bool _nativeTabBehavior{false};
    bool _smoothScrollEnabled{false};
    int _lastScrollOffset{0};
    int smoothScrollMaximum() const;
    void stopSmoothScroll();
};

#endif // ELAWORKSPACE_ELAWIDGETTOOLS_ELATABBAR_H_

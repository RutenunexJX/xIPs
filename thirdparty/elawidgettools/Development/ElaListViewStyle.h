#ifndef ELAWORKSPACE_ELAWIDGETTOOLS_DEVELOPERCOMPONENTS_ELALISTVIEWSTYLE_H_
#define ELAWORKSPACE_ELAWIDGETTOOLS_DEVELOPERCOMPONENTS_ELALISTVIEWSTYLE_H_

#include <QProxyStyle>

#include "ElaWidgetToolsDef.h"
class ElaListViewStyle : public QProxyStyle
{
    Q_OBJECT
    Q_PROPERTY_CREATE(int, ItemHeight)
    Q_PROPERTY_CREATE(bool, IsTransparent)
public:
    explicit ElaListViewStyle(QStyle* style = nullptr);
    ~ElaListViewStyle();
    void setNativeItemContent(bool enabled) { _nativeItemContent = enabled; }
    void drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter, const QWidget* widget = nullptr) const override;
    void drawControl(ControlElement element, const QStyleOption* option, QPainter* painter, const QWidget* widget = nullptr) const override;
    QSize sizeFromContents(ContentsType type, const QStyleOption* option, const QSize& size, const QWidget* widget) const override;
    QRect subElementRect(SubElement element, const QStyleOption* option, const QWidget* widget) const override;

private:
    bool _nativeItemContent{false};
    ElaThemeType::ThemeMode _themeMode;
    int _leftPadding{11};
};

#endif // ELAWORKSPACE_ELAWIDGETTOOLS_DEVELOPERCOMPONENTS_ELALISTVIEWSTYLE_H_

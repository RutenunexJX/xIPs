#include "ElaPushButton.h"

#include "ElaApplication.h"
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QStyleOptionButton>

#include "ElaTheme.h"
#include "private/ElaPushButtonPrivate.h"
Q_PROPERTY_CREATE_Q_CPP(ElaPushButton, int, BorderRadius)
Q_PROPERTY_REF_CREATE_Q_CPP(ElaPushButton, QColor, LightDefaultColor)
Q_PROPERTY_REF_CREATE_Q_CPP(ElaPushButton, QColor, DarkDefaultColor)
Q_PROPERTY_REF_CREATE_Q_CPP(ElaPushButton, QColor, LightHoverColor)
Q_PROPERTY_REF_CREATE_Q_CPP(ElaPushButton, QColor, DarkHoverColor)
Q_PROPERTY_REF_CREATE_Q_CPP(ElaPushButton, QColor, LightPressColor)
Q_PROPERTY_REF_CREATE_Q_CPP(ElaPushButton, QColor, DarkPressColor)
ElaPushButton::ElaPushButton(QWidget* parent)
    : QPushButton(parent), d_ptr(new ElaPushButtonPrivate())
{
    Q_D(ElaPushButton);
    d->q_ptr = this;
    d->_pBorderRadius = 3;
    d->_themeMode = eTheme->getThemeMode();
    d->_pLightDefaultColor = ElaThemeColor(ElaThemeType::Light, BasicBase);
    d->_pDarkDefaultColor = ElaThemeColor(ElaThemeType::Dark, BasicBase);
    d->_pLightHoverColor = ElaThemeColor(ElaThemeType::Light, BasicHover);
    d->_pDarkHoverColor = ElaThemeColor(ElaThemeType::Dark, BasicHover);
    d->_pLightPressColor = ElaThemeColor(ElaThemeType::Light, BasicPress);
    d->_pDarkPressColor = ElaThemeColor(ElaThemeType::Dark, BasicPress);
    d->_lightTextColor = ElaThemeColor(ElaThemeType::Light, BasicText);
    d->_darkTextColor = ElaThemeColor(ElaThemeType::Dark, BasicText);
    setMouseTracking(true);
    setFixedHeight(38);
    QFont font = this->font();
    font.setPixelSize(eApp->getFontPixelSize() + 2);
    setFont(font);
    setObjectName("ElaPushButton");
    setStyleSheet("#ElaPushButton{background-color:transparent;}");
    connect(eTheme, &ElaTheme::themeModeChanged, this, [=](ElaThemeType::ThemeMode themeMode) {
        d->_themeMode = themeMode;
        update();
    });
}

ElaPushButton::ElaPushButton(const QString& text, QWidget* parent)
    : ElaPushButton(parent)
{
    setText(text);
}

ElaPushButton::~ElaPushButton()
{
}

void ElaPushButton::setLightTextColor(const QColor& color)
{
    Q_D(ElaPushButton);
    d->_lightTextColor = color;
}

const QColor& ElaPushButton::getLightTextColor() const
{
    Q_D(const ElaPushButton);
    return d->_lightTextColor;
}

void ElaPushButton::setDarkTextColor(const QColor& color)
{
    Q_D(ElaPushButton);
    d->_darkTextColor = color;
}

const QColor& ElaPushButton::getDarkTextColor() const
{
    Q_D(const ElaPushButton);
    return d->_darkTextColor;
}

void ElaPushButton::mousePressEvent(QMouseEvent* event)
{
    Q_D(ElaPushButton);
    d->_isPressed = true;
    QPushButton::mousePressEvent(event);
}

void ElaPushButton::mouseReleaseEvent(QMouseEvent* event)
{
    Q_D(ElaPushButton);
    d->_isPressed = false;
    QPushButton::mouseReleaseEvent(event);
}

void ElaPushButton::paintEvent(QPaintEvent* event)
{
    Q_D(ElaPushButton);
    QPainter painter(this);
    painter.setRenderHints(QPainter::SmoothPixmapTransform | QPainter::Antialiasing | QPainter::TextAntialiasing);
    // 高性能阴影
    eTheme->drawEffectShadow(&painter, rect(), d->_shadowBorderWidth, d->_pBorderRadius);

    // 背景绘制
    painter.save();
    QRect foregroundRect(d->_shadowBorderWidth, d->_shadowBorderWidth, width() - 2 * (d->_shadowBorderWidth), height() - 2 * d->_shadowBorderWidth);
    if (d->_themeMode == ElaThemeType::Light)
    {
        painter.setPen(ElaThemeColor(ElaThemeType::Light, BasicBorder));
        painter.setBrush(isEnabled() ? (isDown() || isChecked()) ? d->_pLightPressColor : (underMouse() ? d->_pLightHoverColor : d->_pLightDefaultColor) : ElaThemeColor(d->_themeMode, BasicDisable));
    }
    else
    {
        painter.setPen(Qt::NoPen);
        painter.setBrush(isEnabled() ? (isDown() || isChecked()) ? d->_pDarkPressColor : (underMouse() ? d->_pDarkHoverColor : d->_pDarkDefaultColor) : ElaThemeColor(d->_themeMode, BasicDisable));
    }
    painter.drawRoundedRect(foregroundRect, d->_pBorderRadius, d->_pBorderRadius);
    // 底边线绘制
    if (!isDown())
    {
        painter.setPen(ElaThemeColor(d->_themeMode, BasicBaseLine));
        painter.drawLine(foregroundRect.x() + d->_pBorderRadius, height() - d->_shadowBorderWidth, foregroundRect.width(), height() - d->_shadowBorderWidth);
    }
    //文字绘制
    painter.setPen(isEnabled() ? d->_themeMode == ElaThemeType::Light ? d->_lightTextColor : d->_darkTextColor : ElaThemeColor(d->_themeMode, BasicTextDisable));
    QStyleOptionButton option;
    initStyleOption(&option);
    const int mnemonic = style()->styleHint(QStyle::SH_UnderlineShortcut, &option, this)
        ? Qt::TextShowMnemonic : Qt::TextHideMnemonic;
    const int gap = icon().isNull() || text().isEmpty() ? 0 : 6;
    const QSize imageSize = icon().isNull() ? QSize(0, 0) : iconSize();
    const int textWidth = fontMetrics().size(mnemonic, text()).width();
    QRect content = foregroundRect.adjusted(8, 0, -8, 0);
    const int contentWidth = qMin(content.width(), imageSize.width() + gap + textWidth);
    QRect label(content.center().x() - contentWidth / 2, content.y(), contentWidth, content.height());
    if (!icon().isNull()) {
        QRect imageRect(label.x(), label.center().y() - imageSize.height() / 2,
                        imageSize.width(), imageSize.height());
        imageRect = QStyle::visualRect(layoutDirection(), foregroundRect, imageRect);
        icon().paint(&painter, imageRect, Qt::AlignCenter,
                     isEnabled() ? QIcon::Normal : QIcon::Disabled,
                     isChecked() ? QIcon::On : QIcon::Off);
        label.setLeft(label.left() + imageSize.width() + gap);
    }
    label = QStyle::visualRect(layoutDirection(), foregroundRect, label);
    painter.drawText(label, Qt::AlignCenter | mnemonic, text());
    if (isEnabled() && (hasFocus() || isDefault())) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(ElaThemeColor(d->_themeMode, PrimaryNormal), hasFocus() ? 1.5 : 1.0));
        painter.drawRoundedRect(foregroundRect.adjusted(1, 1, -1, -1), d->_pBorderRadius, d->_pBorderRadius);
    }
    painter.restore();
}

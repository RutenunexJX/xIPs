#include "ElaRadioButton.h"

#include "ElaApplication.h"
#include "ElaRadioButtonStyle.h"
#include "ElaTheme.h"
#include "private/ElaRadioButtonPrivate.h"
ElaRadioButton::ElaRadioButton(QWidget* parent)
    : QRadioButton(parent), d_ptr(new ElaRadioButtonPrivate())
{
    Q_D(ElaRadioButton);
    d->q_ptr = this;
    setFixedHeight(20);
    QFont font = this->font();
    font.setPixelSize(eApp->getFontPixelSize() + 2);
    setFont(font);
    auto* radioStyle = new ElaRadioButtonStyle(style());
    radioStyle->setParent(this);
    setStyle(radioStyle);
    d->onThemeChanged(eTheme->getThemeMode());
    connect(eTheme, &ElaTheme::themeModeChanged, d, &ElaRadioButtonPrivate::onThemeChanged);
}

ElaRadioButton::ElaRadioButton(const QString& text, QWidget* parent)
    : ElaRadioButton(parent)
{
    setText(text);
}

ElaRadioButton::~ElaRadioButton()
{
    setStyle(nullptr);
}

void ElaRadioButton::paintEvent(QPaintEvent* event)
{
    Q_D(ElaRadioButton);
    if (palette().color(QPalette::Active, QPalette::WindowText) != ElaThemeColor(d->_themeMode, BasicText))
    {
        d->onThemeChanged(d->_themeMode);
    }
    QRadioButton::paintEvent(event);
}

#include "ElaRadioButtonPrivate.h"

#include "ElaRadioButton.h"
#include "ElaTheme.h"
ElaRadioButtonPrivate::ElaRadioButtonPrivate(QObject* parent)
    : QObject(parent)
{
}

ElaRadioButtonPrivate::~ElaRadioButtonPrivate()
{
}

void ElaRadioButtonPrivate::onThemeChanged(ElaThemeType::ThemeMode themeMode)
{
    Q_Q(ElaRadioButton);
    _themeMode = themeMode;
    QPalette palette = q->palette();
    palette.setColor(QPalette::Text, ElaThemeColor(_themeMode, BasicText));
    palette.setColor(QPalette::WindowText, ElaThemeColor(themeMode, BasicText));
    palette.setColor(QPalette::Disabled, QPalette::Text, ElaThemeColor(themeMode, BasicTextDisable));
    palette.setColor(QPalette::Disabled, QPalette::WindowText, ElaThemeColor(themeMode, BasicTextDisable));
    q->setPalette(palette);
}

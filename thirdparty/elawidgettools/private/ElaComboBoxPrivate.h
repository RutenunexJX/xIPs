#ifndef ELAWORKSPACE_ELAWIDGETTOOLS_PRIVATE_ELACOMBOBOXPRIVATE_H_
#define ELAWORKSPACE_ELAWIDGETTOOLS_PRIVATE_ELACOMBOBOXPRIVATE_H_

#include <QObject>
#include <QPointer>
#include <QPoint>

#include "ElaWidgetToolsDef.h"

class QLineEdit;
class ElaComboBox;
class ElaComboBoxStyle;
class QParallelAnimationGroup;
class QPropertyAnimation;
class QWidget;
class ElaComboBoxPrivate : public QObject
{
    Q_OBJECT
    Q_D_CREATE(ElaComboBox);
    Q_PROPERTY_CREATE_D(int, BorderRadius)

public:
    explicit ElaComboBoxPrivate(QObject* parent = nullptr);
    ~ElaComboBoxPrivate() override;

    Q_SLOT void onThemeChanged(ElaThemeType::ThemeMode themeMode);

private:
    QParallelAnimationGroup* _popupAnimation{nullptr};
    QParallelAnimationGroup* _indicatorAnimation{nullptr};
    QPropertyAnimation* _rotation{nullptr};
    QPropertyAnimation* _mark{nullptr};
    QPointer<QWidget> _popup;
    QPoint _viewPosition;
    int _popupHeight{0};
    bool _settling{false};
    ElaComboBoxStyle* _comboBoxStyle{nullptr};
    ElaThemeType::ThemeMode _themeMode;
};

#endif // ELAWORKSPACE_ELAWIDGETTOOLS_PRIVATE_ELACOMBOBOXPRIVATE_H_

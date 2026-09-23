#ifndef ELAWORKSPACE_ELAWIDGETTOOLS_ELACOMBOBOX_H_
#define ELAWORKSPACE_ELAWIDGETTOOLS_ELACOMBOBOX_H_

#include <QComboBox>

#include "ElaWidgetToolsExport.h"
#include "ElaPropertyMacro.h"

class ElaComboBoxPrivate;
class ELA_EXPORT ElaComboBox : public QComboBox
{
    Q_OBJECT
    Q_Q_CREATE(ElaComboBox);
    Q_PROPERTY_CREATE_Q_H(int, BorderRadius)
public:
    explicit ElaComboBox(QWidget* parent = nullptr);
    ~ElaComboBox() override;

    void setEditable(bool editable);
    bool isPopupAnimating() const;
    void finishPopupAnimation();

protected:
    virtual void showPopup() override;
    virtual void hidePopup() override;
    virtual void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void animateIndicator(bool expanded);
};

#endif // ELAWORKSPACE_ELAWIDGETTOOLS_ELACOMBOBOX_H_

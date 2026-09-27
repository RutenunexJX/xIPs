#pragma once

#include "ElaContentDialog.h"
#include <QStringList>

class ElaComboBox;
class ElaLineEdit;
class ElaListView;
class ElaPushButton;
class ElaText;
class ElaToolButton;
class QFileSystemModel;

namespace xips
{
class ElaFilePicker final : public ElaContentDialog
{
  public:
    enum class Mode { Directory, File, Files };
    ElaFilePicker(QWidget *parent, const QString &title, const QString &initial, Mode mode);
    QStringList selectedPaths() const { return m_selected; }
    static QString getExistingDirectory(QWidget *parent, const QString &title, const QString &initial);
    static QString getOpenFileName(QWidget *parent, const QString &title, const QString &initial);
    static QStringList getOpenFileNames(QWidget *parent, const QString &title, const QString &initial);

  private:
    void navigate(const QString &path);
    void submit();
    void applyTheme();
    void selectionChanged();
    QString absoluteInput() const;
    Mode m_mode;
    QString m_directory;
    QStringList m_selected;
    QFileSystemModel *m_model;
    ElaLineEdit *m_path;
    ElaListView *m_entries;
    ElaComboBox *m_drives;
    ElaToolButton *m_up;
    ElaPushButton *m_accept;
    ElaText *m_message;
};
}

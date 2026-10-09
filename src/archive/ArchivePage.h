#pragma once
#include "ArchiveEngine.h"
#include <QWidget>
#include <QVariantMap>
#include <QPointer>
#include <functional>
class QLineEdit;
class QComboBox;
class QLabel;
class QPushButton;
class QProgressBar;
class QPlainTextEdit;
class QTableWidget;

namespace xips::archive {
class ArchiveBatchPanel;
using InstallationProvider = std::function<QList<Installation>()>;
class ArchivePage final : public QWidget {
    Q_OBJECT
public:
    explicit ArchivePage(QWidget* parent = nullptr, InstallationProvider installed = {});
    ~ArchivePage() override;
    QVariantMap saveState() const;
    void restoreState(const QVariantMap& state);
    QString closeBlockReason() const;
    bool isBusy() const { return busy_ || scanner_; }
    void requestCancel();
    void setAppearancePalette(const QPalette& palette);
signals:
    void stateChanged();
    void busyChanged(bool busy);
    void installationsChanged();
protected:
    void changeEvent(QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
private:
    void selectProject(const QString& path);
    void discover();
    void configure();
    void refreshVersions(const QString& preferred = {});
    void startArchive();
    void startNextArchive();
    void finishQueue();
    void setBusy(bool busy);
    void setStatus(const QString& text, bool error = false);
    void refreshActions();
    void applyAppearance();
    QPalette appearancePalette_;
    QList<Installation> installations_;
    InstallationProvider installed_;
    QComboBox* inputMode_;
    ArchiveBatchPanel* batch_;
    QWidget* singleProject_;
    QWidget* singleName_;
    QLabel* batchVersions_;
    QLineEdit* project_;
    QLineEdit* output_;
    QLineEdit* name_;
    QLabel* nameHint_;
    QComboBox* versions_;
    QLabel* projectInfo_;
    QLabel* status_;
    QLabel* result_;
    QPushButton* browse_;
    QPushButton* outputBrowse_;
    QPushButton* configure_;
    QPushButton* start_ = nullptr;
    QPushButton* cancel_;
    QPushButton* open_;
    QPushButton* details_;
    QWidget* detailsPanel_;
    QWidget* progressPanel_;
    QLabel* progressTitle_;
    QLabel* percent_;
    QList<QLabel*> stageNames_;
    QList<QProgressBar*> stages_;
    QProgressBar* overall_;
    QPlainTextEdit* log_;
    QPointer<ArchiveJob> job_;
    QThread* scanner_ = nullptr;
    QString resultDirectory_;
    QString detectedVersion_;
    bool busy_ = false;
    bool batchActive_ = false;
    bool cancelQueue_ = false;
    QList<Request> requests_;
    QList<int> batchRows_;
    int queueIndex_ = 0;
    int successes_ = 0;
    int failures_ = 0;
    int activeStage_ = -1;
};
}

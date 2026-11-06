#pragma once
#include "ElaContentDialog.h"
#include "library/SnapshotLibrary.h"
#include <memory>
class ElaComboBox;
class ElaText;
class ElaTableView;
class ElaPushButton;
class ElaPlainTextEdit;
class QStandardItemModel;
namespace xips
{
class BrowserPanel;
class OperationControl;
class RevisionCompareDialog final : public ElaContentDialog
{
  public:
    RevisionCompareDialog(BrowserPanel *panel, const CatalogAsset &asset, const QString &target,
                          const QString &base = {});
    ~RevisionCompareDialog() override;
    void done(int result) override;
  protected:
    void showEvent(QShowEvent *event) override;
  private:
    void load();
    void populate();
    void updateActions();
    void openSide(bool base);
    void showTextDifference();
    void cancelRead();
    CatalogAsset m_asset;
    SnapshotResult m_result;
    int m_generation = 0;
    bool m_busy = false;
    std::shared_ptr<OperationControl> m_operation;
    ElaComboBox *m_base, *m_target, *m_filter;
    ElaText *m_status;
    ElaTableView *m_files;
    QStandardItemModel *m_model;
    ElaPushButton *m_openBase, *m_openTarget, *m_textButton;
    ElaPlainTextEdit *m_text;
};
}

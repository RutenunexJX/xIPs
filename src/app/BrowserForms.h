#pragma once
#include "BrowserPanel.h"
#include "UiSupport.h"
#include "ElaLineEdit.h"
#include "ElaPlainTextEdit.h"
#include "ElaContentDialog.h"
#include "ElaComboBox.h"
#include "ElaText.h"
#include "ElaPushButton.h"
#include "ElaMenu.h"
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>
#include <QShowEvent>
#include <QResizeEvent>
#include <type_traits>
namespace xips {
template <class Editor> void editingMenu(Editor *editor, const QPoint &position)
{
    auto *menu = new ElaMenu(editor);
    menu->setObjectName("xipsEditMenu");
    menu->setAttribute(Qt::WA_DeleteOnClose);
    prepareMenu(menu, editor);
    bool selection, undo, redo, empty;
    if constexpr (std::is_base_of_v<QLineEdit, Editor>)
    {
        selection = editor->hasSelectedText();
        undo = editor->isUndoAvailable();
        redo = editor->isRedoAvailable();
        empty = editor->text().isEmpty();
    }
    else
    {
        selection = editor->textCursor().hasSelection();
        undo = editor->document()->isUndoAvailable();
        redo = editor->document()->isRedoAvailable();
        empty = editor->document()->isEmpty();
    }
    const bool writable = !editor->isReadOnly();
    const auto add =
        [&](const QString &text, const QKeySequence &shortcut, auto operation, bool enabled)
    {
        auto *action = menu->addAction(text);
        action->setShortcut(shortcut);
        action->setEnabled(enabled);
        QObject::connect(action, &QAction::triggered, editor, operation);
    };
    add(QStringLiteral("Undo"), QKeySequence::Undo, [editor] { editor->undo(); }, writable && undo);
    add(QStringLiteral("Redo"), QKeySequence::Redo, [editor] { editor->redo(); }, writable && redo);
    menu->addSeparator();
    add(
        QStringLiteral("Cut"), QKeySequence::Cut, [editor] { editor->cut(); },
        writable && selection);
    add(QStringLiteral("Copy"), QKeySequence::Copy, [editor] { editor->copy(); }, selection);
    add(
        QStringLiteral("Paste"), QKeySequence::Paste, [editor] { editor->paste(); },
        writable && !QApplication::clipboard()->text().isEmpty());
    add(
        QStringLiteral("Delete"), {},
        [editor]
        {
            if constexpr (std::is_base_of_v<QLineEdit, Editor>)
                editor->del();
            else
            {
                auto cursor = editor->textCursor();
                cursor.removeSelectedText();
            }
        },
        writable && selection);
    menu->addSeparator();
    add(
        QStringLiteral("Select all"), QKeySequence::SelectAll, [editor] { editor->selectAll(); },
        !empty);
    menu->popup(position);
}
class EnglishLineEdit final : public ElaLineEdit
{
  public:
    explicit EnglishLineEdit(QWidget *parent = nullptr) : ElaLineEdit(parent)
    {
        setTextMargins(10, 0, 6, 0);
        setFixedHeight(30);
    }

  protected:
    void contextMenuEvent(QContextMenuEvent *event) override
    {
        editingMenu(this, event->globalPos());
    }
};
class EnglishPlainTextEdit final : public ElaPlainTextEdit
{
  public:
    explicit EnglishPlainTextEdit(QWidget *parent = nullptr) : ElaPlainTextEdit(parent)
    {
        enableSmoothScrolling(this);
    }

  protected:
    void contextMenuEvent(QContextMenuEvent *event) override
    {
        editingMenu(this, event->globalPos());
    }
};
class Form final : public ElaContentDialog
{
  public:
    QVBoxLayout *body;
    ElaPushButton *acceptButton;
    Form(QWidget *parent, const QString &title, const QString &action)
        : ElaContentDialog(parent->window())
    {
        setObjectName("xipsForm");
        const auto detach = [this] { reject(); setParent(nullptr); };
        connect(parent, &QObject::destroyed, this, detach);
        if (auto *panel = qobject_cast<BrowserPanel *>(parent))
            connect(panel, &BrowserPanel::contextChanging, this, &QDialog::reject);
        if (parent != parent->window())
            connect(parent->window(), &QObject::destroyed, this, detach);
        setWindowTitle(title);
        setFont(parent->font());
        setStandardButtonsVisible(false);
        auto *content = new QWidget(this);
        auto *layout = new QVBoxLayout(content);
        layout->setContentsMargins(14, 12, 14, 12);
        layout->setSpacing(8);
        auto *heading = new ElaText(title, 16, content);
        heading->setTextFormat(Qt::PlainText);
        layout->addWidget(heading);
        body = new QVBoxLayout;
        body->setSpacing(8);
        layout->addLayout(body);
        auto *buttons = new QHBoxLayout;
        buttons->addStretch();
        auto *cancel = new ElaPushButton(QStringLiteral("Cancel"), content);
        acceptButton = new ElaPushButton(action, content);
        acceptButton->setObjectName("formAccept");
        acceptButton->setDefault(true);
        primaryButton(acceptButton);
        buttons->addWidget(cancel);
        buttons->addWidget(acceptButton);
        layout->addLayout(buttons);
        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        connect(acceptButton, &QPushButton::clicked, this, &QDialog::accept);
        setCentralWidget(content);
        setMinimumWidth(400);
    }
    int exec() override
    {
        for (auto *control : findChildren<QWidget *>())
            if (qobject_cast<ElaPushButton *>(control) || qobject_cast<ElaComboBox *>(control))
            {
                control->setFixedHeight(30);
                control->setFont(font());
                if (auto *button = qobject_cast<ElaPushButton *>(control))
                    button->setMinimumWidth(button->fontMetrics().horizontalAdvance(button->text()) + 28);
            }
        return ElaContentDialog::exec();
    }
    void showEvent(QShowEvent *event) override
    {
        ElaContentDialog::showEvent(event);
        keepDialogOnScreen(this);
        QTimer::singleShot(0, this, [this] { keepDialogOnScreen(this); });
    }
    void resizeEvent(QResizeEvent *event) override
    {
        ElaContentDialog::resizeEvent(event);
        if (isVisible()) keepDialogOnScreen(this);
    }
    void message(const QString &text)
    {
        auto *label = new ElaText(text, this);
        label->setTextPixelSize(13);
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        body->addWidget(label);
    }
};
}

#include "ElaPlainTextEdit.h"

#include <QClipboard>
#include <QApplication>
#include <QGuiApplication>
#include <QMimeData>
#include <QPainter>
#include <QPropertyAnimation>

#include "ElaEventBus.h"
#include "ElaMenu.h"
#include "ElaPlainTextEditPrivate.h"
#include "ElaPlainTextEditStyle.h"
#include "ElaScrollBar.h"
#include "ElaTheme.h"
ElaPlainTextEdit::ElaPlainTextEdit(QWidget* parent)
    : QPlainTextEdit(parent), d_ptr(new ElaPlainTextEditPrivate())
{
    Q_D(ElaPlainTextEdit);
    d->q_ptr = this;
    setObjectName("ElaPlainTextEdit");
    setStyleSheet("#ElaPlainTextEdit{background-color:transparent;}");
    setHorizontalScrollBar(new ElaScrollBar(this));
    setVerticalScrollBar(new ElaScrollBar(this));
    setMouseTracking(true);
    // 事件总线
    d->_focusEvent = new ElaEvent("WMWindowClicked", "onWMWindowClickedEvent", d);
    d->_focusEvent->registerAndInit();

    d->_style = new ElaPlainTextEditStyle(style());
    d->_style->setParent(qApp);
    connect(this, &QObject::destroyed, d->_style, &QObject::deleteLater);
    setStyle(d->_style);
    d->_markAnimation = new QPropertyAnimation(d->_style, "pExpandMarkWidth", this);
    d->_markAnimation->setDuration(160);
    d->_markAnimation->setEasingCurve(QEasingCurve::InOutSine);
    connect(d->_markAnimation, &QPropertyAnimation::valueChanged, this, [this] { update(); });
    d->onThemeChanged(eTheme->getThemeMode());
    connect(eTheme, &ElaTheme::themeModeChanged, d, &ElaPlainTextEditPrivate::onThemeChanged);
}

ElaPlainTextEdit::ElaPlainTextEdit(const QString& text, QWidget* parent)
    : ElaPlainTextEdit(parent)
{
    setPlainText(text);
}

ElaPlainTextEdit::~ElaPlainTextEdit()
{
    Q_D(ElaPlainTextEdit);
    d->_markAnimation->stop();
    // The viewport still uses this style during QPlainTextEdit teardown.
    // QObject::destroyed releases it after all widget children are gone.
}

void ElaPlainTextEdit::setNativeTextBehavior(bool enabled)
{
    Q_D(ElaPlainTextEdit);
    if (d->_nativeTextBehavior == enabled)
        return;
    d->_nativeTextBehavior = enabled;
    if (enabled)
    {
        delete d->_focusEvent;
        d->_focusEvent = nullptr;
    }
    else
    {
        d->_focusEvent = new ElaEvent("WMWindowClicked", "onWMWindowClickedEvent", d);
        d->_focusEvent->registerAndInit();
        d->onThemeChanged(eTheme->getThemeMode());
    }
    update();
}

bool ElaPlainTextEdit::nativeTextBehavior() const
{
    Q_D(const ElaPlainTextEdit);
    return d->_nativeTextBehavior;
}

void ElaPlainTextEdit::focusInEvent(QFocusEvent* event)
{
    Q_D(ElaPlainTextEdit);
    if (d->_nativeTextBehavior || event->reason() == Qt::MouseFocusReason)
    {
        d->_markAnimation->stop();
        d->_markAnimation->setStartValue(d->_style->getExpandMarkWidth());
        d->_markAnimation->setEndValue(qMax(0, width() / 2 - 3));
        d->_markAnimation->start();
    }
    QPlainTextEdit::focusInEvent(event);
}

void ElaPlainTextEdit::focusOutEvent(QFocusEvent* event)
{
    Q_D(ElaPlainTextEdit);
    if (event->reason() != Qt::PopupFocusReason)
    {
        d->_markAnimation->stop();
        d->_markAnimation->setStartValue(d->_style->getExpandMarkWidth());
        d->_markAnimation->setEndValue(0);
        d->_markAnimation->start();
    }
    QPlainTextEdit::focusOutEvent(event);
}

void ElaPlainTextEdit::contextMenuEvent(QContextMenuEvent* event)
{
    if (nativeTextBehavior())
    {
        QPlainTextEdit::contextMenuEvent(event);
        return;
    }
    ElaMenu* menu = new ElaMenu(this);
    menu->setMenuItemHeight(27);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    QAction* action = nullptr;
    if (!isReadOnly())
    {
        action = menu->addElaIconAction(ElaIconType::ArrowRotateLeft, "撤销", QKeySequence::Undo);
        action->setEnabled(isUndoRedoEnabled() ? document()->isUndoAvailable() : false);
        connect(action, &QAction::triggered, this, &ElaPlainTextEdit::undo);

        action = menu->addElaIconAction(ElaIconType::ArrowRotateRight, "恢复", QKeySequence::Redo);
        action->setEnabled(isUndoRedoEnabled() ? document()->isRedoAvailable() : false);
        connect(action, &QAction::triggered, this, &ElaPlainTextEdit::redo);
        menu->addSeparator();
    }
#ifndef QT_NO_CLIPBOARD
    if (!isReadOnly())
    {
        action = menu->addElaIconAction(ElaIconType::KnifeKitchen, "剪切", QKeySequence::Cut);
        action->setEnabled(!isReadOnly() && !textCursor().selectedText().isEmpty());
        connect(action, &QAction::triggered, this, &ElaPlainTextEdit::cut);
    }

    action = menu->addElaIconAction(ElaIconType::Copy, "复制", QKeySequence::Copy);
    action->setEnabled(!textCursor().selectedText().isEmpty());
    connect(action, &QAction::triggered, this, &ElaPlainTextEdit::copy);

    if (!isReadOnly())
    {
        action = menu->addElaIconAction(ElaIconType::Paste, "粘贴", QKeySequence::Paste);
        action->setEnabled(!isReadOnly() && !QGuiApplication::clipboard()->text().isEmpty());
        connect(action, &QAction::triggered, this, &ElaPlainTextEdit::paste);
    }
#endif
    if (!isReadOnly())
    {
        action = menu->addElaIconAction(ElaIconType::DeleteLeft, "删除");
        action->setEnabled(!isReadOnly() && !toPlainText().isEmpty() && !textCursor().selectedText().isEmpty());
        connect(action, &QAction::triggered, this, [=](bool checked) {
            if (!textCursor().selectedText().isEmpty())
            {
                textCursor().deleteChar();
            }
        });
    }
    if (!menu->isEmpty())
    {
        menu->addSeparator();
    }
    action = menu->addAction("全选");
    action->setShortcut(QKeySequence::SelectAll);
    action->setEnabled(!toPlainText().isEmpty() && !(textCursor().selectedText() == toPlainText()));
    connect(action, &QAction::triggered, this, &ElaPlainTextEdit::selectAll);
    menu->popup(event->globalPos());
    this->setFocus();
}

void ElaPlainTextEdit::paintEvent(QPaintEvent* event)
{
    Q_D(ElaPlainTextEdit);
    if (!d->_nativeTextBehavior && palette().color(QPalette::Text) != ElaThemeColor(d->_themeMode, BasicText))
    {
        d->onThemeChanged(d->_themeMode);
    }
    QPlainTextEdit::paintEvent(event);
}

#include "UiSupport.h"
#include "ElaMenu.h"
#include "ElaScrollBar.h"
#include "ElaText.h"
#include "ElaToolTip.h"
#include "ElaPushButton.h"
#include "ElaTheme.h"
#include <QAbstractItemView>
#include <QApplication>
#include <QHelpEvent>
#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPlainTextEdit>
#include <QScreen>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QWheelEvent>
#include <memory>

namespace xips
{
void keepDialogOnScreen(QWidget *dialog)
{
    const auto *screen = dialog->screen();
    if (!screen) return;
    const auto area = screen->availableGeometry();
    const auto frame = dialog->frameGeometry();
    const auto topLeft = QPoint(qBound(area.left(), frame.left(), qMax(area.left(), area.right() - frame.width() + 1)),
                               qBound(area.top(), frame.top(), qMax(area.top(), area.bottom() - frame.height() + 1)));
    // QWidget::move positions a top-level frame, including native borders.
    dialog->move(topLeft);
}
void primaryButton(ElaPushButton *button)
{
    using namespace ElaThemeType;
    button->setLightDefaultColor(eTheme->getThemeColor(Light, PrimaryNormal));
    button->setLightHoverColor(eTheme->getThemeColor(Light, PrimaryHover));
    button->setLightPressColor(eTheme->getThemeColor(Light, PrimaryPress));
    button->setDarkDefaultColor(eTheme->getThemeColor(Dark, PrimaryNormal));
    button->setDarkHoverColor(eTheme->getThemeColor(Dark, PrimaryHover));
    button->setDarkPressColor(eTheme->getThemeColor(Dark, PrimaryPress));
    button->setLightTextColor(Qt::white);
    button->setDarkTextColor(QColor("#172235"));
}
namespace
{
void drawIcon(QPainter *painter, const QRectF &rect, UiIcon icon, const QColor &color)
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->translate(rect.topLeft());
    painter->scale(rect.width() / 24, rect.height() / 24);
    painter->setPen(QPen(color, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter->setBrush(Qt::NoBrush);
    QPainterPath path;
    const auto line = [&path](qreal x1, qreal y1, qreal x2, qreal y2)
        { path.moveTo(x1, y1); path.lineTo(x2, y2); };
    const auto box = [&path](qreal x, qreal y, qreal w, qreal h)
        { path.addRoundedRect(QRectF(x, y, w, h), 1.5, 1.5); };
    switch (icon)
    {
    case UiIcon::Add: line(5, 12, 19, 12); line(12, 5, 12, 19); break;
    case UiIcon::Folder: case UiIcon::FolderPlus:
        path.moveTo(3, 7); path.lineTo(3, 5); path.lineTo(9, 5); path.lineTo(11, 7);
        path.lineTo(21, 7); path.lineTo(21, 19); path.lineTo(3, 19); path.closeSubpath();
        if (icon == UiIcon::FolderPlus) { line(9, 13, 15, 13); line(12, 10, 12, 16); }
        break;
    case UiIcon::Collect:
        line(12, 3, 12, 14); line(8, 10, 12, 14); line(12, 14, 16, 10);
        path.moveTo(4, 13); path.lineTo(4, 20); path.lineTo(20, 20); path.lineTo(20, 13); break;
    case UiIcon::Filter:
        path.moveTo(3, 5); path.lineTo(21, 5); path.lineTo(14, 13); path.lineTo(14, 19);
        path.lineTo(10, 21); path.lineTo(10, 13); path.closeSubpath(); break;
    case UiIcon::Refresh:
        path.moveTo(20, 11); path.cubicTo(19, 3, 8, 1, 4, 9);
        path.moveTo(4, 13); path.cubicTo(5, 21, 16, 23, 20, 15);
        line(4, 4, 4, 9); line(4, 9, 9, 9); line(20, 15, 15, 15); line(20, 15, 20, 20); break;
    case UiIcon::Reference: case UiIcon::Unlink:
        path.moveTo(9, 14); path.cubicTo(6, 11, 9, 8, 11, 6); path.cubicTo(17, 0, 24, 7, 18, 13);
        path.moveTo(15, 10); path.cubicTo(18, 13, 15, 16, 13, 18); path.cubicTo(7, 24, 0, 17, 6, 11);
        if (icon == UiIcon::Unlink) { line(3, 3, 6, 6); line(18, 18, 21, 21); }
        break;
    case UiIcon::Edit:
        path.moveTo(4, 20); path.lineTo(5, 15); path.lineTo(17, 3); path.lineTo(21, 7);
        path.lineTo(9, 19); path.closeSubpath(); line(14, 6, 18, 10); break;
    case UiIcon::Archive:
        box(3, 4, 18, 5); box(5, 9, 14, 11); line(10, 13, 14, 13); break;
    case UiIcon::Open:
        path.moveTo(10, 5); path.lineTo(4, 5); path.lineTo(4, 20); path.lineTo(19, 20); path.lineTo(19, 14);
        line(12, 12, 21, 3); line(14, 3, 21, 3); line(21, 3, 21, 10); break;
    case UiIcon::Lock:
        box(5, 10, 14, 11); path.moveTo(8, 10); path.lineTo(8, 7);
        path.cubicTo(8, 1, 16, 1, 16, 7); path.lineTo(16, 10); line(12, 14, 12, 17); break;
    case UiIcon::File: case UiIcon::Receipt:
        path.moveTo(14, 3); path.lineTo(5, 3); path.lineTo(5, 21); path.lineTo(19, 21);
        path.lineTo(19, 8); path.closeSubpath(); line(14, 3, 14, 8); line(14, 8, 19, 8);
        if (icon == UiIcon::File) {
            path.moveTo(10, 11); path.lineTo(8, 14); path.lineTo(10, 17);
            path.moveTo(14, 11); path.lineTo(16, 14); path.lineTo(14, 17);
        } else { line(8, 12, 16, 12); line(8, 16, 16, 16); }
        break;
    case UiIcon::Copy:
        box(8, 8, 12, 13); path.moveTo(15, 4); path.lineTo(4, 4); path.lineTo(4, 16); break;
    case UiIcon::Theme:
        path.moveTo(20, 15); path.cubicTo(7, 18, 7, 4, 10, 3);
        path.cubicTo(-2, 5, 4, 27, 20, 15); break;
    case UiIcon::Trash:
        line(3, 6, 21, 6); line(9, 3, 15, 3); box(6, 6, 12, 15);
        line(10, 10, 10, 17); line(14, 10, 14, 17); break;
    case UiIcon::Warning:
        path.moveTo(12, 3); path.lineTo(22, 21); path.lineTo(2, 21); path.closeSubpath();
        line(12, 9, 12, 14); line(12, 17, 12, 17.2); break;
    }
    painter->drawPath(path);
    painter->restore();
}
class OutlineIcon final : public QIconEngine
{
    UiIcon icon;
    bool primary;
  public:
    OutlineIcon(UiIcon value, bool accent) : icon(value), primary(accent) {}
    QIconEngine *clone() const override { return new OutlineIcon(icon, primary); }
    void paint(QPainter *painter, const QRect &rect, QIcon::Mode state, QIcon::State) override
    {
        const auto mode = eTheme->getThemeMode();
        auto color = primary ? QColor(mode == ElaThemeType::Dark ? "#172235" : "#ffffff")
                             : eTheme->getThemeColor(mode, ElaThemeType::BasicDetailsText);
        if (state == QIcon::Disabled) color.setAlpha(90);
        drawIcon(painter, rect, icon, color);
    }
    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        QPixmap result(size);
        result.fill(Qt::transparent);
        QPainter painter(&result);
        paint(&painter, QRect(QPoint(), size), mode, state);
        return result;
    }
};
class DetailDelegate final : public QStyledItemDelegate
{
    bool revisions;
  public:
    DetailDelegate(bool value, QObject *parent) : QStyledItemDelegate(parent), revisions(value) {}
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override { return {100, 28}; }
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const bool dark = eTheme->getThemeMode() == ElaThemeType::Dark;
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const bool editing = revisions && index.data(Qt::UserRole + 1).toBool();
        auto text = option.palette.color(QPalette::Text);
        const auto muted = eTheme->getThemeColor(eTheme->getThemeMode(), ElaThemeType::BasicDetailsText);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->fillRect(option.rect, selected ? QColor(dark ? "#2b2e34" : "#f0f2f5")
            : option.state.testFlag(QStyle::State_MouseOver) ? QColor(dark ? "#272a30" : "#f6f7f9")
                                                           : option.palette.color(QPalette::Base));
        QRect area = option.rect.adjusted(7, 0, -7, 0);
        auto font = option.font;
        if (revisions && index.column() == 1)
        {
            font.setPixelSize(12);
            painter->setFont(font);
            text = editing ? QColor(dark ? "#f0c47b" : "#8b5a11") : muted;
            if (editing)
            {
                painter->setPen(Qt::NoPen);
                painter->setBrush(QColor(dark ? "#3b3325" : "#faf0dc"));
                painter->drawRoundedRect(QRect(area.left() - 3, area.center().y() - 10,
                    painter->fontMetrics().horizontalAdvance(index.data().toString()) + 24, 21), 4, 4);
            }
            drawIcon(painter, QRect(area.left(), area.center().y() - 6, 12, 12),
                     editing ? UiIcon::Edit : UiIcon::Lock, text);
            area.adjust(18, 0, 0, 0);
        }
        else if (revisions && selected)
        {
            text = QColor(dark ? "#91b8ff" : "#2864db");
            font.setWeight(QFont::DemiBold);
        }
        else if (!revisions)
        {
            drawIcon(painter, QRect(area.left(), area.center().y() - 8, 16, 16), UiIcon::File, muted);
            area.adjust(24, 0, 0, 0);
        }
        painter->setFont(font);
        painter->setPen(text);
        painter->drawText(area, Qt::AlignLeft | Qt::AlignVCenter,
            painter->fontMetrics().elidedText(index.data().toString(), Qt::ElideRight, area.width()));
        if (option.state.testFlag(QStyle::State_HasFocus))
        {
            painter->setPen(QPen(muted, 1, Qt::DotLine));
            painter->setBrush(Qt::NoBrush);
            painter->drawRect(option.rect.adjusted(1, 1, -2, -2));
        }
        painter->restore();
    }
};
class TextUnitScrollBar final : public ElaScrollBar
{
  public:
    using ElaScrollBar::ElaScrollBar;
  protected:
    void wheelEvent(QWheelEvent *event) override
    {
        if (!event->pixelDelta().isNull())
        {
            stopSmoothWheel();
            QScrollBar::wheelEvent(event);
        }
        else
            ElaScrollBar::wheelEvent(event);
    }
};
class WheelRouter final : public QObject
{
    QPointer<ElaScrollBar> horizontal;
    QPointer<ElaScrollBar> vertical;
    bool textUnits;
  public:
    explicit WheelRouter(QAbstractScrollArea *area) : QObject(area),
        horizontal(qobject_cast<ElaScrollBar *>(area->horizontalScrollBar())),
        vertical(qobject_cast<ElaScrollBar *>(area->verticalScrollBar())),
        textUnits(qobject_cast<QPlainTextEdit *>(area) != nullptr)
    {
        area->installEventFilter(this);
        area->viewport()->installEventFilter(this);
    }
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::KeyPress || event->type() == QEvent::MouseButtonPress ||
            event->type() == QEvent::Hide || event->type() == QEvent::Resize ||
            event->type() == QEvent::FocusOut)
            for (const auto &bar : {horizontal, vertical})
                if (bar)
                    bar->stopSmoothWheel();
        if (event->type() != QEvent::Wheel)
            return false;
        auto *wheel = static_cast<QWheelEvent *>(event);
        const auto pixels = wheel->pixelDelta();
        if (pixels.isNull())
            return false;
        if (textUnits)
        {
            for (const auto &bar : {horizontal, vertical})
                if (bar)
                    bar->stopSmoothWheel();
            return false;
        }
        const auto bar = qAbs(pixels.x()) > qAbs(pixels.y()) || wheel->modifiers().testFlag(Qt::ShiftModifier)
            ? horizontal : vertical;
        if (!bar)
            return false;
        wheel->ignore();
        QApplication::sendEvent(bar.data(), wheel);
        return wheel->isAccepted();
    }
};
class ToolTip final : public QObject
{
    QWidget *owner;
    std::unique_ptr<ElaToolTip> tip;
    ElaText *text;
    QTimer dismiss;
  public:
    explicit ToolTip(QWidget *widget) : QObject(widget), owner(widget),
        tip(std::make_unique<ElaToolTip>()), text(new ElaText(tip.get()))
    {
        tip->setObjectName("xipsToolTip");
        tip->setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint);
        tip->setAttribute(Qt::WA_ShowWithoutActivating);
        tip->setFocusPolicy(Qt::NoFocus);
        text->setTextFormat(Qt::PlainText);
        text->setWordWrap(true);
        text->setTextPixelSize(13);
        tip->setCustomWidget(text);
        owner->installEventFilter(this);
        owner->window()->installEventFilter(this);
        if (auto *area = qobject_cast<QAbstractScrollArea *>(owner))
            area->viewport()->installEventFilter(this);
        dismiss.setSingleShot(true);
        connect(&dismiss, &QTimer::timeout, tip.get(), &QWidget::hide);
    }
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::ToolTip)
        {
            const auto *help = static_cast<QHelpEvent *>(event);
            QString value = owner->toolTip();
            if (const auto *view = qobject_cast<QAbstractItemView *>(owner))
                value = view->indexAt(help->pos()).data(Qt::ToolTipRole).toString();
            if (value.isEmpty())
            {
                tip->hide();
                return true;
            }
            const auto *screen = QGuiApplication::screenAt(help->globalPos());
            const QRect bounds = (screen ? screen : owner->screen())->availableGeometry().adjusted(4, 4, -4, -4);
            text->setMaximumWidth(qMin(480, qMax(80, bounds.width() - 40)));
            text->setText(value);
            tip->adjustSize();
            const QPoint requested = help->globalPos() + QPoint(12, 18);
            tip->move(qBound(bounds.left(), requested.x(), qMax(bounds.left(), bounds.right() - tip->width())),
                      qBound(bounds.top(), requested.y(), qMax(bounds.top(), bounds.bottom() - tip->height())));
            tip->show();
            dismiss.start(8000);
            return true;
        }
        if (event->type() == QEvent::Leave || event->type() == QEvent::Hide ||
            event->type() == QEvent::MouseButtonPress || event->type() == QEvent::KeyPress ||
            event->type() == QEvent::Wheel || event->type() == QEvent::WindowDeactivate ||
            event->type() == QEvent::Resize)
        {
            dismiss.stop();
            tip->hide();
        }
        return false;
    }
};
class MenuOwner final : public QObject
{
    ElaMenu *menu;
  public:
    MenuOwner(ElaMenu *value, QWidget *owner) : QObject(value), menu(value)
    {
        owner->installEventFilter(this);
        owner->window()->installEventFilter(this);
        connect(owner, &QObject::destroyed, menu, &QWidget::close);
    }
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::Hide || event->type() == QEvent::Close)
        {
            menu->finishPopupAnimation();
            menu->close();
        }
        return false;
    }
};
}
QIcon uiIcon(UiIcon icon, bool primary) { return QIcon(new OutlineIcon(icon, primary)); }
QAbstractItemDelegate *detailDelegate(bool revisions, QObject *parent) { return new DetailDelegate(revisions, parent); }
void enableSmoothScrolling(QAbstractScrollArea *area)
{
    if (area->property("xipsSmoothScrolling").toBool())
        return;
    area->setProperty("xipsSmoothScrolling", true);
    if (qobject_cast<QPlainTextEdit *>(area))
    {
        area->setHorizontalScrollBar(new TextUnitScrollBar(Qt::Horizontal, area));
        area->setVerticalScrollBar(new TextUnitScrollBar(Qt::Vertical, area));
    }
    new WheelRouter(area);
    for (auto *scroll : {area->horizontalScrollBar(), area->verticalScrollBar()})
        if (auto *bar = qobject_cast<ElaScrollBar *>(scroll))
        {
            bar->setIsAnimation(false);
            bar->setSmoothWheelEnabled(true);
            bar->setWheelAnimationDuration(160);
        }
}
void enableToolTip(QWidget *widget)
{
    if (!widget->property("xipsToolTipEnabled").toBool())
    {
        widget->setProperty("xipsToolTipEnabled", true);
        new ToolTip(widget);
    }
}
void prepareMenu(ElaMenu *menu, QWidget *owner)
{
    menu->setNativeMenuBehavior(true);
    new MenuOwner(menu, owner);
}
QAction *executeMenu(ElaMenu &menu, QWidget *owner, const QPoint &position)
{
    prepareMenu(&menu, owner);
    QPointer<QWidget> guard(owner);
    const auto action = menu.exec(position);
    return guard ? action : nullptr;
}
}

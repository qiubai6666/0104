#include "mainmenubutton.h"

#include <QDynamicPropertyChangeEvent>
#include <QFocusEvent>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionButton>

MainMenuButton::MainMenuButton(QWidget *parent)
    : QPushButton(parent), backgroundAnimation(this)
{
    backgroundAnimation.setEasingCurve(QEasingCurve::OutCubic);
    connect(&backgroundAnimation, &QVariantAnimation::valueChanged, this,
            [this](const QVariant &value) {
                backgroundColor = value.value<QColor>();
                update();
            });
    connect(this, &QPushButton::pressed, this, [this] { updateBackground(); });
    connect(this, &QPushButton::released, this, [this] { updateBackground(); });
    updateBackground(false);
}

void MainMenuButton::updateBackground(bool animate)
{
    const bool dark = palette().color(QPalette::Window).lightness() < 128;
    QColor target = dark ? QColor(118, 151, 168, 0) : QColor(255, 255, 255, 0);
    if (isEnabled()) {
        if (isDown())
            target = dark ? QColor(108, 151, 172, 150) : QColor(212, 230, 237, 180);
        else if (property("menuSelected").toBool())
            target = dark ? QColor(118, 151, 168, 135) : QColor(255, 255, 255, 210);
        else if (hovered)
            target = dark ? QColor(118, 151, 168, 85) : QColor(255, 255, 255, 110);
    }
    // Retarget from the current frame instead of jumping on rapid mouse movement.
    if (animate && backgroundAnimation.state() == QAbstractAnimation::Running &&
        backgroundAnimation.endValue().value<QColor>() == target)
        return;
    const QColor start = backgroundColor;
    backgroundAnimation.stop();
    if (!animate || backgroundColor == target || !isVisible()) {
        backgroundColor = target;
        update();
        return;
    }
    backgroundAnimation.setDuration(isDown() ? 70 : target.alpha() ? 140 : 180);
    backgroundAnimation.setStartValue(start);
    backgroundAnimation.setEndValue(target);
    backgroundAnimation.start();
}

bool MainMenuButton::event(QEvent *event)
{
    const bool result = QPushButton::event(event);
    switch (event->type()) {
    case QEvent::Enter:
        hovered = true;
        updateBackground();
        break;
    case QEvent::Leave:
        hovered = false;
        updateBackground();
        break;
    case QEvent::FocusIn: {
        const auto reason = static_cast<QFocusEvent *>(event)->reason();
        keyboardFocus = reason == Qt::TabFocusReason || reason == Qt::BacktabFocusReason ||
                        reason == Qt::ShortcutFocusReason;
        update();
        break;
    }
    case QEvent::FocusOut:
    case QEvent::MouseButtonPress:
        keyboardFocus = false;
        update();
        break;
    case QEvent::DynamicPropertyChange:
        if (static_cast<QDynamicPropertyChangeEvent *>(event)->propertyName() == "menuSelected")
            updateBackground();
        break;
    case QEvent::EnabledChange:
    case QEvent::PaletteChange:
        updateBackground(false);
        break;
    case QEvent::Hide:
        hovered = false;
        keyboardFocus = false;
        updateBackground(false);
        break;
    default:
        break;
    }
    return result;
}

void MainMenuButton::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF surface = QRectF(rect()).adjusted(1, 1, -1, -1);
    painter.setPen(Qt::NoPen);
    painter.setBrush(backgroundColor);
    painter.drawRoundedRect(surface, 14, 14);
    // Only keyboard navigation gets an outline, never a retained mouse focus.
    if (keyboardFocus && isEnabled()) {
        painter.setPen(QPen(QColor(131, 174, 202), 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(surface, 14, 14);
    }
    // Keep Qt's icon/DPI/disabled rendering, but do not draw its native frame.
    QStyleOptionButton option;
    initStyleOption(&option);
    option.state &= ~(QStyle::State_HasFocus | QStyle::State_MouseOver | QStyle::State_Sunken);
    style()->drawControl(QStyle::CE_PushButtonLabel, &option, &painter, this);
}

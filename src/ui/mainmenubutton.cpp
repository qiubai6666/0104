#include "mainmenubutton.h"

#include <QDynamicPropertyChangeEvent>
#include <QFocusEvent>
#include <QPainter>

MainMenuButton::MainMenuButton(QWidget *parent)
    : QPushButton(parent), colorAnimation(this), clickFeedbackTimer(this)
{
    colorAnimation.setEasingCurve(QEasingCurve::InOutCubic);
    connect(&colorAnimation, &QVariantAnimation::valueChanged, this,
            [this](const QVariant &value) {
                iconColor = value.value<QColor>();
                update();
            });
    clickFeedbackTimer.setSingleShot(true);
    clickFeedbackTimer.setInterval(160);
    connect(&clickFeedbackTimer, &QTimer::timeout, this, [this] { updateIconColor(); });
    connect(this, &QPushButton::pressed, this, [this] { updateIconColor(); });
    connect(this, &QPushButton::released, this, [this] { updateIconColor(); });
    connect(this, &QPushButton::clicked, this, [this] {
        // One-shot actions get brief color feedback, not a persistent selection.
        if (property("menuDirectAction").toBool()) {
            clickFeedbackTimer.start();
            updateIconColor();
        }
    });
    updateIconColor(false);
}

void MainMenuButton::updateIconColor(bool animate)
{
    const bool dark = palette().color(QPalette::Window).lightness() < 128;
    QColor target = dark ? QColor("#8da4b2") : QColor("#68818e");
    if (!isEnabled())
        target = dark ? QColor("#556772") : QColor("#9baab2");
    else if (isDown())
        target = dark ? QColor("#ffffff") : QColor("#0d2939");
    else if (property("menuSelected").toBool() || clickFeedbackTimer.isActive())
        target = dark ? QColor("#f4faff") : QColor("#193f55");
    else if (hovered)
        target = dark ? QColor("#cedde6") : QColor("#385565");

    // Retarget from the current frame instead of jumping on rapid mouse movement.
    if (animate && colorAnimation.state() == QAbstractAnimation::Running &&
        colorAnimation.endValue().value<QColor>() == target)
        return;
    const QColor start = iconColor;
    colorAnimation.stop();
    if (!animate || iconColor == target || !isVisible()) {
        iconColor = target;
        update();
        return;
    }
    colorAnimation.setDuration(isDown() ? 90 : 180);
    colorAnimation.setStartValue(start);
    colorAnimation.setEndValue(target);
    colorAnimation.start();
}

bool MainMenuButton::event(QEvent *event)
{
    const bool result = QPushButton::event(event);
    switch (event->type()) {
    case QEvent::Enter:
        hovered = true;
        updateIconColor();
        break;
    case QEvent::Leave:
        hovered = false;
        updateIconColor();
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
            updateIconColor();
        break;
    case QEvent::EnabledChange:
        clickFeedbackTimer.stop();
        updateIconColor(false);
        break;
    case QEvent::PaletteChange:
        updateIconColor(false);
        break;
    case QEvent::Hide:
        hovered = false;
        keyboardFocus = false;
        clickFeedbackTimer.stop();
        updateIconColor(false);
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
    // Only keyboard navigation gets an outline, never a retained mouse focus.
    if (keyboardFocus && isEnabled()) {
        painter.setPen(QPen(QColor(131, 174, 202), 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 14, 14);
    }
    if (icon().isNull()) return;
    // Tint the existing alpha mask (SVG or spinner), keeping all DPI-aware strokes.
    QPixmap tinted = icon().pixmap(iconSize(), devicePixelRatioF(), QIcon::Normal, QIcon::Off);
    if (tinted.isNull()) return;
    {
        QPainter tintPainter(&tinted);
        tintPainter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        tintPainter.fillRect(QRectF(QPointF(), tinted.deviceIndependentSize()), iconColor);
    }
    // Place at a fixed logical origin. A second QIcon/style conversion can round
    // fractional DPRs again, changing coverage or alignment during recoloring.
    painter.drawPixmap(QPointF((width() - iconSize().width()) / 2.0,
                              (height() - iconSize().height()) / 2.0), tinted);
}

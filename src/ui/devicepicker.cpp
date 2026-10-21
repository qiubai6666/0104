#include "devicepicker.h"
#include "elidedlabel.h"
#include "uihelper.h"

#include <QApplication>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

namespace {
const QColor kAccentLight("#2F80ED");
const QColor kAccentDark("#3F8CF2");
const QColor kOk("#22A35A");
const QColor kWarn("#E29A2D");
const QColor kOff("#9AA8B5");
const char *kDefaultHint = "投屏与设备检测将使用所选设备";

QString rgba(const QColor &color, int alpha)
{
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(color.red()).arg(color.green()).arg(color.blue()).arg(alpha);
}

QString stateLabel(const QString &state)
{
    if (state == QStringLiteral("device")) return QStringLiteral("在线");
    if (state == QStringLiteral("unauthorized")) return QStringLiteral("待授权");
    if (state == QStringLiteral("offline")) return QStringLiteral("离线");
    if (state == QStringLiteral("fastboot")) return QStringLiteral("Fastboot");
    return QStringLiteral("不可用");
}
} // namespace

DevicePicker::DevicePicker(QWidget *parent)
    : QFrame(parent)
{
    setObjectName("devicePicker");
    setAttribute(Qt::WA_StyledBackground, true);
    setFocusPolicy(Qt::StrongFocus);
    setFixedWidth(UIHelper::ControlCenterWidth - 32);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    const bool dark = isDark();
    const QString panel = dark ? QStringLiteral("#182430") : QStringLiteral("#FFFFFF");
    const QString border = dark ? QStringLiteral("rgba(146,166,181,64)")
                                : QStringLiteral("rgba(105,142,162,61)");
    setStyleSheet(QStringLiteral(
        "QFrame#devicePicker { background:%1; border:1px solid %2; border-radius:16px; }")
        .arg(panel, border));
    auto *shadow = new QGraphicsDropShadowEffect(this);
    shadow->setBlurRadius(32);
    shadow->setOffset(0, 10);
    shadow->setColor(QColor(31, 58, 84, dark ? 110 : 46));
    setGraphicsEffect(shadow);

    const QColor ink = dark ? QColor("#e9edf3") : QColor("#22364a");
    const QColor dim = dark ? QColor("#93a7b6") : QColor("#647585");
    const QColor faint = dark ? QColor("#7f95a6") : QColor("#95a5b2");
    const QColor accent = dark ? kAccentDark : kAccentLight;

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(9, 9, 9, 6);
    root->setSpacing(0);

    auto *head = new QWidget(this);
    auto *headLayout = new QHBoxLayout(head);
    headLayout->setContentsMargins(2, 2, 2, 6);
    headLayout->setSpacing(6);
    auto *title = new QLabel(QStringLiteral("选择设备"), head);
    title->setStyleSheet(QStringLiteral("font-size:12.5px; font-weight:600; color:%1;")
                         .arg(ink.name()));
    m_countLabel = new QLabel(QStringLiteral("0 台"), head);
    m_countLabel->setObjectName("devicePickerCount");
    m_countLabel->setStyleSheet(QStringLiteral(
        "background:%1; color:%2; border-radius:7px; padding:2px 6px; font-size:9.5px;")
        .arg(dark ? QStringLiteral("rgba(255,255,255,16)") : QStringLiteral("#EEF3F8"),
             dim.name()));
    headLayout->addWidget(title);
    headLayout->addWidget(m_countLabel);
    headLayout->addStretch(1);

    const QString iconButtonStyle = QStringLiteral(
        "QPushButton { background:transparent; border:none; border-radius:8px; }"
        "QPushButton:hover { background:%1; }"
        "QPushButton:pressed { background:%2; }")
        .arg(rgba(accent, dark ? 40 : 24), rgba(accent, dark ? 64 : 44));
    const QColor iconColor = dim;
    m_refreshButton = new QPushButton(head);
    m_refreshButton->setObjectName("devicePickerRefresh");
    m_refreshButton->setFixedSize(24, 24);
    m_refreshButton->setCursor(Qt::PointingHandCursor);
    m_refreshButton->setStyleSheet(iconButtonStyle);
    m_refreshButton->setIconSize(QSize(13, 13));
    m_refreshButton->setToolTip(QStringLiteral("刷新设备列表"));
    m_refreshButton->setAccessibleName(QStringLiteral("刷新设备列表"));
    m_closeButton = new QPushButton(head);
    m_closeButton->setObjectName("devicePickerClose");
    m_closeButton->setFixedSize(24, 24);
    m_closeButton->setCursor(Qt::PointingHandCursor);
    m_closeButton->setStyleSheet(iconButtonStyle);
    m_closeButton->setIconSize(QSize(13, 13));
    m_closeButton->setIcon(UIHelper::menuIcon(QStringLiteral("x"), iconColor, devicePixelRatioF()));
    m_closeButton->setToolTip(QStringLiteral("关闭"));
    m_closeButton->setAccessibleName(QStringLiteral("关闭设备选择"));
    headLayout->addWidget(m_refreshButton);
    headLayout->addWidget(m_closeButton);
    root->addWidget(head);

    m_scroll = new QScrollArea(this);
    m_scroll->setObjectName("devicePickerList");
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_scroll->setStyleSheet(QStringLiteral(
        "QScrollArea { background:transparent; border:none; }"
        "QScrollArea > QWidget > QWidget { background:transparent; }"
        "QScrollBar:vertical { background:transparent; width:4px; margin:0; }"
        "QScrollBar::handle:vertical { background:rgba(105,142,162,70); border-radius:2px; min-height:24px; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background:transparent; }"));
    m_scroll->viewport()->setAutoFillBackground(false);
    m_listContent = new QWidget;
    m_listContent->setObjectName("devicePickerListContent");
    m_listContent->setAutoFillBackground(false);
    m_listLayout = new QVBoxLayout(m_listContent);
    m_listLayout->setContentsMargins(0, 0, 0, 0);
    m_listLayout->setSpacing(3);
    m_scroll->setWidget(m_listContent);
    root->addWidget(m_scroll);

    auto *line = new QFrame(this);
    line->setFixedHeight(1);
    line->setStyleSheet(QStringLiteral("background:%1;")
        .arg(dark ? QStringLiteral("rgba(146,166,181,38)") : QStringLiteral("#EEF3F7")));
    root->addSpacing(5);
    root->addWidget(line);

    auto *footer = new QWidget(this);
    auto *footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(2, 6, 2, 1);
    footerLayout->setSpacing(6);
    m_hintIcon = new QLabel(QStringLiteral("i"), footer);
    m_hintIcon->setObjectName("devicePickerHintIcon");
    m_hintIcon->setFixedSize(12, 12);
    m_hintIcon->setAlignment(Qt::AlignCenter);
    m_hintIcon->setStyleSheet(QStringLiteral(
        "border:1px solid %1; border-radius:6px; font-size:8px; font-weight:600; color:%1;")
        .arg(faint.name()));
    m_hintLabel = new QLabel(footer);
    m_hintLabel->setObjectName("devicePickerHint");
    m_hintLabel->setWordWrap(true);
    footerLayout->addWidget(m_hintIcon, 0, Qt::AlignTop);
    footerLayout->addWidget(m_hintLabel, 1);
    root->addWidget(footer);

    m_spinTimer = new QTimer(this);
    m_spinTimer->setInterval(45);
    connect(m_spinTimer, &QTimer::timeout, this, [this] {
        m_spinAngle = (m_spinAngle + 30) % 360;
        updateRefreshIcon();
    });
    connect(m_refreshButton, &QPushButton::clicked, this, [this] {
        if (!m_scanning) emit refreshRequested();
    });
    connect(m_closeButton, &QPushButton::clicked, this, &DevicePicker::closeRequested);

    resetHint();
    updateRefreshIcon();
    m_scroll->setFixedHeight(52);
    rebuildList();
}

bool DevicePicker::isDark() const
{
    return palette().color(QPalette::Window).lightness() < 128;
}

int DevicePicker::onlineCount() const
{
    int count = 0;
    for (const auto &entry : m_devices)
        if (entry.isOperable()) ++count;
    return count;
}

void DevicePicker::setDevices(const QVector<DeviceManager::DeviceEntry> &devices,
                              const QString &selectedSerial, const QString &selectedDisplayName)
{
    const bool changed = devices != m_devices || selectedSerial != m_selectedSerial
        || selectedDisplayName != m_selectedDisplayName;
    m_devices = devices;
    m_selectedSerial = selectedSerial;
    m_selectedDisplayName = selectedDisplayName;
    updateCount();
    if (!changed) return;
    rebuildList();
    if (layout()) layout()->activate();
    adjustSize();
}

void DevicePicker::updateCount()
{
    m_countLabel->setText(QStringLiteral("%1 台").arg(m_devices.size()));
}

void DevicePicker::rebuildList()
{
    while (QLayoutItem *item = m_listLayout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }

    const bool dark = isDark();
    const QColor ink = dark ? QColor("#e9edf3") : QColor("#22364a");
    const QColor dim = dark ? QColor("#93a7b6") : QColor("#647585");
    const QColor faint = dark ? QColor("#7f95a6") : QColor("#95a5b2");
    const QColor accent = dark ? kAccentDark : kAccentLight;
    const QString hoverBg = dark ? QStringLiteral("rgba(255,255,255,12)")
                                 : QStringLiteral("#F5F9FC");
    const QString selectedBg = dark ? QStringLiteral("rgba(63,140,242,30)")
                                     : QStringLiteral("rgba(47,128,237,15)");
    const QString selectedHover = dark ? QStringLiteral("rgba(63,140,242,44)")
                                        : QStringLiteral("rgba(47,128,237,22)");
    const QString selectedBorder = dark ? QStringLiteral("rgba(63,140,242,110)")
                                         : QStringLiteral("rgba(47,128,237,77)");

    if (m_devices.isEmpty()) {
        auto *empty = new QLabel(QStringLiteral("未检测到设备\n连接手机并开启 USB 调试后自动刷新"),
                                 m_listContent);
        empty->setObjectName("devicePickerEmpty");
        empty->setAlignment(Qt::AlignCenter);
        empty->setWordWrap(true);
        empty->setStyleSheet(QStringLiteral("font-size:10.5px; color:%1; line-height:1.5;")
                             .arg(dim.name()));
        m_listLayout->addWidget(empty);
        m_scroll->setFixedHeight(52);
        return;
    }

    for (const DeviceManager::DeviceEntry &entry : m_devices) {
        const bool selected = !m_selectedSerial.isEmpty() && entry.serial == m_selectedSerial;
        const bool operable = entry.isOperable();
        const bool unauthorized = entry.state == QStringLiteral("unauthorized");
        QString name = selected && !m_selectedDisplayName.isEmpty()
            ? m_selectedDisplayName : entry.model;
        if (name.isEmpty()) {
            name = entry.state == QStringLiteral("fastboot")
                ? QStringLiteral("Fastboot 设备") : QStringLiteral("Android 设备");
        }

        auto *button = new QPushButton(m_listContent);
        button->setObjectName("devicePickerItem");
        button->setProperty("deviceSerial", entry.serial);
        button->setProperty("deviceName", name);
        button->setProperty("selected", selected);
        button->setAccessibleName(name);
        button->setAccessibleDescription(QStringLiteral("%1，%2").arg(entry.serial, stateLabel(entry.state)));
        button->setToolTip(name + QLatin1Char('\n') + entry.serial);
        button->setCursor(Qt::PointingHandCursor);
        button->setFixedHeight(52);
        button->setStyleSheet(QStringLiteral(
            "QPushButton { background:%1; border:1px solid %2; border-radius:11px; text-align:left; }"
            "QPushButton:hover { background:%3; }"
            "QPushButton:focus { border:1px solid %4; }")
            .arg(selected ? selectedBg : QStringLiteral("transparent"),
                 selected ? selectedBorder : QStringLiteral("transparent"),
                 selected ? selectedHover : hoverBg,
                 rgba(accent, dark ? 120 : 96)));

        auto *row = new QHBoxLayout(button);
        row->setContentsMargins(9, 8, 9, 8);
        row->setSpacing(9);

        auto *avatar = new QLabel(button);
        avatar->setFixedSize(34, 34);
        avatar->setAlignment(Qt::AlignCenter);
        avatar->setAttribute(Qt::WA_TransparentForMouseEvents);
        const QColor avatarInk = selected ? accent : unauthorized ? kWarn : dim;
        avatar->setPixmap(UIHelper::menuIcon(QStringLiteral("smartphone"), avatarInk,
                                             devicePixelRatioF()).pixmap(16, 16));
        avatar->setStyleSheet(QStringLiteral("background:%1; border-radius:10px;")
            .arg(selected ? rgba(accent, dark ? 42 : 31)
                          : unauthorized ? rgba(kWarn, 26)
                          : dark ? QStringLiteral("rgba(255,255,255,14)")
                                 : QStringLiteral("#EEF3F8")));
        row->addWidget(avatar, 0, Qt::AlignVCenter);

        auto *copy = new QVBoxLayout;
        copy->setSpacing(1);
        auto *nameLabel = new ElidedLabel(button);
        nameLabel->setText(name);
        nameLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        nameLabel->setStyleSheet(QStringLiteral("font-size:12px; font-weight:600; color:%1;")
                                 .arg(ink.name()));
        auto *metaRow = new QHBoxLayout;
        metaRow->setSpacing(4);
        auto *dot = new QLabel(button);
        dot->setFixedSize(5, 5);
        dot->setAttribute(Qt::WA_TransparentForMouseEvents);
        const QColor stateColor = operable ? kOk : unauthorized ? kWarn : kOff;
        dot->setStyleSheet(QStringLiteral("background:%1; border-radius:2.5px;")
                           .arg(stateColor.name()));
        auto *serialLabel = new ElidedLabel(button);
        serialLabel->setText(QStringLiteral("%1 · %2").arg(
            entry.serial, entry.state == QStringLiteral("fastboot")
                              ? QStringLiteral("Fastboot") : QStringLiteral("USB")));
        serialLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        QFont mono(QStringLiteral("Consolas"));
        mono.setPixelSize(9);
        serialLabel->setFont(mono);
        serialLabel->setStyleSheet(QStringLiteral("color:%1;").arg(faint.name()));
        metaRow->addWidget(dot, 0, Qt::AlignVCenter);
        metaRow->addWidget(serialLabel, 1);
        copy->addWidget(nameLabel);
        copy->addLayout(metaRow);
        row->addLayout(copy, 1);

        if (selected) {
            auto *check = new QLabel(button);
            check->setFixedSize(18, 18);
            check->setAlignment(Qt::AlignCenter);
            check->setAttribute(Qt::WA_TransparentForMouseEvents);
            check->setPixmap(UIHelper::menuIcon(QStringLiteral("check"), QColor(Qt::white),
                                                devicePixelRatioF()).pixmap(11, 11));
            check->setStyleSheet(QStringLiteral("background:%1; border-radius:9px;")
                                 .arg(accent.name()));
            row->addWidget(check, 0, Qt::AlignVCenter);
        } else if (!operable) {
            auto *warn = new QLabel(stateLabel(entry.state), button);
            warn->setAttribute(Qt::WA_TransparentForMouseEvents);
            const QColor pillInk = unauthorized ? kWarn : kOff;
            warn->setStyleSheet(QStringLiteral(
                "color:%1; background:%2; border:1px solid %3; border-radius:6px;"
                " padding:3px 6px; font-size:9px; font-weight:600;")
                .arg(dark && unauthorized ? QStringLiteral("#f4c66a") : pillInk.name(),
                     rgba(pillInk, dark ? 34 : 26), rgba(pillInk, dark ? 48 : 36)));
            row->addWidget(warn, 0, Qt::AlignVCenter);
        }

        m_listLayout->addWidget(button);

        connect(button, &QPushButton::clicked, this, [this, button, entry] {
            if (!entry.isOperable()) {
                showHint(entry.state == QStringLiteral("unauthorized")
                             ? QStringLiteral("请先在手机上允许 USB 调试")
                             : QStringLiteral("该设备当前不可操作，请重新连接"),
                         true);
                auto *shake = new QPropertyAnimation(button, "pos", button);
                const QPoint origin = button->pos();
                shake->setDuration(280);
                shake->setKeyValueAt(0.0, origin);
                shake->setKeyValueAt(0.2, origin + QPoint(-4, 0));
                shake->setKeyValueAt(0.4, origin + QPoint(4, 0));
                shake->setKeyValueAt(0.6, origin + QPoint(-3, 0));
                shake->setKeyValueAt(0.8, origin + QPoint(2, 0));
                shake->setKeyValueAt(1.0, origin);
                shake->start(QAbstractAnimation::DeleteWhenStopped);
                return;
            }
            if (entry.serial == m_selectedSerial) {
                emit closeRequested();
                return;
            }
            QString error;
            if (!DeviceManager::instance()->selectDevice(entry.serial, &error)) {
                showHint(error, true);
                return;
            }
            emit deviceSelected(entry.serial);
        });
    }

    const int rows = qMin(int(m_devices.size()), 3);
    m_scroll->setFixedHeight(rows * 52 + (rows - 1) * 3);
}

void DevicePicker::setScanning(bool scanning)
{
    if (m_scanning == scanning) return;
    m_scanning = scanning;
    m_refreshButton->setEnabled(!scanning);
    if (scanning) {
        m_spinTimer->start();
    } else {
        m_spinTimer->stop();
        m_spinAngle = 0;
        updateRefreshIcon();
    }
}

void DevicePicker::updateRefreshIcon()
{
    const bool dark = isDark();
    const QColor color = m_scanning
        ? (dark ? kAccentDark : kAccentLight)
        : (dark ? QColor("#93a7b6") : QColor("#647585"));
    const qreal ratio = m_refreshButton->devicePixelRatioF();
    const QIcon base = UIHelper::menuIcon(QStringLiteral("refresh"), color, ratio);
    QPixmap rotated(qRound(13 * ratio), qRound(13 * ratio));
    rotated.setDevicePixelRatio(ratio);
    rotated.fill(Qt::transparent);
    QPainter painter(&rotated);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.translate(6.5, 6.5);
    painter.rotate(m_spinAngle);
    painter.translate(-6.5, -6.5);
    base.paint(&painter, QRect(0, 0, 13, 13));
    painter.end();
    m_refreshButton->setIcon(QIcon(rotated));
}

void DevicePicker::showHint(const QString &text, bool warning)
{
    m_hintText = text;
    m_hintWarning = warning;
    updateHintStyle();
}

void DevicePicker::resetHint()
{
    showHint(QString::fromUtf8(kDefaultHint), false);
}

void DevicePicker::updateHintStyle()
{
    const bool dark = isDark();
    const QColor dim = dark ? QColor("#93a7b6") : QColor("#647585");
    const QColor warnInk = dark ? QColor("#f4c66a") : QColor("#a56a0b");
    const QColor color = m_hintWarning ? warnInk : dim;
    m_hintLabel->setText(m_hintText);
    m_hintLabel->setStyleSheet(QStringLiteral("font-size:10px; color:%1;").arg(color.name()));
    m_hintIcon->setStyleSheet(QStringLiteral(
        "border:1px solid %1; border-radius:6px; font-size:8px; font-weight:600; color:%1;")
        .arg(color.name()));
}

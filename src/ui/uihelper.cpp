#include "uihelper.h"
#include <QPushButton>
#include <QVBoxLayout>
#include <QScreen>
#include <QGuiApplication>
#include <QApplication>
#include <QPainter>
#include <QEvent>
#include <QFile>
#include <QLabel>
#include <QGridLayout>
#include <QSvgRenderer>
#include <algorithm>

namespace {
class FloatingSurfacePainter : public QObject
{
public:
    explicit FloatingSurfacePainter(QWidget *window) : QObject(window) {}
protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (event->type() != QEvent::Paint) return false;
        auto *window = qobject_cast<QWidget *>(object);
        if (!window) return false;
        const bool capsule = window->property("floatingCapsule").toBool();
        const bool controlCenter = window->property("controlCenterSurface").toBool();
        const bool dark = QApplication::palette().color(QPalette::Window).lightness() < 128;
        QColor fill = controlCenter ? (dark ? QColor(35, 42, 48) : QColor(255, 255, 255))
                                  : capsule ? (dark ? QColor(39, 56, 64) : QColor(195, 219, 228))
                                  : (dark ? QColor(35, 42, 48) : QColor(255, 255, 255));
        fill.setAlpha(controlCenter ? 216 : capsule ? 102 : 184);
        QPainter painter(window);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setBrush(fill);
        const qreal borderWidth = controlCenter || !capsule ? 1.5 : 1.0;
        const QColor border = controlCenter
            ? (dark ? QColor(156, 193, 214, 160) : QColor(105, 142, 162, 180))
            : capsule
            ? (dark ? QColor(255, 255, 255, 32) : QColor(255, 255, 255, 180))
            : (dark ? QColor(156, 193, 214, 160) : QColor(105, 142, 162, 180));
        painter.setPen(QPen(border, borderWidth));
        const qreal radius = controlCenter ? 24 : capsule ? window->width() / 2.0 : 28;
        // Keep the full antialiased outline inside the translucent window.
        const qreal inset = borderWidth / 2.0;
        painter.drawRoundedRect(QRectF(window->rect()).adjusted(inset, inset, -inset, -inset), radius, radius);
        return true;
    }
};

// 两种消息框采用相同的显示顺序，先显示再按实际大小居中。
void centerMessageBox(QMessageBox &messageBox)
{
    messageBox.show();
    const QRect screenGeometry = QGuiApplication::primaryScreen()->geometry();
    const int x = (screenGeometry.width() - messageBox.width()) / 2;
    const int y = (screenGeometry.height() - messageBox.height()) / 2;
    messageBox.move(x, y);
}

} // namespace

void UIHelper::applyFloatingSurface(QWidget *window, bool capsule)
{
    window->setAttribute(Qt::WA_TranslucentBackground);
    window->setAttribute(Qt::WA_QuitOnClose, capsule);
    window->setProperty("floatingCapsule", capsule);
    window->setProperty("surfaceOpacity", capsule ? .4 : .72);
    window->setProperty("surfaceRadius", capsule ? MainMenuWidth / 2 : 28);
    if (!capsule) window->setFixedSize(DrawerWidth, DrawerHeight);
    window->installEventFilter(new FloatingSurfacePainter(window));
}

void UIHelper::applyControlCenterSurface(QWidget *panel)
{
    panel->setAttribute(Qt::WA_TranslucentBackground);
    panel->setAttribute(Qt::WA_QuitOnClose, false);
    panel->setProperty("floatingCapsule", false);
    panel->setProperty("controlCenterSurface", true);
    panel->setProperty("surfaceOpacity", .85);
    panel->setProperty("surfaceRadius", 24);
    panel->installEventFilter(new FloatingSurfacePainter(panel));
}

QIcon UIHelper::menuIcon(const QString &name, qreal ratio)
{
    const bool dark = QApplication::palette().color(QPalette::Window).lightness() < 128;
    return menuIcon(name, QColor(dark ? "#dce8ef" : "#385565"), ratio);
}

QIcon UIHelper::menuIcon(const QString &name, const QColor &color, qreal ratio)
{
    QFile file(":/menu-icons/" + name + ".svg");
    if (!file.open(QIODevice::ReadOnly)) return {};
    QByteArray svg = file.readAll();
    svg.replace("currentColor", color.name(QColor::HexRgb).toLatin1());

    // Render the SVG into a DPI-aware pixmap so the icon engine is not
    // required to load the SVG plugin at runtime.
    QSvgRenderer renderer(svg);
    const qreal scale = qMax<qreal>(1.0, ratio);
    const int pixels = qMax(24, qRound(24 * scale));
    QPixmap pixmap(pixels, pixels);
    pixmap.setDevicePixelRatio(scale);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    // setDevicePixelRatio already scales the painter. Its bounds must stay
    // in logical pixels, otherwise DPR > 1 clips the right/bottom strokes.
    renderer.render(&painter, QRectF(0, 0, 24, 24));
    painter.end();
    return QIcon(pixmap);
}
QString UIHelper::textButtonStyle()
{
    const bool dark = QApplication::palette().color(QPalette::Window).lightness() < 128;
    return QStringLiteral(
        "QPushButton { background:transparent; color:%1; border:1px solid transparent;"
        " border-radius:11px; padding:6px 8px; font-family:'Segoe UI','Microsoft YaHei';"
        " font-size:13px; font-weight:normal; text-align:left; }"
        "QPushButton:hover { background:%2; }"
        "QPushButton:pressed { background:%3; }"
        "QPushButton:focus { border:1px solid #83AECA; }"
        "QPushButton:disabled { color:%4; }").arg(
        dark ? "#e9edf3" : "#2c3e50", dark ? "#344950" : "rgba(212,230,237,150)",
        dark ? "#42616e" : "#d4e6ed", dark ? "#71808d" : "#98a5ad");
}

QVector<QPushButton *> UIHelper::createDrawerButtons(QWidget *parent, QVBoxLayout *layout,
    const QString &title, const QString &subtitle, const QStringList &texts,
    int splitIndex, const QString &firstGroup, const QString &secondGroup,
    bool floating, const QStringList &icons, int spanIndex)
{
    // 嵌入控制中心时按预览稿子页渲染：标题由面板头部承担，页内仅保留
    // 分组标签与带图标的行按钮；独立抽屉（回归测试替身）维持原样。
    const bool embedded = !floating;
    if (floating) applyFloatingSurface(parent);
    layout->setContentsMargins(embedded ? 12 : DrawerHorizontalMargin,
                              embedded ? 2 : DrawerVerticalMargin,
                              embedded ? 14 : DrawerHorizontalMargin,
                              embedded ? 10 : DrawerVerticalMargin);
    layout->setSpacing(embedded ? 0 : 4);
    const bool dark = QApplication::palette().color(QPalette::Window).lightness() < 128;
    const QColor inkDim(dark ? "#93a7b6" : "#647585");
    parent->setStyleSheet(QString("QLabel {background:transparent; color:%1; font-family:'Segoe UI','Microsoft YaHei';}")
                         .arg(dark ? "#e9edf3" : "#2c3e50"));
    if (!subtitle.isEmpty() && !embedded) {
        auto *label = new QLabel(subtitle, parent);
        label->setStyleSheet("font-size:11px; color:#71808d;");
        layout->addWidget(label);
    }
    if (!embedded) {
        auto *heading = new QLabel(title, parent);
        heading->setStyleSheet("font-size:17px; font-weight:normal;");
        layout->addWidget(heading);
    }
    // 预览稿 .rb：透明底、13px 墨色文字、悬停 accent 淡底。
    const QString rowStyle = QString(
        "QPushButton { background:transparent; border:none; border-radius:10px;"
        " color:%1; font-size:13px; padding:0 10px; text-align:left; }"
        "QPushButton:hover { background:rgba(47,128,237,26); color:%2; }"
        "QPushButton:pressed { background:rgba(47,128,237,44); }")
        .arg(dark ? "#e9edf3" : "#22364a", dark ? "#3f8cf2" : "#2F80ED");
    QVector<QPushButton *> result;
    for (int group = 0; group < 2; ++group) {
        const int begin = group == 0 ? 0 : splitIndex;
        const int end = group == 0 ? splitIndex : texts.size();
        if (begin == end) continue;
        layout->addSpacing(embedded ? 6 : 8);
        if (group == 1) {
            auto *line = new QWidget(parent);
            line->setFixedHeight(1);
            line->setStyleSheet("background:rgba(146,166,181,42);");
            layout->addWidget(line);
            layout->addSpacing(embedded ? 4 : 6);
        }
        auto *label = new QLabel(group == 0 ? firstGroup : secondGroup, parent);
        label->setStyleSheet(QString("font-size:11px; color:%1; padding:0 2px;").arg(inkDim.name()));
        layout->addWidget(label);
        auto *grid = new QGridLayout;
        grid->setHorizontalSpacing(8);
        grid->setVerticalSpacing(embedded ? 2 : 3);
        grid->setColumnStretch(0, 1);
        grid->setColumnStretch(1, 1);
        for (int index = begin; index < end; ++index) {
            auto *button = new QPushButton(texts[index], parent);
            button->setObjectName(QString("drawerAction%1").arg(index));
            button->setCursor(Qt::PointingHandCursor);
            button->setFixedHeight(embedded ? 37 : DrawerButtonHeight);
            button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            button->setToolTip(texts[index]);
            if (embedded) {
                button->setStyleSheet(rowStyle);
                if (index < icons.size() && !icons[index].isEmpty()) {
                    button->setIcon(menuIcon(icons[index], inkDim, parent->devicePixelRatioF()));
                    button->setIconSize(QSize(16, 16));
                }
            } else {
                button->setStyleSheet(textButtonStyle());
            }
            // spanIndex：嵌入态中占满整行（跨两列）的按钮，如配置页的「打开NDM」。
            const int offset = index - begin;
            if (embedded && index == spanIndex)
                grid->addWidget(button, offset / 2 + (offset % 2), 0, 1, 2);
            else
                grid->addWidget(button, offset / 2, offset % 2);
            result.append(button);
        }
        layout->addLayout(grid);
    }
    layout->addStretch(1);
    if (floating) parent->adjustSize();
    return result;
}

void UIHelper::positionDrawer(QWidget *window, const QRect &menuGeometry)
{
    window->adjustSize();
    QScreen *screen = QGuiApplication::screenAt(menuGeometry.center());
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (!screen) return;
    const QRect area = screen->availableGeometry();
    int x = menuGeometry.left() - window->width() - 12;
    if (x < area.left()) x = menuGeometry.right() + 13;
    x = std::max(area.left(), std::min(x, area.right() - window->width() + 1));
    const int y = std::max(area.top(), std::min(menuGeometry.center().y() - window->height() / 2,
                                               area.bottom() - window->height() + 1));
    window->move(x, y);
}

QVector<QPushButton *> UIHelper::createMenuButtons(QWidget *parent,
                                                 QVBoxLayout *layout,
                                                 const QStringList &texts)
{
    QVector<QPushButton *> buttons;
    for (int index = 0; index < texts.size(); ++index) {
        QPushButton *button = new QPushButton(texts[index], parent);
        button->setFixedHeight(MenuButtonHeight);
        button->setCursor(Qt::PointingHandCursor);
        button->setMinimumWidth(120);
        button->setStyleSheet(getButtonStyle(index, texts.size()));
        layout->addWidget(button);
        buttons.append(button);
    }
    return buttons;
}

QString UIHelper::getButtonStyle(int index, int total)
{
    bool isFirst = (index == 0);
    bool isLast = (index == total - 1);

    QString bgColor = isLast ? "rgba(176, 205, 216, 245)" : "rgba(195, 219, 228, 245)";
    QString hoverColor = isLast ? "rgba(162, 195, 207, 250)" : "rgba(180, 210, 221, 250)";
    QString pressColor = isLast ? "rgba(146, 183, 197, 255)" : "rgba(162, 197, 211, 255)";

    QString border;
    if (isFirst) {
        border = "border-top-left-radius: 10px; border-top-right-radius: 10px;";
    } else if (isLast) {
        border = "border-top: 1px solid rgba(255, 255, 255, 130); "
                 "border-bottom-left-radius: 10px; border-bottom-right-radius: 10px;";
    } else {
        border = "border-top: 1px solid rgba(255, 255, 255, 130);";
    }

    return QString(
        "QPushButton {"
        "   background-color: %1;"
        "   color: #2c3e50;"
        "   border: none;"
        "   %2"
        "   font-size: 13px;"
        "   font-weight: bold;"
        "   padding: 6px 8px;"
        "}"
        "QPushButton:hover {"
        "   background-color: %3;"
        "}"
        "QPushButton:pressed {"
        "   background-color: %4;"
        "}"
    ).arg(bgColor, border, hoverColor, pressColor);
}

QString UIHelper::getStandardButtonStyle()
{
    return QString(
        "QPushButton {"
        "   background-color: rgba(100, 160, 180, 230);"
        "   color: white;"
        "   border: none;"
        "   border-radius: 8px;"
        "   font-size: 13px;"
        "   font-weight: bold;"
        "   padding: 6px 10px;"
        "}"
        "QPushButton:hover {"
        "   background-color: rgba(80, 140, 160, 220);"
        "}"
        "QPushButton:pressed {"
        "   background-color: rgba(60, 120, 140, 240);"
        "}"
        "QPushButton:disabled {"
        "   background-color: rgba(150, 150, 150, 120);"
        "   color: rgba(255, 255, 255, 150);"
        "}"
    );
}

void UIHelper::showCenteredMessageBox(QMessageBox::Icon icon,
                                     const QString &title,
                                     const QString &text,
                                     QWidget *parent)
{
    QMessageBox msgBox(icon, title, text, QMessageBox::Ok, parent);
    msgBox.setWindowFlags(Qt::Dialog | Qt::WindowStaysOnTopHint);

    centerMessageBox(msgBox);

    msgBox.exec();
}

QMessageBox::StandardButton UIHelper::showCenteredQuestion(const QString &title,
                                                          const QString &text,
                                                          QWidget *parent)
{
    QMessageBox msgBox(QMessageBox::Question, title, text,
                      QMessageBox::Yes | QMessageBox::No, parent);
    msgBox.setWindowFlags(Qt::Dialog | Qt::WindowStaysOnTopHint);

    centerMessageBox(msgBox);

    return static_cast<QMessageBox::StandardButton>(msgBox.exec());
}

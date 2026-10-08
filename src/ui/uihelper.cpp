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
        const bool dark = QApplication::palette().color(QPalette::Window).lightness() < 128;
        QColor fill = capsule ? (dark ? QColor(39, 56, 64) : QColor(195, 219, 228))
                              : (dark ? QColor(35, 42, 48) : QColor(255, 255, 255));
        fill.setAlpha(capsule ? 102 : 184);
        QPainter painter(window);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setBrush(fill);
        painter.setPen(QPen(dark ? QColor(255, 255, 255, 32) : QColor(255, 255, 255, 180), 1));
        const qreal radius = capsule ? 29 : 28;
        painter.drawRoundedRect(QRectF(window->rect()).adjusted(.5, .5, -.5, -.5), radius, radius);
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
    window->setProperty("surfaceRadius", capsule ? 29 : 28);
    window->installEventFilter(new FloatingSurfacePainter(window));
}

QIcon UIHelper::menuIcon(const QString &name, qreal ratio)
{
    QFile file(":/menu-icons/" + name + ".svg");
    if (!file.open(QIODevice::ReadOnly)) return {};
    QByteArray svg = file.readAll();
    const bool dark = QApplication::palette().color(QPalette::Window).lightness() < 128;
    svg.replace("currentColor", dark ? "#c8d6df" : "#526d7b");
    QSvgRenderer renderer(svg);
    QPixmap pixmap(qRound(24 * ratio), qRound(24 * ratio));
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    renderer.render(&painter, QRectF(2 * ratio, 2 * ratio, 20 * ratio, 20 * ratio));
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
    int splitIndex, const QString &firstGroup, const QString &secondGroup)
{
    applyFloatingSurface(parent);
    parent->setFixedWidth(318);
    layout->setContentsMargins(22, 20, 22, 18);
    layout->setSpacing(4);
    const bool dark = QApplication::palette().color(QPalette::Window).lightness() < 128;
    parent->setStyleSheet(QString("QLabel {background:transparent; color:%1; font-family:'Segoe UI','Microsoft YaHei';}")
                         .arg(dark ? "#e9edf3" : "#2c3e50"));
    if (!subtitle.isEmpty()) {
        auto *label = new QLabel(subtitle, parent);
        label->setStyleSheet("font-size:11px; color:#71808d;");
        layout->addWidget(label);
    }
    auto *heading = new QLabel(title, parent);
    heading->setStyleSheet("font-size:17px; font-weight:normal;");
    layout->addWidget(heading);
    QVector<QPushButton *> result;
    for (int group = 0; group < 2; ++group) {
        const int begin = group == 0 ? 0 : splitIndex;
        const int end = group == 0 ? splitIndex : texts.size();
        if (begin == end) continue;
        layout->addSpacing(10);
        if (group == 1) {
            auto *line = new QWidget(parent);
            line->setFixedHeight(1);
            line->setStyleSheet("background:rgba(146,166,181,42);");
            layout->addWidget(line);
            layout->addSpacing(6);
        }
        auto *label = new QLabel(group == 0 ? firstGroup : secondGroup, parent);
        label->setStyleSheet(QString("font-size:11px; color:%1;").arg(dark ? "#adb8c4" : "#71808d"));
        layout->addWidget(label);
        auto *grid = new QGridLayout;
        grid->setHorizontalSpacing(8);
        grid->setVerticalSpacing(3);
        grid->setColumnStretch(0, 1);
        grid->setColumnStretch(1, 1);
        for (int index = begin; index < end; ++index) {
            auto *button = new QPushButton(texts[index], parent);
            button->setObjectName(QString("drawerAction%1").arg(index));
            button->setCursor(Qt::PointingHandCursor);
            button->setFixedHeight(38);
            button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            button->setToolTip(texts[index]);
            button->setStyleSheet(textButtonStyle());
            grid->addWidget(button, (index - begin) / 2, (index - begin) % 2);
            result.append(button);
        }
        layout->addLayout(grid);
    }
    parent->adjustSize();
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


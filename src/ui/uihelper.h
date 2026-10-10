#ifndef UIHELPER_H
#define UIHELPER_H

#include <QString>
#include <QStringList>
#include <QVector>
#include <QMessageBox>
#include <QWidget>
#include <QIcon>

class QPushButton;
class QVBoxLayout;

class UIHelper
{
public:
    // 统一菜单密度，不改变文字样式和横向布局。
    enum { MenuButtonHeight = 40 };
    // Shared logical-pixel dimensions for the capsule, control center and first-level drawers.
    enum { MainMenuWidth = 48, MainMenuButtonSize = 38, MainMenuIconSize = 24,
           ControlCenterWidth = 296, ControlCenterHeight = 452,
           DrawerWidth = 296, DrawerHeight = 336,
           DrawerHorizontalMargin = 20, DrawerVerticalMargin = 18, DrawerButtonHeight = 34 };

    // 为纵向菜单创建统一样式的按钮；各窗口自己连接业务信号。
    static QVector<QPushButton *> createMenuButtons(QWidget *parent,
                                                  QVBoxLayout *layout,
                                                  const QStringList &texts);

    // 获取按钮样式（用于菜单窗口）
    static QString getButtonStyle(int index, int total);

    // 获取普通按钮样式
    static QString getStandardButtonStyle();

    // Paint only the background alpha; text and controls remain fully readable.
    static void applyFloatingSurface(QWidget *window, bool capsule = false);
    // Control-center panel: same translucent glass as drawers, but 24px radius
    // and no forced size (the panel manages its own fixed size).
    static void applyControlCenterSurface(QWidget *panel);
    static QIcon menuIcon(const QString &name, qreal devicePixelRatio = 1.0);
    // 控制中心内的强调色图标：按预览稿使用主题 accent 而非墨色。
    static QIcon menuIcon(const QString &name, const QColor &color, qreal devicePixelRatio = 1.0);
    static QString textButtonStyle();
    static QVector<QPushButton *> createDrawerButtons(QWidget *parent, QVBoxLayout *layout,
        const QString &title, const QString &subtitle, const QStringList &texts,
        int splitIndex, const QString &firstGroup, const QString &secondGroup,
        bool floating = true);
    static void positionDrawer(QWidget *window, const QRect &menuGeometry);

    // 显示居中的消息框
    static void showCenteredMessageBox(QMessageBox::Icon icon,
                                      const QString &title,
                                      const QString &text,
                                      QWidget *parent = nullptr);

    // 显示居中的确认对话框
    static QMessageBox::StandardButton showCenteredQuestion(const QString &title,
                                                           const QString &text,
                                                           QWidget *parent = nullptr);

private:
    UIHelper() = default;
};

#endif // UIHELPER_H

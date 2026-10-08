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
    // Shared logical-pixel dimensions for the capsule and its first-level drawers.
    enum { MainMenuWidth = 48, MainMenuButtonSize = 38, MainMenuIconSize = 24,
           DrawerWidth = 318, DrawerHeight = 380 };

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
    static QIcon menuIcon(const QString &name, qreal devicePixelRatio = 1.0);
    static QString textButtonStyle();
    static QVector<QPushButton *> createDrawerButtons(QWidget *parent, QVBoxLayout *layout,
        const QString &title, const QString &subtitle, const QStringList &texts,
        int splitIndex, const QString &firstGroup, const QString &secondGroup);
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

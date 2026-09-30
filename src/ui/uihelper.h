#ifndef UIHELPER_H
#define UIHELPER_H

#include <QString>
#include <QStringList>
#include <QVector>
#include <QMessageBox>
#include <QWidget>

class QPushButton;
class QVBoxLayout;

class UIHelper
{
public:
    // 统一菜单密度，不改变文字样式和横向布局。
    enum { MenuButtonHeight = 40 };

    // 为纵向菜单创建统一样式的按钮；各窗口自己连接业务信号。
    static QVector<QPushButton *> createMenuButtons(QWidget *parent,
                                                  QVBoxLayout *layout,
                                                  const QStringList &texts);

    // 获取按钮样式（用于菜单窗口）
    static QString getButtonStyle(int index, int total);

    // 获取普通按钮样式
    static QString getStandardButtonStyle();

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

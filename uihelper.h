#ifndef UIHELPER_H
#define UIHELPER_H

#include <QString>
#include <QMessageBox>
#include <QWidget>

class UIHelper
{
public:
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

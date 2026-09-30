#include <QApplication>
#include <QDebug>
#include <QMessageBox>
#include "menuwidget.h"
#include "passworddialog.h"
#include "processmanager.h"
#include "resourceextractor.h"
#include "integritychecker.h"
#include "version.h"

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    
    // 设置应用程序信息
    a.setApplicationName(APP_NAME);
    a.setOrganizationName(APP_ORGANIZATION);
    a.setApplicationVersion(APP_VERSION);
    
    // 程序完整性验证
    IntegrityChecker::CheckResult integrityResult = IntegrityChecker::verifyIntegrity();
    if (!integrityResult.success) {
        QString errorMsg = integrityResult.errorMessage;
        if (!integrityResult.missingFiles.isEmpty()) {
            errorMsg += "\n\n缺失文件: " + integrityResult.missingFiles.join(", ");
        }
        if (!integrityResult.corruptedFiles.isEmpty()) {
            errorMsg += "\n\n损坏文件: " + integrityResult.corruptedFiles.join(", ");
        }
        QMessageBox::critical(nullptr, "完整性验证失败", errorMsg);
        qDebug() << "程序完整性验证失败，程序退出";
        return -1;
    }
    
    // 提取嵌入的资源文件
    if (!ResourceExtractor::extractResources()) {
        qDebug() << "资源提取失败，程序可能无法正常工作";
    }
    
    // 清空之前的PID记录
    ProcessManager::clearPIDFile();
    
    // 密码验证
    PasswordDialog passwordDialog;
    if (passwordDialog.exec() != QDialog::Accepted) {
        // 用户取消或密码错误次数过多，退出程序
        qDebug() << "用户取消或密码验证失败，程序退出";
        return 0;
    }
    
    qDebug() << "密码验证成功，启动主界面";
    
    // 密码验证通过后，直接显示主菜单
    MenuWidget menu;
    menu.show();
    
    return a.exec();
}

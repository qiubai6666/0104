#include "uihelper.h"
#include <QScreen>
#include <QGuiApplication>

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
    
    // 显示后再居中
    msgBox.show();
    QRect screenGeometry = QGuiApplication::primaryScreen()->geometry();
    int x = (screenGeometry.width() - msgBox.width()) / 2;
    int y = (screenGeometry.height() - msgBox.height()) / 2;
    msgBox.move(x, y);
    
    msgBox.exec();
}

QMessageBox::StandardButton UIHelper::showCenteredQuestion(const QString &title,
                                                          const QString &text,
                                                          QWidget *parent)
{
    QMessageBox msgBox(QMessageBox::Question, title, text, 
                      QMessageBox::Yes | QMessageBox::No, parent);
    msgBox.setWindowFlags(Qt::Dialog | Qt::WindowStaysOnTopHint);
    
    // 显示后再居中
    msgBox.show();
    QRect screenGeometry = QGuiApplication::primaryScreen()->geometry();
    int x = (screenGeometry.width() - msgBox.width()) / 2;
    int y = (screenGeometry.height() - msgBox.height()) / 2;
    msgBox.move(x, y);
    
    return static_cast<QMessageBox::StandardButton>(msgBox.exec());
}

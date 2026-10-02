#ifndef MENUWIDGET_H
#define MENUWIDGET_H

#include <QWidget>
#include <QVector>
#include <QProcess>

class QPushButton;
class QVBoxLayout;
class DeviceInfoWindow;
class RepairWindow;
class PayloadWindow;
class DeviceCheckWindow;
class ConfigWindow;

class MenuWidget : public QWidget
{
    Q_OBJECT

public:
    explicit MenuWidget(QWidget *parent = nullptr);
    ~MenuWidget();

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onButtonClicked();
    void onLsProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onPullProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onCleanupProcessFinished();

private:
    friend class DeviceOperationTests;
    // 与 setupUI() 中按钮顺序对应，避免在业务代码中直接使用数字索引。
    enum MenuAction {
        ScreenCast, RepairTools, Payload, ExtractImg, DeviceCheck,
        Configuration, ContactAuthor, Exit, Minimize
    };

    void setupUI();
    void updatePosition();
    void extractImg();
    void cleanupAndExit();
    void openAuthorImage();
    void minimizeWindows();

    QVBoxLayout *mainLayout;
    QVector<QPushButton*> buttons;
    DeviceInfoWindow *deviceInfoWindow;
    RepairWindow *repairWindow;
    PayloadWindow *payloadWindow;
    DeviceCheckWindow *deviceCheckWindow;
    ConfigWindow *configWindow;

    QProcess *imgProcess;
    QString latestImgFile;
    QString imgOutputPath;
};

#endif // MENUWIDGET_H

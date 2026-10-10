#ifndef MENUWIDGET_H
#define MENUWIDGET_H

#include <QWidget>
#include <QVector>
#include <QHash>
#include <QProcess>

class QPushButton;
class QStackedWidget;
class QVBoxLayout;
class QLabel;
class ScreenCastController;
class QTimer;
class RepairWindow;
class PayloadWindow;
class DeviceCheckWindow;
class ConfigWindow;
class FloatingBall;

// 控制中心面板：平时隐藏，由悬浮球（FloatingBall）唤起。
// 主页提供投屏开关行、设备行与六个功能 tile；四个子窗口以嵌入页方式翻页展示。
class MenuWidget : public QWidget
{
    Q_OBJECT

public:
    explicit MenuWidget(QWidget *parent = nullptr);
    ~MenuWidget();

    // 仅显示悬浮球；控制中心面板保持隐藏。
    void showBall();

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void changeEvent(QEvent *event) override;
    void setVisible(bool visible) override;

private slots:
    void onButtonClicked();
    void onLsProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onPullProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onCleanupProcessFinished();

private:
    friend class DeviceOperationTests;
    friend class ScreenCastTests;
    // 与 setupUI() 中按钮顺序对应，避免在业务代码中直接使用数字索引。
    enum MenuAction {
        ScreenCast, RepairTools, Payload, ExtractImg, DeviceCheck,
        Configuration, ContactAuthor, Exit, Minimize
    };

    void updateScreenCastButton();
    void paintScreenCastSpinner();
    void setupUI();
    QWidget *buildHomePage();
    void toggleSubmenu(MenuAction action, QWidget *window);
    void showSubmenu(MenuAction action, QWidget *window);
    void showHome();
    void collapsePanel();
    void togglePanel();
    void updateMenuSelection();
    QWidget *fileDropSurface(QWidget *widget);
    bool handleFileDrop(QWidget *surface, QEvent *event);
    bool canAcceptFileTransfer() const;
    void updatePosition();
    void positionPanel();
    void extractImg();
    void cleanupAndExit();
    void openAuthorImage();
    void updateDeviceRow();

    RepairWindow *ensureRepairWindow();
    PayloadWindow *ensurePayloadWindow();
    DeviceCheckWindow *ensureDeviceCheckWindow();
    ConfigWindow *ensureConfigWindow();

    QVBoxLayout *mainLayout;
    QStackedWidget *pages = nullptr;
    QWidget *homePage = nullptr;
    QWidget *header = nullptr;
    QPushButton *backButton = nullptr;
    QLabel *pageTitle = nullptr;
    QLabel *screenCastSubtitle = nullptr;
    QLabel *screenCastPill = nullptr;
    QLabel *screenCastIconLabel = nullptr;
    QLabel *devicePill = nullptr;
    QHash<QWidget *, QString> pageTitles;
    FloatingBall *ball = nullptr;
    QVector<QPushButton*> buttons;
    ScreenCastController *screenCastController;
    QTimer *screenCastSpinnerTimer;
    int screenCastSpinnerAngle = 0;
    RepairWindow *repairWindow;
    PayloadWindow *payloadWindow;
    DeviceCheckWindow *deviceCheckWindow;
    ConfigWindow *configWindow;

    QProcess *imgProcess;
    QString latestImgFile;
    QString imgOutputPath;
};

#endif // MENUWIDGET_H

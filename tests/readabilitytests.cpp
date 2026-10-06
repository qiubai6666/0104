#include "passworddialog.h"
#include "uihelper.h"
#include "version.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QtTest>
#include <QVBoxLayout>
#include <QXmlStreamReader>

// 只测试本地界面和内嵌资源：不会提取资源到运行目录、连接设备、访问网络或执行外部工具。
class ReadabilityTests : public QObject
{
    Q_OBJECT

private slots:
    void embeddedToolsMatchSourceFiles();
    void applicationNameUsesOrangeTools();
    void menuButtonsKeepOrderAndPresentation();
    void menuButtonsHandleEmptyList();
    void passwordDialogKeepsPresentation();
    void passwordAcceptsConfiguredDefault();
    void emptyPasswordDoesNotConsumeAttempt();
    void incorrectPasswordCanBeRetried();
    void threeIncorrectPasswordsRejectDialog();
    void keyboardPasswordSubmission_data();
    void keyboardPasswordSubmission();
    void cancelButtonStillRejectsDialog();
};

void ReadabilityTests::embeddedToolsMatchSourceFiles()
{
    // 先从构建目录向上定位项目，避免 Qt Test 在中文路径下的源码路径编码问题。
    QString manifestPath;
    QDir directory(QCoreApplication::applicationDirPath());
    do {
        if (directory.exists("OrangeTools.pro") && directory.exists("resources.qrc")) {
            manifestPath = directory.filePath("resources.qrc");
            break;
        }
    } while (directory.cdUp());
    if (manifestPath.isEmpty()) {
        manifestPath = QFINDTESTDATA("../resources.qrc");
    }
    QVERIFY2(!manifestPath.isEmpty(), "Cannot find resource manifest");
    QFile manifest(manifestPath);
    QVERIFY(manifest.open(QIODevice::ReadOnly));
    const QDir projectDirectory(QFileInfo(manifestPath).absolutePath());
    QXmlStreamReader xml(&manifest);
    int verifiedFiles = 0;

    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement() || xml.name() != QLatin1String("file")) {
            continue;
        }
        const QString relativePath = xml.readElementText();
        QFile source(projectDirectory.filePath(relativePath));
        QFile embedded(":/qiubai/" + relativePath);
        QVERIFY2(source.open(QIODevice::ReadOnly), qPrintable(source.fileName()));
        QVERIFY2(embedded.open(QIODevice::ReadOnly), qPrintable(embedded.fileName()));
        QCOMPARE(embedded.size(), source.size());
        QCOMPARE(QCryptographicHash::hash(embedded.readAll(), QCryptographicHash::Sha256),
                 QCryptographicHash::hash(source.readAll(), QCryptographicHash::Sha256));
        ++verifiedFiles;
    }
    QVERIFY2(!xml.hasError(), qPrintable(xml.errorString()));
    QVERIFY(verifiedFiles > 0);
    qInfo() << "Verified embedded resource files:" << verifiedFiles;
}

void ReadabilityTests::applicationNameUsesOrangeTools()
{
    QCOMPARE(QString::fromUtf8(APP_NAME), QStringLiteral("Orange Tools"));
}

void ReadabilityTests::menuButtonsKeepOrderAndPresentation()
{
    QWidget panel;
    QVBoxLayout *layout = new QVBoxLayout(&panel);
    const QStringList labels = {"投屏", "提取IMG", "退出"};
    const QVector<QPushButton *> buttons = UIHelper::createMenuButtons(&panel, layout, labels);

    QCOMPARE(buttons.size(), labels.size());
    QCOMPARE(layout->count(), labels.size());
    for (int index = 0; index < buttons.size(); ++index) {
        QCOMPARE(buttons[index]->text(), labels[index]);
        QCOMPARE(buttons[index]->height(), int(UIHelper::MenuButtonHeight));
        QCOMPARE(buttons[index]->minimumWidth(), 120);
        QCOMPARE(buttons[index]->cursor().shape(), Qt::PointingHandCursor);
        QCOMPARE(buttons[index]->styleSheet(), UIHelper::getButtonStyle(index, labels.size()));
        QCOMPARE(layout->itemAt(index)->widget(), buttons[index]);
        QCOMPARE(buttons[index]->parentWidget(), &panel);
    }
}

void ReadabilityTests::menuButtonsHandleEmptyList()
{
    QWidget panel;
    QVBoxLayout *layout = new QVBoxLayout(&panel);
    QVERIFY(UIHelper::createMenuButtons(&panel, layout, {}).isEmpty());
    QCOMPARE(layout->count(), 0);
}

void ReadabilityTests::passwordDialogKeepsPresentation()
{
    PasswordDialog dialog;
    QCOMPARE(dialog.size(), QSize(300, 164));
    QCOMPARE(dialog.findChild<QLineEdit *>()->echoMode(), QLineEdit::Password);
    QVERIFY(dialog.windowFlags().testFlag(Qt::FramelessWindowHint));
}

void ReadabilityTests::passwordAcceptsConfiguredDefault()
{
    PasswordDialog dialog;
    QSignalSpy accepted(&dialog, &QDialog::accepted);
    dialog.findChild<QLineEdit *>()->setText(DEFAULT_PASSWORD);
    QVERIFY(QMetaObject::invokeMethod(&dialog, "onOkClicked", Qt::DirectConnection));
    QCOMPARE(accepted.count(), 1);
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
}

void ReadabilityTests::emptyPasswordDoesNotConsumeAttempt()
{
    PasswordDialog dialog;
    QSignalSpy rejected(&dialog, &QDialog::rejected);
    QSignalSpy accepted(&dialog, &QDialog::accepted);
    for (int attempt = 0; attempt < 3; ++attempt) {
        QVERIFY(QMetaObject::invokeMethod(&dialog, "onOkClicked", Qt::DirectConnection));
    }
    QLineEdit *input = dialog.findChild<QLineEdit *>();
    input->setText("wrong");
    QVERIFY(QMetaObject::invokeMethod(&dialog, "onOkClicked", Qt::DirectConnection));
    bool foundRetryMessage = false;
    for (QLabel *label : dialog.findChildren<QLabel *>()) {
        foundRetryMessage |= label->text().contains("还有 2 次机会");
    }
    QVERIFY(foundRetryMessage);
    input->setText(DEFAULT_PASSWORD);
    QVERIFY(QMetaObject::invokeMethod(&dialog, "onOkClicked", Qt::DirectConnection));
    QCOMPARE(accepted.count(), 1);
    QCOMPARE(rejected.count(), 0);
}

void ReadabilityTests::incorrectPasswordCanBeRetried()
{
    PasswordDialog dialog;
    QSignalSpy accepted(&dialog, &QDialog::accepted);
    QLineEdit *input = dialog.findChild<QLineEdit *>();
    input->setText("wrong");
    QVERIFY(QMetaObject::invokeMethod(&dialog, "onOkClicked", Qt::DirectConnection));
    QVERIFY(input->text().isEmpty());
    QCOMPARE(accepted.count(), 0);
    input->setText(DEFAULT_PASSWORD);
    QVERIFY(QMetaObject::invokeMethod(&dialog, "onOkClicked", Qt::DirectConnection));
    QCOMPARE(accepted.count(), 1);
}

void ReadabilityTests::threeIncorrectPasswordsRejectDialog()
{
    PasswordDialog dialog;
    QSignalSpy rejected(&dialog, &QDialog::rejected);
    QLineEdit *input = dialog.findChild<QLineEdit *>();
    for (int attempt = 0; attempt < 3; ++attempt) {
        input->setText("wrong");
        QVERIFY(QMetaObject::invokeMethod(&dialog, "onOkClicked", Qt::DirectConnection));
    }
    QTRY_COMPARE_WITH_TIMEOUT(rejected.count(), 1, 2500);
    QCOMPARE(dialog.result(), int(QDialog::Rejected));
}

void ReadabilityTests::keyboardPasswordSubmission_data()
{
    QTest::addColumn<int>("key");
    QTest::addColumn<bool>("emptyFirst");
    QTest::addColumn<bool>("exhaustAttempts");
    for (int key : {int(Qt::Key_Return), int(Qt::Key_Enter)}) {
        const QByteArray name = key == Qt::Key_Return ? "return" : "keypad-enter";
        QTest::newRow((name + "-retry").constData()) << key << false << false;
        QTest::newRow((name + "-empty-retry").constData()) << key << true << false;
        QTest::newRow((name + "-three-errors").constData()) << key << false << true;
    }
}

void ReadabilityTests::keyboardPasswordSubmission()
{
    QFETCH(int, key);
    QFETCH(bool, emptyFirst);
    QFETCH(bool, exhaustAttempts);
    PasswordDialog dialog;
    QSignalSpy accepted(&dialog, &QDialog::accepted);
    QSignalSpy rejected(&dialog, &QDialog::rejected);
    QLineEdit *input = dialog.findChild<QLineEdit *>();
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    dialog.activateWindow();
    input->setFocus();
    QTRY_VERIFY(input->hasFocus());

    if (emptyFirst) {
        for (int attempt = 0; attempt < 3; ++attempt) {
            QTest::keyClick(input, Qt::Key(key));
            QCOMPARE(rejected.count(), 0);
            QVERIFY(dialog.isVisible());
        }
    }
    for (int attempt = 0; attempt < (exhaustAttempts ? 3 : 2); ++attempt) {
        input->setText("wrong");
        QTest::keyClick(input, Qt::Key(key));
        QCOMPARE(accepted.count(), 0);
        QCOMPARE(rejected.count(), 0);
        QVERIFY(dialog.isVisible());
        if (attempt < 2) {
            QVERIFY(input->text().isEmpty());
            QVERIFY(input->hasFocus());
            bool foundRetryMessage = false;
            for (QLabel *label : dialog.findChildren<QLabel *>()) {
                foundRetryMessage |= label->text().contains(
                    QString("还有 %1 次机会").arg(2 - attempt));
            }
            QVERIFY(foundRetryMessage);
        }
    }
    if (exhaustAttempts) {
        QTRY_COMPARE_WITH_TIMEOUT(rejected.count(), 1, 2500);
        QVERIFY(!dialog.isVisible());
    } else {
        input->setText(DEFAULT_PASSWORD);
        QTest::keyClick(input, Qt::Key(key));
        QCOMPARE(accepted.count(), 1);
        QCOMPARE(rejected.count(), 0);
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
    }
}

void ReadabilityTests::cancelButtonStillRejectsDialog()
{
    PasswordDialog dialog;
    QSignalSpy rejected(&dialog, &QDialog::rejected);
    for (QPushButton *button : dialog.findChildren<QPushButton *>()) {
        if (button->text() == QStringLiteral("取消")) {
            button->click();
            QCOMPARE(rejected.count(), 1);
            return;
        }
    }
    QFAIL("Cannot find cancel button");
}
QTEST_MAIN(ReadabilityTests)
#include "readabilitytests.moc"

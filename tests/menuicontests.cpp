#include "uihelper.h"
#include "mainmenubutton.h"
#include <QEnterEvent>
#include <QFocusEvent>
#include <QtTest>
#include <QFile>
#include <QFontDatabase>
#include <QPainter>
#include <QSvgRenderer>

namespace {
const QStringList names = {"monitor-smartphone", "wrench", "package-open", "file-down",
    "smartphone", "sliders-horizontal", "message-circle", "power", "panel-right-close"};
const QList<qreal> scales = {1.0, 1.25, 1.5, 1.5625, 1.75, 2.0, 2.5, 3.0};
QImage renderButton(MainMenuButton &button, qreal ratio = 1.0)
{
    QImage image(QSize(qRound(button.width() * ratio), qRound(button.height() * ratio)),
                 QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(ratio);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    button.render(&painter, QPoint(), QRegion(), QWidget::DrawChildren);
    return image;
}
void enterButton(QWidget &button)
{
    QEnterEvent event(QPointF(19, 19), QPointF(19, 19), QPointF(19, 19));
    QCoreApplication::sendEvent(&button, &event);
}
void leaveButton(QWidget &button)
{
    QEvent event(QEvent::Leave);
    QCoreApplication::sendEvent(&button, &event);
}

void prepareButton(MainMenuButton &button, bool dark = false)
{
    QPalette palette = button.palette();
    palette.setColor(QPalette::Window, dark ? QColor(35, 42, 48) : QColor(240, 240, 240));
    button.setPalette(palette);
    button.setFixedSize(38, 38);
    button.setIcon(UIHelper::menuIcon("power", button.devicePixelRatioF()));
    button.setIconSize(QSize(24, 24));
    button.setAttribute(Qt::WA_DontShowOnScreen);
    button.show();
    leaveButton(button);
    auto *animation = button.findChild<QVariantAnimation *>();
    animation->setCurrentTime(animation->duration());
}
QColor strokeColor(const QImage &image)
{
    QColor result(Qt::transparent);
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            const QColor color = image.pixelColor(x, y);
            if (color.alpha() > result.alpha()) result = color;
        }
    return result;
}
QImage alphaMask(const QImage &image)
{
    QImage result(image.size(), QImage::Format_ARGB32);
    result.setDevicePixelRatio(image.devicePixelRatio());
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            result.setPixel(x, y, qRgba(255, 255, 255, qAlpha(image.pixel(x, y))));
    return result;
}

QImage reference(const QString &name, qreal ratio)
{
    QFile file(":/menu-icons/" + name + ".svg");
    if (!file.open(QIODevice::ReadOnly)) return {};
    QByteArray svg = file.readAll();
    const bool dark = QApplication::palette().color(QPalette::Window).lightness() < 128;
    svg.replace("currentColor", dark ? "#dce8ef" : "#385565");
    QSvgRenderer renderer(svg);
    if (!renderer.isValid()) return {};
    // Independent reference: paint at DPR=1, keeping the logical 24px bounds
    // even when the physical storage size must round a fractional pixel.
    const int pixels = qRound(24 * ratio);
    QImage image(pixels, pixels, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        renderer.render(&painter, QRectF(0, 0, 24 * ratio, 24 * ratio));
    }
    image.setDevicePixelRatio(ratio);
    return image;
}
}
class MenuIconTests : public QObject
{
    Q_OBJECT
private slots:
    void completeAtEveryDpi_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<qreal>("ratio");
        for (const auto &name : names)
            for (qreal ratio : scales)
                QTest::newRow(qPrintable(name + "@" + QString::number(ratio))) << name << ratio;
    }
    void completeAtEveryDpi()
    {
        QFETCH(QString, name);
        QFETCH(qreal, ratio);
        const QIcon icon = UIHelper::menuIcon(name, ratio);
        QVERIFY(!icon.isNull());
        const QImage actual = icon.pixmap(QSize(24, 24), ratio).toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
        const QImage expected = reference(name, ratio);
        QVERIFY(!expected.isNull());
        QCOMPARE(actual.size(), expected.size());
        QCOMPARE(actual.devicePixelRatio(), ratio);
        // Compare coverage, including right/bottom strokes; permit raster rounding only.
        int mismatches = 0;
        for (int y = 0; y < actual.height(); ++y)
            for (int x = 0; x < actual.width(); ++x)
                if (qAbs(qAlpha(actual.pixel(x, y)) - qAlpha(expected.pixel(x, y))) > 3)
                    ++mismatches;
        QCOMPARE(mismatches, 0);
    }
    void hoverTransitionsRetargetWithoutJumping()
    {
        MainMenuButton button;
        prepareButton(button);
        auto *animation = button.findChild<QVariantAnimation *>();
        QVERIFY(animation);
        const QImage resting = renderButton(button);
        QCOMPARE(strokeColor(resting), QColor("#68818e"));
        enterButton(button);
        QCOMPARE(renderButton(button), resting);
        animation->setCurrentTime(animation->duration() / 2);
        const auto midpoint = renderButton(button);
        const int lightness = strokeColor(midpoint).lightness();
        QVERIFY(lightness < QColor("#68818e").lightness());
        QVERIFY(lightness > QColor("#385565").lightness());
        QCOMPARE(alphaMask(midpoint), alphaMask(resting));
        leaveButton(button);
        QCOMPARE(renderButton(button), midpoint);
        animation->setCurrentTime(animation->duration() / 2);
        QVERIFY(strokeColor(renderButton(button)).lightness() > lightness);
        animation->setCurrentTime(animation->duration());
        QCOMPARE(renderButton(button), resting);
        QCOMPARE(button.size(), QSize(38, 38));
    }
    void mouseFocusAndDeselectionLeaveNoOutline()
    {
        MainMenuButton button;
        prepareButton(button);
        auto *animation = button.findChild<QVariantAnimation *>();
        QVERIFY(animation);
        const QImage resting = renderButton(button);
        QFocusEvent mouseFocus(QEvent::FocusIn, Qt::MouseFocusReason);
        QCoreApplication::sendEvent(&button, &mouseFocus);
        QCOMPARE(renderButton(button), resting);
        button.setProperty("menuSelected", true);
        animation->setCurrentTime(animation->duration());
        QVERIFY(renderButton(button) != resting);
        QCOMPARE(alphaMask(renderButton(button)), alphaMask(resting));
        button.setProperty("menuSelected", false);
        animation->setCurrentTime(animation->duration());
        QCOMPARE(renderButton(button), resting);
        QFocusEvent keyboardFocus(QEvent::FocusIn, Qt::TabFocusReason);
        QCoreApplication::sendEvent(&button, &keyboardFocus);
        QVERIFY(renderButton(button) != resting);
        QCoreApplication::sendEvent(&button, &mouseFocus);
        QCOMPARE(renderButton(button), resting);
    }
    void selectionAndHoverRemainIndependent()
    {
        MainMenuButton button;
        prepareButton(button);
        auto *animation = button.findChild<QVariantAnimation *>();
        QVERIFY(animation);
        button.setProperty("menuSelected", true);
        animation->setCurrentTime(animation->duration());
        QCOMPARE(strokeColor(renderButton(button)), QColor("#193f55"));
        enterButton(button);
        leaveButton(button);
        QCOMPARE(strokeColor(renderButton(button)), QColor("#193f55"));
        enterButton(button);
        button.setProperty("menuSelected", false);
        animation->setCurrentTime(animation->duration());
        QCOMPARE(strokeColor(renderButton(button)), QColor("#385565"));
        button.hide();
        QCOMPARE(strokeColor(renderButton(button)), QColor("#68818e"));
    }
    void themedHoverClearsWhenDisabled_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }
    void themedHoverClearsWhenDisabled()
    {
        QFETCH(bool, dark);
        MainMenuButton button;
        prepareButton(button, dark);
        auto *animation = button.findChild<QVariantAnimation *>();
        QVERIFY(animation);
        const QImage resting = renderButton(button);
        enterButton(button);
        animation->setCurrentTime(animation->duration());
        QCOMPARE(strokeColor(renderButton(button)), QColor(dark ? "#cedde6" : "#385565"));
        button.setProperty("menuSelected", true);
        animation->setCurrentTime(animation->duration());
        QCOMPARE(strokeColor(renderButton(button)), QColor(dark ? "#f4faff" : "#193f55"));
        button.setEnabled(false);
        QCOMPARE(strokeColor(renderButton(button)), QColor(dark ? "#556772" : "#9baab2"));
        QCOMPARE(alphaMask(renderButton(button)), alphaMask(resting));
        QCOMPARE(animation->state(), QAbstractAnimation::Stopped);
    }
    void pressAndDirectClickUseColorOnly()
    {
        MainMenuButton button;
        prepareButton(button);
        button.setProperty("menuDirectAction", true);
        auto *animation = button.findChild<QVariantAnimation *>();
        auto *feedback = button.findChild<QTimer *>();
        QVERIFY(animation);
        QVERIFY(feedback);
        const QImage resting = renderButton(button);
        QTest::mousePress(&button, Qt::LeftButton, Qt::NoModifier, QPoint(19, 19));
        animation->setCurrentTime(animation->duration());
        QCOMPARE(strokeColor(renderButton(button)), QColor("#0d2939"));
        QCOMPARE(alphaMask(renderButton(button)), alphaMask(resting));
        QTest::mouseRelease(&button, Qt::LeftButton, Qt::NoModifier, QPoint(19, 19));
        QVERIFY(feedback->isActive());
        animation->setCurrentTime(animation->duration());
        QCOMPARE(strokeColor(renderButton(button)), QColor("#193f55"));
        QVERIFY(!button.property("menuSelected").toBool());
        // Simulate expiry deterministically, without depending on wall-clock timing.
        feedback->stop();
        QVERIFY(QMetaObject::invokeMethod(feedback, "timeout", Qt::DirectConnection));
        animation->setCurrentTime(animation->duration());
        leaveButton(button);
        animation->setCurrentTime(animation->duration());
        QCOMPARE(renderButton(button), resting);
        button.click();
        QVERIFY(feedback->isActive());
        button.hide();
        QVERIFY(!feedback->isActive());
        QCOMPARE(renderButton(button), resting);
    }
    void spinnerTintPreservesPartialAlpha()
    {
        MainMenuButton button;
        prepareButton(button);
        QPixmap source(24, 24);
        source.fill(Qt::transparent);
        {
            QPainter painter(&source);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(QPen(QColor(44, 62, 80, 45), 2.5));
            painter.drawEllipse(QRectF(3, 3, 18, 18));
            painter.setPen(QPen(QColor("#2c3e50"), 2.5));
            painter.drawArc(QRectF(3, 3, 18, 18), 0, 260 * 16);
        }
        button.setIcon(QIcon(source));
        const QImage resting = renderButton(button);
        button.setProperty("menuSelected", true);
        auto *animation = button.findChild<QVariantAnimation *>();
        animation->setCurrentTime(animation->duration());
        const QImage selected = renderButton(button);
        QCOMPARE(alphaMask(selected), alphaMask(resting));
        QCOMPARE(strokeColor(selected), QColor("#193f55"));
    }
    void buttonIconsCompleteAtCurrentDpi_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("state");
        for (const auto &name : names)
            for (const auto &state : {"resting", "hovered", "selected", "pressed", "disabled"})
                QTest::newRow(qPrintable(name + "-" + state)) << name << QString(state);
    }
    void buttonIconsCompleteAtCurrentDpi()
    {
        QFETCH(QString, name);
        QFETCH(QString, state);
        MainMenuButton button;
        prepareButton(button);
        const qreal ratio = button.devicePixelRatioF();
        button.setIcon(UIHelper::menuIcon(name, ratio));
        if (state == "hovered") enterButton(button);
        if (state == "selected") button.setProperty("menuSelected", true);
        if (state == "pressed") QTest::mousePress(&button, Qt::LeftButton, Qt::NoModifier, QPoint(19, 19));
        if (state == "disabled") button.setEnabled(false);
        auto *animation = button.findChild<QVariantAnimation *>();
        animation->setCurrentTime(animation->duration());
        const QImage actual = renderButton(button, ratio);
        QImage expected(actual.size(), QImage::Format_ARGB32_Premultiplied);
        expected.setDevicePixelRatio(ratio);
        expected.fill(Qt::transparent);
        {
            QPainter painter(&expected);
            painter.drawImage(QPointF(7, 7), reference(name, ratio));
        }
        int mismatches = 0;
        for (int y = 0; y < actual.height(); ++y)
            for (int x = 0; x < actual.width(); ++x)
                if (qAbs(qAlpha(actual.pixel(x, y)) - qAlpha(expected.pixel(x, y))) > 3)
                    ++mismatches;
        QCOMPARE(mismatches, 0);
    }
    void floatingSurfaceOutline_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<bool>("capsule");
        QTest::addColumn<qreal>("ratio");
        for (bool dark : {false, true})
            for (bool capsule : {false, true})
                for (qreal ratio : scales)
                    QTest::newRow(qPrintable(QString("%1-%2@%3")
                        .arg(dark ? "dark" : "light", capsule ? "capsule" : "drawer")
                        .arg(ratio))) << dark << capsule << ratio;
    }
    void floatingSurfaceOutline()
    {
        QFETCH(bool, dark);
        QFETCH(bool, capsule);
        QFETCH(qreal, ratio);
        const QPalette originalPalette = QApplication::palette();
        QPalette palette = originalPalette;
        palette.setColor(QPalette::Window, dark ? QColor(35, 42, 48) : QColor(240, 240, 240));
        QApplication::setPalette(palette);
        QWidget window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        UIHelper::applyFloatingSurface(&window, capsule);
        if (capsule) window.setFixedSize(48, 372);
        const QSize pixelSize(qRound(window.width() * ratio), qRound(window.height() * ratio));
        QImage actual(pixelSize, QImage::Format_ARGB32_Premultiplied);
        actual.setDevicePixelRatio(ratio);
        actual.fill(Qt::transparent);
        {
            QPainter painter(&actual);
            window.render(&painter);
        }
        QApplication::setPalette(originalPalette);

        // Independent raster reference: the capsule keeps its original outline;
        // only the 28px drawers receive the clearer 1.5px blue-grey outline.
        QImage expected(pixelSize, QImage::Format_ARGB32_Premultiplied);
        expected.setDevicePixelRatio(ratio);
        expected.fill(Qt::transparent);
        QColor fill = capsule ? (dark ? QColor(39, 56, 64) : QColor(195, 219, 228))
                              : (dark ? QColor(35, 42, 48) : QColor(255, 255, 255));
        fill.setAlpha(capsule ? 102 : 184);
        const QColor border = capsule
            ? (dark ? QColor(255, 255, 255, 32) : QColor(255, 255, 255, 180))
            : (dark ? QColor(156, 193, 214, 160) : QColor(105, 142, 162, 180));
        const qreal inset = capsule ? .5 : .75;
        const qreal radius = capsule ? 24 : 28;
        {
            QPainter painter(&expected);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setBrush(fill);
            painter.setPen(QPen(border, capsule ? 1.0 : 1.5));
            painter.drawRoundedRect(QRectF(QPointF(), QSizeF(window.size()))
                .adjusted(inset, inset, -inset, -inset), radius, radius);
        }
        QCOMPARE(actual, expected);
        QCOMPARE(actual.pixelColor(0, 0).alpha(), 0);
        QCOMPARE(actual.pixelColor(actual.width() / 2, actual.height() / 2).alpha(), fill.alpha());
        QCOMPARE(window.property("surfaceRadius").toInt(), capsule ? 24 : 28);
        QCOMPARE(window.property("surfaceOpacity").toDouble(), capsule ? .4 : .72);
        if (!capsule) QCOMPARE(window.size(), QSize(296, 336));
    }
    void cleanupTestCase()
    {
        const QString output = qEnvironmentVariable("MENU_ICON_PREVIEW");
        if (output.isEmpty()) return;
        QImage sheet(810, 70 + scales.size() * 112, QImage::Format_ARGB32_Premultiplied);
        sheet.fill(QColor("#edf4f7"));
        QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf"));
        QPainter painter(&sheet);
        painter.setFont(QFont(QStringLiteral("Segoe UI"), 11));
        painter.setPen(QColor("#385565"));
        painter.drawText(18, 24, "OrangeTools: actual menuIcon output / 9 icons / 8 DPI scales");
        for (int row = 0; row < scales.size(); ++row) {
            const qreal ratio = scales[row];
            painter.drawText(18, 60 + row * 112, QString::number(ratio * 100) + "%");
            for (int col = 0; col < names.size(); ++col) {
                QImage icon = UIHelper::menuIcon(names[col], ratio).pixmap(QSize(24, 24), ratio).toImage();
                // Enlarged to expose any missing strokes; source is the production helper.
                painter.drawImage(QRect(18 + col * 88, 70 + row * 112, 72, 72), icon);
            }
        }
        painter.end();
        QVERIFY(sheet.save(output));
    }
};
QTEST_MAIN(MenuIconTests)
#include "menuicontests.moc"


#include "uihelper.h"
#include <QtTest>
#include <QFile>
#include <QFontDatabase>
#include <QPainter>
#include <QSvgRenderer>

namespace {
const QStringList names = {"monitor-smartphone", "wrench", "package-open", "file-down",
    "smartphone", "sliders-horizontal", "message-circle", "power", "panel-right-close"};
const QList<qreal> scales = {1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0};
QImage reference(const QString &name, qreal ratio)
{
    QFile file(":/menu-icons/" + name + ".svg");
    if (!file.open(QIODevice::ReadOnly)) return {};
    QByteArray svg = file.readAll();
    const bool dark = QApplication::palette().color(QPalette::Window).lightness() < 128;
    svg.replace("currentColor", dark ? "#dce8ef" : "#385565");
    QSvgRenderer renderer(svg);
    if (!renderer.isValid()) return {};
    // Independent reference: paint physical pixels at DPR=1, then tag the image.
    const int pixels = qRound(24 * ratio);
    QImage image(pixels, pixels, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        renderer.render(&painter, QRectF(0, 0, pixels, pixels));
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
        painter.drawText(18, 24, "OrangeTools: actual menuIcon output / 9 icons / 7 DPI scales");
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


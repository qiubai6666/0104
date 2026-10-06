#include "encodedstring.h"
#include "version.h"
#include <QtTest>

class EncodedStringTests : public QObject {
    Q_OBJECT
private slots:
    void decodesWithoutChangingBytes() {
        static constexpr auto ascii = OrangeEncoding::encode<0xA7>("test...");
        static constexpr auto empty = OrangeEncoding::encode<0x39>("");
        static constexpr auto utf8 = OrangeEncoding::encode<0xD1>("中文 UTF-8");
        QCOMPARE(ascii.decode(), QStringLiteral("test..."));
        QCOMPARE(empty.decode(), QString());
        QCOMPARE(utf8.decode(), QString::fromUtf8("中文 UTF-8"));
        QCOMPARE(ascii.decode(), ascii.decode());
        QVERIFY(ascii.bytes[0] != static_cast<unsigned char>('t'));
    }
    void configuredPasswordIsUnchanged() {
        QCOMPARE(DEFAULT_PASSWORD, QStringLiteral("123456..."));
    }
};
QTEST_GUILESS_MAIN(EncodedStringTests)
#include "encodedstringtests.moc"

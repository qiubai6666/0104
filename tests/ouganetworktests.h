#ifndef OUGANETWORKTESTS_H
#define OUGANETWORKTESTS_H
#include <QByteArray>
#include <QObject>
#include <QString>
#include <QUrl>
class OugaNetworkTests : public QObject {
  Q_OBJECT
  QString dir, base;
  const QByteArray data = "0123456789abcdef";
  void seed(const QString &file, const QUrl &url,
            const QByteArray &validator = "\"v1\"");
private slots:
  void initTestCase();
  void init();
  void endpointContract_data();
  void endpointContract();
  void defaultOffline();
  void downloadIntegrity_data();
  void downloadIntegrity();
  void resumeValidation_data();
  void resumeValidation();
  void redirectStripsCredentials();
  void invalidRedirect();
  void cancellationPreservesPartial();
  void interruptedThenResume();
};
#endif

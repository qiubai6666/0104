#ifndef DEVICEOPERATIONLEASE_H
#define DEVICEOPERATIONLEASE_H
#include <QObject>
#include <QPointer>
#include <functional>
// One application-wide command chain owns the device channel, including waits.
class DeviceOperationLease : public QObject {
  Q_OBJECT
public:
  static DeviceOperationLease *instance();
  static bool acquire(QObject *owner, QString *error = nullptr);
  static void release(QObject *owner);
  static bool busyFor(QObject *owner = nullptr);
  static QObject *owner();
  static void setIdleCheck(std::function<bool()> check);
signals:
  void changed(bool held);

protected:
  bool eventFilter(QObject *watched, QEvent *event) override;

private:
  QPointer<QObject> m_owner;
  QMetaObject::Connection m_ownerDestroyed;
  std::function<bool()> m_idle;
};
#endif

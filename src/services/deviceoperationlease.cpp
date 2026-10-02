#include "deviceoperationlease.h"
#include <QCoreApplication>
#include <QEvent>
DeviceOperationLease *DeviceOperationLease::instance() {
  static auto *lease = new DeviceOperationLease;
  if (qApp)
    qApp->installEventFilter(lease);
  return lease;
}
bool DeviceOperationLease::acquire(QObject *owner, QString *error) {
  auto l = instance();
  if (!owner)
    return false;
  if (l->m_owner == owner)
    return true;
  if (l->m_owner || (l->m_idle && !l->m_idle())) {
    if (error)
      *error = "其他设备操作/工具尚未结束，请先等待其完成";
    return false;
  }
  l->m_owner = owner;
  QObject::disconnect(l->m_ownerDestroyed);
  l->m_ownerDestroyed =
      QObject::connect(owner, &QObject::destroyed, l, [l, owner] {
        if (!l->m_owner || l->m_owner == owner) {
          l->m_owner = nullptr;
          emit l->changed(false);
        }
      });
  emit l->changed(true);
  return true;
}
void DeviceOperationLease::release(QObject *owner) {
  auto l = instance();
  if (l->m_owner != owner)
    return;
  QObject::disconnect(l->m_ownerDestroyed);
  l->m_ownerDestroyed = {};
  l->m_owner = nullptr;
  emit l->changed(false);
}
bool DeviceOperationLease::busyFor(QObject *owner) {
  auto p = instance()->m_owner;
  return p && p != owner;
}
QObject *DeviceOperationLease::owner() { return instance()->m_owner; }
void DeviceOperationLease::setIdleCheck(std::function<bool()> check) {
  instance()->m_idle = std::move(check);
}
bool DeviceOperationLease::eventFilter(QObject *watched, QEvent *event) {
  if (m_owner && event->type() == QEvent::Close) {
    for (QObject *p = m_owner; p; p = p->parent())
      if (p == watched) {
        event->ignore();
        return true;
      }
  }
  if (m_owner && event->type() == QEvent::Quit) {
    event->ignore();
    return true;
  }
  return false;
}

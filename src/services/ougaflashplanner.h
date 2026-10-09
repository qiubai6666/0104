#ifndef OUGAFLASHPLANNER_H
#define OUGAFLASHPLANNER_H
#include "ougaflashtypes.h"
class OugaFlashPlanner {
public:
  static bool compatibleTable(const QVector<Ouga::Partition> &partitions,
                              const Ouga::Device &device, QString *error);
  static QVector<Ouga::Step> postActions(const Ouga::Device &device,
                                         const Ouga::Options &options);
  static bool build(const QVector<Ouga::Partition> &partitions,
                    const Ouga::Device &device, const Ouga::Options &options,
                    Ouga::Plan *plan, QString *error);
};
#endif

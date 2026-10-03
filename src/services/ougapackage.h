#ifndef OUGAPACKAGE_H
#define OUGAPACKAGE_H
#include "ougaflashtypes.h"
struct OugaPayloadEntry {
  QString name;
  quint64 size = 0, oldSize = 0;
  QByteArray hash, oldHash;
};
class OugaPackage {
public:
  // A lightweight presence check only; scan() remains the validation gate.
  static bool hasImageCandidates(const QString &directory);
  static QVector<Ouga::Partition> scan(const QString &directory,
                                       QString *error);
  static bool inspect(const QString &name, const QString &file,
                      Ouga::Partition *image, QString *error);
  static QByteArray digest(const QString &file, QString *error);
  static qint64 expandedSize(const QString &file, QString *error);
  static bool readArb(const QString &file, quint32 *index, QString *error);
  static QStringList parsePayloadList(const QString &output);
  static QString findPayload(const QString &directory);
  static bool payloadManifest(const QString &file,
                              QVector<OugaPayloadEntry> *entries, bool *delta,
                              QString *error);
  static bool lpmakeArguments(const QString &directory, const QString &output,
                              QStringList *args, QSet<QString> *merged,
                              QString *error);
  static bool safeArchiveListing(const QString &listing, QString *error);
  static bool inside(const QString &root, const QString &file);
  static bool superContents(const QString &file, QSet<QString> *names,
                            QString *error);
  static bool toRaw(const QString &source, const QString &destination,
                    quint64 allocated, QString *error);
};
#endif

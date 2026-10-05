#ifndef OUGAPACKAGE_H
#define OUGAPACKAGE_H
#include "ougaflashtypes.h"
struct OugaPayloadExtent {
  quint64 start = 0, blocks = 0;
};
struct OugaPayloadOperation {
  int type = -1;
  quint64 dataOffset = 0, dataLength = 0;
  bool hasDataOffset = false, hasSource = false;
  QByteArray dataHash;
  QVector<OugaPayloadExtent> destination;
};
struct OugaPayloadEntry {
  QString name;
  quint64 size = 0, oldSize = 0, operations = 0;
  QByteArray hash, oldHash;
  bool requiresOldImage = false;
  QVector<OugaPayloadOperation> ops;
};
// Absolute byte positions inside the source file (payload.bin or a ZIP that
// stores it uncompressed); dataOffset is where operation data begins.
struct OugaPayloadLayout {
  quint64 base = 0, end = 0, dataOffset = 0, blockSize = 4096;
};
class OugaPackage {
public:
  // A lightweight presence check only; scan() remains the validation gate.
  static bool hasImageCandidates(const QString &directory);
  // known: canonical path -> SHA-256 already established for this exact
  // invocation (e.g. a payload written from hash-verified operations). The
  // value becomes the expected digest that flashing re-verifies on disk.
  static QVector<Ouga::Partition>
  scan(const QString &directory, QString *error,
       const QMap<QString, QByteArray> &known = {}, bool hash = true);
  // hash=false validates structure only and leaves sha256 empty; such images
  // must be hashed (hashImages) before planning, like the reference loader.
  static bool inspect(const QString &name, const QString &file,
                      Ouga::Partition *image, QString *error,
                      const QByteArray &knownSha256 = {}, bool hash = true);
  // Fills missing SHA-256 of selected images in parallel; size must still match.
  static bool hashImages(QVector<Ouga::Partition> *images, QString *error);
  static QByteArray digest(const QString &file, QString *error);
  static qint64 expandedSize(const QString &file, QString *error);
  static bool readArb(const QString &file, quint32 *index, QString *error);
  // Informational only: a model label is not a compatibility/ARB check.
  // Empty means unavailable or malformed metadata, not a package error.
  static QString payloadDeviceModel(const QString &source);
  static QStringList parsePayloadList(const QString &output);
  static QString findPayload(const QString &directory);
  static bool payloadManifest(const QString &file,
                              QVector<OugaPayloadEntry> *entries, bool *delta,
                              QString *error,
                              OugaPayloadLayout *layout = nullptr);
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

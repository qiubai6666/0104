// Test-only encoders for decodable in-process Payload fixtures.
#include <QByteArray>
#include <bzlib.h>
#include <lzma.h>
QByteArray xzBytes(const QByteArray &in) {
  QByteArray out(qsizetype(lzma_stream_buffer_bound(size_t(in.size()))), 0);
  size_t pos = 0;
  if (lzma_easy_buffer_encode(6, LZMA_CHECK_CRC64, nullptr,
                              reinterpret_cast<const uint8_t *>(in.constData()),
                              size_t(in.size()),
                              reinterpret_cast<uint8_t *>(out.data()), &pos,
                              size_t(out.size())) != LZMA_OK)
    return {};
  out.resize(qsizetype(pos));
  return out;
}
QByteArray bzBytes(const QByteArray &in) {
  QByteArray out(in.size() + in.size() / 100 + 600, 0);
  unsigned length = unsigned(out.size());
  if (BZ2_bzBuffToBuffCompress(out.data(), &length,
                               const_cast<char *>(in.constData()),
                               unsigned(in.size()), 9, 0, 0) != BZ_OK)
    return {};
  out.resize(qsizetype(length));
  return out;
}

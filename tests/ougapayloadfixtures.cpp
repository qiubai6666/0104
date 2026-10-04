// Test-only encoders for decodable in-process Payload fixtures.
#include <QByteArray>
#include <QtEndian>
#include <algorithm>
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

// Decoder-only builds deliberately do not link a compressor. This checksummed
// compressed-block fixture was generated once with upstream Zstandard 1.5.7,
// level 3, for the exact nativeImage(2, 'z') pattern in ougatests.cpp.
QByteArray zstdCompressedBytes() {
  return QByteArray::fromHex(
      "28b52ffd64001fb5080024107a7b7c7d7e7f808182838485868788898a8b8c8d8e8f909192939495"
      "969798999a9b9c9d9e9fa0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbd"
      "bebfc0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedfe0e1e2e3e4e5"
      "e6e7e8e9eaebecedeeeff0f1f2f3f4f5f6f7f8f9fafbfcfdfeff000102030405060708090a0b0c0d"
      "0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f303132333435"
      "363738393a3b3c3d3e3f404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d"
      "5e5f606162636465666768696a6b6c6d6e6f707172737475767778797a7b04005e063e8803ca42ac"
      "070aec05eef79f1494ca0910");
}
// Zstandard RAW/RLE frames, not a compression implementation. These exercise
// frame sizes, multiple blocks and frames with no advertised content size.
QByteArray zstdBytes(const QByteArray &in, bool unknownSize, bool rle) {
  if (rle && (in.isEmpty() ||
              std::any_of(in.cbegin(), in.cend(), [&](char c) { return c != in[0]; })))
    return {};
  QByteArray frame = QByteArray::fromHex("28b52ffd");
  frame += char(unknownSize ? 0 : 0xa0); // FCS absent or 4-byte single segment
  if (unknownSize) {
    frame += char(0x38); // 128 KiB window
  } else {
    QByteArray size(4, 0);
    qToLittleEndian(quint32(in.size()), reinterpret_cast<uchar *>(size.data()));
    frame += size;
  }
  qsizetype at = 0;
  do {
    const quint32 length = quint32(qMin<qsizetype>(128 * 1024, in.size() - at));
    const quint32 header = (length << 3) | (rle ? 2u : 0u) |
                           (at + length == in.size() ? 1u : 0u);
    for (int b = 0; b < 3; ++b)
      frame += char(header >> (b * 8));
    frame += rle ? QByteArray(1, in[0]) : in.mid(at, length);
    at += length;
  } while (at < in.size());
  return frame;
}

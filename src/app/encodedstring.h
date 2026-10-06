#ifndef ORANGE_ENCODEDSTRING_H
#define ORANGE_ENCODEDSTRING_H

#include <QByteArray>
#include <QString>
#include <cstddef>

// C++11 index sequences; this is reversible obfuscation, not encryption.
namespace OrangeEncoding {
template<std::size_t... I> struct Indices {};
template<std::size_t N, std::size_t... I>
struct MakeIndices : MakeIndices<N - 1, N - 1, I...> {};
template<std::size_t... I> struct MakeIndices<0, I...> { typedef Indices<I...> Type; };

constexpr unsigned char mask(unsigned char key, std::size_t index)
{
    return static_cast<unsigned char>(key + index * 29u);
}

template<std::size_t N, unsigned char Key> struct Encoded {
    unsigned char bytes[N];
    template<std::size_t... I>
    constexpr Encoded(const char (&text)[N], Indices<I...>)
        : bytes{static_cast<unsigned char>(static_cast<unsigned char>(text[I]) ^ mask(Key, I))...} {}

    // Volatile reads prevent LTO from replacing this with a plaintext constant.
    // The decoded value still exists in memory while being used.
    QString decode() const
    {
        const volatile unsigned char *data = bytes;
        volatile unsigned char key = Key;
        QByteArray decoded(static_cast<int>(N - 1), '\0');
        for (std::size_t i = 0; i + 1 < N; ++i)
            decoded[static_cast<int>(i)] = static_cast<char>(data[i] ^ mask(key, i));
        return QString::fromUtf8(decoded);
    }
};

template<unsigned char Key, std::size_t N>
constexpr Encoded<N, Key> encode(const char (&text)[N])
{
    return Encoded<N, Key>(text, typename MakeIndices<N>::Type());
}
}

#ifdef ORANGE_PROTECTED_RELEASE
#define ORANGE_SENSITIVE_STRING(name, text) \
    inline QString name() { \
        static constexpr auto encoded = OrangeEncoding::encode<0xA7>(text); \
        return encoded.decode(); \
    }
#else
#define ORANGE_SENSITIVE_STRING(name, text) \
    inline QString name() { return QString::fromUtf8(text); }
#endif
#endif

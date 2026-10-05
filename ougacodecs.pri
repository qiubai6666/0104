# In-process Payload extraction (ougapackage.cpp locates ZIP-stored payloads
# through it). xz/bzip2 come from the Qt MinGW kit (mingw64/opt); -idirafter
# keeps that folder's unrelated headers from shadowing system ones.
# Override with OUGA_CODEC_ROOT=<dir containing include/ and lib/>.
SOURCES += $$PWD/src/services/ougapayloadextractor.cpp $$PWD/src/services/ougapayloadreader.cpp
HEADERS += $$PWD/src/services/ougapayloadextractor.h $$PWD/src/services/ougapayloadreader.h
# Pinned decoder-only Zstandard 1.5.7, statically compiled from upstream's
# single-file library. No zstd executable, Python module or extra DLL is needed.
SOURCES += $$PWD/third_party/zstd/zstddeclib.c
HEADERS += $$PWD/third_party/zstd/zstd.h $$PWD/third_party/zstd/zstd_errors.h
INCLUDEPATH += $$PWD/third_party/zstd
isEmpty(OUGA_CODEC_ROOT) {
    OUGA_COMPILER = $$system(where $$QMAKE_CXX 2>nul)
    OUGA_COMPILER = $$first(OUGA_COMPILER)
    OUGA_CODEC_ROOT = $$clean_path($$dirname(OUGA_COMPILER)/../opt)
}
!exists($$OUGA_CODEC_ROOT/include/lzma.h)|!exists($$OUGA_CODEC_ROOT/include/bzlib.h): \
    error("lzma.h/bzlib.h not found under $$OUGA_CODEC_ROOT; set OUGA_CODEC_ROOT")
DEFINES += LZMA_API_STATIC
QMAKE_CXXFLAGS += -idirafter $$shell_quote($$OUGA_CODEC_ROOT/include)
# liblzma links statically; bzip2 needs libbz2-1.dll beside the executable.
LIBS += -L$$shell_quote($$OUGA_CODEC_ROOT/lib) -llzma -lbz2

# CNG is a Windows system library; no extra distributable DLL is required.
win32: LIBS += -lbcrypt

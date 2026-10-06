# Explicit opt-in. Debug builds keep their normal flags and readable strings.
contains(CONFIG, protected_release):CONFIG(release, debug|release) {
    !win32-g++: error("protected_release requires the Windows MinGW toolchain")
    DEFINES += ORANGE_PROTECTED_RELEASE
    # Resource bytes have no business logic. Qt's two-pass RCC avoids putting the
    # giant generated array through LTO/DWARF while preserving embedded resources.
    CONFIG += resources_big
    RCC_CXX = $$QMAKE_CXX $(CXXFLAGS) -fno-lto -g0
    QMAKE_CFLAGS_RELEASE -= -O -O1 -O2 -O3 -Os -Ofast
    QMAKE_CXXFLAGS_RELEASE -= -O -O1 -O2 -O3 -Os -Ofast
    QMAKE_CFLAGS_RELEASE += -O2 -g
    QMAKE_CXXFLAGS_RELEASE += -O2 -g
    # ASCII drive prefix avoids the MinGW/qmake UTF-8 argument issue in Chinese paths.
    QMAKE_CFLAGS += -flto -ffile-prefix-map=$$section(PWD, /, 0, 0)/=./
    QMAKE_CXXFLAGS += -flto -ffile-prefix-map=$$section(PWD, /, 0, 0)/=./
    # Keep DWARF until Finalize-ProtectedExecutable extracts a private symbol file.
    QMAKE_LFLAGS -= -Wl,-s -s
    QMAKE_LFLAGS_RELEASE -= -Wl,-s -s
    # Keep LTO resolution/intermediate files beside the ASCII output basename.
    # This avoids MinGW 13 resolving a Unicode absolute TEMP filename incorrectly.
    QMAKE_LFLAGS += -flto -g -save-temps=obj
}

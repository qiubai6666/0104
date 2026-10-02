QT += core concurrent testlib
QT -= gui
CONFIG += c++17 testcase console
CONFIG -= app_bundle
TARGET = OugaDependencyTests
INCLUDEPATH += $$PWD/../src/app $$PWD/../src/services
SOURCES += $$PWD/ougadependencytests.cpp \
    $$PWD/../src/services/resourceextractor.cpp \
    $$PWD/../src/services/ougacommandrunner.cpp \
    $$PWD/../src/services/ougapreparation.cpp \
    $$PWD/../src/services/ougapackage.cpp \
    $$PWD/../src/services/ougaflashtypes.cpp \
    $$PWD/../src/services/deviceoperationlease.cpp
HEADERS += $$PWD/../src/services/resourceextractor.h \
    $$PWD/../src/services/ougacommandrunner.h \
    $$PWD/../src/services/ougapreparation.h \
    $$PWD/../src/services/ougapackage.h \
    $$PWD/../src/services/ougaflashtypes.h \
    $$PWD/../src/services/deviceoperationlease.h
# Default tests use inert fixtures; no test falls back to installed device tools.
# Deployment validation can embed the exact production resource bundle explicitly.
contains(CONFIG, bundled_dependency_resources) {
    include($$PWD/../resources.pri)
    DEFINES += ORANGE_TEST_REAL_RESOURCES
} else {
    RESOURCES += $$PWD/fixtures/ougadependencies/resources.qrc
}

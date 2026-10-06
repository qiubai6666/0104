QT += core testlib
QT -= gui
CONFIG += c++11 testcase console
TARGET = EncodedStringTests
INCLUDEPATH += $$PWD/../src/app
SOURCES += $$PWD/encodedstringtests.cpp
include($$PWD/../protected-release.pri)

QT += core gui widgets network concurrent testlib
CONFIG += c++17 testcase console
CONFIG -= app_bundle
TARGET = OugaTests
INCLUDEPATH += $$PWD/../src/app $$PWD/../src/ui $$PWD/../src/services
include($$PWD/../ouga.pri)
SOURCES += $$PWD/ougatests.cpp $$PWD/ougapayloadfixtures.cpp $$PWD/../src/services/deviceoperationlease.cpp $$PWD/../src/ui/uihelper.cpp
HEADERS += $$PWD/../src/services/deviceoperationlease.h
# No embedded platform-tools, no production ResourceExtractor. The tests supply isolated paths.

include($$PWD/../protected-release.pri)

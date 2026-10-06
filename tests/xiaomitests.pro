QT += core gui widgets testlib
CONFIG += c++17 testcase console
CONFIG -= app_bundle
TARGET = XiaomiTests
INCLUDEPATH += $$PWD/../src/ui $$PWD/../src/services
include($$PWD/../xiaomi.pri)
SOURCES += $$PWD/xiaomitests.cpp $$PWD/../src/services/deviceoperationlease.cpp
HEADERS += $$PWD/../src/services/deviceoperationlease.h
RESOURCES += $$PWD/../src/ui/ouga.qrc

include($$PWD/../protected-release.pri)

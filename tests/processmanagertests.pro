QT += core testlib
QT -= gui
CONFIG += c++11 testcase console
CONFIG -= app_bundle
TARGET = ProcessManagerTests

INCLUDEPATH += $$PWD/../src/services
SOURCES += \
    $$PWD/processmanagertests.cpp \
    $$PWD/../src/services/processmanager.cpp
HEADERS += $$PWD/../src/services/processmanager.h
SOURCES += $$PWD/../src/services/deviceoperationlease.cpp
HEADERS += $$PWD/../src/services/deviceoperationlease.h

include($$PWD/../protected-release.pri)

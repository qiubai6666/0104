QT += core gui widgets testlib
CONFIG += c++17 testcase console
CONFIG -= app_bundle
TARGET = DeviceInformationTests
INCLUDEPATH += $$PWD/../src/app $$PWD/../src/ui $$PWD/../src/services
# Only task-private fake tools are linked; never extract or execute real device tools.
SOURCES += $$PWD/deviceinformationtests.cpp \
    $$PWD/../src/services/devicemanager.cpp \
    $$PWD/../src/services/processmanager.cpp \
    $$PWD/../src/services/deviceoperationlease.cpp \
    $$PWD/../src/ui/deviceinfowindow.cpp \
    $$PWD/../src/ui/devicecheckwindow.cpp \
    $$PWD/../src/ui/uihelper.cpp
HEADERS += $$PWD/../src/services/devicemanager.h \
    $$PWD/../src/services/processmanager.h \
    $$PWD/../src/services/deviceoperationlease.h \
    $$PWD/../src/ui/deviceinfowindow.h \
    $$PWD/../src/ui/devicecheckwindow.h \
    $$PWD/../src/ui/uihelper.h

include($$PWD/../protected-release.pri)

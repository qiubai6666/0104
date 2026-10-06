QT += core gui widgets testlib
CONFIG += c++11 testcase console
CONFIG -= app_bundle
TARGET = ReadabilityTests

# 校验与主程序相同的内嵌资源和构建配置，不执行任何工具。
include($$PWD/../resources.pri)

INCLUDEPATH += $$PWD/../src/app $$PWD/../src/ui

SOURCES += \
    $$PWD/readabilitytests.cpp \
    $$PWD/../src/ui/passworddialog.cpp \
    $$PWD/../src/ui/uihelper.cpp

HEADERS += \
    $$PWD/../src/ui/passworddialog.h \
    $$PWD/../src/ui/uihelper.h \
    $$PWD/../src/app/version.h

include($$PWD/../protected-release.pri)

SOURCES += $$PWD/../src/services/resourceextractor.cpp $$PWD/../src/services/integritychecker.cpp
INCLUDEPATH += $$PWD/../src/services

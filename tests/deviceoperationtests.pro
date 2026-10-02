QT += core gui widgets network testlib
CONFIG += c++11 testcase console
CONFIG -= app_bundle
TARGET = DeviceOperationTests
INCLUDEPATH += $$PWD/../src/app $$PWD/../src/ui $$PWD/../src/services
# 路径由测试替身提供；绝不链接资源提取器、访问真实 ADB 或下载网络配置。
SOURCES += $$PWD/deviceoperationtests.cpp \
    $$PWD/../src/services/processmanager.cpp \
    $$PWD/../src/services/devicemanager.cpp \
    $$PWD/../src/ui/devicecheckwindow.cpp \
    $$PWD/../src/ui/menuwidget.cpp \
    $$PWD/../src/ui/repairwindow.cpp \
    $$PWD/../src/ui/deviceinfowindow.cpp \
    $$PWD/../src/ui/configwindow.cpp \
    $$PWD/../src/ui/payloadwindow.cpp \
    $$PWD/../src/ui/uihelper.cpp
HEADERS += $$PWD/../src/services/devicemanager.h \
    $$PWD/../src/services/processmanager.h \
    $$PWD/../src/services/shellcommand.h \
    $$PWD/../src/ui/devicecheckwindow.h \
    $$PWD/../src/ui/menuwidget.h \
    $$PWD/../src/ui/repairwindow.h \
    $$PWD/../src/ui/deviceinfowindow.h \
    $$PWD/../src/ui/configwindow.h \
    $$PWD/../src/ui/payloadwindow.h

include($$PWD/../ouga.pri)
SOURCES += $$PWD/../src/services/deviceoperationlease.cpp
HEADERS += $$PWD/../src/services/deviceoperationlease.h

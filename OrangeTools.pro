QT += core gui widgets network

CONFIG += c++11

# 应用名称与输出文件名统一；引号保留名称中的空格。
TARGET = "Orange Tools"

# 源码按职责分层；旧的本地头文件名保持不变。
INCLUDEPATH += $$PWD/src/app $$PWD/src/ui $$PWD/src/services

# 设置程序图标和资源
win32 {
    RC_FILE = app.rc
}

# 嵌入qiubai文件夹资源
include(resources.pri)

SOURCES += \
    src/app/main.cpp \
    src/ui/passworddialog.cpp \
    src/ui/menuwidget.cpp \
    src/services/screencastcontroller.cpp \
    src/ui/repairwindow.cpp \
    src/ui/payloadwindow.cpp \
    src/ui/devicecheckwindow.cpp \
    src/ui/configwindow.cpp \
    src/services/processmanager.cpp \
    src/services/resourceextractor.cpp \
    src/ui/uihelper.cpp \
    src/services/devicemanager.cpp \
    src/services/integritychecker.cpp

HEADERS += \
    src/ui/menuwidget.h \
    src/services/screencastcontroller.h \
    src/ui/passworddialog.h \
    src/ui/repairwindow.h \
    src/ui/payloadwindow.h \
    src/ui/devicecheckwindow.h \
    src/ui/configwindow.h \
    src/services/processmanager.h \
    src/services/shellcommand.h \
    src/services/resourceextractor.h \
    src/app/version.h \
    src/ui/uihelper.h \
    src/services/devicemanager.h \
    src/services/integritychecker.h

include($$PWD/ouga.pri)
SOURCES += $$PWD/src/services/deviceoperationlease.cpp
HEADERS += $$PWD/src/services/deviceoperationlease.h
include($$PWD/xiaomi.pri)

include($$PWD/protected-release.pri)
HEADERS += $$PWD/src/app/encodedstring.h $$PWD/src/app/sensitivestrings.h

QT += core gui widgets network

CONFIG += c++11

# 设置程序图标和资源
win32 {
    RC_FILE = app.rc
}

# 嵌入qiubai文件夹资源
RESOURCES += resources.qrc

SOURCES += \
    main.cpp \
    menuwidget.cpp \
    deviceinfowindow.cpp \
    repairwindow.cpp \
    payloadwindow.cpp \
    devicecheckwindow.cpp \
    configwindow.cpp \
    processmanager.cpp \
    resourceextractor.cpp \
    uihelper.cpp \
    devicemanager.cpp \
    integritychecker.cpp

HEADERS += \
    menuwidget.h \
    deviceinfowindow.h \
    passworddialog.h \
    repairwindow.h \
    payloadwindow.h \
    devicecheckwindow.h \
    configwindow.h \
    processmanager.h \
    resourceextractor.h \
    version.h \
    uihelper.h \
    devicemanager.h \
    integritychecker.h

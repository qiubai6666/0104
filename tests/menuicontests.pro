QT += core gui widgets svg testlib
CONFIG += c++17 testcase console
CONFIG -= app_bundle
TARGET = MenuIconTests
INCLUDEPATH += $$PWD/../src/ui
SOURCES += $$PWD/menuicontests.cpp $$PWD/../src/ui/uihelper.cpp
HEADERS += $$PWD/../src/ui/uihelper.h
RESOURCES += $$PWD/../src/ui/menu-icons.qrc

QT += core network testlib
CONFIG += c++11 testcase console
CONFIG -= app_bundle
TARGET = ConfigDownloadTests
INCLUDEPATH += $$PWD/../src/services
SOURCES += $$PWD/configdownloadtests.cpp
HEADERS += $$PWD/../src/services/downloadfilewriter.h

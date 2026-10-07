QT += core gui widgets network testlib
CONFIG += c++17 testcase console
TARGET = OpenListTests
INCLUDEPATH += $$PWD/../src/ui $$PWD/../src/services $$PWD/../src/app
include($$PWD/../openlist.pri)
SOURCES += $$PWD/openlisttests.cpp $$PWD/../src/services/processmanager.cpp \
           $$PWD/../src/services/deviceoperationlease.cpp $$PWD/../src/services/devicemanager.cpp
HEADERS += $$PWD/../src/services/deviceoperationlease.h $$PWD/../src/services/devicemanager.h
# Command runner base and existing adapter provide their MOC definitions; no flash service is used.
HEADERS += $$PWD/../src/services/ougacommandrunner.h

SOURCES += $$PWD/../src/services/ougacommandrunner.cpp

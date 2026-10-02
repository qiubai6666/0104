QT += core network concurrent testlib
CONFIG += c++17 testcase console
CONFIG -= app_bundle
TARGET = OugaNetworkTests
INCLUDEPATH += $$PWD/../src/services
SOURCES += $$PWD/ouganetworktests.cpp $$PWD/../src/services/ougaromservice.cpp $$PWD/../src/services/ougapackage.cpp $$PWD/../src/services/ougaflashtypes.cpp
HEADERS += $$PWD/ouganetworktests.h $$PWD/../src/services/ougaromservice.h $$PWD/../src/services/ougapackage.h $$PWD/../src/services/ougaflashtypes.h

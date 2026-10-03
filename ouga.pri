QT += concurrent network svg
CONFIG += c++17
SOURCES += \
    $$PWD/src/services/ougaflashtypes.cpp \
    $$PWD/src/services/ougapackage.cpp \
    $$PWD/src/services/ougaflashplanner.cpp \
    $$PWD/src/services/ougacommandrunner.cpp \
    $$PWD/src/services/ougaflashservice.cpp \
    $$PWD/src/services/ougapreparation.cpp \
    $$PWD/src/services/ougapayloadprocess.cpp \
    $$PWD/src/services/ougaromservice.cpp \
    $$PWD/src/ui/ougaflashwindow.cpp \
    $$PWD/src/ui/ougaflashworkflow.cpp \
    $$PWD/src/ui/ougaflashdialogs.cpp
HEADERS += \
    $$PWD/src/services/ougaflashtypes.h \
    $$PWD/src/services/ougapackage.h \
    $$PWD/src/services/ougaflashplanner.h \
    $$PWD/src/services/ougacommandrunner.h \
    $$PWD/src/services/ougaflashservice.h \
    $$PWD/src/services/ougapreparation.h \
    $$PWD/src/services/ougapayloadprocess.h \
    $$PWD/src/services/ougaromservice.h \
    $$PWD/src/ui/ougaflashwindow.h

RESOURCES += $$PWD/src/ui/ouga.qrc

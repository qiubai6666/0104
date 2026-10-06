QT += core gui widgets network svg concurrent
CONFIG += c++11 console
CONFIG -= app_bundle
TARGET = DeploymentSmoke
SOURCES += $$PWD/deploymentsmoke.cpp

RESOURCES += $$PWD/../src/ui/ouga.qrc

# Match the application RCC configuration without running embedded tools.
include($$PWD/../resources.pri)

include($$PWD/../protected-release.pri)

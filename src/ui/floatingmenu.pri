# Shared SVG resources for the application and isolated UI regressions.
QT += svg
RESOURCES += $$PWD/menu-icons.qrc

# Animated mascot for the floating entry ball (played via QMovie).
RESOURCES += $$PWD/mascot.qrc

# Shared animated capsule buttons for the app and isolated UI regressions.
SOURCES += $$PWD/mainmenubutton.cpp
HEADERS += $$PWD/mainmenubutton.h

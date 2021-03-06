#-------------------------------------------------
#
# Project created by QtCreator 2019-04-03T12:20:46
#
#-------------------------------------------------

QT       += core gui

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

TARGET = NurikabeMaker
TEMPLATE = app

# The following define makes your compiler emit warnings if you use
# any feature of Qt which has been marked as deprecated (the exact warnings
# depend on your compiler). Please consult the documentation of the
# deprecated API in order to know how to port your code away from it.
DEFINES += QT_DEPRECATED_WARNINGS

# You can also make your code fail to compile if you use deprecated APIs.
# In order to do so, uncomment the following line.
# You can also select to disable deprecated APIs only up to a certain version of Qt.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

CONFIG += c++17

QMAKE_CXXFLAGS_RELEASE -= -O2
QMAKE_LFLAGS_RELEASE -= -O1
QMAKE_CXXFLAGS_RELEASE += -O3

SOURCES += \
        src/main.cpp \
        src/mainwindow.cpp \
    src/tinyxml2/tinyxml2.cpp \
    src/nurikabe/nurikabe.cpp \
    src/newfile.cpp \
    src/generator.cpp

HEADERS += \
        src/mainwindow.h \
    src/tinyxml2/tinyxml2.h \
    src/nurikabe/nurikabe.h \
    src/newfile.h \
    src/generator.h

FORMS += \
        mainwindow.ui \
    newfile.ui

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

RESOURCES += \
    assets.qrc


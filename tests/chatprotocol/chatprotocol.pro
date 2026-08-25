QT += core network
QT -= gui
CONFIG += console
CONFIG -= app_bundle
TEMPLATE = app
TARGET = chatprotocoltest

DEFINES += HEADLESS
DEFINES += APP_VERSION=\\\"3.12.3dev\\\"

INCLUDEPATH += ../../src

win32 {
    DEFINES += NOMINMAX
}

SOURCES += \
    tst_chatprotocol.cpp \
    ../../src/protocol.cpp \
    ../../src/util.cpp

HEADERS += \
    ../../src/protocol.h \
    ../../src/util.h \
    ../../src/chatmessage.h

SOURCES += tst_qbluetoothsocket.cpp
TARGET = tst_qbluetoothsocket
CONFIG += testcase
testcase.timeout = 250 # this test is slow

QT = core concurrent network bluetooth testlib
osx:QT += widgets
osx:CONFIG += insignificant_test

OTHER_FILES += \
    README.txt

DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0

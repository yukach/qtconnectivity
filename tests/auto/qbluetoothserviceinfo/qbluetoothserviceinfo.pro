SOURCES += tst_qbluetoothserviceinfo.cpp
TARGET = tst_qbluetoothserviceinfo
CONFIG += testcase

QT = core concurrent bluetooth testlib
osx:QT += widgets
osx:CONFIG += insignificant_test

DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0

android:!android-no-sdk {
    DEFINES += QT_ANDROID_BLUETOOTH
}

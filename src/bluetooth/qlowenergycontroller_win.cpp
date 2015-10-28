/****************************************************************************
**
** Copyright (C) 2015 The Qt Company Ltd.
** Copyright (C) 2014 Denis Shienkov <denis.shienkov@gmail.com>
** Contact: http://www.qt.io/licensing/
**
** This file is part of the QtBluetooth module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL21$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see http://www.qt.io/terms-conditions. For further
** information use the contact form at http://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 or version 3 as published by the Free
** Software Foundation and appearing in the file LICENSE.LGPLv21 and
** LICENSE.LGPLv3 included in the packaging of this file. Please review the
** following information to ensure the GNU Lesser General Public License
** requirements will be met: https://www.gnu.org/licenses/lgpl.html and
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** As a special exception, The Qt Company gives you certain additional
** rights. These rights are described in The Qt Company LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qlowenergycontroller_p.h"
#include "qbluetoothdevicediscoveryagent_p.h"

#include <QtCore/QLoggingCategory>
#include <QtCore/QIODevice> // for open modes

#include <algorithm> // for std::max

#include <windows/qwinlowenergybluetooth_p.h>

#include <setupapi.h>

QT_BEGIN_NAMESPACE

Q_DECLARE_LOGGING_CATEGORY(QT_BT_WINDOWS)

Q_GLOBAL_STATIC(QLibrary, bluetoothapis)

static bool gattFunctionsResolved = false;

static QString getServiceSystemPath(const QBluetoothUuid &serviceUuid, int *systemErrorCode)
{
    const HDEVINFO deviceInfoSet = ::SetupDiGetClassDevs(
                reinterpret_cast<const GUID *>(&serviceUuid),
                NULL,
                0,
                DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

    if (deviceInfoSet == INVALID_HANDLE_VALUE) {
        *systemErrorCode = ::GetLastError();
        return QString();
    }

    QString foundSystemPath;
    DWORD index = 0;

    forever {
        SP_DEVICE_INTERFACE_DATA deviceInterfaceData;
        ::ZeroMemory(&deviceInterfaceData, sizeof(deviceInterfaceData));
        deviceInterfaceData.cbSize = sizeof(deviceInterfaceData);

        if (!::SetupDiEnumDeviceInterfaces(
                    deviceInfoSet,
                    NULL,
                    reinterpret_cast<const GUID *>(&serviceUuid),
                    index++,
                    &deviceInterfaceData)) {
            *systemErrorCode = ::GetLastError();
            break;
        }

        DWORD deviceInterfaceDetailDataSize = 0;
        if (!::SetupDiGetDeviceInterfaceDetail(
                    deviceInfoSet,
                    &deviceInterfaceData,
                    NULL,
                    deviceInterfaceDetailDataSize,
                    &deviceInterfaceDetailDataSize,
                    NULL)) {
            if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
                *systemErrorCode = ::GetLastError();
                break;
            }
        }

        SP_DEVINFO_DATA deviceInfoData;
        ::ZeroMemory(&deviceInfoData, sizeof(deviceInfoData));
        deviceInfoData.cbSize = sizeof(deviceInfoData);

        QByteArray deviceInterfaceDetailDataBuffer(
                    deviceInterfaceDetailDataSize, 0);

        PSP_INTERFACE_DEVICE_DETAIL_DATA deviceInterfaceDetailData =
                reinterpret_cast<PSP_INTERFACE_DEVICE_DETAIL_DATA>
                (deviceInterfaceDetailDataBuffer.data());

        deviceInterfaceDetailData->cbSize =
                sizeof(SP_INTERFACE_DEVICE_DETAIL_DATA);

        if (!::SetupDiGetDeviceInterfaceDetail(
                    deviceInfoSet,
                    &deviceInterfaceData,
                    deviceInterfaceDetailData,
                    deviceInterfaceDetailDataBuffer.size(),
                    &deviceInterfaceDetailDataSize,
                    &deviceInfoData)) {
            *systemErrorCode = ::GetLastError();
            break;
        }

        foundSystemPath = QString::fromWCharArray(deviceInterfaceDetailData->DevicePath);
        *systemErrorCode = NO_ERROR;
        break;
    }

    ::SetupDiDestroyDeviceInfoList(deviceInfoSet);
    return foundSystemPath;
}

static HANDLE openSystemDevice(
        const QString &systemPath, QIODevice::OpenMode openMode, int *systemErrorCode)
{
    DWORD desiredAccess = 0;
    DWORD shareMode = 0;

    if (openMode & QIODevice::ReadOnly) {
        desiredAccess |= GENERIC_READ;
        shareMode |= FILE_SHARE_READ;
    }

    if (openMode & QIODevice::WriteOnly) {
        desiredAccess |= GENERIC_WRITE;
        shareMode |= FILE_SHARE_WRITE;
    }

    const HANDLE hDevice = ::CreateFile(
                reinterpret_cast<const wchar_t *>(systemPath.utf16()),
                desiredAccess,
                shareMode,
                NULL,
                OPEN_EXISTING,
                0,
                NULL);

    *systemErrorCode = (INVALID_HANDLE_VALUE == hDevice)
            ? ::GetLastError() : NO_ERROR;
    return hDevice;
}

static HANDLE openSystemService(
        const QBluetoothUuid &service, QIODevice::OpenMode openMode, int *systemErrorCode)
{
    const QString serviceSystemPath = getServiceSystemPath(
                service, systemErrorCode);

    if (*systemErrorCode != NO_ERROR)
        return INVALID_HANDLE_VALUE;

    const HANDLE hService = openSystemDevice(
                serviceSystemPath, openMode, systemErrorCode);

    if (*systemErrorCode != NO_ERROR)
        return INVALID_HANDLE_VALUE;

    return hService;
}

static void closeSystemDevice(HANDLE hDevice)
{
    if (hDevice && hDevice != INVALID_HANDLE_VALUE)
        ::CloseHandle(hDevice);
}

static QVector<BTH_LE_GATT_SERVICE> enumeratePrimaryGattServices(
        HANDLE hDevice, int *systemErrorCode)
{
    if (!gattFunctionsResolved) {
        *systemErrorCode = ERROR_NOT_SUPPORTED;
        return QVector<BTH_LE_GATT_SERVICE>();
    }

    QVector<BTH_LE_GATT_SERVICE> foundServices;
    USHORT servicesCount = 0;
    forever {
        const HRESULT hr = ::BluetoothGATTGetServices(
                    hDevice,
                    servicesCount,
                    foundServices.isEmpty() ? NULL : &foundServices[0],
                    &servicesCount,
                    BLUETOOTH_GATT_FLAG_NONE);

        if (hr == HRESULT_FROM_WIN32(ERROR_MORE_DATA)) {
            foundServices.resize(servicesCount);
        } else if (hr == S_OK) {
            *systemErrorCode = NO_ERROR;
            return foundServices;
        } else {
            *systemErrorCode = ::GetLastError();
            return QVector<BTH_LE_GATT_SERVICE>();
        }
    }
}

static QVector<BTH_LE_GATT_CHARACTERISTIC> enumerateGattCharacteristics(
        HANDLE hService, PBTH_LE_GATT_SERVICE gattService, int *systemErrorCode)
{
    if (!gattFunctionsResolved) {
        *systemErrorCode = ERROR_NOT_SUPPORTED;
        return QVector<BTH_LE_GATT_CHARACTERISTIC>();
    }

    QVector<BTH_LE_GATT_CHARACTERISTIC> foundCharacteristics;
    USHORT characteristicsCount = 0;
    forever {
        const HRESULT hr = ::BluetoothGATTGetCharacteristics(
                    hService,
                    gattService,
                    characteristicsCount,
                    foundCharacteristics.isEmpty() ? NULL : &foundCharacteristics[0],
                    &characteristicsCount,
                    BLUETOOTH_GATT_FLAG_NONE);

        if (hr == HRESULT_FROM_WIN32(ERROR_MORE_DATA)) {
            foundCharacteristics.resize(characteristicsCount);
        } else if (hr == S_OK) {
            *systemErrorCode = NO_ERROR;
            return foundCharacteristics;
        } else {
            *systemErrorCode = ::GetLastError();
            return QVector<BTH_LE_GATT_CHARACTERISTIC>();
        }
    }
}

static QByteArray getGattCharacteristicValue(
        HANDLE hService, PBTH_LE_GATT_CHARACTERISTIC gattCharacteristic, int *systemErrorCode)
{
    if (!gattFunctionsResolved) {
        *systemErrorCode = ERROR_NOT_SUPPORTED;
        return QByteArray();
    }

    QByteArray valueBuffer;
    USHORT valueBufferSize = 0;
    forever {
        const HRESULT hr = ::BluetoothGATTGetCharacteristicValue(
                    hService,
                    gattCharacteristic,
                    valueBufferSize,
                    valueBuffer.isEmpty() ? NULL : reinterpret_cast<PBTH_LE_GATT_CHARACTERISTIC_VALUE>(valueBuffer.data()),
                    &valueBufferSize,
                    BLUETOOTH_GATT_FLAG_NONE);

        if (hr == HRESULT_FROM_WIN32(ERROR_MORE_DATA)) {
            valueBuffer.resize(valueBufferSize);
        } else if (hr == S_OK) {
            *systemErrorCode = NO_ERROR;
            const PBTH_LE_GATT_CHARACTERISTIC_VALUE value = reinterpret_cast<
                    PBTH_LE_GATT_CHARACTERISTIC_VALUE>(valueBuffer.data());

            return QByteArray(reinterpret_cast<const char *>(&value->Data[0]), value->DataSize);
        } else {
            *systemErrorCode = ::GetLastError();
            return QByteArray();
        }
    }
}

static void setGattCharacteristicValue(
        HANDLE hService, PBTH_LE_GATT_CHARACTERISTIC gattCharacteristic,
        const QByteArray &value, DWORD flags, int *systemErrorCode)
{
    if (!gattFunctionsResolved) {
        *systemErrorCode = ERROR_NOT_SUPPORTED;
        return;
    }

    QByteArray valueBuffer;
    QDataStream out(&valueBuffer, QIODevice::WriteOnly);
    ULONG dataSize = value.size();
    out.writeRawData(reinterpret_cast<const char *>(&dataSize), sizeof(dataSize));
    out.writeRawData(value.constData(), value.size());

    BTH_LE_GATT_RELIABLE_WRITE_CONTEXT reliableWriteContext = 0;

    if (::BluetoothGATTSetCharacteristicValue(
                hService,
                gattCharacteristic,
                reinterpret_cast<PBTH_LE_GATT_CHARACTERISTIC_VALUE>(valueBuffer.data()),
                reliableWriteContext,
                flags) != S_OK) {
        *systemErrorCode = ::GetLastError();
    } else {
        *systemErrorCode = NO_ERROR;
    }
}

static QVector<BTH_LE_GATT_DESCRIPTOR> enumerateGattDescriptors(
        HANDLE hService, PBTH_LE_GATT_CHARACTERISTIC gattCharacteristic, int *systemErrorCode)
{
    if (!gattFunctionsResolved) {
        *systemErrorCode = ERROR_NOT_SUPPORTED;
        return QVector<BTH_LE_GATT_DESCRIPTOR>();
    }

    QVector<BTH_LE_GATT_DESCRIPTOR> foundDescriptors;
    USHORT descriptorsCount = 0;
    forever {
        const HRESULT hr = ::BluetoothGATTGetDescriptors(
                    hService,
                    gattCharacteristic,
                    descriptorsCount,
                    foundDescriptors.isEmpty() ? NULL : &foundDescriptors[0],
                    &descriptorsCount,
                    BLUETOOTH_GATT_FLAG_NONE);

        if (hr == HRESULT_FROM_WIN32(ERROR_MORE_DATA)) {
            foundDescriptors.resize(descriptorsCount);
        } else if (hr == S_OK) {
            *systemErrorCode = NO_ERROR;
            return foundDescriptors;
        } else {
            *systemErrorCode = ::GetLastError();
            return QVector<BTH_LE_GATT_DESCRIPTOR>();
        }
    }
}

static QByteArray getGattDescriptorValue(
        HANDLE hService, PBTH_LE_GATT_DESCRIPTOR gattDescriptor, int *systemErrorCode)
{
    if (!gattFunctionsResolved) {
        *systemErrorCode = ERROR_NOT_SUPPORTED;
        return QByteArray();
    }

    QByteArray valueBuffer;
    USHORT valueBufferSize = 0;
    forever {
        const HRESULT hr = ::BluetoothGATTGetDescriptorValue(
                    hService,
                    gattDescriptor,
                    valueBufferSize,
                    valueBuffer.isEmpty() ? NULL : reinterpret_cast<PBTH_LE_GATT_DESCRIPTOR_VALUE>(valueBuffer.data()),
                    &valueBufferSize,
                    BLUETOOTH_GATT_FLAG_NONE);

        if (hr == HRESULT_FROM_WIN32(ERROR_MORE_DATA)) {
            valueBuffer.resize(valueBufferSize);
        } else if (hr == S_OK) {
            *systemErrorCode = NO_ERROR;
            const PBTH_LE_GATT_DESCRIPTOR_VALUE value = reinterpret_cast<
                    PBTH_LE_GATT_DESCRIPTOR_VALUE>(valueBuffer.data());

            return QByteArray(reinterpret_cast<const char *>(&value->Data[0]), value->DataSize);
        } else {
            *systemErrorCode = ::GetLastError();
            return QByteArray();
        }
    }
}

static QBluetoothUuid qtBluetoothUuidFromNativeLeUuid(const BTH_LE_UUID &uuid)
{
    return uuid.IsShortUuid ? QBluetoothUuid(uuid.Value.ShortUuid)
                            : QBluetoothUuid(uuid.Value.LongUuid);
}

static BTH_LE_UUID nativeLeUuidFromQtBluetoothUuid(const QBluetoothUuid &uuid)
{
    BTH_LE_UUID gattUuid;
    ::ZeroMemory(&gattUuid, sizeof(gattUuid));
    if (uuid.minimumSize() == 2) {
        gattUuid.IsShortUuid = TRUE;
        gattUuid.Value.ShortUuid = uuid.data1; // other fields should be empty!
    } else {
        gattUuid.Value.LongUuid = uuid;
    }
    return gattUuid;
}

static BTH_LE_GATT_CHARACTERISTIC recoverNativeLeGattCharacteristic(
        QLowEnergyHandle serviceHandle, QLowEnergyHandle characteristicHandle,
        const QLowEnergyServicePrivate::CharData &characteristicData)
{
    BTH_LE_GATT_CHARACTERISTIC gattCharacteristic;

    gattCharacteristic.ServiceHandle = serviceHandle;
    gattCharacteristic.AttributeHandle = characteristicHandle;
    gattCharacteristic.CharacteristicValueHandle = characteristicData.valueHandle;

    gattCharacteristic.CharacteristicUuid = nativeLeUuidFromQtBluetoothUuid(
                characteristicData.uuid);

    gattCharacteristic.HasExtendedProperties = bool(characteristicData.properties
            & QLowEnergyCharacteristic::ExtendedProperty);
    gattCharacteristic.IsBroadcastable = bool(characteristicData.properties
            & QLowEnergyCharacteristic::Broadcasting);
    gattCharacteristic.IsIndicatable = bool(characteristicData.properties
            & QLowEnergyCharacteristic::Indicate);
    gattCharacteristic.IsNotifiable = bool(characteristicData.properties
            & QLowEnergyCharacteristic::Notify);
    gattCharacteristic.IsReadable = bool(characteristicData.properties
            & QLowEnergyCharacteristic::Read);
    gattCharacteristic.IsSignedWritable = bool(characteristicData.properties
            & QLowEnergyCharacteristic::WriteSigned);
    gattCharacteristic.IsWritable = bool(characteristicData.properties
            & QLowEnergyCharacteristic::Write);
    gattCharacteristic.IsWritableWithoutResponse = bool(characteristicData.properties
            & QLowEnergyCharacteristic::WriteNoResponse);

    return gattCharacteristic;
}

QLowEnergyControllerPrivate::QLowEnergyControllerPrivate()
    : QObject()
    , state(QLowEnergyController::UnconnectedState)
    , error(QLowEnergyController::NoError)
{
    gattFunctionsResolved = resolveFunctions(bluetoothapis());
    if (!gattFunctionsResolved) {
        qCWarning(QT_BT_WINDOWS) << "LE is not supported on this OS";
        return;
    }
}

QLowEnergyControllerPrivate::~QLowEnergyControllerPrivate()
{
}

void QLowEnergyControllerPrivate::connectToDevice()
{
    // required to pass unit test on default backend
    if (remoteDevice.isNull()) {
        qWarning() << "Invalid/null remote device address";
        setError(QLowEnergyController::UnknownRemoteDeviceError);
        return;
    }

    if (!deviceSystemPath.isEmpty()) {
        qCDebug(QT_BT_WINDOWS) << "Already is connected";
        return;
    }

    setState(QLowEnergyController::ConnectingState);

    deviceSystemPath =
            QBluetoothDeviceDiscoveryAgentPrivate::discoveredLeDeviceSystemPath(
                remoteDevice);

    if (deviceSystemPath.isEmpty()) {
        qCWarning(QT_BT_WINDOWS) << qt_error_string(ERROR_PATH_NOT_FOUND);
        setError(QLowEnergyController::UnknownRemoteDeviceError);
        setState(QLowEnergyController::UnconnectedState);
        return;
    }

    setState(QLowEnergyController::ConnectedState);

    Q_Q(QLowEnergyController);
    emit q->connected();
}

void QLowEnergyControllerPrivate::disconnectFromDevice()
{
    if (deviceSystemPath.isEmpty()) {
        qCDebug(QT_BT_WINDOWS) << "Already is disconnected";
        return;
    }

    setState(QLowEnergyController::ClosingState);
    deviceSystemPath.clear();
    setState(QLowEnergyController::UnconnectedState);

    Q_Q(QLowEnergyController);
    emit q->disconnected();
}

void QLowEnergyControllerPrivate::discoverServices()
{
    int systemErrorCode = NO_ERROR;

    const HANDLE hDevice = openSystemDevice(
                deviceSystemPath, QIODevice::ReadOnly, &systemErrorCode);

    if (systemErrorCode != NO_ERROR) {
        qCWarning(QT_BT_WINDOWS) << qt_error_string(systemErrorCode);
        setError(QLowEnergyController::NetworkError);
        setState(QLowEnergyController::ConnectedState);
        return;
    }

    const QVector<BTH_LE_GATT_SERVICE> foundServices =
            enumeratePrimaryGattServices(hDevice, &systemErrorCode);

    closeSystemDevice(hDevice);

    if (systemErrorCode != NO_ERROR) {
        qCWarning(QT_BT_WINDOWS) << qt_error_string(systemErrorCode);
        setError(QLowEnergyController::NetworkError);
        setState(QLowEnergyController::ConnectedState);
        return;
    }

    setState(QLowEnergyController::DiscoveringState);

    Q_Q(QLowEnergyController);

    foreach (const BTH_LE_GATT_SERVICE &service, foundServices) {
        const QBluetoothUuid uuid = qtBluetoothUuidFromNativeLeUuid(
                    service.ServiceUuid);
        qCDebug(QT_BT_WINDOWS) << "Found uuid:" << uuid;

        QLowEnergyServicePrivate *priv = new QLowEnergyServicePrivate();
        priv->uuid = uuid;
        priv->type = QLowEnergyService::PrimaryService;
        priv->startHandle = service.AttributeHandle;
        priv->setController(this);

        QSharedPointer<QLowEnergyServicePrivate> pointer(priv);
        serviceList.insert(uuid, pointer);

        emit q->serviceDiscovered(uuid);
    }

    setState(QLowEnergyController::DiscoveredState);
    emit q->discoveryFinished();
}

void QLowEnergyControllerPrivate::discoverServiceDetails(
        const QBluetoothUuid &service)
{
    if (!serviceList.contains(service)) {
        qCWarning(QT_BT_WINDOWS) << "Discovery of unknown service" << service.toString()
                                 << "not possible";
        return;
    }

    QSharedPointer<QLowEnergyServicePrivate> servicePrivate =
                serviceList.value(service);

    int systemErrorCode = NO_ERROR;

    const HANDLE hService = openSystemService(
                service, QIODevice::ReadOnly, &systemErrorCode);

    if (systemErrorCode != NO_ERROR) {
        qCWarning(QT_BT_WINDOWS) << "Unable to open service" << service.toString()
                                 << ":" << qt_error_string(systemErrorCode);
        servicePrivate->setError(QLowEnergyService::UnknownError);
        servicePrivate->setState(QLowEnergyService::DiscoveryRequired);
        return;
    }

    // We assume that the service does not have any characteristics with descriptors.
    servicePrivate->endHandle = servicePrivate->startHandle;

    const QVector<BTH_LE_GATT_CHARACTERISTIC> foundCharacteristics =
            enumerateGattCharacteristics(hService, NULL, &systemErrorCode);

    if (systemErrorCode != NO_ERROR) {
        closeSystemDevice(hService);
        qCWarning(QT_BT_WINDOWS) << "Unable to get characteristics for service" << service.toString()
                                 << ":" << qt_error_string(systemErrorCode);
        servicePrivate->setError(QLowEnergyService::CharacteristicReadError);
        servicePrivate->setState(QLowEnergyService::DiscoveryRequired);
        return;
    }

    foreach (const BTH_LE_GATT_CHARACTERISTIC &gattCharacteristic, foundCharacteristics) {
        const QLowEnergyHandle characteristicHandle = gattCharacteristic.AttributeHandle;

        QLowEnergyServicePrivate::CharData detailsData;
        detailsData.uuid = qtBluetoothUuidFromNativeLeUuid(
                    gattCharacteristic.CharacteristicUuid);
        detailsData.valueHandle = gattCharacteristic.CharacteristicValueHandle;

        QLowEnergyCharacteristic::PropertyTypes properties = QLowEnergyCharacteristic::Unknown;
        if (gattCharacteristic.HasExtendedProperties)
            properties |= QLowEnergyCharacteristic::ExtendedProperty;
        if (gattCharacteristic.IsBroadcastable)
            properties |= QLowEnergyCharacteristic::Broadcasting;
        if (gattCharacteristic.IsIndicatable)
            properties |= QLowEnergyCharacteristic::Indicate;
        if (gattCharacteristic.IsNotifiable)
            properties |= QLowEnergyCharacteristic::Notify;
        if (gattCharacteristic.IsReadable)
            properties |= QLowEnergyCharacteristic::Read;
        if (gattCharacteristic.IsSignedWritable)
            properties |= QLowEnergyCharacteristic::WriteSigned;
        if (gattCharacteristic.IsWritable)
            properties |= QLowEnergyCharacteristic::Write;
        if (gattCharacteristic.IsWritableWithoutResponse)
            properties |= QLowEnergyCharacteristic::WriteNoResponse;

        detailsData.properties = properties;
        detailsData.value = getGattCharacteristicValue(
                    hService, const_cast<PBTH_LE_GATT_CHARACTERISTIC>(
                        &gattCharacteristic), &systemErrorCode);

        if (systemErrorCode != NO_ERROR) {
            // We do not interrupt enumerating of characteristics
            // if value can not be read
            qCWarning(QT_BT_WINDOWS) << "Unable to get value for characteristic"
                                     << detailsData.uuid.toString()
                                     << "of the service" << service.toString()
                                     << ":" << qt_error_string(systemErrorCode);
        }

        // We assume that the characteristic has no any descriptors. So, the
        // biggest characteristic + 1 will indicate an end handle of service.
        servicePrivate->endHandle = std::max(
                    servicePrivate->endHandle,
                    QLowEnergyHandle(gattCharacteristic.AttributeHandle + 1));

        const QVector<BTH_LE_GATT_DESCRIPTOR> foundDescriptors = enumerateGattDescriptors(
                    hService, const_cast<PBTH_LE_GATT_CHARACTERISTIC>(
                        &gattCharacteristic), &systemErrorCode);

        if (systemErrorCode != NO_ERROR) {
            if (systemErrorCode != ERROR_NOT_FOUND) {
                closeSystemDevice(hService);
                qCWarning(QT_BT_WINDOWS) << "Unable to get descriptor for characteristic"
                                         << detailsData.uuid.toString()
                                         << "of the service" << service.toString()
                                         << ":" << qt_error_string(systemErrorCode);
                servicePrivate->setError(QLowEnergyService::DescriptorReadError);
                servicePrivate->setState(QLowEnergyService::DiscoveryRequired);
                return;
            }
        }

        foreach (const BTH_LE_GATT_DESCRIPTOR &gattDescriptor, foundDescriptors) {
            const QLowEnergyHandle descriptorHandle = gattDescriptor.AttributeHandle;

            QLowEnergyServicePrivate::DescData data;
            data.uuid = qtBluetoothUuidFromNativeLeUuid(
                        gattDescriptor.DescriptorUuid);

            data.value = getGattDescriptorValue(hService, const_cast<PBTH_LE_GATT_DESCRIPTOR>(
                                                    &gattDescriptor), &systemErrorCode);

            if (systemErrorCode != NO_ERROR) {
                closeSystemDevice(hService);
                qCWarning(QT_BT_WINDOWS) << "Unable to get value for descriptor"
                                         << data.uuid.toString()
                                         << "for characteristic"
                                         << detailsData.uuid.toString()
                                         << "of the service" << service.toString()
                                         << ":" << qt_error_string(systemErrorCode);
                servicePrivate->setError(QLowEnergyService::DescriptorReadError);
                servicePrivate->setState(QLowEnergyService::DiscoveryRequired);
                return;
            }

            // Biggest descriptor will contain an end handle of service.
            servicePrivate->endHandle = std::max(
                        servicePrivate->endHandle,
                        QLowEnergyHandle(gattDescriptor.AttributeHandle));

            detailsData.descriptorList.insert(descriptorHandle, data);
        }

        servicePrivate->characteristicList.insert(characteristicHandle, detailsData);
    }

    closeSystemDevice(hService);

    servicePrivate->setState(QLowEnergyService::ServiceDiscovered);
}

void QLowEnergyControllerPrivate::readCharacteristic(
        const QSharedPointer<QLowEnergyServicePrivate> service,
        const QLowEnergyHandle charHandle)
{
    Q_ASSERT(!service.isNull());
    if (!service->characteristicList.contains(charHandle))
        return;

    const QLowEnergyServicePrivate::CharData &charDetails
            = service->characteristicList[charHandle];
    if (!(charDetails.properties & QLowEnergyCharacteristic::Read)) {
        // if this succeeds the device has a bug, char is advertised as
        // non-readable. We try to be permissive and let the remote
        // device answer to the read attempt
        qCWarning(QT_BT_WINDOWS) << "Reading non-readable char" << charHandle;
    }

    int systemErrorCode = NO_ERROR;

    const HANDLE hService = openSystemService(
                service->uuid, QIODevice::ReadOnly, &systemErrorCode);

    if (systemErrorCode != NO_ERROR) {
        qCWarning(QT_BT_WINDOWS) << "Unable to open service" << service->uuid.toString()
                                 << ":" << qt_error_string(systemErrorCode);
        service->setError(QLowEnergyService::CharacteristicReadError);
        return;
    }

    BTH_LE_GATT_CHARACTERISTIC gattCharacteristic = recoverNativeLeGattCharacteristic(
                service->startHandle, charHandle, charDetails);

    const QByteArray characteristicValue = getGattCharacteristicValue(
            hService, &gattCharacteristic, &systemErrorCode);

    if (systemErrorCode != NO_ERROR) {
        qCWarning(QT_BT_WINDOWS) << "Unable to get value for characteristic"
                                 << charDetails.uuid.toString()
                                 << "of the service" << service->uuid.toString()
                                 << ":" << qt_error_string(systemErrorCode);
        service->setError(QLowEnergyService::CharacteristicReadError);
        return;
    }

    updateValueOfCharacteristic(charHandle, characteristicValue, false);

    QLowEnergyCharacteristic ch(service, charHandle);
    emit service->characteristicRead(ch, characteristicValue);
}

void QLowEnergyControllerPrivate::writeCharacteristic(
        const QSharedPointer<QLowEnergyServicePrivate> service,
        const QLowEnergyHandle charHandle,
        const QByteArray &newValue,
        bool writeWithResponse)
{
    Q_ASSERT(!service.isNull());
    if (!service->characteristicList.contains(charHandle))
        return;

    int systemErrorCode = NO_ERROR;

    const HANDLE hService = openSystemService(
                service->uuid, QIODevice::ReadWrite, &systemErrorCode);

    if (systemErrorCode != NO_ERROR) {
        qCWarning(QT_BT_WINDOWS) << "Unable to open service" << service->uuid.toString()
                                 << ":" << qt_error_string(systemErrorCode);
        service->setError(QLowEnergyService::CharacteristicWriteError);
        return;
    }

    const QLowEnergyServicePrivate::CharData &charDetails
            = service->characteristicList[charHandle];

    BTH_LE_GATT_CHARACTERISTIC gattCharacteristic = recoverNativeLeGattCharacteristic(
                service->startHandle, charHandle, charDetails);

    const DWORD flags = writeWithResponse
            ? BLUETOOTH_GATT_FLAG_NONE
            : BLUETOOTH_GATT_FLAG_WRITE_WITHOUT_RESPONSE;

    // TODO: If a device is not connected, this function will block
    // for some time. So, need to re-implement of writeCharacteristic()
    // with use QFutureWatcher.
    setGattCharacteristicValue(hService, &gattCharacteristic,
                               newValue, flags, &systemErrorCode);

    if (systemErrorCode != NO_ERROR) {
        qCWarning(QT_BT_WINDOWS) << "Unable to set value for characteristic"
                                 << charDetails.uuid.toString()
                                 << "of the service" << service->uuid.toString()
                                 << ":" << qt_error_string(systemErrorCode);
        service->setError(QLowEnergyService::CharacteristicWriteError);
        return;
    }

    updateValueOfCharacteristic(charHandle, newValue, false);

    if (writeWithResponse) {
        QLowEnergyCharacteristic ch(service, charHandle);
        emit service->characteristicWritten(ch, newValue);
    }
}

void QLowEnergyControllerPrivate::readDescriptor(
        const QSharedPointer<QLowEnergyServicePrivate> /*service*/,
        const QLowEnergyHandle /*charHandle*/,
        const QLowEnergyHandle /*descriptorHandle*/)
{

}

void QLowEnergyControllerPrivate::writeDescriptor(
        const QSharedPointer<QLowEnergyServicePrivate> /*service*/,
        const QLowEnergyHandle /*charHandle*/,
        const QLowEnergyHandle /*descriptorHandle*/,
        const QByteArray &/*newValue*/)
{

}

QT_END_NAMESPACE

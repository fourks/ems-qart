#include "EmsCart.h"

#include <QDebug>
#include <QDataStream>
#include <QtEndian>

EmsCart::EmsCart(QObject *parent) :
    QObject(parent)
{
    m_deviceHandle = nullptr;
    m_interfaceClaimed = false;

    int result = libusb_init(NULL);
    if (result < 0) {
        emit error(QStringLiteral("Failed to initialize libusb"));
    }
}

EmsCart::~EmsCart()
{
    if (m_interfaceClaimed) {
        libusb_release_interface(m_deviceHandle, 0);
    }

    if (m_deviceHandle) {
        libusb_close(m_deviceHandle);
    }

    libusb_exit(nullptr);
}

void EmsCart::findDevice()
{
    int result = 0;
    if (ready()) {
        libusb_release_interface(m_deviceHandle, 0);
        result = libusb_claim_interface(m_deviceHandle, 0);
        if (result < 0) {
            qWarning() << "usb_claim_interface error " << result;
            m_interfaceClaimed = false;
            libusb_close(m_deviceHandle);
            m_deviceHandle = nullptr;
            emit readyChanged(false);
        } else {
            return;
        }
    }

    ssize_t numDevices = 0;
    libusb_device **deviceList = nullptr;
    struct libusb_device_descriptor deviceDescriptor;

    numDevices = libusb_get_device_list(nullptr, &deviceList);
    if (numDevices >= 0) {
        int i;
        for (i=0; i < numDevices; ++i) {
            (void) memset(&deviceDescriptor, 0, sizeof(deviceDescriptor));
            result = libusb_get_device_descriptor(deviceList[i], &deviceDescriptor);
            if (result == 0) {
                if (deviceDescriptor.idVendor == EmsConstants::UsbVID
                    && deviceDescriptor.idProduct == EmsConstants::UsbPID) {
                    result = libusb_open(deviceList[i], &m_deviceHandle);
                    if (result != 0) {
                        /*
                         * According to the documentation, m_deviceHandle will not
                         * be populated on error, so it should remain nullptr
                         */
                        qWarning() << "Failed to open device, libusb error: " << libusb_error_name(result);
                        if (result == LIBUSB_ERROR_ACCESS) {
                            emit error(QStringLiteral("Device access error. Did you install udev rules? Check README"));
                        } else if (result == LIBUSB_ERROR_NOT_SUPPORTED) {
                            emit error(QStringLiteral("Device not supported. Did you install the drivers? Check README"));
                        }
                    } else {
                        result = libusb_claim_interface(m_deviceHandle, 0);
                        if (result < 0) {
                            qWarning() << "usb_claim_interface error " << result;
                        } else {
                            m_interfaceClaimed = true;
                            emit readyChanged(true);
                        }
                    }
                    break;
                }
            } else {
                qWarning() << "Failed to get device description, libusb error: " << libusb_error_name(result);
            }
        }
        if (i == numDevices) {
            qWarning() << "Could not find device, is it plugged in?";
        }
        libusb_free_device_list(deviceList, 1);
        deviceList = nullptr;
    } else {
      qWarning() << "Failed to get device list: " << libusb_error_name((int)numDevices);
    }
}

QByteArray EmsCart::createCommandBuffer(uint8_t command, uint32_t offset, uint32_t count)
{
    QByteArray commandBuffer;
    commandBuffer.resize(sizeof(command) + sizeof(offset) + sizeof(count));
    QDataStream commandStream(&commandBuffer, QIODevice::ReadWrite);
    commandStream.writeRawData((const char *)&command, sizeof(command));
    uint32_t bigEndianOffset = qToBigEndian(offset);
    commandStream.writeRawData((const char *)&bigEndianOffset, sizeof(bigEndianOffset));
    uint32_t bigEndianCount = qToBigEndian(count);
    commandStream.writeRawData((const char *)&bigEndianCount, sizeof(bigEndianCount));
    return commandBuffer;
}

QByteArray EmsCart::read(EmsMemory from, uint32_t offset, uint32_t count)
{
    int result, transferred;
    uint8_t cmd;

    switch (from) {
        case (ROM):
            cmd = ReadROMCommand;
            break;
        case (SRAM):
            cmd = ReadSRAMCommand;
            break;
        default:
            qWarning() << "from must be ROM or SRAM, aborting";
            return QByteArray();
    }

    QByteArray cmdBuffer = createCommandBuffer(cmd, offset, count);
    // Send the read command
    result = libusb_bulk_transfer(m_deviceHandle, EmsConstants::SendEndpoint, reinterpret_cast<uchar *>(cmdBuffer.data()), cmdBuffer.size(), &transferred, 0);
    if (result < 0) {
        return QByteArray();
    }

    QByteArray outBuffer;
    outBuffer.resize(count);
    // Read the data
    result = libusb_bulk_transfer(m_deviceHandle, EmsConstants::ReceiveEndpoint, reinterpret_cast<uchar *>(outBuffer.data()), count, &transferred,   0);
    if (result < 0) {
        return QByteArray();
    }

    return outBuffer;
}

bool EmsCart::write(EmsMemory to, QByteArray data, uint32_t offset, uint32_t count)
{
    int result, transferred;
    uint8_t cmd;

    switch (to) {
        case (ROM):
            cmd = WriteROMCommand;
            break;
        case (SRAM):
            cmd = WriteSRAMCommand;
            break;
        default:
            qWarning() << "to must be ROM or SRAM, aborting";
            return false;
    }

    QByteArray outBuffer = createCommandBuffer(cmd, offset, count);
    outBuffer.append(data);

    // Send the write command with data
    result = libusb_bulk_transfer(m_deviceHandle, EmsConstants::SendEndpoint, reinterpret_cast<uchar *>(outBuffer.data()), outBuffer.size(), &transferred, 0);
    if (result < 0) {
        return false;
    }

    return true;
}

bool EmsCart::ready()
{
    return (m_deviceHandle != nullptr && m_interfaceClaimed);
}

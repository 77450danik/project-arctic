/*
 * WinUSB: pipe policies, pipe information and the setup packet
 *
 * Arctic: as the Windows SDK's winusbio.h declares them.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_WINUSBIO_H
#define __WINE_WINUSBIO_H

#include <ddk/usb100.h>

#ifndef __DDK_USB_H__
typedef enum _USBD_PIPE_TYPE
{
    UsbdPipeTypeControl,
    UsbdPipeTypeIsochronous,
    UsbdPipeTypeBulk,
    UsbdPipeTypeInterrupt
} USBD_PIPE_TYPE;
#endif

/* WinUsb_SetPowerPolicy */
#define AUTO_SUSPEND            0x81
#define SUSPEND_DELAY           0x83

/* WinUsb_SetPipePolicy */
#define SHORT_PACKET_TERMINATE  0x01
#define AUTO_CLEAR_STALL        0x02
#define PIPE_TRANSFER_TIMEOUT   0x03
#define IGNORE_SHORT_PACKETS    0x04
#define ALLOW_PARTIAL_READS     0x05
#define AUTO_FLUSH              0x06
#define RAW_IO                  0x07
#define MAXIMUM_TRANSFER_SIZE   0x08
#define RESET_PIPE_ON_RESUME    0x09

/* WinUsb_QueryDeviceInformation */
#define DEVICE_SPEED            0x01
#define LowSpeed                0x01
#define FullSpeed               0x02
#define HighSpeed               0x03

typedef struct _WINUSB_PIPE_INFORMATION
{
    USBD_PIPE_TYPE PipeType;
    UCHAR PipeId;
    USHORT MaximumPacketSize;
    UCHAR Interval;
} WINUSB_PIPE_INFORMATION, *PWINUSB_PIPE_INFORMATION;

typedef struct _WINUSB_PIPE_INFORMATION_EX
{
    USBD_PIPE_TYPE PipeType;
    UCHAR PipeId;
    USHORT MaximumPacketSize;
    UCHAR Interval;
    ULONG MaximumBytesPerInterval;
} WINUSB_PIPE_INFORMATION_EX, *PWINUSB_PIPE_INFORMATION_EX;

#include <pshpack1.h>
typedef struct _WINUSB_SETUP_PACKET
{
    UCHAR RequestType;
    UCHAR Request;
    USHORT Value;
    USHORT Index;
    USHORT Length;
} WINUSB_SETUP_PACKET, *PWINUSB_SETUP_PACKET;
#include <poppack.h>

#endif /* __WINE_WINUSBIO_H */

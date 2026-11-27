/*
 * WinUSB
 *
 * Copyright (C) 2022 Mohamad Al-Jaf
 * Arctic: the whole API, as the Windows SDK's winusb.h declares it.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifndef _WINUSB_H_
#define _WINUSB_H_

#include <winusbio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef PVOID WINUSB_INTERFACE_HANDLE, *PWINUSB_INTERFACE_HANDLE;

BOOL WINAPI WinUsb_Initialize(HANDLE,PWINUSB_INTERFACE_HANDLE);
BOOL WINAPI WinUsb_Free(WINUSB_INTERFACE_HANDLE);
BOOL WINAPI WinUsb_GetAssociatedInterface(WINUSB_INTERFACE_HANDLE,UCHAR,PWINUSB_INTERFACE_HANDLE);
BOOL WINAPI WinUsb_GetDescriptor(WINUSB_INTERFACE_HANDLE,UCHAR,UCHAR,USHORT,PUCHAR,ULONG,PULONG);
BOOL WINAPI WinUsb_QueryInterfaceSettings(WINUSB_INTERFACE_HANDLE,UCHAR,PUSB_INTERFACE_DESCRIPTOR);
BOOL WINAPI WinUsb_QueryDeviceInformation(WINUSB_INTERFACE_HANDLE,ULONG,PULONG,PVOID);
BOOL WINAPI WinUsb_SetCurrentAlternateSetting(WINUSB_INTERFACE_HANDLE,UCHAR);
BOOL WINAPI WinUsb_GetCurrentAlternateSetting(WINUSB_INTERFACE_HANDLE,PUCHAR);
BOOL WINAPI WinUsb_QueryPipe(WINUSB_INTERFACE_HANDLE,UCHAR,UCHAR,PWINUSB_PIPE_INFORMATION);
BOOL WINAPI WinUsb_QueryPipeEx(WINUSB_INTERFACE_HANDLE,UCHAR,UCHAR,PWINUSB_PIPE_INFORMATION_EX);
BOOL WINAPI WinUsb_SetPipePolicy(WINUSB_INTERFACE_HANDLE,UCHAR,ULONG,ULONG,PVOID);
BOOL WINAPI WinUsb_GetPipePolicy(WINUSB_INTERFACE_HANDLE,UCHAR,ULONG,PULONG,PVOID);
BOOL WINAPI WinUsb_ReadPipe(WINUSB_INTERFACE_HANDLE,UCHAR,PUCHAR,ULONG,PULONG,LPOVERLAPPED);
BOOL WINAPI WinUsb_WritePipe(WINUSB_INTERFACE_HANDLE,UCHAR,PUCHAR,ULONG,PULONG,LPOVERLAPPED);
BOOL WINAPI WinUsb_ControlTransfer(WINUSB_INTERFACE_HANDLE,WINUSB_SETUP_PACKET,PUCHAR,ULONG,PULONG,LPOVERLAPPED);
BOOL WINAPI WinUsb_ResetPipe(WINUSB_INTERFACE_HANDLE,UCHAR);
BOOL WINAPI WinUsb_AbortPipe(WINUSB_INTERFACE_HANDLE,UCHAR);
BOOL WINAPI WinUsb_FlushPipe(WINUSB_INTERFACE_HANDLE,UCHAR);
BOOL WINAPI WinUsb_SetPowerPolicy(WINUSB_INTERFACE_HANDLE,ULONG,ULONG,PVOID);
BOOL WINAPI WinUsb_GetPowerPolicy(WINUSB_INTERFACE_HANDLE,ULONG,PULONG,PVOID);
BOOL WINAPI WinUsb_GetOverlappedResult(WINUSB_INTERFACE_HANDLE,LPOVERLAPPED,LPDWORD,BOOL);
PUSB_INTERFACE_DESCRIPTOR WINAPI WinUsb_ParseConfigurationDescriptor(PUSB_CONFIGURATION_DESCRIPTOR,PVOID,LONG,LONG,LONG,LONG,LONG);
PUSB_COMMON_DESCRIPTOR WINAPI WinUsb_ParseDescriptors(PVOID,ULONG,PVOID,LONG);

#ifdef __cplusplus
}
#endif

#endif /* _WINUSB_H_ */

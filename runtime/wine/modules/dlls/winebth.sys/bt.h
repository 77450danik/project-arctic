/*
 * The Linux kernel's Bluetooth interface, as the driver uses it: the
 * management channel of the controllers (doc/mgmt-api.txt of the kernel's
 * Bluetooth subsystem, include/net/bluetooth/mgmt.h) and L2CAP sockets.
 * These are the kernel's ABI; the C library has no header for them.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINEBTH_BT_H
#define __WINEBTH_BT_H

#include <stdint.h>
#include <sys/socket.h>

#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#define BTPROTO_L2CAP       0
#define BTPROTO_HCI         1
#define BTPROTO_RFCOMM      3
#define SOL_BLUETOOTH       274
#define SOL_L2CAP           6
#define SOL_RFCOMM          18
#define BT_SECURITY         4
#define BT_SECURITY_LOW     1
#define BT_SECURITY_MEDIUM  2
#define BT_SNDMTU           12
#define BT_RCVMTU           13
#define L2CAP_OPTIONS       0x01
#define BDADDR_BREDR        0x00
#define BDADDR_LE_PUBLIC    0x01
#define BDADDR_LE_RANDOM    0x02

#define PSM_SDP             0x0001
#define PSM_AVCTP           0x0017
#define PSM_AVDTP           0x0019

typedef struct { uint8_t b[6]; } __attribute__((packed)) bdaddr_t;  /* least significant byte first */

struct sockaddr_l2
{
    sa_family_t l2_family;
    uint16_t    l2_psm;
    bdaddr_t    l2_bdaddr;
    uint16_t    l2_cid;
    uint8_t     l2_bdaddr_type;
};

struct sockaddr_rc
{
    sa_family_t rc_family;
    bdaddr_t    rc_bdaddr;
    uint8_t     rc_channel;
};

/* SCO: the voice link; CVSD in the controller, 16-bit PCM at 8 kHz on the socket */
#define BTPROTO_SCO         2
#define SOL_SCO             17
#define SCO_OPTIONS         0x01

struct sockaddr_sco
{
    sa_family_t sco_family;
    bdaddr_t    sco_bdaddr;
};

struct sco_options
{
    uint16_t mtu;
};

struct bt_security
{
    uint8_t level;
    uint8_t key_size;
};

struct l2cap_options
{
    uint16_t omtu;
    uint16_t imtu;
    uint16_t flush_to;
    uint8_t  mode;
    uint8_t  fcs;
    uint8_t  max_tx;
    uint16_t txwin_size;
};

/* the management channel */
#define MGMT_INDEX_NONE              0xffff

#define MGMT_OP_READ_INDEX_LIST      0x0003
#define MGMT_OP_READ_INFO            0x0004
#define MGMT_OP_SET_POWERED          0x0005
#define MGMT_OP_SET_DISCOVERABLE     0x0006
#define MGMT_OP_SET_CONNECTABLE      0x0007
#define MGMT_OP_SET_BONDABLE         0x0009
#define MGMT_OP_SET_SSP              0x000b
#define MGMT_OP_SET_LE               0x000d
#define MGMT_OP_SET_DEV_CLASS        0x000e
#define MGMT_OP_SET_LOCAL_NAME       0x000f
#define MGMT_OP_ADD_UUID             0x0010
#define MGMT_OP_LOAD_LINK_KEYS       0x0012
#define MGMT_OP_DISCONNECT           0x0014
#define MGMT_OP_GET_CONNECTIONS      0x0015
#define MGMT_OP_PIN_CODE_REPLY       0x0016
#define MGMT_OP_PIN_CODE_NEG_REPLY   0x0017
#define MGMT_OP_SET_IO_CAPABILITY    0x0018
#define MGMT_OP_PAIR_DEVICE          0x0019
#define MGMT_OP_CANCEL_PAIR_DEVICE   0x001a
#define MGMT_OP_UNPAIR_DEVICE        0x001b
#define MGMT_OP_USER_CONFIRM_REPLY   0x001c
#define MGMT_OP_USER_CONFIRM_NEG_REPLY 0x001d
#define MGMT_OP_USER_PASSKEY_REPLY   0x001e
#define MGMT_OP_USER_PASSKEY_NEG_REPLY 0x001f
#define MGMT_OP_START_DISCOVERY      0x0023
#define MGMT_OP_STOP_DISCOVERY       0x0024
#define MGMT_OP_CONFIRM_NAME         0x0025
#define MGMT_OP_READ_DEF_SYSTEM_CONFIG 0x004b
#define MGMT_OP_SET_DEF_SYSTEM_CONFIG  0x004c
#define MGMT_OP_ADD_ADV_PATTERNS_MONITOR 0x0052
#define MGMT_OP_REMOVE_ADV_MONITOR   0x0053

/* the default system configuration's LE scan of the advertisement monitors (TLV types) */
#define MGMT_CONFIG_LE_SCAN_INT_ADV_MONITOR    0x0013
#define MGMT_CONFIG_LE_SCAN_WINDOW_ADV_MONITOR 0x0014

#define MGMT_EV_CMD_COMPLETE         0x0001
#define MGMT_EV_CMD_STATUS           0x0002
#define MGMT_EV_INDEX_ADDED          0x0004
#define MGMT_EV_INDEX_REMOVED        0x0005
#define MGMT_EV_NEW_SETTINGS         0x0006
#define MGMT_EV_CLASS_OF_DEV_CHANGED 0x0007
#define MGMT_EV_LOCAL_NAME_CHANGED   0x0008
#define MGMT_EV_NEW_LINK_KEY         0x0009
#define MGMT_EV_DEVICE_CONNECTED     0x000b
#define MGMT_EV_DEVICE_DISCONNECTED  0x000c
#define MGMT_EV_CONNECT_FAILED       0x000d
#define MGMT_EV_PIN_CODE_REQUEST     0x000e
#define MGMT_EV_USER_CONFIRM_REQUEST 0x000f
#define MGMT_EV_USER_PASSKEY_REQUEST 0x0010
#define MGMT_EV_AUTH_FAILED          0x0011
#define MGMT_EV_DEVICE_FOUND         0x0012
#define MGMT_EV_DISCOVERING          0x0013
#define MGMT_EV_DEVICE_UNPAIRED      0x0016
#define MGMT_EV_PASSKEY_NOTIFY       0x0017
#define MGMT_EV_ADV_MONITOR_DEVICE_FOUND 0x002f

#define MGMT_STATUS_SUCCESS          0x00
#define MGMT_STATUS_NOT_CONNECTED    0x02
#define MGMT_STATUS_CONNECT_FAILED   0x04
#define MGMT_STATUS_AUTH_FAILED      0x05
#define MGMT_STATUS_NOT_PAIRED       0x06
#define MGMT_STATUS_TIMEOUT          0x08
#define MGMT_STATUS_ALREADY_CONNECTED 0x09
#define MGMT_STATUS_BUSY             0x0a
#define MGMT_STATUS_REJECTED         0x0b
#define MGMT_STATUS_NOT_SUPPORTED    0x0c
#define MGMT_STATUS_INVALID_PARAMS   0x0d
#define MGMT_STATUS_DISCONNECTED     0x0e
#define MGMT_STATUS_NOT_POWERED      0x0f
#define MGMT_STATUS_CANCELLED        0x10
#define MGMT_STATUS_RFKILLED         0x12
#define MGMT_STATUS_ALREADY_PAIRED   0x13
#define MGMT_STATUS_PERMISSION_DENIED 0x14
#define MGMT_STATUS_LOCAL_TIMEOUT    0xff   /* ours: no answer came */

#define MGMT_SETTING_POWERED         (1 << 0)
#define MGMT_SETTING_CONNECTABLE     (1 << 1)
#define MGMT_SETTING_DISCOVERABLE    (1 << 3)
#define MGMT_SETTING_BONDABLE        (1 << 4)
#define MGMT_SETTING_SSP             (1 << 6)
#define MGMT_SETTING_BREDR           (1 << 7)
#define MGMT_SETTING_LE              (1 << 9)

#define MGMT_DEV_FOUND_CONFIRM_NAME  (1 << 0)
#define MGMT_DEV_FOUND_LEGACY_PAIRING (1 << 1)

#define MGMT_IO_DISPLAY_YES_NO       0x01
#define MGMT_IO_KEYBOARD_DISPLAY     0x04

#define MGMT_MAX_NAME_LENGTH         249
#define MGMT_MAX_SHORT_NAME_LENGTH   11

struct mgmt_hdr
{
    uint16_t opcode;
    uint16_t index;
    uint16_t len;
} __attribute__((packed));

struct mgmt_addr_info
{
    bdaddr_t bdaddr;
    uint8_t  type;
} __attribute__((packed));

struct mgmt_rp_read_info
{
    bdaddr_t bdaddr;
    uint8_t  version;
    uint16_t manufacturer;
    uint32_t supported_settings;
    uint32_t current_settings;
    uint8_t  dev_class[3];
    uint8_t  name[MGMT_MAX_NAME_LENGTH];
    uint8_t  short_name[MGMT_MAX_SHORT_NAME_LENGTH];
} __attribute__((packed));

struct mgmt_link_key_info
{
    struct mgmt_addr_info addr;
    uint8_t type;
    uint8_t val[16];
    uint8_t pin_len;
} __attribute__((packed));

struct mgmt_ev_device_found
{
    struct mgmt_addr_info addr;
    int8_t   rssi;
    uint32_t flags;
    uint16_t eir_len;
    uint8_t  eir[];
} __attribute__((packed));

struct mgmt_ev_device_connected
{
    struct mgmt_addr_info addr;
    uint32_t flags;
    uint16_t eir_len;
    uint8_t  eir[];
} __attribute__((packed));

struct mgmt_adv_pattern
{
    uint8_t ad_type;
    uint8_t offset;
    uint8_t length;
    uint8_t value[31];
} __attribute__((packed));

/* EIR data types (Bluetooth Assigned Numbers, "Common Data Types") */
#define EIR_UUID16_SOME      0x02
#define EIR_UUID16_ALL       0x03
#define EIR_NAME_SHORT       0x08
#define EIR_NAME_COMPLETE    0x09
#define EIR_CLASS_OF_DEV     0x0d
#define EIR_SERVICE_DATA16   0x16

/* a connection to the root helper (/run/arctic/bt.sock, host/init/arctic-bt.c):
 * the descriptor it hands, or -1 */
int bt_helper( const char *request );

#endif

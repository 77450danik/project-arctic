/*
 * Devices and Printers of the Control Panel (devicescpl.dll)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __DEVICESCPL_H
#define __DEVICESCPL_H

#define IDS_NAME                1
#define IDS_TIP                 2

/* the page */
#define IDS_BLUETOOTH           10
#define IDS_VISIBLE             11
#define IDS_NO_RADIO            12
#define IDS_DEVICES             13
#define IDS_ADD_DEVICE          14
#define IDS_ADD_DEVICE_HINT     15
#define IDS_ADD                 16
#define IDS_RADIO_OFF           17
#define IDS_CONNECTED           20
#define IDS_CONNECTED_MUSIC     21
#define IDS_PAIRED              22
#define IDS_CONNECT             23
#define IDS_DISCONNECT          24
#define IDS_REMOVE              25
#define IDS_CONNECTING          26
#define IDS_REMOVE_CONFIRM      27
#define IDS_PRINTERS            30
#define IDS_PRINTERS_HINT       31
#define IDS_PRINTER             32
#define IDS_SEND_FILES          33
#define IDS_SEND_FILES_HINT     34

/* the wizard "Додавання пристрою" */
#define IDS_WIZ_TITLE           40
#define IDS_WIZ_CHOOSE          41
#define IDS_WIZ_BLUETOOTH_HINT  42
#define IDS_WIZ_PRINTER         43
#define IDS_WIZ_PRINTER_HINT    44
#define IDS_WIZ_SEARCH          45
#define IDS_WIZ_SEARCHING       46
#define IDS_WIZ_CANCEL          47
#define IDS_WIZ_DONE            48
#define IDS_WIZ_READY           49
#define IDS_WIZ_FAILED          50
#define IDS_WIZ_FAILED_HINT     51
#define IDS_WIZ_RETRY           52
#define IDS_WIZ_COMPARE         53
#define IDS_WIZ_TYPE_ON_DEVICE  54
#define IDS_WIZ_ENTER_PASSKEY   55
#define IDS_WIZ_ENTER_PIN       56
#define IDS_WIZ_CONNECT         57
#define IDS_WIZ_NO_RADIO        58
#define IDS_WIZ_UNNAMED         59
#define IDS_WIZ_PAIRING         60

/* sending files (fsquirt.exe) */
#define IDS_SEND_TITLE          70
#define IDS_SEND_CHOOSE         71
#define IDS_SEND_NO_DEVICES     72
#define IDS_SEND_PROGRESS       73
#define IDS_SEND_DONE           74
#define IDS_SEND_FAILED         75

/* the batteries' rings */
#define IDS_BATTERY_LEFT        80
#define IDS_BATTERY_RIGHT       81
#define IDS_BATTERY_CASE        82

/* glyphs.c: the pictures of devices, as Windows 11's Settings draws them
 * (Segoe Fluent Icons' glyphs), drawn here */
enum device_glyph
{
    GLYPH_BLUETOOTH, GLYPH_HEADPHONES, GLYPH_SPEAKER, GLYPH_PHONE, GLYPH_LAPTOP, GLYPH_KEYBOARD, GLYPH_MOUSE,
    GLYPH_GAMEPAD, GLYPH_PRINTER, GLYPH_ADD, GLYPH_SEND,
};

enum device_glyph glyph_of_class( UINT32 class_of_device );
void draw_device_glyph( HDC hdc, enum device_glyph glyph, int x, int y, int size, COLORREF color );
HICON make_device_icon( enum device_glyph glyph, int size, int inset, COLORREF color );

/* wizard.c */
void add_device_wizard( HWND owner, BOOL bluetooth_only );

/* bluetooth.c: the radio and its devices, through bluetoothapis.dll and winebth.sys */
#define BT_BATTERY_UNKNOWN 0xff
struct bt_battery
{
    /* 0-100: the device's own, the left and the right earbud's, their case's;
     * charging: a bit for each, in that order */
    BYTE level, left, right, case_level, charging;
};

struct bt_device
{
    BLUETOOTH_DEVICE_INFO info;
    BOOL audio;                     /* its music is connected */
    struct bt_battery battery;
};

BOOL bt_battery( HANDLE radio, BTH_ADDR address, struct bt_battery *battery );

/* battery.c: the batteries as rings (the control "ArcticBatteryRings", its text
 * the levels: "level,left,right,case,charging") */
#define BATTERY_RINGS_CLASS L"ArcticBatteryRings"
void battery_rings_register( void );
int battery_rings_width( const struct bt_battery *battery );
void battery_rings_text( const struct bt_battery *battery, WCHAR *text, UINT size );
void draw_battery_rings( HDC hdc, const struct bt_battery *battery, const RECT *rect, BOOL dark, BOOL compact,
                         HFONT number, HFONT caption );

HANDLE bt_open_radio(void);
BOOL bt_radio_state( HANDLE radio, BOOL *powered, BOOL *discovering, BTH_ADDR *audio );
BOOL bt_set_power( HANDLE radio, BOOL on );
UINT bt_devices( HANDLE radio, struct bt_device *list, UINT max, BOOL paired );
void bt_connect( BTH_ADDR address );
void bt_disconnect( BTH_ADDR address );
DWORD bt_remove( BTH_ADDR address );
void bt_discovery( HANDLE radio, BOOL on );
void bt_radio_name( HANDLE radio, WCHAR *name, UINT size );

#endif

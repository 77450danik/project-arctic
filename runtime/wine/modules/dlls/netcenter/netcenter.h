/*
 * Network and Sharing Center (netcenter.dll)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __NETCENTER_H
#define __NETCENTER_H

#include "cpanel.h"
#include "commctrl.h"

/* the dialogs of Windows (tcpipcfg.dll, netshell.dll), their control ids kept */
#define IDD_DNS_SERVER         2304
#define IDD_ADVANCED_IP        2308
#define IDD_GATEWAY4           2309
#define IDD_ADDRESS4           2310
#define IDD_ADVANCED_DNS       2322
#define IDD_IPV4_GENERAL       2323
#define IDD_ADVANCED_WINS      2324
#define IDD_IPV4_ALTERNATE     2334
#define IDD_ADDRESS6           2337
#define IDD_GATEWAY6           2338
#define IDD_IPV6_GENERAL       2339
#define IDD_PROPERTIES         16004
#define IDD_STATUS_LAN         23500
#define IDD_STATUS_WLAN        24050
#define IDD_DETAILS            24300

/* IPv4 and IPv6, General */
#define IDC_AUTO_IP            1000
#define IDC_STATIC_IP          1001
#define IDC_IP                 1002
#define IDC_MASK               1003     /* IPv6: the prefix length */
#define IDC_GATEWAY            1004
#define IDC_AUTO_DNS           1009
#define IDC_STATIC_DNS         1010
#define IDC_DNS1               1011
#define IDC_DNS2               1012
#define IDC_ADVANCED           1015
#define IDC_VALIDATE           1098
/* Alternate configuration */
#define IDC_ALT_APIPA          1070
#define IDC_ALT_USER           1071
#define IDC_ALT_IP             1073
#define IDC_ALT_MASK           1075
#define IDC_ALT_GATEWAY        1077
#define IDC_ALT_DNS1           1079
#define IDC_ALT_DNS2           1081
#define IDC_ALT_WINS1          1083
#define IDC_ALT_WINS2          1085
/* Advanced: IP settings */
#define IDC_ADDRESSES          1016
#define IDC_ADDRESS_ADD        1017
#define IDC_ADDRESS_EDIT       1018
#define IDC_ADDRESS_REMOVE     1019
#define IDC_GATEWAYS           1004
#define IDC_GATEWAY_ADD        1020
#define IDC_GATEWAY_EDIT       1021
#define IDC_GATEWAY_REMOVE     1022
#define IDC_AUTO_METRIC        1030
#define IDC_METRIC_LABEL       1073
#define IDC_METRIC             1029
/* Advanced: DNS */
#define IDC_DNS_LIST           1040
#define IDC_DNS_ADD            1041
#define IDC_DNS_EDIT           1042
#define IDC_DNS_REMOVE         1043
#define IDC_DNS_UP             1044
#define IDC_DNS_DOWN           1045
#define IDC_DNS_SUFFIX         1031
#define IDC_DNS_REGISTER       1057
/* Advanced: WINS */
#define IDC_NETBIOS_DEFAULT    1070
#define IDC_NETBIOS_ON         1068
#define IDC_NETBIOS_OFF        1069
#define IDC_LMHOSTS            1066
/* the small dialogs */
#define IDC_EDIT_ADDRESS       1025
#define IDC_EDIT_MASK          1026     /* IPv6: the prefix length */
#define IDC_EDIT_GATEWAY       1027
#define IDC_EDIT_METRIC        1028
#define IDC_EDIT_AUTO_METRIC   1090
#define IDC_EDIT_DNS           1055
/* the connection's properties */
#define IDC_ADAPTER            15003
#define IDC_CONFIGURE          15031
#define IDC_COMPONENTS         15007
#define IDC_INSTALL            15008
#define IDC_UNINSTALL          15012
#define IDC_COMPONENT_PROPS    15011
#define IDC_DESCRIPTION        15020
#define IDC_ADAPTER_ICON       16006
/* the connection's status */
#define IDC_IPV4_STATE         23502
#define IDC_IPV6_STATE         23504
#define IDC_MEDIA_STATE        1007
#define IDC_DURATION           1008
#define IDC_SPEED              1009
#define IDC_SSID               24018
#define IDC_SIGNAL             1044
#define IDC_DETAILS            23506
#define IDC_WIRELESS_PROPS     24020
#define IDC_ACTIVITY_ICON      1010
#define IDC_SENT               1003
#define IDC_RECEIVED           1005
#define IDC_PROPERTIES         1026
#define IDC_DISABLE            1025
#define IDC_DIAGNOSE           23505
#define IDC_DETAILS_LIST       24301

/* strings: Windows' (netcenter.dll.mui, tcpipcfg, netshell) by their numbers there where it has them */
#define IDS_NETCENTER          1
#define IDS_NETCENTER_TIP      2
#define IDS_INTERNET           22
#define IDS_PUBLIC_NETWORK     83
#define IDS_PRIVATE_NETWORK    86
#define IDS_ACCESS_INTERNET    93
#define IDS_ACCESS_NO_INTERNET 94
#define IDS_ACCESS_NO_NETWORK  95
#define IDS_CHANGE_ADAPTER     189
#define IDS_CHANGE_SHARING     190
#define IDS_ACTIVE_NETWORKS    1100
#define IDS_TITLE              1101
#define IDS_NOT_CONNECTED      1102
#define IDS_CHANGE_SETTINGS    1106
#define IDS_NEW_CONNECTION     1107
#define IDS_NEW_CONNECTION_TIP 1108
#define IDS_TROUBLESHOOT       1109
#define IDS_TROUBLESHOOT_TIP   1110
#define IDS_ACCESS_TYPE        1119
#define IDS_HOMEGROUP          1121
#define IDS_CONNECTIONS        1122

/* Windows 7's, which Windows 10 has no more, and Arctic's own */
#define IDS_MANAGE_WIRELESS    2000
#define IDS_CONNECT_TO         2001
#define IDS_CONNECT_TO_TIP     2002
#define IDS_THIS_COMPUTER      2003
#define IDS_FULL_MAP           2004
#define IDS_NETWORK            2005
#define IDS_SEE_HOMEGROUP      2006
#define IDS_SEE_FIREWALL       2007
#define IDS_SEE_INTERNET_OPTIONS 2008
#define IDS_LAN_CONNECTION     2010
#define IDS_WIRELESS_CONNECTION 2011
#define IDS_HOMEGROUP_SHARING  2012
#define IDS_HOMEGROUP_SHARING_TIP 2013
#define IDS_STATUS_TITLE       2020     /* "Стан: %1" */
#define IDS_PROPERTIES_TITLE   2021     /* "%1: властивості" */
#define IDS_GENERAL            2022
#define IDS_NETWORKING         2023
#define IDS_CONNECTED          2024
#define IDS_DISCONNECTED       2025
#define IDS_DISABLED           2026
#define IDS_ENABLE             2027
#define IDS_MBPS               2028
#define IDS_GBPS               2029
#define IDS_NO_ACCESS          2030
#define IDS_INTERNET_ACCESS    2031
#define IDS_NO_INTERNET        2032
#define IDS_NO_NETWORK         2033
#define IDS_BYTES_SENT         2034
#define IDS_BYTES_RECEIVED     2035
#define IDS_DETAILS_PROPERTY   2040
#define IDS_DETAILS_VALUE      2041
#define IDS_DET_SUFFIX         2042
#define IDS_DET_DESCRIPTION    2043
#define IDS_DET_PHYSICAL       2044
#define IDS_DET_DHCP           2045
#define IDS_DET_IPV4           2046
#define IDS_DET_MASK           2047
#define IDS_DET_GATEWAY4       2048
#define IDS_DET_DNS4           2049
#define IDS_DET_IPV6           2050
#define IDS_DET_LINK_LOCAL     2051
#define IDS_DET_GATEWAY6       2052
#define IDS_DET_DNS6           2053
#define IDS_DET_NETBIOS        2054
#define IDS_YES                2055
#define IDS_NO                 2056
#define IDS_IPV4_PROPERTIES    2060
#define IDS_IPV6_PROPERTIES    2061
#define IDS_ADVANCED_TITLE     2062
#define IDS_IP_SETTINGS        2063
#define IDS_DHCP_ENABLED       2064
#define IDS_AUTOMATIC          2065
#define IDS_ADDRESS_COLUMN     2066
#define IDS_MASK_COLUMN        2067
#define IDS_GATEWAY_COLUMN     2068
#define IDS_METRIC_COLUMN      2069
#define IDS_PREFIX_COLUMN      2070
#define IDS_BAD_ADDRESS        2071
#define IDS_BAD_MASK           2072
#define IDS_BAD_GATEWAY        2073
#define IDS_BAD_DNS            2074
#define IDS_APPLY_FAILED       2075
#define IDS_ADD                2076
#define IDS_COMP_CLIENT        2080
#define IDS_COMP_CLIENT_TIP    2081
#define IDS_COMP_QOS           2082
#define IDS_COMP_QOS_TIP       2083
#define IDS_COMP_SHARING       2084
#define IDS_COMP_SHARING_TIP   2085
#define IDS_COMP_IPV6          2086
#define IDS_COMP_IPV6_TIP      2087
#define IDS_COMP_IPV4          2088
#define IDS_COMP_IPV4_TIP      2089
#define IDS_COMP_LLTD_MAPPER   2090
#define IDS_COMP_LLTD_MAPPER_TIP 2091
#define IDS_COMP_LLTD_RESPONDER 2092
#define IDS_COMP_LLTD_RESPONDER_TIP 2093
#define IDS_NO_HOST            2094
#define IDS_DAYS               2095
#define IDS_DISABLE            2096
#define IDS_SHARING_TITLE      2100
#define IDS_SHARING_INTRO      2101
#define IDS_DISCOVERY          2102
#define IDS_DISCOVERY_TEXT     2103
#define IDS_DISCOVERY_ON       2104
#define IDS_DISCOVERY_OFF      2105
#define IDS_FILE_SHARING       2106
#define IDS_FILE_SHARING_TEXT  2107
#define IDS_DRIVES_PASSWORDS   2108
#define IDS_DRIVES_PASSWORDS_TEXT 2109
#define IDS_MAP_DRIVE          2110
#define IDS_DISCONNECT_DRIVE   2111
#define IDS_FORGET_PASSWORDS   2112
#define IDS_SAVE_CHANGES       2113
#define IDS_CANCEL             2114
#define IDS_SHARING_PAGE       2115

#define IDI_NETCENTER          1
#define IDI_COMPUTER           6
#define IDI_INTERNET           7
#define IDI_NETWORK            8
#define IDI_NEW_CONNECTION     22
#define IDI_TROUBLESHOOT       27
#define IDI_ETHERNET           28
#define IDI_SIGNAL0            14   /* ... 19: the bars of Wi-Fi */
#define IDI_ACTIVITY           40

/* a connection: an Ethernet or Wi-Fi interface of the host */
struct connection
{
    WCHAR ifname[32];        /* the host's name for it: enp3s0, wlan0 */
    char ifname_a[32];
    WCHAR guid[40];          /* the adapter's, as GetAdaptersAddresses gives it */
    WCHAR name[128];         /* "Бездротове мережне підключення" or what the user renamed it to */
    WCHAR adapter[128];      /* "Intel Wi-Fi 6 AX201" */
    BOOL wireless;
    BOOL up;                 /* media connected */
    BOOL enabled;
    BOOL ipv4, ipv6;         /* has a usable address */
    BOOL gateway4, gateway6;
    WCHAR ssid[64];
    int signal;              /* 0-100 */
    ULONG64 speed;           /* bits per second */
    ULONG luid_index;
    ULONG64 luid;            /* NET_LUID */
};

UINT connections_list( struct connection *list, UINT max );
BOOL connection_find( const WCHAR *id, struct connection *conn );   /* by GUID or host name */
BOOL internet_reachable(void);

/* the TCP/IP settings of a connection (settings.c) */
#define MAX_ADDRESSES 4
struct settings
{
    BOOL enabled;
    BOOL ipv4_on, ipv6_on;               /* the components ticked */
    BOOL static4;
    WCHAR addr4[MAX_ADDRESSES][16], mask4[MAX_ADDRESSES][16];
    UINT n4;
    WCHAR gw4[16];
    BOOL static_dns4;
    WCHAR dns4[MAX_ADDRESSES][16];
    UINT ndns4;
    BOOL static6;
    WCHAR addr6[MAX_ADDRESSES][48];
    UINT prefix6[MAX_ADDRESSES];
    UINT n6;
    WCHAR gw6[48];
    BOOL static_dns6;
    WCHAR dns6[MAX_ADDRESSES][48];
    UINT ndns6;
    /* Alternate configuration and WINS: kept as Windows keeps them */
    BOOL alt_user;
    WCHAR alt_ip[16], alt_mask[16], alt_gw[16], alt_dns1[16], alt_dns2[16];
    UINT netbios;                         /* 0 default, 1 on, 2 off */
};

void settings_load( const struct connection *conn, struct settings *s );
BOOL settings_save( HWND owner, const struct connection *conn, const struct settings *s );

/* the dialogs */
void show_status( HWND owner, const struct connection *conn );
void show_properties( HWND owner, const struct connection *conn );
BOOL show_ipv4( HWND owner, const struct connection *conn, struct settings *s );
BOOL show_ipv6( HWND owner, const struct connection *conn, struct settings *s );
void show_details( HWND owner, const struct connection *conn );

/* the host (unix.c through unixlib.h) */
BOOL host_read_settings( const char *ifname, char *text, UINT size );
BOOL host_write_settings( const char *ifname, const char *text );
BOOL host_link_info( const char *ifname, WCHAR *adapter, UINT count, ULONG64 *speed, ULONG *connected_for );
BOOL host_iwd_addresses( const char *ifname, const char *ipv4, const char *ipv6 );
BOOL host_name_servers( const char *ifname, WCHAR *servers, UINT count );

/* commctrl's macros take SendMessage without W, which Wine's modules have not */
#define prop_changed( dlg ) SendMessageW( GetParent( dlg ), PSM_CHANGED, (WPARAM)(dlg), 0 )
#define prop_press( dlg, button ) PostMessageW( GetParent( dlg ), PSM_PRESSBUTTON, (button), 0 )
static inline void lv_set_state( HWND list, int index, UINT state, UINT mask )
{
    LVITEMW item;
    item.state = state;
    item.stateMask = mask;
    SendMessageW( list, LVM_SETITEMSTATE, index, (LPARAM)&item );
}
#define lv_set_check( list, index, on ) lv_set_state( list, index, INDEXTOSTATEIMAGEMASK( (on) ? 2 : 1 ), LVIS_STATEIMAGEMASK )
#define lv_get_check( list, index ) \
    ((((UINT)SendMessageW( list, LVM_GETITEMSTATE, index, LVIS_STATEIMAGEMASK )) >> 12) == 2)

/* helpers */
BOOL valid_ipv4( const WCHAR *text );
BOOL valid_ipv6( const WCHAR *text );
UINT mask_to_prefix( const WCHAR *mask );
void prefix_to_mask( UINT prefix, WCHAR *mask, size_t count );
void ipaddr_get( HWND dlg, UINT id, WCHAR *text, size_t count );
void ipaddr_set( HWND dlg, UINT id, const WCHAR *text );
HICON load_icon( UINT id, int size );
void center_page( struct view *view );   /* page_center.c */
void center_timer( struct view *view );
void sharing_page( struct view *view );  /* page_sharing.c */
BOOL sharing_command( struct view *view, UINT id, UINT code, HWND control );
void open_adapter_settings(void);

#endif

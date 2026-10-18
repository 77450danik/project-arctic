/*
 * Task Manager: resource identifiers
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define IDI_TASKMGR             1

#define IDR_MENU                100
#define IDR_POPUP               101    /* context menus, one submenu each */
#define IDR_ACCEL               102

/* menu commands */
#define IDM_RUN                 200
#define IDM_EXIT                201
#define IDM_ALWAYS_ON_TOP       210
#define IDM_MINIMIZE_ON_USE     211
#define IDM_HIDE_WHEN_MIN       212
#define IDM_REFRESH             220
#define IDM_SPEED_HIGH          221
#define IDM_SPEED_NORMAL        222
#define IDM_SPEED_LOW           223
#define IDM_SPEED_PAUSED        224
#define IDM_GROUP_BY_TYPE       225
#define IDM_EXPAND_ALL          226
#define IDM_COLLAPSE_ALL        227
#define IDM_END_TASK            230
#define IDM_END_TREE            231
#define IDM_SWITCH_TO           232
#define IDM_OPEN_LOCATION       233
#define IDM_GO_TO_DETAILS       234
#define IDM_PROPERTIES          235
#define IDM_SEARCH_ONLINE       236
#define IDM_PRIO_REALTIME       240
#define IDM_PRIO_HIGH           241
#define IDM_PRIO_ABOVE          242
#define IDM_PRIO_NORMAL         243
#define IDM_PRIO_BELOW          244
#define IDM_PRIO_IDLE           245
#define IDM_SERVICE_START       250
#define IDM_SERVICE_STOP        251
#define IDM_SERVICE_RESTART     252
#define IDM_OPEN_SERVICES       253
#define IDM_STARTUP_TOGGLE      260
#define IDM_COPY                270
#define IDM_PERF_SUMMARY        271
#define IDM_CPU_OVERALL         272
#define IDM_CPU_LOGICAL         273

/* controls */
#define IDC_TABS                300
#define IDC_END_TASK            301
#define IDC_LESS_DETAILS        302
#define IDC_GRID                303
#define IDC_COMPACT_LIST        304
#define IDC_PERF_NAV            305
#define IDC_OPEN_SERVICES       306
#define IDC_RUN_EDIT            310
#define IDC_RUN_ADMIN           311
#define IDC_RUN_BROWSE          312

#define IDD_RUN                 400

/* strings */
#define IDS_APP_TITLE           1000
#define IDS_TAB_PROCESSES       1001
#define IDS_TAB_PERFORMANCE     1002
#define IDS_TAB_HISTORY         1003
#define IDS_TAB_STARTUP         1004
#define IDS_TAB_USERS           1005
#define IDS_TAB_DETAILS         1006
#define IDS_TAB_SERVICES        1007
#define IDS_END_TASK            1010
#define IDS_RESTART             1011
#define IDS_LESS_DETAILS        1012
#define IDS_MORE_DETAILS        1013
#define IDS_ENABLE              1014
#define IDS_DISABLE             1015
#define IDS_DISCONNECT          1016
#define IDS_OPEN_SERVICES       1017

#define IDS_COL_NAME            1020
#define IDS_COL_STATUS          1021
#define IDS_COL_CPU             1022
#define IDS_COL_MEMORY          1023
#define IDS_COL_DISK            1024
#define IDS_COL_NETWORK         1025
#define IDS_COL_PID             1026
#define IDS_COL_USER            1027
#define IDS_COL_DESCRIPTION     1028
#define IDS_COL_THREADS         1029
#define IDS_COL_HANDLES         1030
#define IDS_COL_PRIORITY        1031
#define IDS_COL_GROUP           1032
#define IDS_COL_PUBLISHER       1033
#define IDS_COL_STARTUP_STATE   1034
#define IDS_COL_STARTUP_IMPACT  1035
#define IDS_COL_CPU_TIME        1036
#define IDS_COL_ID              1037
#define IDS_COL_SESSION         1038
#define IDS_COL_COMMAND         1039
#define IDS_COL_MEM_COMMIT      1040
#define IDS_COL_DISK_IO         1041
#define IDS_COL_STARTUP_TYPE    1042

#define IDS_GROUP_APPS          1050
#define IDS_GROUP_BACKGROUND    1051
#define IDS_GROUP_WINDOWS       1052
#define IDS_SUSPENDED           1053
#define IDS_RUNNING             1054
#define IDS_STOPPED             1055
#define IDS_STARTING            1056
#define IDS_STOPPING            1057
#define IDS_ENABLED             1058
#define IDS_DISABLED            1059
#define IDS_IMPACT_NOT_MEASURED 1060
#define IDS_IMPACT_LOW          1061
#define IDS_IMPACT_MEDIUM       1062
#define IDS_IMPACT_HIGH         1063
#define IDS_ACTIVE              1064
#define IDS_AUTO                1065
#define IDS_MANUAL              1066
#define IDS_PRIO_REALTIME       1067
#define IDS_PRIO_HIGH           1068
#define IDS_PRIO_ABOVE          1069
#define IDS_PRIO_NORMAL         1070
#define IDS_PRIO_BELOW          1071
#define IDS_PRIO_IDLE           1072

#define IDS_PERF_CPU            1100
#define IDS_PERF_MEMORY         1101
#define IDS_PERF_DISK           1102
#define IDS_PERF_ETHERNET       1103
#define IDS_PERF_WIFI           1104
#define IDS_PERF_UTIL           1105    /* "% використання" */
#define IDS_PERF_60S            1106
#define IDS_PERF_UTILIZATION    1107
#define IDS_PERF_SPEED          1108
#define IDS_PERF_PROCESSES      1109
#define IDS_PERF_THREADS        1110
#define IDS_PERF_HANDLES        1111
#define IDS_PERF_UPTIME         1112
#define IDS_PERF_BASE_SPEED     1113
#define IDS_PERF_SOCKETS        1114
#define IDS_PERF_CORES          1115
#define IDS_PERF_LOGICAL        1116
#define IDS_PERF_VIRTUALIZATION 1117
#define IDS_PERF_L1             1118
#define IDS_PERF_L2             1119
#define IDS_PERF_L3             1120
#define IDS_PERF_ENABLED        1121
#define IDS_PERF_NOT_SUPPORTED  1122
#define IDS_PERF_MEM_USAGE      1123
#define IDS_PERF_MEM_COMPOSITION 1124
#define IDS_PERF_IN_USE         1125
#define IDS_PERF_AVAILABLE      1126
#define IDS_PERF_COMMITTED      1127
#define IDS_PERF_CACHED         1128
#define IDS_PERF_PAGED          1129
#define IDS_PERF_NONPAGED       1130
#define IDS_PERF_MEM_SPEED      1131
#define IDS_PERF_SLOTS          1132
#define IDS_PERF_FORM           1133
#define IDS_PERF_HW_RESERVED    1134
#define IDS_PERF_ACTIVE_TIME    1135
#define IDS_PERF_TRANSFER       1136
#define IDS_PERF_READ_SPEED     1137
#define IDS_PERF_WRITE_SPEED    1138
#define IDS_PERF_CAPACITY       1139
#define IDS_PERF_FORMATTED      1140
#define IDS_PERF_SYSTEM_DISK    1141
#define IDS_PERF_PAGE_FILE      1142
#define IDS_PERF_YES            1143
#define IDS_PERF_NO             1144
#define IDS_PERF_THROUGHPUT     1145
#define IDS_PERF_SEND           1146
#define IDS_PERF_RECEIVE        1147
#define IDS_PERF_ADAPTER        1148
#define IDS_PERF_CONN_TYPE      1149
#define IDS_PERF_IPV4           1150
#define IDS_PERF_IPV6           1151
#define IDS_PERF_OF_SLOTS       1152    /* "%u з %u" */
#define IDS_PERF_MEM_MAX        1153
#define IDS_PERF_NOT_CONNECTED  1154
#define IDS_PERF_RESPONSE       1155
#define IDS_PERF_TYPE           1156
#define IDS_PERF_SSD            1157
#define IDS_PERF_HDD            1158
#define IDS_PERF_REMOVABLE      1159
#define IDS_PERF_GPU            1160
#define IDS_PERF_GPU_MEM        1161
#define IDS_PERF_GPU_MEM_USAGE  1162
#define IDS_PERF_GPU_TEMP       1163
#define IDS_PERF_DRIVER         1164
#define IDS_PERF_LOCATION       1165
#define IDS_PERF_PCI            1166    /* "PCI-шина %u, пристрій %u, функція %u" */
#define IDS_PERF_GPU_SHORT      1167

#define IDS_UNIT_B              1200
#define IDS_UNIT_KB             1201
#define IDS_UNIT_MB             1202
#define IDS_UNIT_GB             1203
#define IDS_UNIT_TB             1204
#define IDS_UNIT_PER_S          1205    /* "%s/с" */
#define IDS_UNIT_KBPS           1206
#define IDS_UNIT_MBPS           1207
#define IDS_UNIT_GBPS           1208
#define IDS_UNIT_GHZ            1209
#define IDS_UNIT_MHZ            1210
#define IDS_UNIT_MS             1211

#define IDS_CONFIRM_END_TITLE   1300
#define IDS_CONFIRM_END_TEXT    1301
#define IDS_CONFIRM_END_SYSTEM  1302
#define IDS_END_FAILED          1303
#define IDS_RUN_TITLE           1304
#define IDS_USER_NAME_SESSION   1305
#define IDS_EXPLORER            1306
#define IDS_FORM_OTHER          1307
#define IDS_COMPACT_EMPTY       1308
#define IDS_RUN_TEXT            1309
#define IDS_EXPAND              1310
#define IDS_COLLAPSE            1311

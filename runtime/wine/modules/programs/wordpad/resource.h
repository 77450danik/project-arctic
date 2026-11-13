/*
 * Arctic WordPad resources
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define IDI_WORDPAD             1
#define IDI_DOCUMENT            2
#define IDI_RTF                 3
#define IDR_ICONS               100
#define IDA_ACCEL               101

/* commands */
#define IDM_NEW                 1000
#define IDM_OPEN                1001
#define IDM_SAVE                1002
#define IDM_SAVEAS              1003
#define IDM_PRINT               1004
#define IDM_QUICKPRINT          1005
#define IDM_PAGESETUP           1006
#define IDM_CLOSE               1007
#define IDM_EXIT                1008
#define IDM_FILE                1009
#define IDM_ABOUT               1010
#define IDM_INFO                1011
#define IDM_EXPORT_PDF          1012

#define IDM_UNDO                1100
#define IDM_REDO                1101
#define IDM_CUT                 1102
#define IDM_COPY                1103
#define IDM_PASTE               1104
#define IDM_PASTE_MENU          1105
#define IDM_PASTE_TEXT          1106
#define IDM_FORMATPAINTER       1107
#define IDM_SELECTALL           1108
#define IDM_FIND                1109
#define IDM_REPLACE             1110
#define IDM_SELECT_MENU         1111
#define IDM_FINDNEXT            1112
#define IDM_SEARCHBOX           1113

#define IDM_FONTFACE            1200
#define IDM_FONTSIZE            1201
#define IDM_GROW                1202
#define IDM_SHRINK              1203
#define IDM_CASE                1204
#define IDM_CASE_SENTENCE       1205
#define IDM_CASE_LOWER          1206
#define IDM_CASE_UPPER          1207
#define IDM_CASE_TITLE          1208
#define IDM_CASE_TOGGLE         1209
#define IDM_CLEARFORMAT         1210
#define IDM_BOLD                1211
#define IDM_ITALIC              1212
#define IDM_UNDERLINE           1213
#define IDM_UNDERLINE_MENU      1214
#define IDM_UL_SINGLE           1215
#define IDM_UL_DOUBLE           1216
#define IDM_UL_DOTTED           1217
#define IDM_UL_WORD             1218
#define IDM_STRIKE              1219
#define IDM_SUBSCRIPT           1220
#define IDM_SUPERSCRIPT         1221
#define IDM_HIGHLIGHT           1222
#define IDM_HIGHLIGHT_MENU      1223
#define IDM_FONTCOLOR           1224
#define IDM_FONTCOLOR_MENU      1225
#define IDM_FONTDIALOG          1226
#define IDM_TEXTEFFECTS         1227
#define IDM_FX_SMALLCAPS        1228
#define IDM_FX_ALLCAPS          1229
#define IDM_FX_HIDDEN           1230

#define IDM_BULLETS             1300
#define IDM_BULLETS_MENU        1301
#define IDM_NUMBERING           1302
#define IDM_NUMBERING_MENU      1303
#define IDM_OUTDENT             1304
#define IDM_INDENT              1305
#define IDM_SORT                1306
#define IDM_ALIGN_LEFT          1307
#define IDM_ALIGN_CENTER        1308
#define IDM_ALIGN_RIGHT         1309
#define IDM_ALIGN_JUSTIFY       1310
#define IDM_LINESPACING         1311
#define IDM_LS_100              1312
#define IDM_LS_115              1313
#define IDM_LS_150              1314
#define IDM_LS_200              1315
#define IDM_LS_250              1316
#define IDM_LS_300              1317
#define IDM_LS_BEFORE           1318
#define IDM_LS_AFTER            1319
#define IDM_PARADIALOG          1320
#define IDM_BULLET_DOT          1321
#define IDM_BULLET_CIRCLE       1322
#define IDM_BULLET_SQUARE       1323
#define IDM_BULLET_DASH         1324
#define IDM_BULLET_NONE         1325
#define IDM_NUM_ARABIC          1326
#define IDM_NUM_PAREN           1327
#define IDM_NUM_UCLETTER        1328
#define IDM_NUM_LCLETTER        1329
#define IDM_NUM_UCROMAN         1330
#define IDM_NUM_LCROMAN         1331
#define IDM_NUM_NONE            1332
#define IDM_MULTILEVEL          1333
#define IDM_SHADING             1334

#define IDM_STYLE_FIRST         1400   /* + the style's index */
#define IDM_STYLES_UP           1420
#define IDM_STYLES_DOWN         1421

#define IDM_PAGEBREAK           1500
#define IDM_TABLE               1501
#define IDM_PICTURE             1502
#define IDM_LINK                1503
#define IDM_DATETIME            1504
#define IDM_SYMBOL              1505
#define IDM_TABLE_PICK          1506

#define IDM_MARGINS             1600
#define IDM_MARGIN_NORMAL       1601
#define IDM_MARGIN_NARROW       1602
#define IDM_MARGIN_MODERATE     1603
#define IDM_MARGIN_WIDE         1604
#define IDM_ORIENTATION         1605
#define IDM_PORTRAIT            1606
#define IDM_LANDSCAPE           1607
#define IDM_SIZE                1608
#define IDM_SIZE_A4             1609
#define IDM_SIZE_A5             1610
#define IDM_SIZE_LETTER         1611
#define IDM_SIZE_LEGAL          1612
#define IDM_SIZE_A3             1613
#define IDM_INDENT_LEFT         1620
#define IDM_INDENT_RIGHT        1621
#define IDM_SPACE_BEFORE        1622
#define IDM_SPACE_AFTER         1623

#define IDM_VIEW_PRINT          1700
#define IDM_VIEW_WEB            1701
#define IDM_ZOOM                1702
#define IDM_ZOOM_100            1703
#define IDM_ONEPAGE             1704
#define IDM_MULTIPAGE           1705
#define IDM_PAGEWIDTH           1706
#define IDM_ZOOM_IN             1707
#define IDM_ZOOM_OUT            1708
#define IDM_WORDCOUNT           1709

#define IDM_RECENT_FIRST        1800   /* + the file's index */

/* strings */
#define IDS_APP_NAME            100
#define IDS_DEFAULT_NAME        101
#define IDS_SAVED               102
#define IDS_SEARCH              103
#define IDS_SAVE_CHANGES        104
#define IDS_SAVE                105
#define IDS_DONT_SAVE           106
#define IDS_CANCEL              107
#define IDS_FILTER_OPEN         108
#define IDS_FILTER_SAVE         109
#define IDS_CANT_OPEN           110
#define IDS_CANT_SAVE           111
#define IDS_PAGE_OF             112
#define IDS_WORDS               113
#define IDS_WORD_ONE            114
#define IDS_NOT_FOUND           115
#define IDS_REPLACED            116
#define IDS_DOC_FORMAT_LOSS     117
#define IDS_PRINTING            118
#define IDS_MINIMIZE            119
#define IDS_MAXIMIZE            120
#define IDS_RESTORE             121
#define IDS_CLOSE_WINDOW        122
#define IDS_AUTOMATIC           123
#define IDS_NO_COLOR            124
#define IDS_THEME_COLORS        125
#define IDS_STANDARD_COLORS     126
#define IDS_MORE_COLORS         127
#define IDS_TABLE_SIZE          128
#define IDS_INSERT_TABLE        129
#define IDS_ZOOM_PERCENT        130
#define IDS_CHARS               131
#define IDS_PARAGRAPHS          132
#define IDS_LINES               133
#define IDS_PAGES               134
#define IDS_WORDCOUNT_TITLE     135
#define IDS_PICTURE_FILTER      136
#define IDS_ABOUT_TEXT          137
#define IDS_RECENT              138
#define IDS_BLANK_DOCUMENT      139
#define IDS_BROWSE              140
#define IDS_NO_RECENT           141
#define IDS_INFO_LOCATION       142
#define IDS_INFO_SIZE           143
#define IDS_INFO_MODIFIED       144
#define IDS_INFO_STATS          145
#define IDS_PRINT_COPIES        146
#define IDS_PRINTER             147
#define IDS_ODT_READONLY        148
#define IDS_NOT_SAVED           149
#define IDS_READ_ONLY_FORMAT    150
#define IDS_GOOD_MORNING        151
#define IDS_GOOD_DAY            152
#define IDS_GOOD_EVENING        153
#define IDS_DOC_UNSUPPORTED     154
#define IDS_UNITS_CM            155
#define IDS_UNITS_PT            156
#define IDS_LEFT                157
#define IDS_RIGHT               158
#define IDS_BEFORE              159
#define IDS_AFTER               160
#define IDS_FORMAT_DOCX         161
#define IDS_FORMAT_RTF          162
#define IDS_FORMAT_TXT          163
#define IDS_FORMAT_ODT          164

/* tabs */
#define IDS_TAB_FILE            200
#define IDS_TAB_HOME            201
#define IDS_TAB_INSERT          202
#define IDS_TAB_LAYOUT          203
#define IDS_TAB_VIEW            204

/* groups */
#define IDS_G_UNDO              220
#define IDS_G_CLIPBOARD         221
#define IDS_G_FONT              222
#define IDS_G_PARAGRAPH         223
#define IDS_G_STYLES            224
#define IDS_G_EDITING           225
#define IDS_G_PAGES             226
#define IDS_G_TABLES            227
#define IDS_G_ILLUSTRATIONS     228
#define IDS_G_LINKS             229
#define IDS_G_TEXT              230
#define IDS_G_SYMBOLS           231
#define IDS_G_PAGESETUP         232
#define IDS_G_INDENT            233
#define IDS_G_SPACING           234
#define IDS_G_VIEWS             235
#define IDS_G_ZOOM              236
#define IDS_G_PROOFING          237

/* the labels and tips of the commands: 1000 + the command - 1000 + 2000 */
#define IDS_CMD(id)             (2000 + (id) - 1000)
#define IDS_TIP(id)             (4000 + (id) - 1000)

/* styles */
#define IDS_STYLE_NORMAL        300
#define IDS_STYLE_NOSPACING     301
#define IDS_STYLE_H1            302
#define IDS_STYLE_H2            303
#define IDS_STYLE_H3            304
#define IDS_STYLE_TITLE         305
#define IDS_STYLE_SUBTITLE      306
#define IDS_STYLE_QUOTE         307
#define IDS_STYLE_LIST          308

/* the File page */
#define IDS_BS_BACK             320
#define IDS_BS_HOME             321
#define IDS_BS_NEW              322
#define IDS_BS_OPEN             323
#define IDS_BS_INFO             324
#define IDS_BS_SAVE             325
#define IDS_BS_SAVEAS           326
#define IDS_BS_PRINT            327
#define IDS_BS_CLOSE            328
#define IDS_BS_ABOUT            329

/* dialogs */
#define IDD_PARAGRAPH           500
#define IDC_PARA_ALIGN          501
#define IDC_PARA_LEFT           502
#define IDC_PARA_RIGHT          503
#define IDC_PARA_SPECIAL        504
#define IDC_PARA_BY             505
#define IDC_PARA_BEFORE         506
#define IDC_PARA_AFTER          507
#define IDC_PARA_LINE           508
#define IDC_PARA_AT             509
#define IDC_PARA_PAGEBREAK      510
#define IDC_PARA_KEEPNEXT       511

#define IDD_LINK                520
#define IDC_LINK_TEXT           521
#define IDC_LINK_URL            522

#define IDD_DATETIME            530
#define IDC_DATETIME_LIST       531

#define IDD_ZOOM                540
#define IDC_ZOOM_200            541
#define IDC_ZOOM_100            542
#define IDC_ZOOM_75             543
#define IDC_ZOOM_WIDTH          544
#define IDC_ZOOM_WHOLE          545
#define IDC_ZOOM_PERCENT        546
#define IDC_ZOOM_SPIN           547

#define IDD_WORDCOUNT           550
#define IDC_WC_LIST             551

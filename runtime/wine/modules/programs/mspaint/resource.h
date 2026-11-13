/*
 * Paint: resources and commands
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define IDI_PAINT           1
#define IDR_ICONS           100
#define IDA_ACCEL           101
#define IDD_RESIZE          200
#define IDD_PROPERTIES      201

/* the resize dialog */
#define IDC_BY_PERCENT      1001
#define IDC_BY_PIXELS       1002
#define IDC_HORZ            1003
#define IDC_VERT            1004
#define IDC_KEEP_RATIO      1005
#define IDC_SKEW_H          1006
#define IDC_SKEW_V          1007
#define IDC_WIDTH           1010
#define IDC_HEIGHT          1011

/* the commands */
#define IDM_NEW             2001
#define IDM_OPEN            2002
#define IDM_SAVE            2003
#define IDM_SAVEAS          2004
#define IDM_SAVE_PNG        2005
#define IDM_SAVE_JPEG       2006
#define IDM_SAVE_BMP        2007
#define IDM_SAVE_GIF        2008
#define IDM_IMPORT          2009
#define IDM_PRINT           2010
#define IDM_WALL_FILL       2011
#define IDM_WALL_TILE       2012
#define IDM_WALL_CENTER     2013
#define IDM_PROPERTIES      2014
#define IDM_ABOUT           2015
#define IDM_EXIT            2016
#define IDM_RECENT_FIRST    2020    /* up to 2029 */

#define IDM_UNDO            2101
#define IDM_REDO            2102
#define IDM_CUT             2103
#define IDM_COPY            2104
#define IDM_PASTE           2105
#define IDM_PASTE_FROM      2106
#define IDM_SELECT_ALL      2107
#define IDM_INVERT_SEL      2108
#define IDM_DELETE          2109
#define IDM_TRANSPARENT     2110
#define IDM_SEL_RECT        2111
#define IDM_SEL_FREE        2112
#define IDM_CROP            2113
#define IDM_RESIZE          2114
#define IDM_ROTATE_RIGHT    2115
#define IDM_ROTATE_LEFT     2116
#define IDM_ROTATE_180      2117
#define IDM_FLIP_V          2118
#define IDM_FLIP_H          2119
#define IDM_ESCAPE          2120

#define IDM_ZOOM_IN         2201
#define IDM_ZOOM_OUT        2202
#define IDM_ZOOM_100        2203
#define IDM_ZOOM_FIT        2204
#define IDM_GRID            2205
#define IDM_STATUSBAR       2206
#define IDM_FULLSCREEN      2207
#define IDM_THEME_LIGHT     2208
#define IDM_THEME_DARK      2209
#define IDM_THEME_SYSTEM    2210

#define IDM_TOOL_FIRST      2300    /* + enum tool */
#define IDM_BRUSH_FIRST     2320    /* + enum brush_kind */
#define IDM_SHAPE_FIRST     2340    /* + enum shape_kind */
#define IDM_SIZE_FIRST      2380    /* + the index of a thickness */
#define IDM_OUTLINE_NONE    2390
#define IDM_OUTLINE_SOLID   2391
#define IDM_FILL_NONE       2392
#define IDM_FILL_SOLID      2393
#define IDM_EDIT_COLORS     2394
#define IDM_SWAP_COLORS     2395

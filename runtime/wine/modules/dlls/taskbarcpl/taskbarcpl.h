/*
 * Taskbar and Start Menu of the Control Panel (taskbarcpl.dll)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __TASKBARCPL_H
#define __TASKBARCPL_H

#define IDS_NAME                1
#define IDS_TIP                 2
#define IDS_TASKBAR             10
#define IDS_LOOK                11
#define IDS_BUTTONS             12
#define IDS_START               13
#define IDS_NOTIFY              14
#define IDS_NO_TASKBAR          20
#define IDS_PERSONALIZE         21
#define IDS_DISPLAY             22
#define IDS_RELATED             23
#define IDS_TASKBAR_HINT        24
#define IDS_LOOK_HINT           25
#define IDS_BUTTONS_HINT        26
#define IDS_START_HINT          27
#define IDS_NOTIFY_HINT         28

/* the taskbar */
#define IDS_BEHAVIOR            30
#define IDS_LOCK                31
#define IDS_AUTOHIDE            32
#define IDS_ONTOP               33
#define IDS_SMALL               34
#define IDS_COMBINE             35
#define IDS_GLOM_ALWAYS         36
#define IDS_GLOM_FULL           37
#define IDS_GLOM_NEVER          38

/* the look */
#define IDS_PRESET              40
#define IDS_PRESET_HINT         41
#define IDS_LOOK_DETAILS        42
#define IDS_EFFECT              43
#define IDS_COLOR               44
#define IDS_OPACITY             45
#define IDS_BLUR                46
#define IDS_TEXT                47
#define IDS_PERCENT             48
#define IDS_NUMBER              49

/* the buttons */
#define IDS_BUTTON_STYLE        50
#define IDS_ALIGNMENT           51
#define IDS_ALIGN_LEFT          52
#define IDS_ALIGN_CENTER        53
#define IDS_START_BUTTON        54
#define IDS_START_STANDARD      55
#define IDS_START_IMAGE         56
#define IDS_START_PICTURE       57
#define IDS_BROWSE              58
#define IDS_WIDGETS             59
#define IDS_WIDGETS_BOTH        60
#define IDS_WIDGETS_ICON        61
#define IDS_WIDGETS_OFF         62
#define IDS_IMAGES              63

/* Start */
#define IDS_START_STYLE         70
#define IDS_STYLE_STANDARD      71
#define IDS_STYLE_WIN10         72
#define IDS_STYLE_WIN11         73
#define IDS_STYLE_CLASSIC       74
#define IDS_START_CONTENT       75
#define IDS_START_CONTENT_HINT  76
#define IDS_CUSTOMIZE           77
#define IDS_MENU_LOOK           78
#define IDS_INHERIT             79
#define IDS_INHERIT_HINT        80

/* the notification area */
#define IDS_CLOCK               85
#define IDS_SECONDS             86
#define IDS_DESKTOP_BUTTON      87
#define IDS_DESKTOP_HINT        88

/* the cards' pictures: Windows 11's, the icon theme's when it has them
 * (runtime/icons/We10X/taskbarcpl, tools/make-we10x-icons.py) */
#define ICON_TASKBAR            101
#define ICON_LOOK               102
#define ICON_BUTTONS            103
#define ICON_START              104
#define ICON_NOTIFY             105
#define ICON_LOCK               106
#define ICON_AUTOHIDE           107
#define ICON_SMALL_BUTTONS              108
#define ICON_COMBINE            109
#define ICON_CLOCK              110
#define ICON_DESKTOP            111
#define ICON_WIDGETS            112
#define ICON_ONTOP              113
#define ICON_EFFECT             114
#define ICON_PRESET             115

#endif

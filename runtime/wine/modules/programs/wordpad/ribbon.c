/*
 * Arctic WordPad: the ribbon
 *
 * Word's ribbon of Windows 11: the tabs on the colour of the title bar, the
 * chosen one underlined; under them a white card of groups, each with its
 * name at the bottom. Everything is drawn here; the fields of the font and
 * its size take typing in an edit control laid over them.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdio.h>
#include "wordpad.h"

enum kind { C_BIG, C_ICON, C_LABEL, C_LETTER, C_FONT, C_SIZE, C_GALLERY, C_SPIN, C_GAP, C_CHECK };

#define R2      0x01        /* the second row of a group in rows */
#define DROP    0x02        /* opens a menu: an arrow after it */
#define SPLIT   0x04        /* a button and, beside it, the arrow of its menu (drop = the menu's command) */
#define BAR     0x08        /* a bar of colour under the picture */
#define NEWCOL  0x10        /* stacked: a new column */
#define R3      0x20        /* the third row */

struct ctl
{
    int kind;
    int cmd;
    const char *icon;
    const WCHAR *letter;
    int font;               /* of the letter */
    int flags;
    int drop;               /* SPLIT: the command of the arrow */
    int width;              /* fields */
    RECT rc, drop_rc;
};

enum layout { L_STACK, L_ROWS };

struct group
{
    UINT label;
    int launcher;
    enum layout layout;
    struct ctl *ctls;
    int count;
    RECT rc;
    BOOL collapsed;         /* no room: one button whose menu has the group's commands */
};

#define CTLS(a) a, ARRAY_SIZE(a)

/* Home */
static struct ctl undo_ctls[] =
{
    { C_LABEL, IDM_UNDO, "undo" },
    { C_LABEL, IDM_REDO, "redo" },
};
static struct ctl clip_ctls[] =
{
    { C_BIG, IDM_PASTE, "paste", NULL, 0, SPLIT, IDM_PASTE_MENU },
    { C_LABEL, IDM_CUT, "cut" },
    { C_LABEL, IDM_COPY, "copy" },
    { C_LABEL, IDM_FORMATPAINTER, "formatpainter" },
};
static struct ctl font_ctls[] =
{
    { C_FONT, IDM_FONTFACE, NULL, NULL, 0, 0, 0, 128 },
    { C_SIZE, IDM_FONTSIZE, NULL, NULL, 0, 0, 0, 46 },
    { C_ICON, IDM_GROW, "growfont" },
    { C_ICON, IDM_SHRINK, "shrinkfont" },
    { C_GAP },
    { C_LETTER, IDM_CASE, NULL, L"Aa", F_LETTER, DROP },
    { C_ICON, IDM_CLEARFORMAT, "clearformat" },
    { C_LETTER, IDM_BOLD, NULL, L"B", F_LETTER_BOLD, R2 },
    { C_LETTER, IDM_ITALIC, NULL, L"I", F_LETTER_ITALIC, R2 },
    { C_LETTER, IDM_UNDERLINE, NULL, L"U", F_LETTER_UNDER, R2 | SPLIT, IDM_UNDERLINE_MENU },
    { C_LETTER, IDM_STRIKE, NULL, L"ab", F_LETTER_STRIKE, R2 },
    { C_LETTER, IDM_SUBSCRIPT, NULL, L"x\x2082", F_LETTER, R2 },
    { C_LETTER, IDM_SUPERSCRIPT, NULL, L"x\x00b2", F_LETTER, R2 },
    { C_GAP, 0, NULL, NULL, 0, R2 },
    { C_ICON, IDM_TEXTEFFECTS, "texteffects", NULL, 0, R2 | DROP },
    { C_ICON, IDM_HIGHLIGHT, "highlight", NULL, 0, R2 | SPLIT | BAR, IDM_HIGHLIGHT_MENU },
    { C_ICON, IDM_FONTCOLOR, "fontcolor", NULL, 0, R2 | SPLIT | BAR, IDM_FONTCOLOR_MENU },
};
static struct ctl para_ctls[] =
{
    { C_ICON, IDM_BULLETS, "bullets", NULL, 0, SPLIT, IDM_BULLETS_MENU },
    { C_ICON, IDM_NUMBERING, "numbering", NULL, 0, SPLIT, IDM_NUMBERING_MENU },
    { C_ICON, IDM_MULTILEVEL, "multilevel", NULL, 0, DROP },
    { C_GAP },
    { C_ICON, IDM_OUTDENT, "outdent" },
    { C_ICON, IDM_INDENT, "indent" },
    { C_GAP },
    { C_ICON, IDM_SORT, "sort" },
    { C_ICON, IDM_ALIGN_LEFT, "alignleft", NULL, 0, R2 },
    { C_ICON, IDM_ALIGN_CENTER, "aligncenter", NULL, 0, R2 },
    { C_ICON, IDM_ALIGN_RIGHT, "alignright", NULL, 0, R2 },
    { C_ICON, IDM_ALIGN_JUSTIFY, "justify", NULL, 0, R2 },
    { C_GAP, 0, NULL, NULL, 0, R2 },
    { C_ICON, IDM_LINESPACING, "linespacing", NULL, 0, R2 | DROP },
    { C_GAP, 0, NULL, NULL, 0, R2 },
    { C_ICON, IDM_SHADING, "shading", NULL, 0, R2 | SPLIT | BAR, IDM_SHADING },
};
static struct ctl style_ctls[] =
{
    { C_GALLERY, IDM_STYLE_FIRST },
};
static struct ctl edit_ctls[] =
{
    { C_LABEL, IDM_FIND, "search" },
    { C_LABEL, IDM_REPLACE, "replace" },
    { C_LABEL, IDM_SELECT_MENU, "select", NULL, 0, DROP },
};

static struct group home_groups[] =
{
    { IDS_G_UNDO, 0, L_STACK, CTLS(undo_ctls) },
    { IDS_G_CLIPBOARD, 0, L_STACK, CTLS(clip_ctls) },
    { IDS_G_FONT, IDM_FONTDIALOG, L_ROWS, CTLS(font_ctls) },
    { IDS_G_PARAGRAPH, IDM_PARADIALOG, L_ROWS, CTLS(para_ctls) },
    { IDS_G_STYLES, 0, L_STACK, CTLS(style_ctls) },
    { IDS_G_EDITING, 0, L_STACK, CTLS(edit_ctls) },
};

/* Insert */
static struct ctl pages_ctls[] = { { C_BIG, IDM_PAGEBREAK, "pagebreak" } };
static struct ctl table_ctls[] = { { C_BIG, IDM_TABLE, "table", NULL, 0, DROP } };
static struct ctl illus_ctls[] = { { C_BIG, IDM_PICTURE, "picture" } };
static struct ctl link_ctls[] = { { C_BIG, IDM_LINK, "link" } };
static struct ctl text_ctls[] = { { C_BIG, IDM_DATETIME, "datetime" } };
static struct ctl symbol_ctls[] = { { C_BIG, IDM_SYMBOL, "symbol", NULL, 0, DROP } };

static struct group insert_groups[] =
{
    { IDS_G_PAGES, 0, L_STACK, CTLS(pages_ctls) },
    { IDS_G_TABLES, 0, L_STACK, CTLS(table_ctls) },
    { IDS_G_ILLUSTRATIONS, 0, L_STACK, CTLS(illus_ctls) },
    { IDS_G_LINKS, 0, L_STACK, CTLS(link_ctls) },
    { IDS_G_TEXT, 0, L_STACK, CTLS(text_ctls) },
    { IDS_G_SYMBOLS, 0, L_STACK, CTLS(symbol_ctls) },
};

/* Layout */
static struct ctl setup_ctls[] =
{
    { C_BIG, IDM_MARGINS, "margins", NULL, 0, DROP },
    { C_BIG, IDM_ORIENTATION, "orientation", NULL, 0, DROP },
    { C_BIG, IDM_SIZE, "size", NULL, 0, DROP },
};
static struct ctl indent_ctls[] =
{
    { C_SPIN, IDM_INDENT_LEFT, "indent", NULL, 0, 0, 0, 150 },
    { C_SPIN, IDM_INDENT_RIGHT, "outdent", NULL, 0, 0, 0, 150 },
};
static struct ctl spacing_ctls[] =
{
    { C_SPIN, IDM_SPACE_BEFORE, "linespacing", NULL, 0, 0, 0, 150 },
    { C_SPIN, IDM_SPACE_AFTER, "linespacing", NULL, 0, 0, 0, 150 },
};

static struct group layout_groups[] =
{
    { IDS_G_PAGESETUP, IDM_PAGESETUP, L_STACK, CTLS(setup_ctls) },
    { IDS_G_INDENT, IDM_PARADIALOG, L_STACK, CTLS(indent_ctls) },
    { IDS_G_SPACING, IDM_PARADIALOG, L_STACK, CTLS(spacing_ctls) },
};

/* View */
static struct ctl views_ctls[] =
{
    { C_BIG, IDM_VIEW_PRINT, "printlayout" },
    { C_BIG, IDM_VIEW_WEB, "weblayout" },
};
static struct ctl zoom_ctls[] =
{
    { C_BIG, IDM_ZOOM, "zoom" },
    { C_BIG, IDM_ZOOM_100, "document" },
    { C_LABEL, IDM_ONEPAGE, "onepage" },
    { C_LABEL, IDM_MULTIPAGE, "multipage" },
    { C_LABEL, IDM_PAGEWIDTH, "pagewidth" },
};
static struct ctl proof_ctls[] =
{
    { C_BIG, IDM_WORDCOUNT, "info" },
};

static struct group view_groups[] =
{
    { IDS_G_VIEWS, 0, L_STACK, CTLS(views_ctls) },
    { IDS_G_ZOOM, 0, L_STACK, CTLS(zoom_ctls) },
    { IDS_G_PROOFING, 0, L_STACK, CTLS(proof_ctls) },
};

struct tab
{
    UINT label;
    struct group *groups;
    int count;
    RECT rc;
};

static struct tab tabs[] =
{
    { IDS_TAB_FILE },
    { IDS_TAB_HOME, CTLS(home_groups) },
    { IDS_TAB_INSERT, CTLS(insert_groups) },
    { IDS_TAB_LAYOUT, CTLS(layout_groups) },
    { IDS_TAB_VIEW, CTLS(view_groups) },
};

static HWND ribbon, tooltip, size_edit;
static struct surface surf;
static int current_tab = 1;
static int style_first;         /* the first style the gallery shows */
static RECT gallery_rc, gallery_up, gallery_down, gallery_more, card_rc;

/* what is under the mouse and pressed */
enum hot_kind { H_NONE, H_TAB, H_CTL, H_DROP, H_LAUNCHER, H_STYLE, H_GALLERY_UP, H_GALLERY_DOWN, H_GALLERY_MORE,
                H_SPIN_UP, H_SPIN_DOWN, H_GROUP };
static int hot_kind, hot_index, hot_sub, down_kind, down_index, down_sub;
static struct ctl *hot_ctl, *down_ctl;

/* the state of the selection */
static CHARFORMAT2W sel_cf;
static PARAFORMAT2 sel_pf;
static int sel_style;
static COLORREF font_color = RGB( 0xc0, 0x00, 0x00 ), highlight_color = RGB( 0xff, 0xff, 0x00 ), shade_color = RGB( 0xff, 0xff, 0x00 );

#define TABS_H      px(36)
#define CARD_H      px(96)
#define CARD_TOP    (TABS_H)
#define ROW_H       px(24)
#define LABEL_H     px(20)

int ribbon_height(void) { return TABS_H + CARD_H + px( 8 ); }

static int gallery_cards = 4;
static int gallery_count(void) { return gallery_cards; }

/**********************************************************************
 *          Layout
 */

static int ctl_width( const struct ctl *c )
{
    int w = 0;

    switch (c->kind)
    {
    case C_ICON:
    case C_LETTER:
        w = px( 26 );
        if (c->kind == C_LETTER && c->letter && lstrlenW( c->letter ) > 1)
            w = max( w, text_width( get_font( c->font ), c->letter, -1 ) + px( 10 ) );
        if (c->flags & (DROP | SPLIT)) w += px( 13 );
        break;
    case C_LABEL:
        w = px( 30 ) + text_width( get_font( F_UI ), string( IDS_CMD( c->cmd ) ), -1 ) + px( 8 );
        if (c->flags & DROP) w += px( 12 );
        break;
    case C_BIG:
    {
        int t = text_width( get_font( F_UI ), string( IDS_CMD( c->cmd ) ), -1 );
        w = max( px( 46 ), t + px( 14 ) );
        if (c->flags & (DROP | SPLIT) && t + px( 26 ) > w) w = t + px( 26 );
        break;
    }
    case C_FONT:
    case C_SIZE:
        w = px( c->width );
        break;
    case C_SPIN:
        w = px( c->width );
        break;
    case C_GAP:
        w = px( 8 );
        break;
    case C_GALLERY:
        w = gallery_count() * px( 78 ) + px( 22 );
        break;
    }
    return w;
}

/* places the controls of a group from its left; returns its width */
static const char *group_icon( const struct group *g )
{
    for (int i = 0; i < g->count; i++) if (g->ctls[i].icon) return g->ctls[i].icon;
    return "document";
}

static int layout_group( struct group *g, int left, int top )
{
    int x = left + px( 6 ), width = 0;

    if (g->collapsed)
    {
        width = max( px( 64 ), text_width( get_font( F_UI ), string( g->label ), -1 ) + px( 20 ) );
        for (int i = 0; i < g->count; i++) SetRectEmpty( &g->ctls[i].rc );
        SetRect( &g->rc, left, top, left + width, top + CARD_H );
        return width;
    }

    if (g->layout == L_ROWS)
    {
        int xs[3] = { x, x, x };
        int ys[3] = { top + px( 6 ), top + px( 38 ), top + px( 38 ) };

        for (int i = 0; i < g->count; i++)
        {
            struct ctl *c = &g->ctls[i];
            int row = c->flags & R2 ? 1 : 0, w = ctl_width( c );

            SetRect( &c->rc, xs[row], ys[row], xs[row] + w, ys[row] + ROW_H );
            if (c->flags & SPLIT) SetRect( &c->drop_rc, c->rc.right - px( 13 ), c->rc.top, c->rc.right, c->rc.bottom );
            xs[row] += w + (c->kind == C_FONT ? px( 4 ) : 0);
        }
        width = max( xs[0], xs[1] ) - left + px( 6 );
    }
    else
    {
        int col_w = 0, n = 0, rows = 3;

        for (int i = 0; i < g->count; i++)
        {
            struct ctl *c = &g->ctls[i];
            int w = ctl_width( c );

            if (c->kind == C_BIG || c->kind == C_GALLERY)
            {
                x += col_w;
                if (col_w) x += px( 4 );
                col_w = n = 0;
                SetRect( &c->rc, x, top + px( 4 ), x + w, top + CARD_H - LABEL_H - px( 6 ) );
                if (c->flags & SPLIT)
                {
                    c->drop_rc = c->rc;
                    c->drop_rc.top = c->rc.top + px( 44 );
                }
                x += w + px( 2 );
                continue;
            }
            if (n == rows || (c->flags & NEWCOL))
            {
                x += col_w + px( 4 );
                col_w = n = 0;
            }
            {
                int stack = 0;
                for (int j = 0; j < g->count; j++)
                    if (g->ctls[j].kind != C_BIG && g->ctls[j].kind != C_GALLERY) stack++;
                stack = min( stack, rows );
                /* two in a column stand apart as Word's Undo and Redo */
                {
                    int step = stack == 2 ? px( 30 ) : px( 23 ), first = stack == 2 ? px( 8 ) : px( 4 );
                    SetRect( &c->rc, x, top + first + n * step, x + w, top + first + n * step + px( 22 ) );
                }
            }
            col_w = max( col_w, w );
            n++;
        }
        for (int i = 0; i < g->count; i++)
            if (g->ctls[i].kind != C_BIG && g->ctls[i].kind != C_GALLERY && g->ctls[i].rc.left >= x)
                g->ctls[i].rc.right = g->ctls[i].rc.left + col_w;
        x += col_w;
        width = x - left + px( 6 );
    }
    {
        int label = text_width( get_font( F_SMALL ), string( g->label ), -1 ) + px( 16 ) +
                    (g->launcher ? px( 20 ) : 0);
        width = max( width, label );
    }
    SetRect( &g->rc, left, top, left + width, top + CARD_H );
    return width;
}

static void layout(void)
{
    RECT client;
    struct tab *t = &tabs[current_tab];
    int x;

    GetClientRect( ribbon, &client );
    /* the tabs */
    x = px( 12 );
    for (int i = 0; i < ARRAY_SIZE(tabs); i++)
    {
        int w = text_width( get_font( F_TAB ), string( tabs[i].label ), -1 ) + px( 22 );
        SetRect( &tabs[i].rc, x, px( 2 ), x + w, TABS_H - px( 2 ) );
        x += w;
    }
    SetRect( &card_rc, px( 8 ), CARD_TOP, client.right - px( 8 ), CARD_TOP + CARD_H );
    /* as much as there is room for: fewer styles first, then the groups
     * from the right folded into a button each */
    for (int i = 0; i < t->count; i++) t->groups[i].collapsed = FALSE;
    for (int fold = 0; fold <= t->count; fold++)
    {
        for (gallery_cards = fold ? 1 : 4; gallery_cards >= 1; gallery_cards--)
        {
            x = card_rc.left + px( 4 );
            for (int i = 0; i < t->count; i++)
                x += layout_group( &t->groups[i], x, CARD_TOP ) + px( 1 );
            if (x <= card_rc.right - px( 4 )) return;
            if (fold) break;
        }
        if (fold >= t->count) break;
        t->groups[t->count - 1 - fold].collapsed = TRUE;
        gallery_cards = 1;
    }
}

/**********************************************************************
 *          State
 */

static BOOL ctl_checked( const struct ctl *c )
{
    switch (c->cmd)
    {
    case IDM_BOLD: return (sel_cf.dwMask & CFM_BOLD) && (sel_cf.dwEffects & CFE_BOLD);
    case IDM_ITALIC: return (sel_cf.dwMask & CFM_ITALIC) && (sel_cf.dwEffects & CFE_ITALIC);
    case IDM_UNDERLINE: return (sel_cf.dwMask & CFM_UNDERLINE) && (sel_cf.dwEffects & CFE_UNDERLINE);
    case IDM_STRIKE: return (sel_cf.dwMask & CFM_STRIKEOUT) && (sel_cf.dwEffects & CFE_STRIKEOUT);
    case IDM_SUBSCRIPT: return (sel_cf.dwMask & CFM_SUBSCRIPT) && (sel_cf.dwEffects & CFE_SUBSCRIPT);
    case IDM_SUPERSCRIPT: return (sel_cf.dwMask & CFM_SUPERSCRIPT) && (sel_cf.dwEffects & CFE_SUPERSCRIPT);
    case IDM_ALIGN_LEFT: return (sel_pf.dwMask & PFM_ALIGNMENT) && sel_pf.wAlignment == PFA_LEFT;
    case IDM_ALIGN_CENTER: return (sel_pf.dwMask & PFM_ALIGNMENT) && sel_pf.wAlignment == PFA_CENTER;
    case IDM_ALIGN_RIGHT: return (sel_pf.dwMask & PFM_ALIGNMENT) && sel_pf.wAlignment == PFA_RIGHT;
    case IDM_ALIGN_JUSTIFY: return (sel_pf.dwMask & PFM_ALIGNMENT) && sel_pf.wAlignment == PFA_JUSTIFY;
    case IDM_BULLETS: return (sel_pf.dwMask & PFM_NUMBERING) && sel_pf.wNumbering == PFN_BULLET;
    case IDM_NUMBERING: return (sel_pf.dwMask & PFM_NUMBERING) && sel_pf.wNumbering > PFN_BULLET;
    case IDM_FORMATPAINTER:
    case IDM_VIEW_PRINT:
    case IDM_VIEW_WEB:
        return command_checked( c->cmd );
    }
    return FALSE;
}

void ribbon_refresh(void)
{
    if (!ribbon) return;
    sel_cf.cbSize = sizeof(sel_cf);
    ed( EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM)&sel_cf );
    sel_pf.cbSize = sizeof(sel_pf);
    ed( EM_GETPARAFORMAT, 0, (LPARAM)&sel_pf );
    sel_style = current_style();
    InvalidateRect( ribbon, NULL, FALSE );
}

void ribbon_theme_changed(void)
{
    layout();
    InvalidateRect( ribbon, NULL, FALSE );
}

void ribbon_set_tab( int tab )
{
    if (tab == 0)
    {
        show_backstage( TRUE, 0 );
        return;
    }
    current_tab = tab;
    layout();
    InvalidateRect( ribbon, NULL, FALSE );
}

/**********************************************************************
 *          Painting
 */

static void spin_text( const struct ctl *c, WCHAR *label, int label_len, WCHAR *value, int value_len )
{
    BOOL cm = c->cmd == IDM_INDENT_LEFT || c->cmd == IDM_INDENT_RIGHT;
    int v = 0;

    switch (c->cmd)
    {
    case IDM_INDENT_LEFT: v = sel_pf.dxStartIndent + sel_pf.dxOffset; lstrcpynW( label, string( IDS_LEFT ), label_len ); break;
    case IDM_INDENT_RIGHT: v = sel_pf.dxRightIndent; lstrcpynW( label, string( IDS_RIGHT ), label_len ); break;
    case IDM_SPACE_BEFORE: v = sel_pf.dySpaceBefore; lstrcpynW( label, string( IDS_BEFORE ), label_len ); break;
    case IDM_SPACE_AFTER: v = sel_pf.dySpaceAfter; lstrcpynW( label, string( IDS_AFTER ), label_len ); break;
    }
    if (cm)
    {
        int tenths = (v * 254 + 720) / 1440;   /* tenths of a centimetre */
        swprintf( value, value_len, L"%d,%d %s", tenths / 10, abs( tenths % 10 ), string( IDS_UNITS_CM ) );
    }
    else swprintf( value, value_len, L"%d %s", (v + 10) / 20, string( IDS_UNITS_PT ) );
}

static void paint_button_back( const RECT *r, BOOL hot, BOOL down, BOOL checked )
{
    if (checked)
    {
        fill_round( &surf, r, (float)px( 4 ), hot ? mix( th.checked, th.text, 20 ) : th.checked, 255 );
    }
    else if (down) fill_round( &surf, r, (float)px( 4 ), th.pressed, 255 );
    else if (hot) fill_round( &surf, r, (float)px( 4 ), th.hover, 255 );
}

static void paint_gallery( struct ctl *c )
{
    RECT r = c->rc;
    int card_w = px( 78 ), n = gallery_count();

    gallery_rc = r;
    for (int i = 0; i < n && style_first + i < STYLE_COUNT; i++)
    {
        int s = style_first + i;
        RECT card = { r.left + i * card_w, r.top + px( 2 ), r.left + (i + 1) * card_w - px( 2 ), r.bottom - px( 2 ) };
        RECT sample = card, name = card;
        BOOL hot = hot_kind == H_STYLE && hot_index == s;
        LOGFONTW lf = { 0 };
        HFONT font;

        if (s == sel_style) outline_round( &surf, &card, (float)px( 4 ), th.accent, 255 );
        if (hot) fill_round( &surf, &card, (float)px( 4 ), th.hover, 255 );
        if (s == sel_style && hot) outline_round( &surf, &card, (float)px( 4 ), th.accent, 255 );

        lf.lfHeight = -px( max( 10, min( styles[s].size * 7 / 11, 20 ) ) );
        lf.lfWeight = styles[s].bold ? FW_BOLD : FW_NORMAL;
        lf.lfItalic = styles[s].italic;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = CLEARTYPE_QUALITY;
        lstrcpynW( lf.lfFaceName, styles[s].face ? head_face : body_face, LF_FACESIZE );
        font = CreateFontIndirectW( &lf );
        sample.bottom = card.top + px( 34 );
        sample.top += px( 6 );
        draw_text( &surf, L"AaBbCcDd", -1, &sample, font,
                   styles[s].color == CLR_DEFAULT || th.dark ? th.text : styles[s].color, DT_CENTER );
        DeleteObject( font );
        name.top = sample.bottom;
        name.bottom = card.bottom - px( 4 );
        InflateRect( &name, -px( 2 ), 0 );
        draw_text( &surf, string( styles[s].name ), -1, &name, get_font( F_SMALL ), th.text, DT_CENTER );
    }
    /* the arrows on the right */
    {
        int x = r.right - px( 20 ), h = (r.bottom - r.top) / 3;
        RECT *rects[3] = { &gallery_up, &gallery_down, &gallery_more };
        int kinds[3] = { H_GALLERY_UP, H_GALLERY_DOWN, H_GALLERY_MORE };
        int glyphs[3] = { G_UP, G_DOWN, G_MORE };

        for (int i = 0; i < 3; i++)
        {
            BOOL enabled = i == 2 || (i == 0 ? style_first > 0 : style_first + n < STYLE_COUNT);

            SetRect( rects[i], x, r.top + i * h, r.right, r.top + (i + 1) * h );
            if (hot_kind == kinds[i] && enabled) fill_round( &surf, rects[i], (float)px( 3 ), th.hover, 255 );
            draw_glyph( &surf, glyphs[i], x + px( 3 ), rects[i]->top + (h - px( 14 )) / 2, px( 14 ),
                        enabled ? th.dim : th.faint );
        }
        RECT line = { x - px( 1 ), r.top + px( 4 ), x, r.bottom - px( 4 ) };
        fill( &surf, &line, th.card_line );
    }
}

static void paint_field( struct ctl *c )
{
    RECT r = c->rc, text;
    WCHAR buf[LF_FACESIZE + 8];
    BOOL hot = hot_ctl == c;

    fill_round( &surf, &r, (float)px( 4 ), th.field, 255 );
    outline_round( &surf, &r, (float)px( 4 ), hot ? mix( th.field_line, th.text, 80 ) : th.field_line, 255 );
    buf[0] = 0;
    if (c->kind == C_FONT)
    {
        if (sel_cf.dwMask & CFM_FACE) lstrcpynW( buf, sel_cf.szFaceName, ARRAY_SIZE(buf) );
    }
    else if (sel_cf.dwMask & CFM_SIZE)
    {
        int half = sel_cf.yHeight / 10;
        if (half % 2) swprintf( buf, ARRAY_SIZE(buf), L"%d,5", half / 2 );
        else swprintf( buf, ARRAY_SIZE(buf), L"%d", half / 2 );
    }
    text = r;
    text.left += px( 6 );
    text.right -= px( 18 );
    draw_text( &surf, buf, -1, &text, get_font( F_UI ), th.text, DT_LEFT );
    draw_glyph( &surf, G_DOWN_SMALL, r.right - px( 17 ), r.top + (r.bottom - r.top - px( 14 )) / 2, px( 14 ), th.dim );
}

static void paint_ctl( struct ctl *c )
{
    BOOL enabled = command_enabled( c->cmd ), checked = ctl_checked( c );
    BOOL hot = hot_ctl == c, down = down_ctl == c;
    COLORREF text_color = enabled ? th.text : th.faint;
    int icon = px( 16 );
    RECT r = c->rc;

    switch (c->kind)
    {
    case C_GAP:
        return;
    case C_GALLERY:
        paint_gallery( c );
        return;
    case C_FONT:
    case C_SIZE:
        paint_field( c );
        return;
    case C_SPIN:
    {
        WCHAR label[64], value[32];
        RECT t = r, field = r, up, dn;

        spin_text( c, label, ARRAY_SIZE(label), value, ARRAY_SIZE(value) );
        draw_icon( &surf, c->icon, r.left + px( 2 ), r.top + px( 3 ), icon, enabled ? IS_NORMAL : IS_DISABLED );
        t.left += px( 22 );
        t.right = r.left + px( 72 );
        draw_text( &surf, label, -1, &t, get_font( F_UI ), text_color, DT_LEFT );
        field.left = t.right;
        fill_round( &surf, &field, (float)px( 4 ), th.field, 255 );
        outline_round( &surf, &field, (float)px( 4 ), hot ? mix( th.field_line, th.text, 80 ) : th.field_line, 255 );
        t = field;
        t.left += px( 6 );
        t.right -= px( 16 );
        draw_text( &surf, value, -1, &t, get_font( F_UI ), text_color, DT_LEFT );
        SetRect( &up, field.right - px( 16 ), field.top + px( 1 ), field.right - px( 1 ), (field.top + field.bottom) / 2 );
        SetRect( &dn, up.left, up.bottom, up.right, field.bottom - px( 1 ) );
        c->drop_rc = field;
        if (hot && hot_kind == H_SPIN_UP) fill_round( &surf, &up, (float)px( 2 ), th.hover, 255 );
        if (hot && hot_kind == H_SPIN_DOWN) fill_round( &surf, &dn, (float)px( 2 ), th.hover, 255 );
        draw_glyph( &surf, G_UP, up.left + px( 2 ), up.top - px( 1 ), px( 11 ), th.dim );
        draw_glyph( &surf, G_DOWN, dn.left + px( 2 ), dn.top - px( 1 ), px( 11 ), th.dim );
        return;
    }
    case C_BIG:
    {
        RECT label = r;
        const WCHAR *text = string( IDS_CMD( c->cmd ) );
        int big = px( 32 );

        if (c->flags & SPLIT)
        {
            RECT top = r, bottom = c->drop_rc;
            top.bottom = bottom.top;
            if (hot && hot_kind == H_CTL) paint_button_back( &top, TRUE, down && down_kind == H_CTL, FALSE );
            if (hot && hot_kind == H_DROP) paint_button_back( &bottom, TRUE, down && down_kind == H_DROP, FALSE );
            if (hot && hot_kind != H_NONE) outline_round( &surf, &r, (float)px( 4 ), th.pressed, 255 );
        }
        else paint_button_back( &r, hot, down, checked );
        draw_icon( &surf, c->icon, (r.left + r.right - big) / 2, r.top + px( 4 ), big, enabled ? IS_NORMAL : IS_DISABLED );
        label.top = r.top + px( 40 );
        label.bottom = label.top + px( 16 );
        draw_text( &surf, text, -1, &label, get_font( F_UI ), text_color, DT_CENTER );
        if (c->flags & (DROP | SPLIT))
            draw_glyph( &surf, G_DOWN_SMALL, (r.left + r.right - px( 12 )) / 2, label.bottom, px( 12 ), th.dim );
        return;
    }
    default:
        break;
    }

    /* small buttons */
    if (c->flags & SPLIT)
    {
        RECT main = r;
        main.right = c->drop_rc.left;
        if (checked) paint_button_back( &main, hot && hot_kind == H_CTL, FALSE, TRUE );
        if (hot && hot_kind == H_CTL) paint_button_back( &main, TRUE, down, FALSE );
        if (hot && hot_kind == H_DROP) paint_button_back( &c->drop_rc, TRUE, down, FALSE );
        if (hot && !checked) outline_round( &surf, &r, (float)px( 4 ), th.pressed, 255 );
    }
    else paint_button_back( &r, hot, down, checked );

    if (c->kind == C_LETTER)
    {
        RECT t = r;
        if (c->flags & (DROP | SPLIT)) t.right -= px( 13 );
        draw_text( &surf, c->letter, -1, &t, get_font( c->font ), text_color, DT_CENTER );
    }
    else if (c->kind == C_ICON)
    {
        int y = r.top + (r.bottom - r.top - icon) / 2 - ((c->flags & BAR) ? px( 2 ) : 0);
        draw_icon( &surf, c->icon, r.left + px( 5 ), y, icon, enabled ? IS_NORMAL : IS_DISABLED );
        if (c->flags & BAR)
        {
            COLORREF color = c->cmd == IDM_FONTCOLOR ? font_color : c->cmd == IDM_HIGHLIGHT ? highlight_color : shade_color;
            RECT bar = { r.left + px( 5 ), y + icon, r.left + px( 5 ) + icon, y + icon + px( 3 ) };
            fill( &surf, &bar, enabled ? color : th.faint );
        }
    }
    else if (c->kind == C_LABEL)
    {
        RECT t = r;
        draw_icon( &surf, c->icon, r.left + px( 4 ), r.top + (r.bottom - r.top - icon) / 2, icon,
                   enabled ? IS_NORMAL : IS_DISABLED );
        t.left += px( 26 );
        draw_text( &surf, string( IDS_CMD( c->cmd ) ), -1, &t, get_font( F_UI ), text_color, DT_LEFT );
        if (c->flags & DROP)
        {
            int w = text_width( get_font( F_UI ), string( IDS_CMD( c->cmd ) ), -1 );
            draw_glyph( &surf, G_DOWN_SMALL, t.left + w + px( 2 ), r.top + (r.bottom - r.top - px( 12 )) / 2, px( 12 ), th.dim );
        }
    }
    if (c->flags & (DROP | SPLIT) && c->kind != C_LABEL)
        draw_glyph( &surf, G_DOWN_SMALL, r.right - px( 13 ), r.top + (r.bottom - r.top - px( 12 )) / 2, px( 12 ), th.dim );
}

static void paint( HDC hdc )
{
    RECT client, r;
    struct tab *t = &tabs[current_tab];

    GetClientRect( ribbon, &client );
    if (!surface_size( &surf, client.right, client.bottom )) return;
    fill( &surf, &client, th.chrome );

    /* the tabs */
    for (int i = 0; i < ARRAY_SIZE(tabs); i++)
    {
        BOOL sel = i == current_tab, hot = hot_kind == H_TAB && hot_index == i;
        const WCHAR *label = string( tabs[i].label );

        r = tabs[i].rc;
        if (hot && !sel)
        {
            RECT h = r;
            InflateRect( &h, -px( 2 ), -px( 3 ) );
            fill_round( &surf, &h, (float)px( 4 ), th.dark ? th.hover : RGB( 0xe5, 0xe8, 0xec ), 255 );
        }
        draw_text( &surf, label, -1, &r, get_font( sel ? F_UI_SEMI : F_TAB ), sel ? th.accent : th.text, DT_CENTER );
        if (sel)
        {
            int w = text_width( get_font( F_UI_SEMI ), label, -1 ) - px( 4 );
            RECT line = { (r.left + r.right - w) / 2, r.bottom - px( 5 ), (r.left + r.right + w) / 2, r.bottom - px( 2 ) };
            fill_round( &surf, &line, (float)px( 1 ) + 0.5f, th.accent, 255 );
        }
    }

    /* the card */
    {
        RECT shadow = card_rc;
        OffsetRect( &shadow, 0, px( 1 ) );
        fill_round( &surf, &shadow, (float)px( 8 ), th.dark ? RGB( 0, 0, 0 ) : RGB( 0xd8, 0xdb, 0xdf ), 255 );
        fill_round( &surf, &card_rc, (float)px( 8 ), th.card, 255 );
        if (th.dark) outline_round( &surf, &card_rc, (float)px( 8 ), th.card_line, 255 );
    }

    for (int g = 0; g < t->count; g++)
    {
        struct group *grp = &t->groups[g];
        RECT label = grp->rc;

        if (grp->collapsed)
        {
            RECT b = grp->rc, l;
            InflateRect( &b, -px( 3 ), -px( 4 ) );
            if (hot_kind == H_GROUP && hot_index == g) fill_round( &surf, &b, (float)px( 4 ), th.hover, 255 );
            draw_icon( &surf, group_icon( grp ), (b.left + b.right - px( 32 )) / 2, b.top + px( 6 ), px( 32 ), IS_NORMAL );
            SetRect( &l, b.left, b.top + px( 44 ), b.right, b.top + px( 62 ) );
            draw_text( &surf, string( grp->label ), -1, &l, get_font( F_UI ), th.text, DT_CENTER );
            draw_glyph( &surf, G_DOWN_SMALL, (b.left + b.right - px( 12 )) / 2, l.bottom, px( 12 ), th.dim );
            if (g < t->count - 1)
            {
                RECT sep = { grp->rc.right, grp->rc.top + px( 10 ), grp->rc.right + px( 1 ), grp->rc.bottom - px( 10 ) };
                fill( &surf, &sep, th.card_line );
            }
            continue;
        }
        for (int i = 0; i < grp->count; i++) paint_ctl( &grp->ctls[i] );

        label.top = grp->rc.bottom - LABEL_H - px( 2 );
        label.bottom = grp->rc.bottom - px( 2 );
        if (grp->launcher) label.right -= px( 18 );
        draw_text( &surf, string( grp->label ), -1, &label, get_font( F_SMALL ), th.dim, DT_CENTER );
        if (grp->launcher)
        {
            RECT l = { grp->rc.right - px( 22 ), label.top + px( 2 ), grp->rc.right - px( 6 ), label.top + px( 18 ) };
            if (hot_kind == H_LAUNCHER && hot_index == g) fill_round( &surf, &l, (float)px( 3 ), th.hover, 255 );
            draw_glyph( &surf, G_LAUNCHER, l.left + px( 2 ), l.top + px( 2 ), px( 12 ), th.dim );
        }
        if (g < t->count - 1)
        {
            RECT sep = { grp->rc.right, grp->rc.top + px( 10 ), grp->rc.right + px( 1 ), grp->rc.bottom - px( 10 ) };
            fill( &surf, &sep, th.card_line );
        }
    }
    GdiFlush();
    BitBlt( hdc, 0, 0, client.right, client.bottom, surf.dc, 0, 0, SRCCOPY );
}

/**********************************************************************
 *          The mouse
 */

static int hit( POINT pt, struct ctl **ctl, int *sub )
{
    struct tab *t = &tabs[current_tab];

    *ctl = NULL;
    *sub = 0;
    for (int i = 0; i < ARRAY_SIZE(tabs); i++)
        if (PtInRect( &tabs[i].rc, pt ))
        {
            *sub = i;
            return H_TAB;
        }
    for (int g = 0; g < t->count; g++)
    {
        struct group *grp = &t->groups[g];

        if (!PtInRect( &grp->rc, pt )) continue;
        if (grp->collapsed)
        {
            *sub = g;
            return H_GROUP;
        }
        if (grp->launcher)
        {
            RECT l = { grp->rc.right - px( 22 ), grp->rc.bottom - LABEL_H, grp->rc.right - px( 4 ), grp->rc.bottom };
            if (PtInRect( &l, pt ))
            {
                *sub = g;
                return H_LAUNCHER;
            }
        }
        for (int i = 0; i < grp->count; i++)
        {
            struct ctl *c = &grp->ctls[i];

            if (c->kind == C_GAP || !PtInRect( &c->rc, pt )) continue;
            *ctl = c;
            if (c->kind == C_GALLERY)
            {
                if (PtInRect( &gallery_up, pt )) return H_GALLERY_UP;
                if (PtInRect( &gallery_down, pt )) return H_GALLERY_DOWN;
                if (PtInRect( &gallery_more, pt )) return H_GALLERY_MORE;
                *sub = style_first + (pt.x - c->rc.left) / px( 78 );
                return *sub < STYLE_COUNT && *sub < style_first + gallery_count() ? H_STYLE : H_NONE;
            }
            if (c->kind == C_SPIN)
            {
                RECT f = c->drop_rc;
                if (pt.x >= f.right - px( 16 )) return pt.y < (f.top + f.bottom) / 2 ? H_SPIN_UP : H_SPIN_DOWN;
                return H_CTL;
            }
            if ((c->flags & SPLIT) && PtInRect( &c->drop_rc, pt )) return H_DROP;
            return H_CTL;
        }
    }
    return H_NONE;
}

static void anchor_of( const struct ctl *c, RECT *r )
{
    *r = c->rc;
    MapWindowPoints( ribbon, NULL, (POINT *)r, 2 );
}

static void spin( struct ctl *c, int dir )
{
    PARAFORMAT2 pf = { sizeof(pf) };
    int step_cm = 1440 * 25 / 254 / 2 * 2;  /* a quarter of a centimetre, nearly */

    step_cm = 142;   /* 0.25 cm */
    ed( EM_GETPARAFORMAT, 0, (LPARAM)&pf );
    switch (c->cmd)
    {
    case IDM_INDENT_LEFT:
    {
        int left = max( 0, pf.dxStartIndent + pf.dxOffset + dir * step_cm );
        pf.dwMask = PFM_STARTINDENT | PFM_OFFSET;
        pf.dxStartIndent = left - pf.dxOffset;
        break;
    }
    case IDM_INDENT_RIGHT:
        pf.dwMask = PFM_RIGHTINDENT;
        pf.dxRightIndent = max( 0, pf.dxRightIndent + dir * step_cm );
        break;
    case IDM_SPACE_BEFORE:
        pf.dwMask = PFM_SPACEBEFORE;
        pf.dySpaceBefore = max( 0, (pf.dySpaceBefore + 60) / 120 * 120 + dir * 120 );
        break;
    case IDM_SPACE_AFTER:
        pf.dwMask = PFM_SPACEAFTER;
        pf.dySpaceAfter = max( 0, (pf.dySpaceAfter + 60) / 120 * 120 + dir * 120 );
        break;
    }
    ed( EM_SETPARAFORMAT, 0, (LPARAM)&pf );
    ribbon_refresh();
}

/* the size typed over its field */
static LRESULT CALLBACK size_edit_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR data )
{
    if (msg == WM_KEYDOWN && (wparam == VK_RETURN || wparam == VK_ESCAPE))
    {
        if (wparam == VK_RETURN)
        {
            WCHAR text[32];
            GetWindowTextW( hwnd, text, ARRAY_SIZE(text) );
            if (data == IDM_FONTSIZE)
            {
                double size;
                for (WCHAR *p = text; *p; p++) if (*p == ',') *p = '.';
                size = wcstod( text, NULL );
                if (size >= 1 && size <= 1638) command_arg( IDM_FONTSIZE, (LPARAM)(size * 2 + 0.5) );
            }
        }
        DestroyWindow( hwnd );
        size_edit = NULL;
        view_focus();
        return 0;
    }
    if (msg == WM_KILLFOCUS)
    {
        PostMessageW( hwnd, WM_CLOSE, 0, 0 );
    }
    if (msg == WM_CLOSE)
    {
        DestroyWindow( hwnd );
        if (size_edit == hwnd) size_edit = NULL;
        return 0;
    }
    if (msg == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    return DefSubclassProc( hwnd, msg, wparam, lparam );
}

static void edit_size( struct ctl *c )
{
    RECT r = c->rc;
    WCHAR text[16] = L"";

    if (size_edit) DestroyWindow( size_edit );
    if (sel_cf.dwMask & CFM_SIZE)
    {
        int half = sel_cf.yHeight / 10;
        if (half % 2) swprintf( text, ARRAY_SIZE(text), L"%d,5", half / 2 );
        else swprintf( text, ARRAY_SIZE(text), L"%d", half / 2 );
    }
    InflateRect( &r, -px( 4 ), -px( 4 ) );
    r.right -= px( 14 );
    size_edit = CreateWindowExW( 0, L"EDIT", text, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, r.left, r.top,
                                 r.right - r.left, r.bottom - r.top, ribbon, NULL, instance, NULL );
    SendMessageW( size_edit, WM_SETFONT, (WPARAM)get_font( F_UI ), 0 );
    SetWindowSubclass( size_edit, size_edit_proc, 1, c->cmd );
    SetFocus( size_edit );
    SendMessageW( size_edit, EM_SETSEL, 0, -1 );
}

static void click( int kind, struct ctl *c, int sub, POINT pt )
{
    RECT anchor;

    switch (kind)
    {
    case H_TAB:
        ribbon_set_tab( sub );
        return;
    case H_LAUNCHER:
        command( tabs[current_tab].groups[sub].launcher );
        return;
    case H_GROUP:
    {
        /* a folded group: its commands as a menu */
        struct group *grp = &tabs[current_tab].groups[sub];
        int ids[40], n = 0, id;
        const char *icons[40];

        anchor = grp->rc;
        MapWindowPoints( ribbon, NULL, (POINT *)&anchor, 2 );
        for (int i = 0; i < grp->count && n < 40; i++)
        {
            struct ctl *c = &grp->ctls[i];
            if (c->kind == C_GAP || c->kind == C_FONT || c->kind == C_SIZE || c->kind == C_SPIN) continue;
            if (c->kind == C_GALLERY)
            {
                for (int s = 0; s < STYLE_COUNT && n < 40; s++) { icons[n] = NULL; ids[n++] = IDM_STYLE_FIRST + s; }
                continue;
            }
            icons[n] = c->icon;
            ids[n++] = c->cmd;
        }
        if ((id = menu_popup( frame, &anchor, ids, icons, n, -1 ))) command( id );
        return;
    }
    case H_STYLE:
        command( IDM_STYLE_FIRST + sub );
        return;
    case H_GALLERY_UP:
        if (style_first > 0) style_first--;
        InvalidateRect( ribbon, NULL, FALSE );
        return;
    case H_GALLERY_DOWN:
        if (style_first + gallery_count() < STYLE_COUNT) style_first++;
        InvalidateRect( ribbon, NULL, FALSE );
        return;
    case H_GALLERY_MORE:
    {
        int ids[STYLE_COUNT], id;
        anchor_of( c, &anchor );
        for (int i = 0; i < STYLE_COUNT; i++) ids[i] = IDM_STYLE_FIRST + i;
        id = menu_popup( frame, &anchor, ids, NULL, STYLE_COUNT, sel_style >= 0 ? IDM_STYLE_FIRST + sel_style : -1 );
        if (id) command( id );
        return;
    }
    case H_SPIN_UP:
    case H_SPIN_DOWN:
        spin( c, kind == H_SPIN_UP ? 1 : -1 );
        return;
    case H_DROP:
        anchor_of( c, &anchor );
        command_arg( c->drop, (LPARAM)&anchor );
        return;
    case H_CTL:
        anchor_of( c, &anchor );
        if (c->kind == C_SIZE && pt.x < c->rc.right - px( 16 ))
        {
            edit_size( c );
            return;
        }
        if (c->kind == C_SPIN)
        {
            command( IDM_PARADIALOG );
            return;
        }
        command_arg( c->cmd, (LPARAM)&anchor );
        return;
    }
}

static void set_hot( int kind, struct ctl *c, int sub )
{
    if (kind == hot_kind && c == hot_ctl && sub == hot_index) return;
    hot_kind = kind;
    hot_ctl = c;
    hot_index = sub;
    if (tooltip)
    {
        TOOLINFOW info = { sizeof(info), 0, ribbon, 1 };

        SendMessageW( tooltip, TTM_POP, 0, 0 );
        if (c && (kind == H_CTL || kind == H_DROP)) info.rect = c->rc;
        else if (kind == H_TAB) info.rect = tabs[sub].rc;
        SendMessageW( tooltip, TTM_NEWTOOLRECTW, 0, (LPARAM)&info );
    }
    InvalidateRect( ribbon, NULL, FALSE );
}

static LRESULT CALLBACK ribbon_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    POINT pt = { GET_X( lparam ), GET_Y( lparam ) };

    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint( hwnd, &ps );
        paint( hdc );
        EndPaint( hwnd, &ps );
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        layout();
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    case WM_MOUSEMOVE:
    {
        struct ctl *c;
        int sub, kind = hit( pt, &c, &sub );
        TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, hwnd };

        set_hot( kind, c, sub );
        TrackMouseEvent( &track );
        return 0;
    }
    case WM_MOUSELEAVE:
        set_hot( H_NONE, NULL, 0 );
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    {
        struct ctl *c;
        int sub, kind = hit( pt, &c, &sub );

        if (c && !command_enabled( c->cmd ) && kind != H_DROP) return 0;
        down_kind = kind;
        down_ctl = c;
        down_sub = sub;
        InvalidateRect( hwnd, NULL, FALSE );
        UpdateWindow( hwnd );
        /* menus and fields open on the press, as Word's */
        if (kind == H_DROP || kind == H_TAB || kind == H_SPIN_UP || kind == H_SPIN_DOWN || kind == H_GROUP ||
            (c && (c->flags & DROP || c->kind == C_FONT || c->kind == C_SIZE)))
        {
            down_kind = H_NONE;
            down_ctl = NULL;
            click( kind, c, sub, pt );
            InvalidateRect( hwnd, NULL, FALSE );
            return 0;
        }
        SetCapture( hwnd );
        return 0;
    }
    case WM_LBUTTONUP:
    {
        struct ctl *c;
        int sub, kind = hit( pt, &c, &sub );
        int was_kind = down_kind;
        struct ctl *was = down_ctl;

        if (GetCapture() == hwnd) ReleaseCapture();
        down_kind = H_NONE;
        down_ctl = NULL;
        InvalidateRect( hwnd, NULL, FALSE );
        if (was_kind != H_NONE && kind == was_kind && c == was && sub == down_sub)
        {
            click( kind, c, sub, pt );
            if (kind == H_CTL || kind == H_STYLE) view_focus();
        }
        return 0;
    }
    case WM_NOTIFY:
    {
        NMHDR *nm = (NMHDR *)lparam;

        if (nm->code == TTN_GETDISPINFOW)
        {
            NMTTDISPINFOW *info = (NMTTDISPINFOW *)lparam;
            const WCHAR *tip = L"";

            if (hot_ctl && (hot_kind == H_CTL || hot_kind == H_DROP))
            {
                tip = string( IDS_TIP( hot_ctl->cmd ) );
                if (!tip[0]) tip = string( IDS_CMD( hot_ctl->cmd ) );
            }
            info->lpszText = (WCHAR *)tip;
            return 0;
        }
        break;
    }
    case WM_CTLCOLOREDIT:
    {
        static HBRUSH brush;
        static COLORREF brush_color;

        if (!brush || brush_color != th.field)
        {
            if (brush) DeleteObject( brush );
            brush = CreateSolidBrush( brush_color = th.field );
        }
        SetTextColor( (HDC)wparam, th.text );
        SetBkColor( (HDC)wparam, th.field );
        return (LRESULT)brush;
    }
    case WM_SETTINGCHANGE:
        fonts_reset();
        layout();
        InvalidateRect( hwnd, NULL, FALSE );
        break;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

HWND ribbon_create( HWND parent )
{
    WNDCLASSW cls = { CS_DBLCLKS, ribbon_proc, 0, 0, instance, NULL, NULL, NULL, NULL, L"ArcticWordPadRibbon" };
    TOOLINFOW info = { sizeof(info), TTF_SUBCLASS, NULL, 1 };

    cls.hCursor = LoadCursorW( NULL, (LPCWSTR)IDC_ARROW );
    RegisterClassW( &cls );
    ribbon = CreateWindowExW( 0, cls.lpszClassName, NULL, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                              0, 0, 100, ribbon_height(), parent, NULL, instance, NULL );
    tooltip = CreateWindowExW( WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0, 0,
                               ribbon, NULL, instance, NULL );
    info.hwnd = ribbon;
    info.lpszText = LPSTR_TEXTCALLBACKW;
    SendMessageW( tooltip, TTM_ADDTOOLW, 0, (LPARAM)&info );
    SendMessageW( tooltip, TTM_SETMAXTIPWIDTH, 0, px( 320 ) );
    sel_style = 0;
    layout();
    return ribbon;
}

/* the colours the buttons remember, for a click on their main part */
COLORREF ribbon_color( int cmd )
{
    return cmd == IDM_FONTCOLOR ? font_color : cmd == IDM_HIGHLIGHT ? highlight_color : shade_color;
}

void ribbon_set_color( int cmd, COLORREF color )
{
    if (cmd == IDM_FONTCOLOR) font_color = color;
    else if (cmd == IDM_HIGHLIGHT) highlight_color = color;
    else shade_color = color;
    InvalidateRect( ribbon, NULL, FALSE );
}

void ribbon_font_changed( const WCHAR *face, int size_half_points )
{
    ribbon_refresh();
}

/*
 * Arctic WordPad: Word documents (DOCX)
 *
 * Reading: the package's document part with its styles, numbering, theme
 * fonts and pictures becomes RTF, which riched20 reads. What riched20 has
 * no place for (headers and footers, footnotes, comments, the settings) is
 * kept from the package, and goes back into it when the document is saved.
 *
 * Writing: riched20's own RTF becomes the document part again, with styles
 * and numbering parts of its own when the document was not a DOCX.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdio.h>
#include <ctype.h>
#include <limits.h>
#include "wordpad.h"

#define UNSET INT_MIN

/**********************************************************************
 *          Links: riched20 keeps no addresses, the program does
 */

struct link { WCHAR *text; char *url; };
static struct link *links;
static int link_count, link_size;

void link_register( const WCHAR *text, const char *url )
{
    if (!text[0] || !url[0]) return;
    for (int i = 0; i < link_count; i++)
        if (!wcscmp( links[i].text, text ))
        {
            free( links[i].url );
            links[i].url = _strdup( url );
            return;
        }
    if (link_count == link_size)
    {
        int size = max( 16, link_size * 2 );
        struct link *n = realloc( links, size * sizeof(*n) );
        if (!n) return;
        links = n;
        link_size = size;
    }
    links[link_count].text = _wcsdup( text );
    links[link_count].url = _strdup( url );
    link_count++;
}

static const char *link_find( const WCHAR *text )
{
    for (int i = 0; i < link_count; i++)
        if (!wcscmp( links[i].text, text )) return links[i].url;
    return NULL;
}

#define LINK_COLOR RGB( 0x05, 0x63, 0xc1 )

/**********************************************************************
 *          What is kept of a package
 */

struct docx_keep
{
    BYTE   *data;           /* the file as it was read */
    size_t  len;
    struct zip *zip;
    char    main_part[128]; /* word/document.xml, usually */
    char   *root;           /* the document element's start tag, with its namespaces */
    char   *sect_pr;        /* the body's last section, as written */
    char  **style_ids;      /* the styles of numbers 20 and up (\s) */
    int     style_count;
    BOOL    has_styles;
};

void docx_keep_free( struct docx_keep *keep )
{
    if (!keep) return;
    zip_close( keep->zip );
    free( keep->data );
    free( keep->root );
    free( keep->sect_pr );
    for (int i = 0; i < keep->style_count; i++) free( keep->style_ids[i] );
    free( keep->style_ids );
    free( keep );
}

/**********************************************************************
 *          Properties
 */

struct rprops
{
    int b, i, strike, caps, smallcaps, vanish;     /* UNSET, 0, 1 */
    int u;                                          /* 0 none, 1 single, 2 double, 3 dotted, 4 word, 5 wave */
    int va;                                         /* 0, 1 super, 2 sub */
    int sz;                                         /* half points */
    int color, highlight, shade;                    /* -2: automatic / none */
    int spacing;
    char font[64];                                  /* "+major", "+minor" for the theme's */
    char rstyle[64];
};

struct pprops
{
    int jc;                                         /* 0 left, 1 center, 2 right, 3 both */
    int left, right, first;                         /* first: + first line, - hanging */
    int before, after, line, line_rule;             /* line_rule 0 auto, 1 exact, 2 at least */
    int keep_next, keep_lines, page_break;
    int num_id, ilvl, outline;
    char style[64];
    int tab_count;                                  /* UNSET: none set here */
    int tabs[32];                                   /* position | kind << 24 (1 center, 2 right, 3 decimal) | leader << 28 */
};

static void rp_init( struct rprops *r )
{
    r->b = r->i = r->strike = r->caps = r->smallcaps = r->vanish = UNSET;
    r->u = r->va = r->sz = r->color = r->highlight = r->shade = r->spacing = UNSET;
    r->font[0] = r->rstyle[0] = 0;
}

static void pp_init( struct pprops *p )
{
    p->jc = p->left = p->right = p->first = p->before = p->after = p->line = p->line_rule = UNSET;
    p->keep_next = p->keep_lines = p->page_break = p->num_id = p->ilvl = p->outline = UNSET;
    p->style[0] = 0;
    p->tab_count = UNSET;
}

#define MERGE(f) if (src->f != UNSET) dst->f = src->f
static void rp_merge( struct rprops *dst, const struct rprops *src )
{
    MERGE(b); MERGE(i); MERGE(strike); MERGE(caps); MERGE(smallcaps); MERGE(vanish);
    MERGE(u); MERGE(va); MERGE(sz); MERGE(color); MERGE(highlight); MERGE(shade); MERGE(spacing);
    if (src->font[0]) strcpy( dst->font, src->font );
}

static void pp_merge( struct pprops *dst, const struct pprops *src )
{
    MERGE(jc); MERGE(left); MERGE(right); MERGE(first); MERGE(before); MERGE(after); MERGE(line); MERGE(line_rule);
    MERGE(keep_next); MERGE(keep_lines); MERGE(page_break); MERGE(num_id); MERGE(ilvl); MERGE(outline);
    if (src->tab_count != UNSET)
    {
        dst->tab_count = src->tab_count;
        memcpy( dst->tabs, src->tabs, sizeof(dst->tabs) );
    }
}
#undef MERGE

static int hex_color( const char *v )
{
    unsigned int c;

    if (!v[0] || !strcmp( v, "auto" )) return -2;
    if (sscanf( v, "%6x", &c ) != 1) return UNSET;
    return RGB( (c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff );
}

static const struct { const char *name; COLORREF color; } highlight_names[] =
{
    { "yellow", RGB( 0xff, 0xff, 0x00 ) }, { "green", RGB( 0x00, 0xff, 0x00 ) }, { "cyan", RGB( 0x00, 0xff, 0xff ) },
    { "magenta", RGB( 0xff, 0x00, 0xff ) }, { "blue", RGB( 0x00, 0x00, 0xff ) }, { "red", RGB( 0xff, 0x00, 0x00 ) },
    { "darkBlue", RGB( 0x00, 0x00, 0x80 ) }, { "darkCyan", RGB( 0x00, 0x80, 0x80 ) }, { "darkGreen", RGB( 0x00, 0x80, 0x00 ) },
    { "darkMagenta", RGB( 0x80, 0x00, 0x80 ) }, { "darkRed", RGB( 0x80, 0x00, 0x00 ) }, { "darkYellow", RGB( 0x80, 0x80, 0x00 ) },
    { "darkGray", RGB( 0x80, 0x80, 0x80 ) }, { "lightGray", RGB( 0xc0, 0xc0, 0xc0 ) }, { "black", RGB( 0x00, 0x00, 0x00 ) },
    { "white", RGB( 0xff, 0xff, 0xff ) },
};

/* the properties of a w:rPr, its element just opened */
static void read_rpr( struct xml *x, struct rprops *r )
{
    int depth = x->depth;
    char v[64];

    if (x->empty) { xml_next( x ); return; }
    for (;;)
    {
        enum xml_token t = xml_next( x );

        if (t == XML_END || (t == XML_CLOSE && x->depth < depth)) return;
        if (t != XML_OPEN || x->depth != depth + 1) continue;
        if (xml_is( x, "w:b" )) r->b = xml_attr_bool( x, "w:val", TRUE );
        else if (xml_is( x, "w:i" )) r->i = xml_attr_bool( x, "w:val", TRUE );
        else if (xml_is( x, "w:strike" ) || xml_is( x, "w:dstrike" )) r->strike = xml_attr_bool( x, "w:val", TRUE );
        else if (xml_is( x, "w:caps" )) r->caps = xml_attr_bool( x, "w:val", TRUE );
        else if (xml_is( x, "w:smallCaps" )) r->smallcaps = xml_attr_bool( x, "w:val", TRUE );
        else if (xml_is( x, "w:vanish" )) r->vanish = xml_attr_bool( x, "w:val", TRUE );
        else if (xml_is( x, "w:sz" )) r->sz = xml_attr_int( x, "w:val", r->sz );
        else if (xml_is( x, "w:spacing" )) r->spacing = xml_attr_int( x, "w:val", 0 );
        else if (xml_is( x, "w:rStyle" )) xml_attr( x, "w:val", r->rstyle, sizeof(r->rstyle) );
        else if (xml_is( x, "w:color" ))
        {
            xml_attr( x, "w:val", v, sizeof(v) );
            r->color = hex_color( v );
        }
        else if (xml_is( x, "w:highlight" ))
        {
            xml_attr( x, "w:val", v, sizeof(v) );
            r->highlight = -2;
            for (int i = 0; i < ARRAY_SIZE(highlight_names); i++)
                if (!strcmp( v, highlight_names[i].name )) r->highlight = highlight_names[i].color;
        }
        else if (xml_is( x, "w:shd" ))
        {
            xml_attr( x, "w:fill", v, sizeof(v) );
            r->shade = hex_color( v );
            if (r->shade == RGB( 255, 255, 255 )) r->shade = -2;
        }
        else if (xml_is( x, "w:u" ))
        {
            xml_attr( x, "w:val", v, sizeof(v) );
            r->u = !strcmp( v, "none" ) ? 0 : !strcmp( v, "double" ) ? 2 : !strncmp( v, "dot", 3 ) ? 3 :
                   !strcmp( v, "words" ) ? 4 : strstr( v, "ave" ) ? 5 : 1;
        }
        else if (xml_is( x, "w:vertAlign" ))
        {
            xml_attr( x, "w:val", v, sizeof(v) );
            r->va = !strcmp( v, "superscript" ) ? 1 : !strcmp( v, "subscript" ) ? 2 : 0;
        }
        else if (xml_is( x, "w:rFonts" ))
        {
            if (xml_attr( x, "w:asciiTheme", v, sizeof(v) ) || xml_attr( x, "w:hAnsiTheme", v, sizeof(v) ))
                strcpy( r->font, strncmp( v, "major", 5 ) ? "+minor" : "+major" );
            else if (xml_attr( x, "w:ascii", v, sizeof(v) ) || xml_attr( x, "w:hAnsi", v, sizeof(v) ))
                lstrcpynA( r->font, v, sizeof(r->font) );
        }
    }
}

static void read_ppr( struct xml *x, struct pprops *p, struct rprops *mark, BOOL *section_break )
{
    int depth = x->depth;
    char v[64];

    if (x->empty) { xml_next( x ); return; }
    for (;;)
    {
        enum xml_token t = xml_next( x );

        if (t == XML_END || (t == XML_CLOSE && x->depth < depth)) return;
        if (t != XML_OPEN || x->depth != depth + 1) continue;
        if (xml_is( x, "w:pStyle" )) xml_attr( x, "w:val", p->style, sizeof(p->style) );
        else if (xml_is( x, "w:jc" ))
        {
            xml_attr( x, "w:val", v, sizeof(v) );
            p->jc = !strcmp( v, "center" ) ? 1 : (!strcmp( v, "right" ) || !strcmp( v, "end" )) ? 2 :
                    (!strcmp( v, "both" ) || !strcmp( v, "distribute" )) ? 3 : 0;
        }
        else if (xml_is( x, "w:ind" ))
        {
            int hanging = xml_attr_int( x, "w:hanging", UNSET ), first = xml_attr_int( x, "w:firstLine", UNSET );
            int left = xml_attr_int( x, "w:left", UNSET ), right = xml_attr_int( x, "w:right", UNSET );

            if (left == UNSET) left = xml_attr_int( x, "w:start", UNSET );
            if (right == UNSET) right = xml_attr_int( x, "w:end", UNSET );
            if (left != UNSET) p->left = left;
            if (right != UNSET) p->right = right;
            if (hanging != UNSET) p->first = -hanging;
            else if (first != UNSET) p->first = first;
        }
        else if (xml_is( x, "w:spacing" ))
        {
            int before = xml_attr_int( x, "w:before", UNSET ), after = xml_attr_int( x, "w:after", UNSET );
            int line = xml_attr_int( x, "w:line", UNSET );

            if (xml_attr_bool( x, "w:beforeAutospacing", FALSE )) before = 280;
            if (xml_attr_bool( x, "w:afterAutospacing", FALSE )) after = 280;
            if (before != UNSET) p->before = before;
            if (after != UNSET) p->after = after;
            if (line != UNSET)
            {
                p->line = line;
                xml_attr( x, "w:lineRule", v, sizeof(v) );
                p->line_rule = !strcmp( v, "exact" ) ? 1 : !strcmp( v, "atLeast" ) ? 2 : 0;
            }
        }
        else if (xml_is( x, "w:tabs" ))
        {
            int d2 = x->depth;
            enum xml_token t2;

            p->tab_count = 0;
            if (xml_enter( x ))
                while ((t2 = xml_next( x )) != XML_END && !(t2 == XML_CLOSE && x->depth < d2))
                {
                    char kind[16], leader[16];
                    int pos, k, l;

                    if (t2 != XML_OPEN || !xml_is( x, "w:tab" ) || p->tab_count >= 32) continue;
                    xml_attr( x, "w:val", kind, sizeof(kind) );
                    xml_attr( x, "w:leader", leader, sizeof(leader) );
                    pos = xml_attr_int( x, "w:pos", 0 );
                    if (!strcmp( kind, "clear" ) || pos <= 0) continue;
                    k = !strcmp( kind, "center" ) ? 1 : (!strcmp( kind, "right" ) || !strcmp( kind, "end" )) ? 2 :
                        !strcmp( kind, "decimal" ) ? 3 : 0;
                    l = !strcmp( leader, "dot" ) ? 1 : !strcmp( leader, "hyphen" ) ? 2 : !strcmp( leader, "underscore" ) ? 3 : 0;
                    p->tabs[p->tab_count++] = (pos & 0xffffff) | (k << 24) | (l << 28);
                }
        }
        else if (xml_is( x, "w:keepNext" )) p->keep_next = xml_attr_bool( x, "w:val", TRUE );
        else if (xml_is( x, "w:keepLines" )) p->keep_lines = xml_attr_bool( x, "w:val", TRUE );
        else if (xml_is( x, "w:pageBreakBefore" )) p->page_break = xml_attr_bool( x, "w:val", TRUE );
        else if (xml_is( x, "w:outlineLvl" )) p->outline = xml_attr_int( x, "w:val", UNSET );
        else if (xml_is( x, "w:numPr" ))
        {
            int d = x->depth;
            if (xml_enter( x )) for (;;)
            {
                enum xml_token t2 = xml_next( x );
                if (t2 == XML_END || (t2 == XML_CLOSE && x->depth < d)) break;
                if (t2 != XML_OPEN) continue;
                if (xml_is( x, "w:ilvl" )) p->ilvl = xml_attr_int( x, "w:val", 0 );
                else if (xml_is( x, "w:numId" )) p->num_id = xml_attr_int( x, "w:val", 0 );
            }
        }
        else if (xml_is( x, "w:rPr" ) && mark) read_rpr( x, mark );
        else if (xml_is( x, "w:sectPr" ))
        {
            char type[32] = "";
            int d = x->depth;
            if (xml_enter( x )) for (;;)
            {
                enum xml_token t2 = xml_next( x );
                if (t2 == XML_END || (t2 == XML_CLOSE && x->depth < d)) break;
                if (t2 == XML_OPEN && xml_is( x, "w:type" )) xml_attr( x, "w:val", type, sizeof(type) );
            }
            if (section_break && strcmp( type, "continuous" )) *section_break = TRUE;
        }
        else if (!x->empty) xml_skip( x );
    }
}

/**********************************************************************
 *          Styles, numbering, theme
 */

struct style_def
{
    char id[64], based[64];
    int type;                           /* 1 paragraph, 2 character, 3 table */
    struct pprops p;
    struct rprops r;
    BOOL borders;                       /* a table style that draws lines */
    BOOL is_default;
};

struct lvl
{
    int fmt;                            /* 0 bullet, 1 decimal, 2 lower letter, 3 upper letter, 4 lower roman, 5 upper roman, 6 none */
    int start, left, hanging;
    int paren;
};

struct abstract_num { int id; struct lvl lvl[9]; };
struct num_def { int id, abstract; int start[9]; };

struct reader
{
    struct zip *zip;
    char base[128];                     /* the folder of the main part, with its slash */
    struct { char id[64]; char target[512]; BOOL external; } *rels;
    int rel_count;
    struct style_def *styles;
    int style_count;
    struct abstract_num *abstracts;
    int abstract_count;
    struct num_def *nums;
    int num_count;
    char major[64], minor[64];
    struct rprops default_r;
    struct pprops default_p;
    BOOL table_borders_default;

    struct buffer fonts, colors, body;
    char font_names[256][64];
    int font_count;
    COLORREF color_list[256];
    int color_count;

    int field_depth, field_instr;       /* complex fields: skipping their instructions */
    char field_url[512];
    char link_url[512];
    struct buffer link_text;
    BOOL in_link;
    BOOL page_break_pending;
    BOOL in_table;
    int cell_shade;                     /* the fill of the cell being read, -1: none */
    struct page_setup *setup;
    struct docx_keep *keep;
};

static struct style_def *find_style( struct reader *rd, const char *id )
{
    for (int i = 0; i < rd->style_count; i++)
        if (!strcmp( rd->styles[i].id, id )) return &rd->styles[i];
    return NULL;
}

static struct style_def *default_style( struct reader *rd, int type )
{
    for (int i = 0; i < rd->style_count; i++)
        if (rd->styles[i].type == type && rd->styles[i].is_default) return &rd->styles[i];
    return NULL;
}

/* a style with all it is based on, the base first */
static void resolve_style( struct reader *rd, const char *id, struct pprops *p, struct rprops *r, int depth )
{
    struct style_def *s = find_style( rd, id );

    if (!s || depth > 16) return;
    if (s->based[0]) resolve_style( rd, s->based, p, r, depth + 1 );
    if (p) pp_merge( p, &s->p );
    if (r) rp_merge( r, &s->r );
}

static void read_styles( struct reader *rd, const char *data, size_t len )
{
    struct xml x;
    enum xml_token t;
    char v[64];

    xml_init( &x, data, len );
    while ((t = xml_next( &x )) != XML_END)
    {
        if (t != XML_OPEN) continue;
        if (xml_is( &x, "w:rPrDefault" ) || xml_is( &x, "w:pPrDefault" ))
        {
            int d = x.depth;
            while ((t = xml_next( &x )) != XML_END && !(t == XML_CLOSE && x.depth < d))
            {
                if (t != XML_OPEN) continue;
                if (xml_is( &x, "w:rPr" )) read_rpr( &x, &rd->default_r );
                else if (xml_is( &x, "w:pPr" )) read_ppr( &x, &rd->default_p, NULL, NULL );
            }
        }
        else if (xml_is( &x, "w:style" ))
        {
            struct style_def *s;
            int d = x.depth;

            if (!(rd->style_count % 64))
            {
                void *n = realloc( rd->styles, (rd->style_count + 64) * sizeof(*rd->styles) );
                if (!n) break;
                rd->styles = n;
            }
            s = &rd->styles[rd->style_count++];
            memset( s, 0, sizeof(*s) );
            pp_init( &s->p );
            rp_init( &s->r );
            xml_attr( &x, "w:styleId", s->id, sizeof(s->id) );
            xml_attr( &x, "w:type", v, sizeof(v) );
            s->type = !strcmp( v, "paragraph" ) ? 1 : !strcmp( v, "character" ) ? 2 : !strcmp( v, "table" ) ? 3 : 4;
            s->is_default = xml_attr_bool( &x, "w:default", FALSE );
            while ((t = xml_next( &x )) != XML_END && !(t == XML_CLOSE && x.depth < d))
            {
                if (t != XML_OPEN || x.depth != d + 1) continue;
                if (xml_is( &x, "w:basedOn" )) xml_attr( &x, "w:val", s->based, sizeof(s->based) );
                else if (xml_is( &x, "w:rPr" )) read_rpr( &x, &s->r );
                else if (xml_is( &x, "w:pPr" )) read_ppr( &x, &s->p, NULL, NULL );
                else if (xml_is( &x, "w:tblPr" ))
                {
                    int dd = x.depth;
                    while ((t = xml_next( &x )) != XML_END && !(t == XML_CLOSE && x.depth < dd))
                        if (t == XML_OPEN && x.depth > dd + 1 && xml_attr( &x, "w:val", v, sizeof(v) ) &&
                            strcmp( v, "nil" ) && strcmp( v, "none" ))
                            s->borders = TRUE;
                }
                else if (!x.empty) xml_skip( &x );
            }
        }
    }
    xml_free( &x );
}

static int number_format( const char *v )
{
    if (!strcmp( v, "bullet" )) return 0;
    if (!strcmp( v, "lowerLetter" )) return 2;
    if (!strcmp( v, "upperLetter" )) return 3;
    if (!strcmp( v, "lowerRoman" )) return 4;
    if (!strcmp( v, "upperRoman" )) return 5;
    if (!strcmp( v, "none" )) return 6;
    return 1;
}

static void read_numbering( struct reader *rd, const char *data, size_t len )
{
    struct xml x;
    enum xml_token t;
    char v[64];

    xml_init( &x, data, len );
    while ((t = xml_next( &x )) != XML_END)
    {
        if (t != XML_OPEN) continue;
        if (xml_is( &x, "w:abstractNum" ))
        {
            struct abstract_num *a;
            int d = x.depth, level = 0;

            if (!(rd->abstract_count % 32))
            {
                void *n = realloc( rd->abstracts, (rd->abstract_count + 32) * sizeof(*rd->abstracts) );
                if (!n) break;
                rd->abstracts = n;
            }
            a = &rd->abstracts[rd->abstract_count++];
            memset( a, 0, sizeof(*a) );
            a->id = xml_attr_int( &x, "w:abstractNumId", 0 );
            for (int i = 0; i < 9; i++)
            {
                a->lvl[i].fmt = 1;
                a->lvl[i].start = 1;
                a->lvl[i].left = 720 * (i + 1);
                a->lvl[i].hanging = 360;
            }
            while ((t = xml_next( &x )) != XML_END && !(t == XML_CLOSE && x.depth < d))
            {
                if (t != XML_OPEN) continue;
                if (xml_is( &x, "w:lvl" )) level = min( max( xml_attr_int( &x, "w:ilvl", 0 ), 0 ), 8 );
                else if (xml_is( &x, "w:numFmt" ))
                {
                    xml_attr( &x, "w:val", v, sizeof(v) );
                    a->lvl[level].fmt = number_format( v );
                }
                else if (xml_is( &x, "w:start" )) a->lvl[level].start = xml_attr_int( &x, "w:val", 1 );
                else if (xml_is( &x, "w:lvlText" ))
                {
                    xml_attr( &x, "w:val", v, sizeof(v) );
                    a->lvl[level].paren = strchr( v, ')' ) != NULL;
                }
                else if (xml_is( &x, "w:ind" ))
                {
                    int left = xml_attr_int( &x, "w:left", UNSET );
                    if (left == UNSET) left = xml_attr_int( &x, "w:start", UNSET );
                    if (left != UNSET) a->lvl[level].left = left;
                    a->lvl[level].hanging = xml_attr_int( &x, "w:hanging", a->lvl[level].hanging );
                }
            }
        }
        else if (xml_is( &x, "w:num" ))
        {
            struct num_def *n;
            int d = x.depth, level = 0;

            if (!(rd->num_count % 32))
            {
                void *p = realloc( rd->nums, (rd->num_count + 32) * sizeof(*rd->nums) );
                if (!p) break;
                rd->nums = p;
            }
            n = &rd->nums[rd->num_count++];
            memset( n, 0, sizeof(*n) );
            n->id = xml_attr_int( &x, "w:numId", 0 );
            for (int i = 0; i < 9; i++) n->start[i] = UNSET;
            while ((t = xml_next( &x )) != XML_END && !(t == XML_CLOSE && x.depth < d))
            {
                if (t != XML_OPEN) continue;
                if (xml_is( &x, "w:abstractNumId" )) n->abstract = xml_attr_int( &x, "w:val", 0 );
                else if (xml_is( &x, "w:lvlOverride" )) level = min( max( xml_attr_int( &x, "w:ilvl", 0 ), 0 ), 8 );
                else if (xml_is( &x, "w:startOverride" )) n->start[level] = xml_attr_int( &x, "w:val", 1 );
            }
        }
    }
    xml_free( &x );
}

static const struct lvl *find_level( struct reader *rd, int num_id, int ilvl, int *start )
{
    ilvl = min( max( ilvl, 0 ), 8 );
    for (int i = 0; i < rd->num_count; i++)
    {
        if (rd->nums[i].id != num_id) continue;
        for (int j = 0; j < rd->abstract_count; j++)
            if (rd->abstracts[j].id == rd->nums[i].abstract)
            {
                *start = rd->nums[i].start[ilvl] != UNSET ? rd->nums[i].start[ilvl] : rd->abstracts[j].lvl[ilvl].start;
                return &rd->abstracts[j].lvl[ilvl];
            }
    }
    return NULL;
}

static void read_theme( struct reader *rd, const char *data, size_t len )
{
    struct xml x;
    enum xml_token t;
    int which = 0;

    xml_init( &x, data, len );
    while ((t = xml_next( &x )) != XML_END)
    {
        if (t != XML_OPEN) continue;
        if (!strcmp( x.name, "majorFont" )) which = 1;
        else if (!strcmp( x.name, "minorFont" )) which = 2;
        else if (!strcmp( x.name, "latin" ) && which)
        {
            xml_attr( &x, "typeface", which == 1 ? rd->major : rd->minor, 64 );
            which = 0;
        }
    }
    xml_free( &x );
}

static void read_rels( struct reader *rd, const char *data, size_t len )
{
    struct xml x;
    enum xml_token t;
    char mode[32];

    xml_init( &x, data, len );
    while ((t = xml_next( &x )) != XML_END)
    {
        if (t != XML_OPEN || strcmp( x.name, "Relationship" )) continue;
        if (!(rd->rel_count % 32))
        {
            void *n = realloc( rd->rels, (rd->rel_count + 32) * sizeof(*rd->rels) );
            if (!n) break;
            rd->rels = n;
        }
        xml_attr( &x, "Id", rd->rels[rd->rel_count].id, sizeof(rd->rels[0].id) );
        xml_attr( &x, "Target", rd->rels[rd->rel_count].target, sizeof(rd->rels[0].target) );
        xml_attr( &x, "TargetMode", mode, sizeof(mode) );
        rd->rels[rd->rel_count].external = !strcmp( mode, "External" );
        rd->rel_count++;
    }
    xml_free( &x );
}

static const char *rel_target( struct reader *rd, const char *id, BOOL *external )
{
    for (int i = 0; i < rd->rel_count; i++)
        if (!strcmp( rd->rels[i].id, id ))
        {
            if (external) *external = rd->rels[i].external;
            return rd->rels[i].target;
        }
    return NULL;
}

/* a part named by a relationship of the main part */
static BYTE *read_related( struct reader *rd, const char *target, size_t *len )
{
    char path[640];

    if (target[0] == '/') lstrcpynA( path, target + 1, sizeof(path) );
    else
    {
        char *p;
        snprintf( path, sizeof(path), "%s%s", rd->base, target );
        /* "../media/x.png" */
        while ((p = strstr( path, "/../" )))
        {
            char *start = p;
            while (start > path && start[-1] != '/') start--;
            memmove( start, p + 4, strlen( p + 4 ) + 1 );
        }
    }
    return zip_read( rd->zip, path, len );
}

/**********************************************************************
 *          RTF out
 */

static int font_index( struct reader *rd, const char *name )
{
    if (!strcmp( name, "+major" )) name = rd->major[0] ? rd->major : "Calibri Light";
    else if (!strcmp( name, "+minor" )) name = rd->minor[0] ? rd->minor : "Calibri";
    if (!name[0]) name = rd->minor[0] ? rd->minor : "Calibri";
    for (int i = 0; i < rd->font_count; i++)
        if (!strcmp( rd->font_names[i], name )) return i;
    if (rd->font_count >= ARRAY_SIZE(rd->font_names)) return 0;
    lstrcpynA( rd->font_names[rd->font_count], name, 64 );
    return rd->font_count++;
}

static int color_index( struct reader *rd, COLORREF c )
{
    for (int i = 0; i < rd->color_count; i++)
        if (rd->color_list[i] == c) return i + 1;
    if (rd->color_count >= ARRAY_SIZE(rd->color_list)) return 0;
    rd->color_list[rd->color_count++] = c;
    return rd->color_count;
}

/* UTF-8 text as RTF */
static void rtf_text( struct buffer *out, const char *utf8, size_t len )
{
    int n = MultiByteToWideChar( CP_UTF8, 0, utf8, (int)len, NULL, 0 );
    WCHAR *w = malloc( (n + 1) * sizeof(WCHAR) );

    if (!w) return;
    MultiByteToWideChar( CP_UTF8, 0, utf8, (int)len, w, n );
    for (int i = 0; i < n; i++)
    {
        WCHAR c = w[i];

        if (c == '\\' || c == '{' || c == '}') buf_printf( out, "\\%c", c );
        else if (c == '\t') buf_str( out, "\\tab " );
        else if (c == '\n' || c == '\r') buf_str( out, "\\line " );
        else if (c >= 0x20 && c < 0x80) buf_add( out, &c, 1 );
        else if (c < 0x20) continue;
        else if (c >= 0xf000 && c <= 0xf0ff) buf_printf( out, "\\u%d?", (short)(0x2022) );   /* a symbol font's bullet */
        else buf_printf( out, "\\u%d?", (short)c );
    }
    free( w );
}

static void rtf_char_props( struct reader *rd, struct buffer *out, const struct rprops *r )
{
    buf_printf( out, "\\f%d\\fs%d", font_index( rd, r->font ), r->sz != UNSET ? r->sz : 22 );
    if (r->b == 1) buf_str( out, "\\b" );
    if (r->i == 1) buf_str( out, "\\i" );
    if (r->strike == 1) buf_str( out, "\\strike" );
    if (r->caps == 1) buf_str( out, "\\caps" );
    if (r->smallcaps == 1) buf_str( out, "\\scaps" );
    switch (r->u == UNSET ? 0 : r->u)
    {
    case 1: case 5: buf_str( out, "\\ul" ); break;
    case 2: buf_str( out, "\\uldb" ); break;
    case 3: buf_str( out, "\\uld" ); break;
    case 4: buf_str( out, "\\ulw" ); break;
    }
    if (r->va == 1) buf_str( out, "\\super" );
    else if (r->va == 2) buf_str( out, "\\sub" );
    if (r->color != UNSET && r->color >= 0) buf_printf( out, "\\cf%d", color_index( rd, r->color ) );
    if (r->highlight != UNSET && r->highlight >= 0) buf_printf( out, "\\highlight%d", color_index( rd, r->highlight ) );
    else if (r->shade != UNSET && r->shade >= 0) buf_printf( out, "\\highlight%d", color_index( rd, r->shade ) );
    if (r->spacing != UNSET && r->spacing) buf_printf( out, "\\expndtw%d", r->spacing );
    buf_str( out, " " );
}

/* the gallery's styles keep their places; any other is a number from 20 */
static int style_number( struct reader *rd, const char *id )
{
    if (!id[0]) return 1;
    for (int i = 0; i < STYLE_COUNT; i++)
    {
        char name[64];
        WideCharToMultiByte( CP_UTF8, 0, styles[i].docx_id, -1, name, sizeof(name), NULL, NULL );
        if (!_stricmp( name, id )) return i + 1;
    }
    if (!rd->keep) return 0;
    for (int i = 0; i < rd->keep->style_count; i++)
        if (!strcmp( rd->keep->style_ids[i], id )) return 20 + i;
    if (!(rd->keep->style_count % 32))
    {
        void *n = realloc( rd->keep->style_ids, (rd->keep->style_count + 32) * sizeof(char *) );
        if (!n) return 0;
        rd->keep->style_ids = n;
    }
    rd->keep->style_ids[rd->keep->style_count] = _strdup( id );
    return 20 + rd->keep->style_count++;
}

/**********************************************************************
 *          The document part
 */

struct para_state
{
    struct pprops p;
    struct rprops base;             /* of the paragraph's style */
    struct rprops mark;             /* of its mark */
    struct buffer runs;
    BOOL has_content, page_break, section_break;
};

static void emit_paragraph( struct reader *rd, struct para_state *ps, struct buffer *out, BOOL last_in_cell );

static void picture( struct reader *rd, struct buffer *out, const char *rel, int width_emu, int height_emu )
{
    const char *target = rel_target( rd, rel, NULL );
    BYTE *data;
    size_t len;
    int w = width_emu / 635, h = height_emu / 635;   /* twips */

    if (!target || !(data = read_related( rd, target, &len ))) return;
    if (len > 4 && *(const DWORD *)data == 1 && len > 44 && *(const DWORD *)(data + 40) == 0x464d4520)
    {
        /* an EMF: as it is */
        buf_printf( out, "{\\pict\\emfblip\\picwgoal%d\\pichgoal%d\n", w, h );
        hex_bytes( out, data, len );
        buf_str( out, "}" );
    }
    else if (len > 22 && *(const DWORD *)data == 0x9ac6cdd7)
    {
        /* a placeable WMF: its header off */
        buf_printf( out, "{\\pict\\wmetafile8\\picw%d\\pich%d\\picwgoal%d\\pichgoal%d\n",
                    MulDiv( w, 2540, 1440 ), MulDiv( h, 2540, 1440 ), w, h );
        hex_bytes( out, data + 22, len - 22 );
        buf_str( out, "}" );
    }
    else picture_rtf( out, data, len, w, h );
    free( data );
}

/* a drawing: the first picture in it, at its extent */
static void read_drawing( struct reader *rd, struct xml *x, struct buffer *out )
{
    int d = x->depth, cx = 0, cy = 0;
    char embed[64] = "";
    enum xml_token t;

    if (xml_enter( x )) while ((t = xml_next( x )) != XML_END && !(t == XML_CLOSE && x->depth < d))
    {
        if (t != XML_OPEN) continue;
        if (!strcmp( x->name, "extent" ) && !cx)
        {
            cx = xml_attr_int( x, "cx", 0 );
            cy = xml_attr_int( x, "cy", 0 );
        }
        else if (!strcmp( x->name, "blip" ) && !embed[0]) xml_attr( x, "r:embed", embed, sizeof(embed) );
    }
    if (embed[0] && cx > 0 && cy > 0) picture( rd, out, embed, cx, cy );
}

/* VML: v:imagedata in a v:shape whose style says its size */
static void read_vml( struct reader *rd, struct xml *x, struct buffer *out )
{
    int d = x->depth, w = 0, h = 0;
    char id[64] = "", style[512];
    enum xml_token t;

    if (xml_enter( x )) while ((t = xml_next( x )) != XML_END && !(t == XML_CLOSE && x->depth < d))
    {
        if (t != XML_OPEN) continue;
        if (!strcmp( x->name, "shape" ) && xml_attr( x, "style", style, sizeof(style) ))
        {
            char *p;
            if ((p = strstr( style, "width:" ))) w = (int)(atof( p + 6 ) * 12700);
            if ((p = strstr( style, "height:" ))) h = (int)(atof( p + 7 ) * 12700);
        }
        else if (!strcmp( x->name, "imagedata" ) && !id[0]) xml_attr( x, "r:id", id, sizeof(id) );
    }
    if (id[0] && w > 0 && h > 0) picture( rd, out, id, w, h );
}

static void read_paragraph( struct reader *rd, struct xml *x, struct buffer *out, BOOL last_in_cell );
static void read_table( struct reader *rd, struct xml *x, struct buffer *out );

static void flush_link( struct reader *rd )
{
    if (rd->in_link && rd->link_url[0] && rd->link_text.len)
    {
        int n = MultiByteToWideChar( CP_UTF8, 0, rd->link_text.data, (int)rd->link_text.len, NULL, 0 );
        WCHAR *w = malloc( (n + 1) * sizeof(WCHAR) );
        if (w)
        {
            MultiByteToWideChar( CP_UTF8, 0, rd->link_text.data, (int)rd->link_text.len, w, n );
            w[n] = 0;
            link_register( w, rd->link_url );
            free( w );
        }
    }
    rd->link_text.len = 0;
}

static void read_run( struct reader *rd, struct xml *x, struct para_state *ps, struct buffer *out )
{
    struct rprops r = ps->base;
    struct rprops direct;
    int d = x->depth;
    enum xml_token t;
    BOOL started = FALSE;

    rp_init( &direct );
    if (xml_enter( x )) while ((t = xml_next( x )) != XML_END && !(t == XML_CLOSE && x->depth < d))
    {
        if (t != XML_OPEN || x->depth != d + 1)
        {
            if (t == XML_OPEN && !x->empty) xml_skip( x );
            continue;
        }
        if (xml_is( x, "w:rPr" ))
        {
            read_rpr( x, &direct );
            if (direct.rstyle[0]) resolve_style( rd, direct.rstyle, NULL, &r, 0 );
            rp_merge( &r, &direct );
            if (rd->in_link)
            {
                r.color = LINK_COLOR;   /* one colour: the one docx.c knows links by */
                r.u = 1;
            }
            continue;
        }
        if (xml_is( x, "w:fldChar" ))
        {
            char type[32];
            xml_attr( x, "w:fldCharType", type, sizeof(type) );
            if (!strcmp( type, "begin" ))
            {
                rd->field_depth++;
                rd->field_instr = rd->field_depth;
                rd->field_url[0] = 0;
            }
            else if (!strcmp( type, "separate" ))
            {
                if (rd->field_instr == rd->field_depth) rd->field_instr = 0;
                if (rd->field_url[0])
                {
                    lstrcpynA( rd->link_url, rd->field_url, sizeof(rd->link_url) );
                    rd->in_link = TRUE;
                    rd->link_text.len = 0;
                }
            }
            else if (!strcmp( type, "end" ))
            {
                if (rd->field_depth) rd->field_depth--;
                if (rd->field_instr > rd->field_depth) rd->field_instr = 0;
                if (rd->in_link && rd->field_url[0])
                {
                    flush_link( rd );
                    rd->in_link = FALSE;
                    rd->field_url[0] = 0;
                }
            }
            if (!x->empty) xml_skip( x );
            else xml_next( x );
            continue;
        }
        if (xml_is( x, "w:instrText" ))
        {
            int dd = x->depth;
            if (xml_enter( x )) while ((t = xml_next( x )) != XML_END && !(t == XML_CLOSE && x->depth < dd))
            {
                if (t == XML_TEXT && x->text.data)
                {
                    char *h = strstr( x->text.data, "HYPERLINK" ), *q;
                    if (h && (q = strchr( h, '"' )))
                    {
                        char *e = strchr( q + 1, '"' );
                        if (e) { *e = 0; lstrcpynA( rd->field_url, q + 1, sizeof(rd->field_url) ); }
                    }
                }
            }
            continue;
        }
        if (rd->field_instr)
        {
            if (!x->empty) xml_skip( x );
            else xml_next( x );
            continue;
        }

        if (!started)
        {
            /* riched20 fills no cells: their colour behind their text */
            if (rd->in_link)
            {
                r.color = LINK_COLOR;
                r.u = 1;
            }
            buf_str( out, "{" );
            rtf_char_props( rd, out, &r );
            started = TRUE;
        }
        if (xml_is( x, "w:t" ))
        {
            int dd = x->depth;
            if (xml_enter( x )) while ((t = xml_next( x )) != XML_END && !(t == XML_CLOSE && x->depth < dd))
                if (t == XML_TEXT && x->text.len)
                {
                    if (r.vanish != 1) rtf_text( out, x->text.data, x->text.len );
                    if (rd->in_link) buf_add( &rd->link_text, x->text.data, x->text.len );
                    ps->has_content = TRUE;
                }
            continue;
        }
        if (xml_is( x, "w:tab" ) || xml_is( x, "w:ptab" )) { buf_str( out, "\\tab " ); ps->has_content = TRUE; }
        else if (xml_is( x, "w:br" ) || xml_is( x, "w:cr" ))
        {
            char type[32] = "";
            xml_attr( x, "w:type", type, sizeof(type) );
            if (!strcmp( type, "page" ) || !strcmp( type, "column" ))
            {
                if (ps->has_content)
                {
                    /* the paragraph so far, and the rest on a new page */
                    buf_str( out, "}" );
                    ps->runs = *out;
                    emit_paragraph( rd, ps, &rd->body, FALSE );
                    memset( out, 0, sizeof(*out) );
                    ps->page_break = TRUE;
                    ps->has_content = FALSE;
                    buf_str( out, "{" );
                    rtf_char_props( rd, out, &r );
                }
                else ps->page_break = TRUE;
            }
            else { buf_str( out, "\\line " ); ps->has_content = TRUE; }
        }
        else if (xml_is( x, "w:noBreakHyphen" )) buf_str( out, "\\u8209?" );
        else if (xml_is( x, "w:sym" ))
        {
            char c[16];
            unsigned int code = 0;
            xml_attr( x, "w:char", c, sizeof(c) );
            sscanf( c, "%x", &code );
            if (code >= 0xf000) code -= 0xf000;
            buf_printf( out, "\\u%d?", (short)(code < 0x20 ? 0x2022 : code == 0xb7 ? 0x2022 : code) );
            ps->has_content = TRUE;
        }
        else if (xml_is( x, "w:drawing" ))
        {
            read_drawing( rd, x, out );
            ps->has_content = TRUE;
            continue;
        }
        else if (xml_is( x, "w:pict" ) || xml_is( x, "w:object" ))
        {
            read_vml( rd, x, out );
            ps->has_content = TRUE;
            continue;
        }
        else if (xml_is( x, "mc:AlternateContent" ))
        {
            /* the choice's drawing, never the fallback's */
            int dd = x->depth;
            while ((t = xml_next( x )) != XML_END && !(t == XML_CLOSE && x->depth < dd))
            {
                if (t != XML_OPEN) continue;
                if (xml_is( x, "mc:Fallback" )) { if (!x->empty) xml_skip( x ); else xml_next( x ); continue; }
                if (xml_is( x, "w:drawing" )) { read_drawing( rd, x, out ); ps->has_content = TRUE; }
                else if (xml_is( x, "w:pict" )) { read_vml( rd, x, out ); ps->has_content = TRUE; }
            }
            continue;
        }
        else if (xml_is( x, "w:footnoteReference" ) || xml_is( x, "w:endnoteReference" ))
        {
            buf_printf( out, "{\\super %d}", xml_attr_int( x, "w:id", 0 ) );
            ps->has_content = TRUE;
        }
        if (!x->empty) xml_skip( x );
        else xml_next( x );
    }
    if (started) buf_str( out, "}" );
}

/* the children of a paragraph that hold runs */
static void read_inline( struct reader *rd, struct xml *x, struct para_state *ps, int depth )
{
    enum xml_token t;

    while ((t = xml_next( x )) != XML_END && !(t == XML_CLOSE && x->depth < depth))
    {
        if (t != XML_OPEN) continue;
        if (xml_is( x, "w:r" )) read_run( rd, x, ps, &ps->runs );
        else if (xml_is( x, "w:hyperlink" ))
        {
            char id[64];
            BOOL external = FALSE;
            const char *url = NULL;

            if (xml_attr( x, "r:id", id, sizeof(id) )) url = rel_target( rd, id, &external );
            if (url)
            {
                lstrcpynA( rd->link_url, url, sizeof(rd->link_url) );
                rd->in_link = TRUE;
                rd->link_text.len = 0;
            }
            if (!x->empty) read_inline( rd, x, ps, x->depth );
            else xml_next( x );
            if (url) flush_link( rd );
            rd->in_link = FALSE;
        }
        else if (xml_is( x, "w:fldSimple" ) || xml_is( x, "w:ins" ) || xml_is( x, "w:moveTo" ) ||
                 xml_is( x, "w:smartTag" ) || xml_is( x, "w:customXml" ) || xml_is( x, "w:sdtContent" ) ||
                 xml_is( x, "w:dir" ) || xml_is( x, "w:bdo" ))
        {
            if (!x->empty) read_inline( rd, x, ps, x->depth );
            else xml_next( x );
        }
        else if (xml_is( x, "w:sdt" ))
        {
            if (!x->empty) read_inline( rd, x, ps, x->depth );
            else xml_next( x );
        }
        else if (xml_is( x, "m:oMath" ) || xml_is( x, "m:oMathPara" ))
        {
            /* an equation: its text */
            int dd = x->depth;
            if (xml_enter( x )) while ((t = xml_next( x )) != XML_END && !(t == XML_CLOSE && x->depth < dd))
                if (t == XML_TEXT && x->text.len && !strcmp( x->name, "t" ))
                {
                    buf_str( &ps->runs, "{" );
                    rtf_char_props( rd, &ps->runs, &ps->base );
                    rtf_text( &ps->runs, x->text.data, x->text.len );
                    buf_str( &ps->runs, "}" );
                    ps->has_content = TRUE;
                }
        }
        else if (!x->empty) xml_skip( x );
        else xml_next( x );
    }
}

static void emit_paragraph( struct reader *rd, struct para_state *ps, struct buffer *out, BOOL last_in_cell )
{
    struct pprops *p = &ps->p;
    int left = p->left != UNSET ? p->left : 0, first = p->first != UNSET ? p->first : 0;
    int number = style_number( rd, p->style );

    buf_str( out, "\\pard" );
    if (rd->in_table) buf_str( out, "\\intbl" );
    if (number) buf_printf( out, "\\s%d", number );
    if (p->num_id > 0)
    {
        int start = 1;
        const struct lvl *l = find_level( rd, p->num_id, p->ilvl != UNSET ? p->ilvl : 0, &start );

        if (l && l->fmt != 6)
        {
            int f = font_index( rd, ps->base.font );

            if (p->left == UNSET) left = l->left;
            if (p->first == UNSET) first = -l->hanging;
            if (l->fmt == 0)
                buf_printf( out, "{\\pntext\\f%d\\u8226?\\tab}{\\*\\pn\\pnlvlblt\\pnf%d\\pnindent%d{\\pntxtb\\u8226?}}",
                            f, f, l->hanging );
            else
            {
                static const char *const kinds[] = { "", "\\pndec", "\\pnlcltr", "\\pnucltr", "\\pnlcrm", "\\pnucrm" };
                buf_printf( out, "{\\pntext\\f%d %d.\\tab}{\\*\\pn\\pnlvlbody\\pnf%d\\pnindent%d\\pnstart%d%s{\\pntxta%s}}",
                            f, start, f, l->hanging, start, kinds[l->fmt], l->paren ? ")" : "." );
            }
        }
    }
    switch (p->jc)
    {
    case 1: buf_str( out, "\\qc" ); break;
    case 2: buf_str( out, "\\qr" ); break;
    case 3: buf_str( out, "\\qj" ); break;
    }
    if (left) buf_printf( out, "\\li%d", left );
    if (first) buf_printf( out, "\\fi%d", first );
    if (p->right != UNSET && p->right) buf_printf( out, "\\ri%d", p->right );
    if (p->before != UNSET && p->before) buf_printf( out, "\\sb%d", p->before );
    if (p->after != UNSET && p->after) buf_printf( out, "\\sa%d", p->after );
    if (p->line != UNSET && p->line > 0)
    {
        if (p->line_rule == 1) buf_printf( out, "\\sl-%d\\slmult0", p->line );
        else if (p->line_rule == 2) buf_printf( out, "\\sl%d\\slmult0", p->line );
        else if (p->line != 240) buf_printf( out, "\\sl%d\\slmult1", p->line );
    }
    for (int i = 0; p->tab_count != UNSET && i < p->tab_count; i++)
    {
        static const char *const kinds[] = { "", "\\tqc", "\\tqr", "\\tqdec" };
        static const char *const leaders[] = { "", "\\tldot", "\\tlhyph", "\\tlul" };
        buf_printf( out, "%s%s\\tx%d", kinds[(p->tabs[i] >> 24) & 3], leaders[(p->tabs[i] >> 28) & 3], p->tabs[i] & 0xffffff );
    }
    if (p->keep_next == 1) buf_str( out, "\\keepn" );
    if (p->keep_lines == 1) buf_str( out, "\\keep" );
    if (p->page_break == 1 || ps->page_break || rd->page_break_pending) buf_str( out, "\\pagebb" );
    rd->page_break_pending = FALSE;
    ps->page_break = FALSE;
    buf_str( out, " " );
    if (ps->runs.len) buf_add( out, ps->runs.data, ps->runs.len );
    buf_str( out, "{" );
    rtf_char_props( rd, out, &ps->mark );
    buf_str( out, last_in_cell ? "\\cell}\n" : "\\par}\n" );
    buf_free( &ps->runs );
    if (ps->section_break) rd->page_break_pending = TRUE;
}

static void read_paragraph( struct reader *rd, struct xml *x, struct buffer *out, BOOL last_in_cell )
{
    struct para_state ps = { 0 };
    struct pprops direct;
    struct rprops mark_direct;
    int d = x->depth;
    enum xml_token t;
    BOOL ppr_done = FALSE;

    pp_init( &ps.p );
    pp_init( &direct );
    rp_init( &ps.base );
    rp_init( &mark_direct );
    BOOL empty = !xml_enter( x );

    /* the defaults, then the style, then what is set on the paragraph */
    pp_merge( &ps.p, &rd->default_p );
    rp_merge( &ps.base, &rd->default_r );
    if (!empty)
    {
        /* w:pPr comes first */
        const char *save = x->p;
        int save_depth = x->depth;

        while ((t = xml_next( x )) != XML_END && !(t == XML_CLOSE && x->depth < d))
        {
            if (t == XML_OPEN && x->depth == d + 1 && xml_is( x, "w:pPr" ))
            {
                read_ppr( x, &direct, &mark_direct, &ps.section_break );
                ppr_done = TRUE;
            }
            break;
        }
        if (!ppr_done)
        {
            x->p = save;
            x->depth = save_depth;
            x->pending_close = FALSE;
        }
    }
    {
        const char *style = direct.style[0] ? direct.style : NULL;
        struct style_def *def = style ? find_style( rd, style ) : default_style( rd, 1 );

        if (def)
        {
            resolve_style( rd, def->id, &ps.p, &ps.base, 0 );
            lstrcpynA( ps.p.style, def->id, sizeof(ps.p.style) );
        }
        pp_merge( &ps.p, &direct );
        if (direct.style[0]) lstrcpynA( ps.p.style, direct.style, sizeof(ps.p.style) );
    }
    ps.mark = ps.base;
    rp_merge( &ps.mark, &mark_direct );
    if (!empty) read_inline( rd, x, &ps, d );
    emit_paragraph( rd, &ps, out, last_in_cell );
}

struct cell
{
    struct buffer content;
    int span, width;
    BOOL borders[4];            /* top, left, bottom, right */
    int colors[4];              /* of those lines, -1: the table's */
    int shade;                  /* the fill, -1: none */
    BOOL merged;                /* a vertical merge going on */
};

/* the block content of a cell, its paragraphs ending in \par, the last in \cell */
static void read_cell_content( struct reader *rd, struct xml *x, struct cell *c, int depth )
{
    enum xml_token t;
    size_t last_par = (size_t)-1;

    while ((t = xml_next( x )) != XML_END && !(t == XML_CLOSE && x->depth < depth))
    {
        if (t != XML_OPEN) continue;
        if (xml_is( x, "w:p" ))
        {
            last_par = c->content.len;
            read_paragraph( rd, x, &c->content, FALSE );
        }
        else if (xml_is( x, "w:tbl" ))
        {
            /* a table in a table: its cells' paragraphs, one after the other */
            int dd = x->depth;
            while ((t = xml_next( x )) != XML_END && !(t == XML_CLOSE && x->depth < dd))
                if (t == XML_OPEN && xml_is( x, "w:p" ))
                {
                    last_par = c->content.len;
                    read_paragraph( rd, x, &c->content, FALSE );
                }
        }
        else if (xml_is( x, "w:sdt" ) || xml_is( x, "w:sdtContent" ) || xml_is( x, "w:customXml" ))
        {
            if (!x->empty) read_cell_content( rd, x, c, x->depth );
            else xml_next( x );
        }
        else if (xml_is( x, "w:tcPr" ))
        {
            int dd = x->depth, side;
            char v[32];
            if (xml_enter( x )) while ((t = xml_next( x )) != XML_END && !(t == XML_CLOSE && x->depth < dd))
            {
                if (t != XML_OPEN) continue;
                if (xml_is( x, "w:gridSpan" )) c->span = max( 1, xml_attr_int( x, "w:val", 1 ) );
                else if (xml_is( x, "w:tcW" ))
                {
                    char type[16] = "";
                    xml_attr( x, "w:type", type, sizeof(type) );
                    if (!strcmp( type, "dxa" ) || !type[0]) c->width = xml_attr_int( x, "w:w", 0 );
                }
                else if (xml_is( x, "w:shd" ) && x->depth == dd + 1)
                {
                    int fill;
                    xml_attr( x, "w:fill", v, sizeof(v) );
                    fill = hex_color( v );
                    c->shade = fill >= 0 && fill != RGB( 255, 255, 255 ) ? fill : -1;
                }
                else if (xml_is( x, "w:vMerge" ))
                {
                    xml_attr( x, "w:val", v, sizeof(v) );
                    c->merged = strcmp( v, "restart" ) != 0;
                }
                else if ((side = xml_is( x, "w:top" ) ? 0 : xml_is( x, "w:left" ) || xml_is( x, "w:start" ) ? 1 :
                          xml_is( x, "w:bottom" ) ? 2 : xml_is( x, "w:right" ) || xml_is( x, "w:end" ) ? 3 : -1) >= 0)
                {
                    xml_attr( x, "w:val", v, sizeof(v) );
                    c->borders[side] = strcmp( v, "nil" ) && strcmp( v, "none" );
                    if (xml_attr( x, "w:color", v, sizeof(v) )) c->colors[side] = hex_color( v );
                }
            }
        }
        else if (!x->empty) xml_skip( x );
        else xml_next( x );
    }
    /* the cell's last paragraph ends it */
    if (last_par == (size_t)-1)
    {
        buf_str( &c->content, "\\pard\\intbl\\cell\n" );
        return;
    }
    {
        char *end = c->content.data + c->content.len - 7;   /* "\par}\n" */
        if (c->content.len >= 7 && !memcmp( end + 1, "\\par}\n", 6 ))
        {
            c->content.len = end + 1 - c->content.data;
            buf_str( &c->content, "\\cell}\n" );
        }
    }
}

static void read_table( struct reader *rd, struct xml *x, struct buffer *out )
{
    int d = x->depth, grid[64], grid_count = 0, indent = 0, row_index = 0;
    BOOL borders[6] = { 0 };     /* top, left, bottom, right, inside h, inside v */
    int colors[6] = { -1, -1, -1, -1, -1, -1 };
    BOOL any_borders = rd->table_borders_default;
    enum xml_token t;
    char v[64];
    BOOL was_in_table = rd->in_table;

    rd->in_table = TRUE;
    while ((t = xml_next( x )) != XML_END && !(t == XML_CLOSE && x->depth < d))
    {
        if (t != XML_OPEN || x->depth != d + 1) continue;
        if (xml_is( x, "w:tblPr" ))
        {
            int dd = x->depth;
            if (xml_enter( x )) while ((t = xml_next( x )) != XML_END && !(t == XML_CLOSE && x->depth < dd))
            {
                if (t != XML_OPEN) continue;
                if (xml_is( x, "w:tblStyle" ))
                {
                    struct style_def *s;
                    xml_attr( x, "w:val", v, sizeof(v) );
                    if ((s = find_style( rd, v )))
                    {
                        any_borders = s->borders;
                        for (int i = 0; i < 6; i++) borders[i] = s->borders;
                    }
                    else if (strstr( v, "Grid" )) { any_borders = TRUE; for (int i = 0; i < 6; i++) borders[i] = TRUE; }
                }
                else if (xml_is( x, "w:tblInd" )) indent = xml_attr_int( x, "w:w", 0 );
                else if (xml_is( x, "w:tblBorders" ))
                {
                    int ddd = x->depth;
                    if (xml_enter( x )) while ((t = xml_next( x )) != XML_END && !(t == XML_CLOSE && x->depth < ddd))
                    {
                        int side;
                        if (t != XML_OPEN) continue;
                        side = xml_is( x, "w:top" ) ? 0 : (xml_is( x, "w:left" ) || xml_is( x, "w:start" )) ? 1 :
                               xml_is( x, "w:bottom" ) ? 2 : (xml_is( x, "w:right" ) || xml_is( x, "w:end" )) ? 3 :
                               xml_is( x, "w:insideH" ) ? 4 : xml_is( x, "w:insideV" ) ? 5 : -1;
                        if (side < 0) continue;
                        xml_attr( x, "w:val", v, sizeof(v) );
                        borders[side] = strcmp( v, "nil" ) && strcmp( v, "none" );
                        if (borders[side]) any_borders = TRUE;
                        if (xml_attr( x, "w:color", v, sizeof(v) )) colors[side] = hex_color( v );
                    }
                }
            }
        }
        else if (xml_is( x, "w:tblGrid" ))
        {
            int dd = x->depth;
            if (xml_enter( x )) while ((t = xml_next( x )) != XML_END && !(t == XML_CLOSE && x->depth < dd))
                if (t == XML_OPEN && xml_is( x, "w:gridCol" ) && grid_count < ARRAY_SIZE(grid))
                    grid[grid_count++] = xml_attr_int( x, "w:w", 0 );
        }
        else if (xml_is( x, "w:tr" ))
        {
            struct cell cells[64];
            int count = 0, dd = x->depth, pos = indent, col = 0;

            memset( cells, 0, sizeof(cells) );
            while ((t = xml_next( x )) != XML_END && !(t == XML_CLOSE && x->depth < dd))
            {
                if (t != XML_OPEN || x->depth != dd + 1) continue;
                if (xml_is( x, "w:tc" ) && count < ARRAY_SIZE(cells))
                {
                    struct cell *c = &cells[count++];
                    c->span = 1;
                    for (int i = 0; i < 4; i++) c->borders[i] = -1, c->colors[i] = -1;
                    c->shade = -1;
                    rd->cell_shade = -1;
                    read_cell_content( rd, x, c, x->depth );
                    rd->cell_shade = -1;
                }
                else if (xml_is( x, "w:sdt" ) || xml_is( x, "w:customXml" )) { /* a cell inside: descend */ }
                else if (!x->empty) xml_skip( x );
            }
            if (!count) continue;
            /* the row's definition, then its cells */
            /* \pard first: the row's own hidden paragraph takes the props in
             * force, and those of the paragraph before (centred, spaced) are
             * not the row's */
            buf_printf( out, "\\pard\\plain\\trowd\\trgaph108\\trleft%d", indent );
            for (int i = 0; i < count; i++)
            {
                struct cell *c = &cells[i];
                int width = 0;
                BOOL side[4];

                for (int s = 0; s < c->span; s++) width += col + s < grid_count ? grid[col + s] : 0;
                if (!width) width = c->width ? c->width : 2000;
                col += c->span;
                pos += width;
                side[0] = row_index == 0 ? borders[0] : borders[4];
                side[1] = i == 0 ? borders[1] : borders[5];
                side[2] = borders[2] || borders[4];
                side[3] = i == count - 1 ? borders[3] : borders[5];
                if (c->merged) side[0] = FALSE;
                for (int s = 0; s < 4; s++)
                {
                    static const int table_side[4][2] = { { 0, 4 }, { 1, 5 }, { 2, 4 }, { 3, 5 } };
                    int color = c->colors[s];

                    if (c->borders[s] != (BOOL)-1) side[s] = c->borders[s];
                    if (color < 0) color = colors[table_side[s][0]] >= 0 ? colors[table_side[s][0]] : colors[table_side[s][1]];
                    if (!side[s]) continue;
                    buf_printf( out, "\\clbrdr%c\\brdrs\\brdrw10", "tlbr"[s] );
                    if (color >= 0) buf_printf( out, "\\brdrcf%d", color_index( rd, color ) );
                }
                if (c->shade >= 0) buf_printf( out, "\\clcbpat%d", color_index( rd, c->shade ) );
                buf_printf( out, "\\cellx%d", pos );
            }
            buf_str( out, "\n" );
            for (int i = 0; i < count; i++)
            {
                if (cells[i].content.len) buf_add( out, cells[i].content.data, cells[i].content.len );
                buf_free( &cells[i].content );
            }
            buf_str( out, "\\row\n" );
            row_index++;
        }
        else if (!x->empty) xml_skip( x );
    }
    (void)any_borders;
    rd->in_table = was_in_table;
}

static void read_section( struct reader *rd, struct xml *x, const char *start )
{
    int d = x->depth;
    enum xml_token t;

    while ((t = xml_next( x )) != XML_END && !(t == XML_CLOSE && x->depth < d))
    {
        if (t != XML_OPEN) continue;
        if (xml_is( x, "w:pgSz" ))
        {
            rd->setup->width = xml_attr_int( x, "w:w", rd->setup->width );
            rd->setup->height = xml_attr_int( x, "w:h", rd->setup->height );
        }
        else if (xml_is( x, "w:pgMar" ))
        {
            rd->setup->top = abs( xml_attr_int( x, "w:top", rd->setup->top ) );
            rd->setup->bottom = abs( xml_attr_int( x, "w:bottom", rd->setup->bottom ) );
            rd->setup->left = xml_attr_int( x, "w:left", rd->setup->left ) + xml_attr_int( x, "w:gutter", 0 );
            rd->setup->right = xml_attr_int( x, "w:right", rd->setup->right );
        }
    }
    if (rd->keep && start)
    {
        size_t len = x->p - start;
        free( rd->keep->sect_pr );
        if ((rd->keep->sect_pr = malloc( len + 1 )))
        {
            memcpy( rd->keep->sect_pr, start, len );
            rd->keep->sect_pr[len] = 0;
        }
    }
}

static void read_body( struct reader *rd, struct xml *x, int depth )
{
    enum xml_token t;

    while ((t = xml_next( x )) != XML_END && !(t == XML_CLOSE && x->depth < depth))
    {
        if (t != XML_OPEN) continue;
        if (xml_is( x, "w:p" )) read_paragraph( rd, x, &rd->body, FALSE );
        else if (xml_is( x, "w:tbl" )) read_table( rd, x, &rd->body );
        else if (xml_is( x, "w:sdt" ) || xml_is( x, "w:sdtContent" ) || xml_is( x, "w:customXml" ) ||
                 xml_is( x, "w:ins" ) || xml_is( x, "w:moveTo" ))
        {
            if (!x->empty) read_body( rd, x, x->depth );
            else xml_next( x );
        }
        else if (xml_is( x, "w:sectPr" ))
        {
            /* back to the '<' of the element */
            const char *start = x->p;
            while (start > x->attrs && *start != '<') start--;
            while (start > x->p - 4096 && *start != '<') start--;
            {
                const char *s = x->attrs;
                while (s > x->p - 8192 && *s != '<') s--;
                start = s;
            }
            if (x->empty)
            {
                xml_next( x );
                if (rd->keep)
                {
                    size_t len = x->p - start;
                    free( rd->keep->sect_pr );
                    if ((rd->keep->sect_pr = malloc( len + 1 )))
                    {
                        memcpy( rd->keep->sect_pr, start, len );
                        rd->keep->sect_pr[len] = 0;
                    }
                }
            }
            else read_section( rd, x, start );
        }
        else if (!x->empty) xml_skip( x );
        else xml_next( x );
    }
}

static void free_reader( struct reader *rd )
{
    free( rd->rels );
    free( rd->styles );
    free( rd->abstracts );
    free( rd->nums );
    buf_free( &rd->body );
    buf_free( &rd->link_text );
}

static BOOL find_main_part( struct zip *zip, char *name, int size )
{
    size_t len;
    BYTE *rels = zip_read( zip, "_rels/.rels", &len );
    struct xml x;
    enum xml_token t;
    BOOL found = FALSE;

    lstrcpynA( name, "word/document.xml", size );
    if (!rels) return zip_read( zip, name, &len ) != NULL;
    xml_init( &x, (char *)rels, len );
    while ((t = xml_next( &x )) != XML_END)
    {
        char type[256], target[256];
        if (t != XML_OPEN || strcmp( x.name, "Relationship" )) continue;
        xml_attr( &x, "Type", type, sizeof(type) );
        /* ".../relationships/officeDocument", not ".../officeDocument/2006/..." of the others */
        if (strlen( type ) < 15 || strcmp( type + strlen( type ) - 15, "/officeDocument" )) continue;
        xml_attr( &x, "Target", target, sizeof(target) );
        lstrcpynA( name, target[0] == '/' ? target + 1 : target, size );
        found = TRUE;
        break;
    }
    xml_free( &x );
    free( rels );
    return found;
}

char *docx_to_rtf( const BYTE *data, size_t size, size_t *rtf_len, struct page_setup *setup, struct docx_keep **keep_out )
{
    struct reader rd = { 0 };
    struct buffer out = { 0 };
    char path[256], *slash;
    size_t len;
    BYTE *part, *doc;
    struct xml x;
    enum xml_token t;
    struct docx_keep *keep;

    if (!(rd.zip = zip_open( data, size ))) return NULL;
    if (!find_main_part( rd.zip, path, sizeof(path) ) || !(doc = zip_read( rd.zip, path, &len )))
    {
        zip_close( rd.zip );
        return NULL;
    }
    /* an ODT is a ZIP too, but without this part */
    if (!(keep = calloc( 1, sizeof(*keep) )) || !(keep->data = malloc( size )))
    {
        free( keep );
        free( doc );
        zip_close( rd.zip );
        return NULL;
    }
    memcpy( keep->data, data, size );
    keep->len = size;
    zip_close( rd.zip );
    rd.zip = keep->zip = zip_open( keep->data, size );
    lstrcpynA( keep->main_part, path, sizeof(keep->main_part) );
    rd.keep = keep;
    rd.setup = setup;
    lstrcpynA( rd.base, path, sizeof(rd.base) );
    if ((slash = strrchr( rd.base, '/' ))) slash[1] = 0;
    else rd.base[0] = 0;
    rp_init( &rd.default_r );
    pp_init( &rd.default_p );
    rd.cell_shade = -1;

    /* the main part's relationships: styles, numbering, theme, pictures, links */
    {
        char rels_path[300];
        const char *name = strrchr( path, '/' ) ? strrchr( path, '/' ) + 1 : path;
        BYTE *rels;

        snprintf( rels_path, sizeof(rels_path), "%s_rels/%s.rels", rd.base, name );
        if ((rels = zip_read( rd.zip, rels_path, &len )))
        {
            read_rels( &rd, (char *)rels, len );
            free( rels );
        }
    }
    {
        char p2[300];
        snprintf( p2, sizeof(p2), "%stheme/theme1.xml", rd.base );
        if ((part = zip_read( rd.zip, p2, &len ))) { read_theme( &rd, (char *)part, len ); free( part ); }
        snprintf( p2, sizeof(p2), "%sstyles.xml", rd.base );
        if ((part = zip_read( rd.zip, p2, &len ))) { read_styles( &rd, (char *)part, len ); free( part ); keep->has_styles = TRUE; }
        snprintf( p2, sizeof(p2), "%snumbering.xml", rd.base );
        if ((part = zip_read( rd.zip, p2, &len ))) { read_numbering( &rd, (char *)part, len ); free( part ); }
    }
    if (rd.default_r.sz == UNSET) rd.default_r.sz = 20;   /* Word's own default, 10 pt */

    xml_init( &x, (char *)doc, len = strlen( (char *)doc ) );
    while ((t = xml_next( &x )) != XML_END)
    {
        if (t != XML_OPEN) continue;
        if (xml_is( &x, "w:document" ))
        {
            const char *s = x.attrs;
            while (s > (char *)doc && *s != '<') s--;
            if ((keep->root = malloc( x.p - s + 1 )))
            {
                memcpy( keep->root, s, x.p - s );
                keep->root[x.p - s] = 0;
            }
        }
        else if (xml_is( &x, "w:body" ))
        {
            read_body( &rd, &x, x.depth );
            break;
        }
    }
    xml_free( &x );
    free( doc );

    /* the header now that the fonts and colours are known */
    buf_str( &out, "{\\rtf1\\ansi\\ansicpg1252\\deff0\\uc1{\\fonttbl" );
    if (!rd.font_count) font_index( &rd, "+minor" );
    for (int i = 0; i < rd.font_count; i++)
    {
        buf_printf( &out, "{\\f%d\\fnil\\fcharset1 ", i );
        rtf_text( &out, rd.font_names[i], strlen( rd.font_names[i] ) );
        buf_str( &out, ";}" );
    }
    buf_str( &out, "}{\\colortbl;" );
    for (int i = 0; i < rd.color_count; i++)
        buf_printf( &out, "\\red%d\\green%d\\blue%d;", GetRValue( rd.color_list[i] ), GetGValue( rd.color_list[i] ),
                    GetBValue( rd.color_list[i] ) );
    buf_str( &out, "}\n" );
    if (rd.body.len) buf_add( &out, rd.body.data, rd.body.len );
    buf_str( &out, "}" );
    free_reader( &rd );
    *rtf_len = out.len;
    *keep_out = keep;
    return out.data;
}

/**********************************************************************
 *          OpenDocument text: its paragraphs and their text
 */

char *odt_to_rtf( const BYTE *data, size_t size, size_t *rtf_len )
{
    struct zip *zip = zip_open( data, size );
    struct buffer out = { 0 };
    struct xml x;
    enum xml_token t;
    BYTE *content;
    size_t len;
    BOOL in_para = FALSE;

    if (!zip) return NULL;
    if (!(content = zip_read( zip, "content.xml", &len )))
    {
        zip_close( zip );
        return NULL;
    }
    buf_str( &out, "{\\rtf1\\ansi\\deff0{\\fonttbl{\\f0\\fnil\\fcharset1 Calibri;}}\\f0\\fs22 " );
    xml_init( &x, (char *)content, len );
    while ((t = xml_next( &x )) != XML_END)
    {
        if (t == XML_OPEN)
        {
            if (xml_is( &x, "text:p" ) || xml_is( &x, "text:h" ))
            {
                in_para = TRUE;
                buf_str( &out, xml_is( &x, "text:h" ) ? "\\pard\\sb240\\sa120\\b\\fs32 " : "\\pard\\sa160\\b0\\fs22 " );
            }
            else if (xml_is( &x, "text:tab" )) buf_str( &out, "\\tab " );
            else if (xml_is( &x, "text:line-break" )) buf_str( &out, "\\line " );
            else if (xml_is( &x, "text:s" ))
            {
                int n = xml_attr_int( &x, "text:c", 1 );
                while (n-- > 0) buf_str( &out, " " );
            }
        }
        else if (t == XML_CLOSE && (xml_is( &x, "text:p" ) || xml_is( &x, "text:h" )))
        {
            buf_str( &out, "\\par\n" );
            in_para = FALSE;
        }
        else if (t == XML_TEXT && in_para && x.text.len) rtf_text( &out, x.text.data, x.text.len );
    }
    xml_free( &x );
    buf_str( &out, "}" );
    free( content );
    zip_close( zip );
    *rtf_len = out.len;
    return out.data;
}

/**********************************************************************
 *          Writing: riched20's RTF into the document part
 */

struct cprops
{
    int font, size, b, i, u, strike, script, caps, scaps, hidden, color, highlight, spacing;
};

struct pstate
{
    int align, left, first, right, before, after, sl, slmult, keepn, keep, pagebb, style, intbl;
    int numbering, num_style, num_start;
    int tabs[32], tab_count, tab_kind, tab_leader;
};

struct cellx
{
    int right;
    BOOL borders[4];
    int colors[4];              /* colour numbers, 0: automatic */
    int shade;                  /* colour number, 0: none */
};

struct writer
{
    const char *p, *end;
    struct buffer body, para, table, row, cell;
    struct buffer rels, media_types;
    struct zip_writer *zip;
    char fonts[256][64];
    COLORREF colors[256];
    int color_count;
    struct cprops stack[64], cp;
    int depth;
    struct pstate ps;
    int uc, skip;
    UINT codepage;

    /* the run being written */
    char run_props[1024];
    BOOL run_open;
    struct buffer run_text;

    /* the link being written */
    size_t link_start;
    struct buffer link_text;
    BOOL link_open;

    /* tables */
    struct cellx defs[64], cur_def;
    int cur_side;
    int def_count, cell_index;
    BOOL in_table;
    int table_grid[256], grid_count;
    struct buffer table_rows;
    int trleft;

    int image_count, rel_id;
    BOOL used_numbering[8];
    const struct docx_keep *keep;
    const struct page_setup *setup;
};

static void xml_escape( struct buffer *b, const char *s, size_t len )
{
    for (size_t i = 0; i < len; i++)
    {
        switch (s[i])
        {
        case '<': buf_str( b, "&lt;" ); break;
        case '>': buf_str( b, "&gt;" ); break;
        case '&': buf_str( b, "&amp;" ); break;
        case '"': buf_str( b, "&quot;" ); break;
        default:
            if ((unsigned char)s[i] < 0x20 && s[i] != '\t' && s[i] != '\n') break;
            buf_add( b, s + i, 1 );
        }
    }
}

static void xml_escape_w( struct buffer *b, const WCHAR *s, int len )
{
    struct buffer t = { 0 };

    buf_wstr_utf8( &t, s, len );
    if (t.len) xml_escape( b, t.data, t.len );
    buf_free( &t );
}

static const char *highlight_name( COLORREF c )
{
    for (int i = 0; i < ARRAY_SIZE(highlight_names) - 1; i++)
        if (highlight_names[i].color == c) return highlight_names[i].name;
    return NULL;
}

static void make_run_props( struct writer *w, char *out, size_t size )
{
    struct cprops *c = &w->cp;
    struct buffer b = { 0 };

    if (c->font >= 0 && c->font < 256 && w->fonts[c->font][0])
    {
        buf_str( &b, "<w:rFonts w:ascii=\"" );
        xml_escape( &b, w->fonts[c->font], strlen( w->fonts[c->font] ) );
        buf_str( &b, "\" w:hAnsi=\"" );
        xml_escape( &b, w->fonts[c->font], strlen( w->fonts[c->font] ) );
        buf_str( &b, "\" w:cs=\"" );
        xml_escape( &b, w->fonts[c->font], strlen( w->fonts[c->font] ) );
        buf_str( &b, "\"/>" );
    }
    if (c->b) buf_str( &b, "<w:b/><w:bCs/>" );
    if (c->i) buf_str( &b, "<w:i/><w:iCs/>" );
    if (c->caps) buf_str( &b, "<w:caps/>" );
    if (c->scaps) buf_str( &b, "<w:smallCaps/>" );
    if (c->strike) buf_str( &b, "<w:strike/>" );
    if (c->hidden) buf_str( &b, "<w:vanish/>" );
    if (c->color > 0 && c->color <= w->color_count)
    {
        COLORREF col = w->colors[c->color - 1];
        buf_printf( &b, "<w:color w:val=\"%02X%02X%02X\"/>", GetRValue( col ), GetGValue( col ), GetBValue( col ) );
    }
    if (c->spacing) buf_printf( &b, "<w:spacing w:val=\"%d\"/>", c->spacing );
    if (c->size > 0) buf_printf( &b, "<w:sz w:val=\"%d\"/><w:szCs w:val=\"%d\"/>", c->size, c->size );
    if (c->highlight > 0 && c->highlight <= w->color_count)
    {
        COLORREF col = w->colors[c->highlight - 1];
        const char *name = highlight_name( col );
        if (name) buf_printf( &b, "<w:highlight w:val=\"%s\"/>", name );
    }
    switch (c->u)
    {
    case 1: buf_str( &b, "<w:u w:val=\"single\"/>" ); break;
    case 2: buf_str( &b, "<w:u w:val=\"double\"/>" ); break;
    case 3: buf_str( &b, "<w:u w:val=\"dotted\"/>" ); break;
    case 4: buf_str( &b, "<w:u w:val=\"words\"/>" ); break;
    case 5: buf_str( &b, "<w:u w:val=\"wave\"/>" ); break;
    }
    if (c->highlight > 0 && c->highlight <= w->color_count && !highlight_name( w->colors[c->highlight - 1] ))
    {
        COLORREF col = w->colors[c->highlight - 1];
        buf_printf( &b, "<w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"%02X%02X%02X\"/>", GetRValue( col ),
                    GetGValue( col ), GetBValue( col ) );
    }
    if (c->script == 1) buf_str( &b, "<w:vertAlign w:val=\"superscript\"/>" );
    else if (c->script == 2) buf_str( &b, "<w:vertAlign w:val=\"subscript\"/>" );
    lstrcpynA( out, b.data ? b.data : "", (int)size );
    buf_free( &b );
}

static BOOL is_link_style( struct writer *w )
{
    return w->cp.u == 1 && w->cp.color > 0 && w->cp.color <= w->color_count &&
           w->colors[w->cp.color - 1] == LINK_COLOR;
}

static void close_run( struct writer *w )
{
    if (!w->run_open) return;
    if (w->run_text.len)
    {
        buf_str( &w->para, "<w:t xml:space=\"preserve\">" );
        buf_add( &w->para, w->run_text.data, w->run_text.len );
        buf_str( &w->para, "</w:t>" );
        w->run_text.len = 0;
    }
    buf_str( &w->para, "</w:r>" );
    w->run_open = FALSE;
}

static int add_rel( struct writer *w, const char *type, const char *target, BOOL external )
{
    int id = ++w->rel_id;

    buf_printf( &w->rels, "<Relationship Id=\"rIdAw%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/%s\" Target=\"",
                id, type );
    xml_escape( &w->rels, target, strlen( target ) );
    buf_printf( &w->rels, "\"%s/>", external ? " TargetMode=\"External\"" : "" );
    return id;
}

static void close_link( struct writer *w )
{
    const char *url;
    WCHAR *text;
    int n;

    if (!w->link_open) return;
    close_run( w );
    w->link_open = FALSE;
    n = MultiByteToWideChar( CP_UTF8, 0, w->link_text.data ? w->link_text.data : "", (int)w->link_text.len, NULL, 0 );
    if (!(text = malloc( (n + 1) * sizeof(WCHAR) ))) return;
    MultiByteToWideChar( CP_UTF8, 0, w->link_text.data ? w->link_text.data : "", (int)w->link_text.len, text, n );
    text[n] = 0;
    if ((url = link_find( text )))
    {
        char head[64];
        int id = add_rel( w, "hyperlink", url, TRUE ), hl;

        size_t tail = w->para.len - w->link_start;

        hl = sprintf( head, "<w:hyperlink r:id=\"rIdAw%d\" w:history=\"1\">", id );
        buf_add( &w->para, head, hl );   /* room, then the link's runs move after the head */
        if (w->para.len == w->link_start + tail + hl)
        {
            memmove( w->para.data + w->link_start + hl, w->para.data + w->link_start, tail );
            memcpy( w->para.data + w->link_start, head, hl );
            buf_str( &w->para, "</w:hyperlink>" );
        }
    }
    free( text );
    w->link_text.len = 0;
}

static void open_run( struct writer *w )
{
    char props[1024];
    BOOL link = is_link_style( w );

    if (link && !w->link_open)
    {
        close_run( w );
        w->link_open = TRUE;
        w->link_start = w->para.len;
        w->link_text.len = 0;
    }
    else if (!link && w->link_open) close_link( w );
    make_run_props( w, props, sizeof(props) );
    if (w->run_open && !strcmp( props, w->run_props )) return;
    close_run( w );
    lstrcpynA( w->run_props, props, sizeof(w->run_props) );
    buf_str( &w->para, "<w:r>" );
    if (props[0]) buf_printf( &w->para, "<w:rPr>%s</w:rPr>", props );
    w->run_open = TRUE;
}

static void add_char( struct writer *w, WCHAR c )
{
    char utf8[4];
    int n;

    open_run( w );
    n = WideCharToMultiByte( CP_UTF8, 0, &c, 1, utf8, sizeof(utf8), NULL, NULL );
    xml_escape( &w->run_text, utf8, n );
    if (w->link_open) buf_add( &w->link_text, utf8, n );
}

static void add_element( struct writer *w, const char *xml )
{
    open_run( w );
    if (w->run_text.len)
    {
        buf_str( &w->para, "<w:t xml:space=\"preserve\">" );
        buf_add( &w->para, w->run_text.data, w->run_text.len );
        buf_str( &w->para, "</w:t>" );
        w->run_text.len = 0;
    }
    buf_str( &w->para, xml );
}

static const char *style_id( struct writer *w, int number, char *buf, int size )
{
    if (number >= 1 && number <= STYLE_COUNT)
    {
        WideCharToMultiByte( CP_UTF8, 0, styles[number - 1].docx_id, -1, buf, size, NULL, NULL );
        return buf;
    }
    if (w->keep && number >= 20 && number - 20 < w->keep->style_count) return w->keep->style_ids[number - 20];
    return NULL;
}

static int numbering_id( int numbering, int style )
{
    switch (numbering)
    {
    case PFN_BULLET: return 1;
    case PFN_ARABIC: return (style & 0xf00) == PFNS_PAREN ? 3 : 2;
    case PFN_UCLETTER: return 4;
    case PFN_LCLETTER: return 5;
    case PFN_UCROMAN: return 6;
    case PFN_LCROMAN: return 7;
    }
    return 0;
}

static void end_paragraph( struct writer *w, struct buffer *target )
{
    struct pstate *ps = &w->ps;
    struct buffer ppr = { 0 };
    char sid[64];
    const char *style = style_id( w, ps->style, sid, sizeof(sid) );
    int num = ps->numbering ? numbering_id( ps->numbering, ps->num_style ) : 0;
    int left = ps->left, first = ps->first;

    close_link( w );
    close_run( w );
    if (style && strcmp( style, "Normal" ))
    {
        buf_str( &ppr, "<w:pStyle w:val=\"" );
        xml_escape( &ppr, style, strlen( style ) );
        buf_str( &ppr, "\"/>" );
    }
    if (ps->keepn) buf_str( &ppr, "<w:keepNext/>" );
    if (ps->keep) buf_str( &ppr, "<w:keepLines/>" );
    if (ps->pagebb) buf_str( &ppr, "<w:pageBreakBefore/>" );
    if (num)
    {
        buf_printf( &ppr, "<w:numPr><w:ilvl w:val=\"0\"/><w:numId w:val=\"%d\"/></w:numPr>", num );
        w->used_numbering[num] = TRUE;
    }
    if (ps->tab_count)
    {
        static const char *const kinds[] = { "left", "center", "right", "decimal" };
        static const char *const leaders[] = { "", " w:leader=\"dot\"", " w:leader=\"hyphen\"", " w:leader=\"underscore\"" };

        buf_str( &ppr, "<w:tabs>" );
        for (int i = 0; i < ps->tab_count; i++)
            buf_printf( &ppr, "<w:tab w:val=\"%s\"%s w:pos=\"%d\"/>", kinds[(ps->tabs[i] >> 24) & 3],
                        leaders[(ps->tabs[i] >> 28) & 3], ps->tabs[i] & 0xffffff );
        buf_str( &ppr, "</w:tabs>" );
    }
    {
        BOOL any = TRUE;
        buf_printf( &ppr, "<w:spacing w:before=\"%d\" w:after=\"%d\"", ps->before, ps->after );
        if (ps->sl && ps->slmult) buf_printf( &ppr, " w:line=\"%d\" w:lineRule=\"auto\"", abs( ps->sl ) );
        else if (ps->sl > 0) buf_printf( &ppr, " w:line=\"%d\" w:lineRule=\"atLeast\"", ps->sl );
        else if (ps->sl < 0) buf_printf( &ppr, " w:line=\"%d\" w:lineRule=\"exact\"", -ps->sl );
        else buf_str( &ppr, " w:line=\"240\" w:lineRule=\"auto\"" );
        buf_str( &ppr, "/>" );
        (void)any;
    }
    if (left || first || ps->right)
    {
        buf_printf( &ppr, "<w:ind w:left=\"%d\" w:right=\"%d\"", left, ps->right );
        if (first > 0) buf_printf( &ppr, " w:firstLine=\"%d\"", first );
        else if (first < 0) buf_printf( &ppr, " w:hanging=\"%d\"", -first );
        buf_str( &ppr, "/>" );
    }
    else if (num) buf_str( &ppr, "<w:ind w:left=\"720\" w:hanging=\"360\"/>" );
    switch (ps->align)
    {
    case 1: buf_str( &ppr, "<w:jc w:val=\"center\"/>" ); break;
    case 2: buf_str( &ppr, "<w:jc w:val=\"right\"/>" ); break;
    case 3: buf_str( &ppr, "<w:jc w:val=\"both\"/>" ); break;
    }
    if (ps->style >= 3 && ps->style <= 5) buf_printf( &ppr, "<w:outlineLvl w:val=\"%d\"/>", ps->style - 3 );
    buf_str( target, "<w:p>" );
    if (ppr.len) buf_printf( target, "<w:pPr>%s</w:pPr>", ppr.data );
    if (w->para.len) buf_add( target, w->para.data, w->para.len );
    buf_str( target, "</w:p>" );
    w->para.len = 0;
    buf_free( &ppr );
    /* a page break before, once */
    ps->pagebb = FALSE;
}

static void end_table( struct writer *w )
{
    int prev = 0;

    if (!w->in_table) return;
    w->in_table = FALSE;
    buf_str( &w->body, "<w:tbl><w:tblPr><w:tblStyle w:val=\"TableGrid\"/><w:tblW w:w=\"0\" w:type=\"auto\"/>" );
    if (w->trleft) buf_printf( &w->body, "<w:tblInd w:w=\"%d\" w:type=\"dxa\"/>", w->trleft );
    buf_str( &w->body, "<w:tblLayout w:type=\"fixed\"/><w:tblLook w:val=\"04A0\" w:firstRow=\"1\" w:lastRow=\"0\" "
                       "w:firstColumn=\"1\" w:lastColumn=\"0\" w:noHBand=\"0\" w:noVBand=\"1\"/></w:tblPr><w:tblGrid>" );
    for (int i = 0; i < w->grid_count; i++)
    {
        buf_printf( &w->body, "<w:gridCol w:w=\"%d\"/>", w->table_grid[i] - prev );
        prev = w->table_grid[i];
    }
    buf_str( &w->body, "</w:tblGrid>" );
    /* the rows, their spans counted on the grid now complete */
    {
        char *p = w->table_rows.data, *end = p + w->table_rows.len;

        while (p && p < end)
        {
            char *mark = strstr( p, "\x01SPAN:" );
            if (!mark || mark >= end)
            {
                buf_add( &w->body, p, end - p );
                break;
            }
            buf_add( &w->body, p, mark - p );
            {
                int left = 0, right = 0, span = 0;
                char *close = strchr( mark, '\x02' );
                sscanf( mark + 6, "%d:%d", &left, &right );
                for (int i = 0; i < w->grid_count; i++)
                    if (w->table_grid[i] > left && w->table_grid[i] <= right) span++;
                if (span > 1) buf_printf( &w->body, "<w:gridSpan w:val=\"%d\"/>", span );
                p = close ? close + 1 : end;
            }
        }
    }
    buf_str( &w->body, "</w:tbl>" );
    w->table_rows.len = 0;
    w->grid_count = 0;
}

static void add_grid( struct writer *w, int right )
{
    int i;

    for (i = 0; i < w->grid_count; i++)
    {
        if (w->table_grid[i] == right) return;
        if (w->table_grid[i] > right) break;
    }
    if (w->grid_count >= ARRAY_SIZE(w->table_grid)) return;
    memmove( w->table_grid + i + 1, w->table_grid + i, (w->grid_count - i) * sizeof(int) );
    w->table_grid[i] = right;
    w->grid_count++;
}

static void end_cell( struct writer *w )
{
    struct cellx *def = w->cell_index < w->def_count ? &w->defs[w->cell_index] : NULL;
    int left = w->cell_index > 0 && w->cell_index - 1 < w->def_count ? w->defs[w->cell_index - 1].right : w->trleft;
    int right = def ? def->right : left + 2000;

    end_paragraph( w, &w->cell );
    buf_printf( &w->row, "<w:tc><w:tcPr><w:tcW w:w=\"%d\" w:type=\"dxa\"/>\x01SPAN:%d:%d\x02", right - left, left, right );
    if (def)
    {
        static const char *const names[4] = { "top", "left", "bottom", "right" };
        buf_str( &w->row, "<w:tcBorders>" );
        for (int s = 0; s < 4; s++)
        {
            char color[8] = "auto";
            int n = def->colors[s];

            if (n > 0 && n <= w->color_count)
                sprintf( color, "%02X%02X%02X", GetRValue( w->colors[n - 1] ), GetGValue( w->colors[n - 1] ),
                         GetBValue( w->colors[n - 1] ) );
            buf_printf( &w->row, "<w:%s w:val=\"%s\" w:sz=\"4\" w:space=\"0\" w:color=\"%s\"/>", names[s],
                        def->borders[s] ? "single" : "nil", color );
        }
        buf_str( &w->row, "</w:tcBorders>" );
        if (def->shade > 0 && def->shade <= w->color_count)
        {
            COLORREF c = w->colors[def->shade - 1];
            buf_printf( &w->row, "<w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"%02X%02X%02X\"/>",
                        GetRValue( c ), GetGValue( c ), GetBValue( c ) );
        }
    }
    buf_str( &w->row, "</w:tcPr>" );
    buf_add( &w->row, w->cell.data ? w->cell.data : "", w->cell.len );
    buf_str( &w->row, "</w:tc>" );
    w->cell.len = 0;
    w->cell_index++;
}

static void end_row( struct writer *w )
{
    if (w->cell.len || w->para.len || w->run_open) end_cell( w );
    if (!w->row.len) return;
    buf_str( &w->table_rows, "<w:tr>" );
    buf_add( &w->table_rows, w->row.data, w->row.len );
    buf_str( &w->table_rows, "</w:tr>" );
    w->row.len = 0;
    w->cell_index = 0;
}

static void add_picture( struct writer *w, const BYTE *bits, size_t len, int kind, int picw, int pich, int goal_w, int goal_h )
{
    BYTE *png;
    size_t png_len;
    int px_w, px_h, id;
    char name[64], xml[2048];

    if (!goal_w || !goal_h) return;
    if (kind == 1 && metafile_size( bits, len, &px_w, &px_h )) {}
    else
    {
        px_w = MulDiv( goal_w, 150, 1440 );
        px_h = MulDiv( goal_h, 150, 1440 );
    }
    if (px_w > 3000 || px_h > 3000)
    {
        int m = max( px_w, px_h );
        px_w = MulDiv( px_w, 3000, m );
        px_h = MulDiv( px_h, 3000, m );
    }
    png = image_from_metafile( bits, len, kind == 1, 8, max( px_w, 1 ), max( px_h, 1 ), &png_len );
    if (!png) return;
    snprintf( name, sizeof(name), "media/awimage%d.png", ++w->image_count );
    {
        char full[80];
        snprintf( full, sizeof(full), "word/%s", name );
        zipw_add( w->zip, full, png, png_len, FALSE );
    }
    free( png );
    id = add_rel( w, "image", name, FALSE );
    snprintf( xml, sizeof(xml),
              "<w:drawing><wp:inline distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\"><wp:extent cx=\"%d\" cy=\"%d\"/>"
              "<wp:effectExtent l=\"0\" t=\"0\" r=\"0\" b=\"0\"/><wp:docPr id=\"%d\" name=\"Picture %d\"/>"
              "<wp:cNvGraphicFramePr><a:graphicFrameLocks xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" noChangeAspect=\"1\"/></wp:cNvGraphicFramePr>"
              "<a:graphic xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\">"
              "<a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/picture\">"
              "<pic:pic xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\">"
              "<pic:nvPicPr><pic:cNvPr id=\"%d\" name=\"awimage%d.png\"/><pic:cNvPicPr/></pic:nvPicPr>"
              "<pic:blipFill><a:blip r:embed=\"rIdAw%d\"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill>"
              "<pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"%d\" cy=\"%d\"/></a:xfrm>"
              "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></pic:spPr></pic:pic></a:graphicData></a:graphic>"
              "</wp:inline></w:drawing>",
              goal_w * 635, goal_h * 635, 1000 + w->image_count, w->image_count, 0, w->image_count, id,
              goal_w * 635, goal_h * 635 );
    add_element( w, xml );
    (void)picw; (void)pich;
}

/* RTF tokens */
enum rtf_tok { T_EOF, T_OPEN, T_CLOSE, T_WORD, T_TEXT, T_HEX };

struct token
{
    enum rtf_tok type;
    char word[32];
    int param;
    BOOL has_param;
    unsigned char c;
};

static void next_token( struct writer *w, struct token *t )
{
    while (w->p < w->end && (*w->p == '\r' || *w->p == '\n')) w->p++;
    if (w->p >= w->end)
    {
        t->type = T_EOF;
        return;
    }
    if (*w->p == '{') { w->p++; t->type = T_OPEN; return; }
    if (*w->p == '}') { w->p++; t->type = T_CLOSE; return; }
    if (*w->p != '\\')
    {
        t->type = T_TEXT;
        t->c = *w->p++;
        return;
    }
    w->p++;
    if (w->p >= w->end) { t->type = T_EOF; return; }
    if (*w->p == '\'')
    {
        unsigned int v = 0;
        w->p++;
        for (int i = 0; i < 2 && w->p < w->end; i++, w->p++)
            v = v * 16 + (isdigit( (unsigned char)*w->p ) ? *w->p - '0' : (tolower( *w->p ) - 'a' + 10));
        t->type = T_HEX;
        t->c = (unsigned char)v;
        return;
    }
    if (!isalpha( (unsigned char)*w->p ))
    {
        t->type = T_WORD;
        t->word[0] = *w->p++;
        t->word[1] = 0;
        t->has_param = FALSE;
        return;
    }
    {
        int n = 0, sign = 1;
        while (w->p < w->end && isalpha( (unsigned char)*w->p ))
        {
            if (n < (int)sizeof(t->word) - 1) t->word[n++] = *w->p;
            w->p++;
        }
        t->word[n] = 0;
        t->has_param = FALSE;
        t->param = 0;
        if (w->p < w->end && *w->p == '-') { sign = -1; w->p++; }
        while (w->p < w->end && isdigit( (unsigned char)*w->p ))
        {
            t->param = t->param * 10 + (*w->p++ - '0');
            t->has_param = TRUE;
        }
        t->param *= sign;
        if (w->p < w->end && *w->p == ' ') w->p++;
        t->type = T_WORD;
    }
}

/* past the group just opened */
static void skip_group( struct writer *w )
{
    struct token t;
    int level = 1;

    while (level)
    {
        next_token( w, &t );
        if (t.type == T_EOF) return;
        if (t.type == T_OPEN) level++;
        else if (t.type == T_CLOSE) level--;
    }
}

static void read_font_table( struct writer *w )
{
    struct token t;
    int level = 1, index = 0;
    char name[64];
    int n = 0;

    while (level)
    {
        next_token( w, &t );
        if (t.type == T_EOF) return;
        if (t.type == T_OPEN) { level++; n = 0; continue; }
        if (t.type == T_CLOSE) { level--; continue; }
        if (t.type == T_WORD)
        {
            if (!strcmp( t.word, "f" )) index = t.param;
            else if (t.word[0] == '*' || !strcmp( t.word, "panose" ) || !strcmp( t.word, "falt" )) { skip_group( w ); level--; }
            continue;
        }
        if (t.type == T_TEXT || t.type == T_HEX)
        {
            if (t.c == ';')
            {
                name[n] = 0;
                if (index >= 0 && index < 256) lstrcpynA( w->fonts[index], name, 64 );
                n = 0;
            }
            else if (n < 63) name[n++] = t.c;
        }
    }
}

static void read_color_table( struct writer *w )
{
    struct token t;
    int r = 0, g = 0, b = 0;
    BOOL any = FALSE, first = TRUE;

    for (;;)
    {
        next_token( w, &t );
        if (t.type == T_EOF || t.type == T_CLOSE) return;
        if (t.type == T_WORD)
        {
            if (!strcmp( t.word, "red" )) { r = t.param; any = TRUE; }
            else if (!strcmp( t.word, "green" )) { g = t.param; any = TRUE; }
            else if (!strcmp( t.word, "blue" )) { b = t.param; any = TRUE; }
        }
        else if (t.type == T_TEXT && t.c == ';')
        {
            /* the first entry, empty, is "automatic" and has no number in our list */
            if (first && !any) { first = FALSE; continue; }
            first = FALSE;
            if (w->color_count < 256) w->colors[w->color_count++] = RGB( r, g, b );
            r = g = b = 0;
            any = FALSE;
        }
    }
}

static void read_picture( struct writer *w )
{
    struct token t;
    int level = 1, kind = 0, picw = 0, pich = 0, goal_w = 0, goal_h = 0;
    struct buffer data = { 0 };
    int nibble = -1;

    while (level)
    {
        while (w->p < w->end && (*w->p == '\r' || *w->p == '\n' || *w->p == ' ' || *w->p == '\t')) w->p++;
        if (w->p < w->end && isxdigit( (unsigned char)*w->p ))
        {
            int v = isdigit( (unsigned char)*w->p ) ? *w->p - '0' : tolower( *w->p ) - 'a' + 10;
            w->p++;
            if (nibble < 0) nibble = v;
            else
            {
                unsigned char byte = (unsigned char)(nibble * 16 + v);
                buf_add( &data, &byte, 1 );
                nibble = -1;
            }
            continue;
        }
        next_token( w, &t );
        if (t.type == T_EOF) break;
        if (t.type == T_OPEN) { level++; continue; }
        if (t.type == T_CLOSE) { level--; continue; }
        if (t.type != T_WORD) continue;
        if (!strcmp( t.word, "emfblip" )) kind = 1;
        else if (!strcmp( t.word, "wmetafile" )) kind = 2;
        else if (!strcmp( t.word, "picw" )) picw = t.param;
        else if (!strcmp( t.word, "pich" )) pich = t.param;
        else if (!strcmp( t.word, "picwgoal" )) goal_w = t.param;
        else if (!strcmp( t.word, "pichgoal" )) goal_h = t.param;
    }
    if (kind && data.len) add_picture( w, (BYTE *)data.data, data.len, kind, picw, pich, goal_w, goal_h );
    buf_free( &data );
}

static void read_numbering_group( struct writer *w )
{
    struct token t;
    int level = 1;

    while (level)
    {
        next_token( w, &t );
        if (t.type == T_EOF) return;
        if (t.type == T_OPEN) { level++; continue; }
        if (t.type == T_CLOSE) { level--; continue; }
        if (t.type != T_WORD) continue;
        if (!strcmp( t.word, "pnlvlblt" )) w->ps.numbering = PFN_BULLET;
        else if (!strcmp( t.word, "pndec" )) w->ps.numbering = PFN_ARABIC;
        else if (!strcmp( t.word, "pnlcltr" )) w->ps.numbering = PFN_LCLETTER;
        else if (!strcmp( t.word, "pnucltr" )) w->ps.numbering = PFN_UCLETTER;
        else if (!strcmp( t.word, "pnlcrm" )) w->ps.numbering = PFN_LCROMAN;
        else if (!strcmp( t.word, "pnucrm" )) w->ps.numbering = PFN_UCROMAN;
        else if (!strcmp( t.word, "pnlvlbody" ) && !w->ps.numbering) w->ps.numbering = PFN_ARABIC;
        else if (!strcmp( t.word, "pnstart" )) w->ps.num_start = t.param;
        else if (!strcmp( t.word, "pntxta" ))
        {
            /* "." or ")" follows */
            if (w->p < w->end && *w->p == ')') w->ps.num_style = PFNS_PAREN;
        }
    }
}

static void put_text_byte( struct writer *w, unsigned char c )
{
    WCHAR wc;

    if (c < 0x80) wc = c;
    else MultiByteToWideChar( w->codepage, 0, (char *)&c, 1, &wc, 1 );
    add_char( w, wc );
}

static void para_reset( struct writer *w )
{
    memset( &w->ps, 0, sizeof(w->ps) );
}

/* a paragraph ended: into the cell when it is in a table, else the body */
static void paragraph_done( struct writer *w )
{
    if (w->ps.intbl && w->in_table) end_paragraph( w, &w->cell );
    else
    {
        end_table( w );
        end_paragraph( w, &w->body );
    }
}

static void parse_rtf( struct writer *w )
{
    struct token t;

    w->cp.size = 22;
    w->codepage = 1252;
    w->uc = 1;
    for (;;)
    {
        next_token( w, &t );
        if (t.type == T_EOF) break;
        if (t.type == T_OPEN)
        {
            if (w->depth < ARRAY_SIZE(w->stack)) w->stack[w->depth] = w->cp;
            w->depth++;
            /* destinations */
            {
                const char *save = w->p;
                struct token d;
                BOOL star = FALSE;

                next_token( w, &d );
                if (d.type == T_WORD && !strcmp( d.word, "*" ))
                {
                    star = TRUE;
                    next_token( w, &d );
                }
                if (d.type == T_WORD)
                {
                    if (!strcmp( d.word, "fonttbl" )) { read_font_table( w ); w->depth--; continue; }
                    if (!strcmp( d.word, "colortbl" )) { read_color_table( w ); w->depth--; continue; }
                    if (!strcmp( d.word, "pict" )) { read_picture( w ); w->depth--; w->cp = w->stack[min( w->depth, 63 )]; continue; }
                    if (!strcmp( d.word, "pn" )) { read_numbering_group( w ); w->depth--; continue; }
                    if (!strcmp( d.word, "shppict" )) continue;   /* its \pict follows */
                    if (!strcmp( d.word, "pntext" ) || !strcmp( d.word, "stylesheet" ) || !strcmp( d.word, "info" ) ||
                        !strcmp( d.word, "nonshppict" ) || !strcmp( d.word, "nesttableprops" ) || !strcmp( d.word, "fldinst" ) ||
                        !strcmp( d.word, "nonesttables" ) || star)
                    {
                        skip_group( w );
                        w->depth--;
                        if (w->depth < ARRAY_SIZE(w->stack)) w->cp = w->stack[w->depth];
                        continue;
                    }
                }
                w->p = save;
            }
            continue;
        }
        if (t.type == T_CLOSE)
        {
            if (w->depth > 0) w->depth--;
            if (w->depth < ARRAY_SIZE(w->stack)) w->cp = w->stack[w->depth];
            continue;
        }
        if (t.type == T_TEXT)
        {
            if (w->skip) { w->skip--; continue; }
            if (t.c == ';' && 0) continue;
            put_text_byte( w, t.c );
            continue;
        }
        if (t.type == T_HEX)
        {
            if (w->skip) { w->skip--; continue; }
            put_text_byte( w, t.c );
            continue;
        }
        /* control words */
        {
            const char *k = t.word;
            int v = t.param;

            if (w->skip && strcmp( k, "u" )) w->skip = 0;
            if (!strcmp( k, "u" ))
            {
                add_char( w, (WCHAR)(v < 0 ? v + 65536 : v) );
                w->skip = w->uc;
            }
            else if (!strcmp( k, "uc" )) w->uc = v;
            else if (!strcmp( k, "ansicpg" )) w->codepage = v;
            else if (!strcmp( k, "par" ) || !strcmp( k, "nestcell" ) || !strcmp( k, "nestrow" )) paragraph_done( w );
            else if (!strcmp( k, "page" ))
            {
                paragraph_done( w );
                w->ps.pagebb = TRUE;
            }
            else if (!strcmp( k, "line" )) add_element( w, "<w:br/>" );
            else if (!strcmp( k, "tab" )) add_element( w, "<w:tab/>" );
            else if (!strcmp( k, "cell" ))
            {
                w->in_table = TRUE;
                end_cell( w );
            }
            else if (!strcmp( k, "row" )) end_row( w );
            else if (!strcmp( k, "trowd" ))
            {
                if (!w->in_table && w->para.len == 0) {}
                w->in_table = TRUE;
                w->def_count = 0;
                memset( &w->cur_def, 0, sizeof(w->cur_def) );
            }
            else if (!strcmp( k, "trleft" )) w->trleft = v;
            else if (!strcmp( k, "clbrdrt" )) w->cur_def.borders[w->cur_side = 0] = TRUE;
            else if (!strcmp( k, "clbrdrl" )) w->cur_def.borders[w->cur_side = 1] = TRUE;
            else if (!strcmp( k, "clbrdrb" )) w->cur_def.borders[w->cur_side = 2] = TRUE;
            else if (!strcmp( k, "clbrdrr" )) w->cur_def.borders[w->cur_side = 3] = TRUE;
            else if (!strcmp( k, "brdrcf" )) w->cur_def.colors[w->cur_side] = v;
            else if (!strcmp( k, "clcbpat" )) w->cur_def.shade = v;
            else if (!strcmp( k, "cellx" ))
            {
                w->cur_def.right = v;
                if (w->def_count < ARRAY_SIZE(w->defs)) w->defs[w->def_count++] = w->cur_def;
                add_grid( w, v );
                memset( &w->cur_def, 0, sizeof(w->cur_def) );
            }
            else if (!strcmp( k, "pard" ))
            {
                BOOL pagebb = w->ps.pagebb;
                para_reset( w );
                w->ps.pagebb = pagebb && !w->para.len ? pagebb : FALSE;
            }
            else if (!strcmp( k, "intbl" )) w->ps.intbl = TRUE;
            else if (!strcmp( k, "ql" )) w->ps.align = 0;
            else if (!strcmp( k, "qc" )) w->ps.align = 1;
            else if (!strcmp( k, "qr" )) w->ps.align = 2;
            else if (!strcmp( k, "qj" )) w->ps.align = 3;
            else if (!strcmp( k, "li" )) w->ps.left = v;
            else if (!strcmp( k, "fi" )) w->ps.first = v;
            else if (!strcmp( k, "ri" )) w->ps.right = v;
            else if (!strcmp( k, "sb" )) w->ps.before = v;
            else if (!strcmp( k, "sa" )) w->ps.after = v;
            else if (!strcmp( k, "sl" )) w->ps.sl = v;
            else if (!strcmp( k, "slmult" )) w->ps.slmult = v;
            else if (!strcmp( k, "tqc" )) w->ps.tab_kind = 1;
            else if (!strcmp( k, "tqr" )) w->ps.tab_kind = 2;
            else if (!strcmp( k, "tqdec" )) w->ps.tab_kind = 3;
            else if (!strcmp( k, "tldot" )) w->ps.tab_leader = 1;
            else if (!strcmp( k, "tlhyph" )) w->ps.tab_leader = 2;
            else if (!strcmp( k, "tlul" )) w->ps.tab_leader = 3;
            else if (!strcmp( k, "tx" ))
            {
                if (w->ps.tab_count < 32)
                    w->ps.tabs[w->ps.tab_count++] = v | (w->ps.tab_kind << 24) | (w->ps.tab_leader << 28);
                w->ps.tab_kind = w->ps.tab_leader = 0;
            }
            else if (!strcmp( k, "keepn" )) w->ps.keepn = TRUE;
            else if (!strcmp( k, "keep" )) w->ps.keep = TRUE;
            else if (!strcmp( k, "pagebb" )) w->ps.pagebb = TRUE;
            else if (!strcmp( k, "s" )) w->ps.style = v;
            else if (!strcmp( k, "plain" ))
            {
                int font = w->cp.font;
                memset( &w->cp, 0, sizeof(w->cp) );
                w->cp.font = font;
                w->cp.size = 24;
            }
            else if (!strcmp( k, "f" )) w->cp.font = v;
            else if (!strcmp( k, "fs" )) w->cp.size = v;
            else if (!strcmp( k, "b" )) w->cp.b = !t.has_param || v;
            else if (!strcmp( k, "i" )) w->cp.i = !t.has_param || v;
            else if (!strcmp( k, "strike" ) || !strcmp( k, "striked" )) w->cp.strike = !t.has_param || v;
            else if (!strcmp( k, "caps" )) w->cp.caps = !t.has_param || v;
            else if (!strcmp( k, "scaps" )) w->cp.scaps = !t.has_param || v;
            else if (!strcmp( k, "v" )) w->cp.hidden = !t.has_param || v;
            else if (!strcmp( k, "ul" )) w->cp.u = !t.has_param || v ? 1 : 0;
            else if (!strcmp( k, "uldb" )) w->cp.u = 2;
            else if (!strcmp( k, "uld" )) w->cp.u = 3;
            else if (!strcmp( k, "ulw" )) w->cp.u = 4;
            else if (!strcmp( k, "ulwave" )) w->cp.u = 5;
            else if (!strcmp( k, "ulnone" )) w->cp.u = 0;
            else if (!strcmp( k, "super" )) w->cp.script = 1;
            else if (!strcmp( k, "sub" )) w->cp.script = 2;
            else if (!strcmp( k, "nosupersub" )) w->cp.script = 0;
            else if (!strcmp( k, "cf" )) w->cp.color = v;
            else if (!strcmp( k, "highlight" ) || !strcmp( k, "cb" )) w->cp.highlight = v;
            else if (!strcmp( k, "expndtw" )) w->cp.spacing = v;
            else if (!strcmp( k, "emdash" )) add_char( w, 0x2014 );
            else if (!strcmp( k, "endash" )) add_char( w, 0x2013 );
            else if (!strcmp( k, "bullet" )) add_char( w, 0x2022 );
            else if (!strcmp( k, "lquote" )) add_char( w, 0x2018 );
            else if (!strcmp( k, "rquote" )) add_char( w, 0x2019 );
            else if (!strcmp( k, "ldblquote" )) add_char( w, 0x201c );
            else if (!strcmp( k, "rdblquote" )) add_char( w, 0x201d );
            else if (!strcmp( k, "~" )) add_char( w, 0xa0 );
            else if (!strcmp( k, "\\" ) || !strcmp( k, "{" ) || !strcmp( k, "}" )) add_char( w, k[0] );
        }
    }
    close_link( w );
    close_run( w );
    if (w->para.len) paragraph_done( w );
    end_table( w );
}

/**********************************************************************
 *          The parts of a new package
 */

static const char styles_xml[] =
"<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
"<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
"<w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:asciiTheme=\"minorHAnsi\" w:eastAsiaTheme=\"minorHAnsi\" w:hAnsiTheme=\"minorHAnsi\" w:cstheme=\"minorBidi\"/>"
"<w:sz w:val=\"22\"/><w:szCs w:val=\"22\"/><w:lang w:val=\"uk-UA\" w:eastAsia=\"en-US\" w:bidi=\"ar-SA\"/></w:rPr></w:rPrDefault>"
"<w:pPrDefault><w:pPr><w:spacing w:after=\"160\" w:line=\"259\" w:lineRule=\"auto\"/></w:pPr></w:pPrDefault></w:docDefaults>"
"<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/><w:qFormat/></w:style>"
"<w:style w:type=\"paragraph\" w:styleId=\"NoSpacing\"><w:name w:val=\"No Spacing\"/><w:uiPriority w:val=\"1\"/><w:qFormat/><w:pPr><w:spacing w:after=\"0\" w:line=\"240\" w:lineRule=\"auto\"/></w:pPr></w:style>"
"<w:style w:type=\"paragraph\" w:styleId=\"Heading1\"><w:name w:val=\"heading 1\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/><w:uiPriority w:val=\"9\"/><w:qFormat/>"
"<w:pPr><w:keepNext/><w:keepLines/><w:spacing w:before=\"480\" w:after=\"0\"/><w:outlineLvl w:val=\"0\"/></w:pPr>"
"<w:rPr><w:rFonts w:asciiTheme=\"majorHAnsi\" w:eastAsiaTheme=\"majorEastAsia\" w:hAnsiTheme=\"majorHAnsi\" w:cstheme=\"majorBidi\"/><w:color w:val=\"2F5496\"/><w:sz w:val=\"32\"/><w:szCs w:val=\"32\"/></w:rPr></w:style>"
"<w:style w:type=\"paragraph\" w:styleId=\"Heading2\"><w:name w:val=\"heading 2\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/><w:uiPriority w:val=\"9\"/><w:unhideWhenUsed/><w:qFormat/>"
"<w:pPr><w:keepNext/><w:keepLines/><w:spacing w:before=\"40\" w:after=\"0\"/><w:outlineLvl w:val=\"1\"/></w:pPr>"
"<w:rPr><w:rFonts w:asciiTheme=\"majorHAnsi\" w:eastAsiaTheme=\"majorEastAsia\" w:hAnsiTheme=\"majorHAnsi\" w:cstheme=\"majorBidi\"/><w:color w:val=\"2F5496\"/><w:sz w:val=\"26\"/><w:szCs w:val=\"26\"/></w:rPr></w:style>"
"<w:style w:type=\"paragraph\" w:styleId=\"Heading3\"><w:name w:val=\"heading 3\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/><w:uiPriority w:val=\"9\"/><w:unhideWhenUsed/><w:qFormat/>"
"<w:pPr><w:keepNext/><w:keepLines/><w:spacing w:before=\"40\" w:after=\"0\"/><w:outlineLvl w:val=\"2\"/></w:pPr>"
"<w:rPr><w:rFonts w:asciiTheme=\"majorHAnsi\" w:eastAsiaTheme=\"majorEastAsia\" w:hAnsiTheme=\"majorHAnsi\" w:cstheme=\"majorBidi\"/><w:color w:val=\"1F3763\"/><w:sz w:val=\"24\"/><w:szCs w:val=\"24\"/></w:rPr></w:style>"
"<w:style w:type=\"paragraph\" w:styleId=\"Title\"><w:name w:val=\"Title\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/><w:uiPriority w:val=\"10\"/><w:qFormat/>"
"<w:pPr><w:spacing w:after=\"0\" w:line=\"240\" w:lineRule=\"auto\"/><w:contextualSpacing/></w:pPr>"
"<w:rPr><w:rFonts w:asciiTheme=\"majorHAnsi\" w:eastAsiaTheme=\"majorEastAsia\" w:hAnsiTheme=\"majorHAnsi\" w:cstheme=\"majorBidi\"/><w:spacing w:val=\"-10\"/><w:kern w:val=\"28\"/><w:sz w:val=\"56\"/><w:szCs w:val=\"56\"/></w:rPr></w:style>"
"<w:style w:type=\"paragraph\" w:styleId=\"Subtitle\"><w:name w:val=\"Subtitle\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/><w:uiPriority w:val=\"11\"/><w:qFormat/>"
"<w:rPr><w:rFonts w:eastAsiaTheme=\"minorEastAsia\"/><w:color w:val=\"5A5A5A\"/><w:spacing w:val=\"15\"/></w:rPr></w:style>"
"<w:style w:type=\"paragraph\" w:styleId=\"Quote\"><w:name w:val=\"Quote\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/><w:uiPriority w:val=\"29\"/><w:qFormat/>"
"<w:pPr><w:spacing w:before=\"200\"/><w:ind w:left=\"864\" w:right=\"864\"/><w:jc w:val=\"center\"/></w:pPr><w:rPr><w:i/><w:iCs/><w:color w:val=\"404040\"/></w:rPr></w:style>"
"<w:style w:type=\"paragraph\" w:styleId=\"ListParagraph\"><w:name w:val=\"List Paragraph\"/><w:basedOn w:val=\"Normal\"/><w:uiPriority w:val=\"34\"/><w:qFormat/><w:pPr><w:ind w:left=\"720\"/><w:contextualSpacing/></w:pPr></w:style>"
"<w:style w:type=\"character\" w:default=\"1\" w:styleId=\"DefaultParagraphFont\"><w:name w:val=\"Default Paragraph Font\"/><w:uiPriority w:val=\"1\"/><w:semiHidden/><w:unhideWhenUsed/></w:style>"
"<w:style w:type=\"character\" w:styleId=\"Hyperlink\"><w:name w:val=\"Hyperlink\"/><w:basedOn w:val=\"DefaultParagraphFont\"/><w:uiPriority w:val=\"99\"/><w:unhideWhenUsed/><w:rPr><w:color w:val=\"0563C1\"/><w:u w:val=\"single\"/></w:rPr></w:style>"
"<w:style w:type=\"table\" w:default=\"1\" w:styleId=\"TableNormal\"><w:name w:val=\"Normal Table\"/><w:uiPriority w:val=\"99\"/><w:semiHidden/><w:unhideWhenUsed/>"
"<w:tblPr><w:tblInd w:w=\"0\" w:type=\"dxa\"/><w:tblCellMar><w:top w:w=\"0\" w:type=\"dxa\"/><w:left w:w=\"108\" w:type=\"dxa\"/><w:bottom w:w=\"0\" w:type=\"dxa\"/><w:right w:w=\"108\" w:type=\"dxa\"/></w:tblCellMar></w:tblPr></w:style>"
"<w:style w:type=\"table\" w:styleId=\"TableGrid\"><w:name w:val=\"Table Grid\"/><w:basedOn w:val=\"TableNormal\"/><w:uiPriority w:val=\"39\"/>"
"<w:pPr><w:spacing w:after=\"0\" w:line=\"240\" w:lineRule=\"auto\"/></w:pPr><w:tblPr><w:tblBorders>"
"<w:top w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"auto\"/><w:left w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"auto\"/>"
"<w:bottom w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"auto\"/><w:right w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"auto\"/>"
"<w:insideH w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"auto\"/><w:insideV w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"auto\"/>"
"</w:tblBorders></w:tblPr></w:style>"
"</w:styles>";

static const char theme_xml[] =
"<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
"<a:theme xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" name=\"Office Theme\"><a:themeElements>"
"<a:clrScheme name=\"Office\"><a:dk1><a:sysClr val=\"windowText\" lastClr=\"000000\"/></a:dk1><a:lt1><a:sysClr val=\"window\" lastClr=\"FFFFFF\"/></a:lt1>"
"<a:dk2><a:srgbClr val=\"44546A\"/></a:dk2><a:lt2><a:srgbClr val=\"E7E6E6\"/></a:lt2><a:accent1><a:srgbClr val=\"4472C4\"/></a:accent1>"
"<a:accent2><a:srgbClr val=\"ED7D31\"/></a:accent2><a:accent3><a:srgbClr val=\"A5A5A5\"/></a:accent3><a:accent4><a:srgbClr val=\"FFC000\"/></a:accent4>"
"<a:accent5><a:srgbClr val=\"5B9BD5\"/></a:accent5><a:accent6><a:srgbClr val=\"70AD47\"/></a:accent6><a:hlink><a:srgbClr val=\"0563C1\"/></a:hlink>"
"<a:folHlink><a:srgbClr val=\"954F72\"/></a:folHlink></a:clrScheme>"
"<a:fontScheme name=\"Office\"><a:majorFont><a:latin typeface=\"Calibri Light\"/><a:ea typeface=\"\"/><a:cs typeface=\"\"/></a:majorFont>"
"<a:minorFont><a:latin typeface=\"Calibri\"/><a:ea typeface=\"\"/><a:cs typeface=\"\"/></a:minorFont></a:fontScheme>"
"<a:fmtScheme name=\"Office\"><a:fillStyleLst><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill></a:fillStyleLst>"
"<a:lnStyleLst><a:ln w=\"6350\"><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill></a:ln><a:ln w=\"12700\"><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill></a:ln><a:ln w=\"19050\"><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill></a:ln></a:lnStyleLst>"
"<a:effectStyleLst><a:effectStyle><a:effectLst/></a:effectStyle><a:effectStyle><a:effectLst/></a:effectStyle><a:effectStyle><a:effectLst/></a:effectStyle></a:effectStyleLst>"
"<a:bgFillStyleLst><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill></a:bgFillStyleLst></a:fmtScheme>"
"</a:themeElements></a:theme>";

static void numbering_part( struct buffer *b )
{
    static const struct { const char *fmt, *text; } kinds[8] =
    {
        { NULL, NULL }, { "bullet", "\xef\x82\xb7" }, { "decimal", "%1." }, { "decimal", "%1)" }, { "upperLetter", "%1." },
        { "lowerLetter", "%1)" }, { "upperRoman", "%1." }, { "lowerRoman", "%1." },
    };

    buf_str( b, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
                "<w:numbering xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">" );
    for (int i = 1; i < 8; i++)
    {
        buf_printf( b, "<w:abstractNum w:abstractNumId=\"%d\"><w:multiLevelType w:val=\"hybridMultilevel\"/>", i );
        for (int lvl = 0; lvl < 9; lvl++)
        {
            buf_printf( b, "<w:lvl w:ilvl=\"%d\"><w:start w:val=\"1\"/><w:numFmt w:val=\"%s\"/>", lvl, kinds[i].fmt );
            if (i == 1)
                buf_printf( b, "<w:lvlText w:val=\"%s\"/><w:lvlJc w:val=\"left\"/><w:pPr><w:ind w:left=\"%d\" w:hanging=\"360\"/></w:pPr>"
                               "<w:rPr><w:rFonts w:ascii=\"Symbol\" w:hAnsi=\"Symbol\" w:hint=\"default\"/></w:rPr></w:lvl>",
                            kinds[i].text, 720 * (lvl + 1) );
            else
            {
                char text[8];
                snprintf( text, sizeof(text), "%%%d%c", lvl + 1, kinds[i].text[2] );
                buf_printf( b, "<w:lvlText w:val=\"%s\"/><w:lvlJc w:val=\"left\"/><w:pPr><w:ind w:left=\"%d\" w:hanging=\"360\"/></w:pPr></w:lvl>",
                            text, 720 * (lvl + 1) );
            }
        }
        buf_str( b, "</w:abstractNum>" );
    }
    for (int i = 1; i < 8; i++) buf_printf( b, "<w:num w:numId=\"%d\"><w:abstractNumId w:val=\"%d\"/></w:num>", i, i );
    buf_str( b, "</w:numbering>" );
}

static const char ns_root[] =
    "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\" "
    "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
    "xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\" "
    "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
    "xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\">";

/* the document element of the package read, with what the new body needs */
static void root_tag( struct buffer *b, const struct docx_keep *keep )
{
    static const struct { const char *attr, *full; } needed[] =
    {
        { "xmlns:w=", "xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"" },
        { "xmlns:r=", "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"" },
        { "xmlns:wp=", "xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\"" },
    };
    size_t len;

    if (!keep || !keep->root || (len = strlen( keep->root )) < 3)
    {
        buf_str( b, ns_root );
        return;
    }
    buf_add( b, keep->root, len - 1 );
    for (int i = 0; i < ARRAY_SIZE(needed); i++)
        if (!strstr( keep->root, needed[i].attr )) buf_printf( b, " %s", needed[i].full );
    buf_str( b, ">" );
}

/* the relationships the package had, but its pictures and links: they are written anew */
static void kept_rels( struct buffer *out, const struct docx_keep *keep, BOOL *has_numbering, BOOL *has_styles,
                       BOOL *has_settings, BOOL *has_theme )
{
    char path[300];
    const char *name = strrchr( keep->main_part, '/' ) ? strrchr( keep->main_part, '/' ) + 1 : keep->main_part;
    char base[128];
    size_t len;
    BYTE *rels;
    struct xml x;
    enum xml_token t;

    lstrcpynA( base, keep->main_part, sizeof(base) );
    if (strrchr( base, '/' )) strrchr( base, '/' )[1] = 0;
    else base[0] = 0;
    snprintf( path, sizeof(path), "%s_rels/%s.rels", base, name );
    if (!(rels = zip_read( keep->zip, path, &len ))) return;
    xml_init( &x, (char *)rels, len );
    while ((t = xml_next( &x )) != XML_END)
    {
        char id[64], type[256], target[512], mode[32];

        if (t != XML_OPEN || strcmp( x.name, "Relationship" )) continue;
        xml_attr( &x, "Id", id, sizeof(id) );
        xml_attr( &x, "Type", type, sizeof(type) );
        xml_attr( &x, "Target", target, sizeof(target) );
        xml_attr( &x, "TargetMode", mode, sizeof(mode) );
        if (strstr( type, "/image" ) || strstr( type, "/hyperlink" ) || strstr( type, "/numbering" )) continue;
        if (strstr( type, "/styles" )) *has_styles = TRUE;
        if (strstr( type, "/settings" )) *has_settings = TRUE;
        if (strstr( type, "/theme" )) *has_theme = TRUE;
        buf_str( out, "<Relationship Id=\"" );
        xml_escape( out, id, strlen( id ) );
        buf_str( out, "\" Type=\"" );
        xml_escape( out, type, strlen( type ) );
        buf_str( out, "\" Target=\"" );
        xml_escape( out, target, strlen( target ) );
        buf_printf( out, "\"%s/>", mode[0] ? " TargetMode=\"External\"" : "" );
    }
    (void)has_numbering;
    xml_free( &x );
    free( rels );
}

BOOL rtf_to_docx( const char *rtf, size_t len, const struct page_setup *setup, const struct docx_keep *keep,
                  BYTE **out, size_t *out_len )
{
    struct writer w = { 0 };
    struct buffer doc = { 0 }, rels = { 0 }, types = { 0 }, part = { 0 };
    BOOL has_styles = FALSE, has_settings = FALSE, has_theme = FALSE, has_numbering = FALSE;
    char base[128] = "word/", main_part[128] = "word/document.xml";
    SYSTEMTIME now;
    char stamp[32];

    w.p = rtf;
    w.end = rtf + len;
    w.keep = keep;
    w.setup = setup;
    w.zip = zipw_create();
    if (!w.zip) return FALSE;
    if (keep)
    {
        lstrcpynA( main_part, keep->main_part, sizeof(main_part) );
        lstrcpynA( base, keep->main_part, sizeof(base) );
        if (strrchr( base, '/' )) strrchr( base, '/' )[1] = 0;
        else base[0] = 0;
    }
    parse_rtf( &w );

    /* document.xml */
    buf_str( &doc, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n" );
    root_tag( &doc, keep );
    buf_str( &doc, "<w:body>" );
    if (w.body.len) buf_add( &doc, w.body.data, w.body.len );
    if (keep && keep->sect_pr) buf_str( &doc, keep->sect_pr );
    else
        buf_printf( &doc, "<w:sectPr><w:pgSz w:w=\"%d\" w:h=\"%d\"%s/><w:pgMar w:top=\"%d\" w:right=\"%d\" w:bottom=\"%d\" "
                          "w:left=\"%d\" w:header=\"708\" w:footer=\"708\" w:gutter=\"0\"/><w:cols w:space=\"708\"/>"
                          "<w:docGrid w:linePitch=\"360\"/></w:sectPr>",
                    setup->width, setup->height, setup->width > setup->height ? " w:orient=\"landscape\"" : "",
                    setup->top, setup->right, setup->bottom, setup->left );
    buf_str( &doc, "</w:body></w:document>" );

    /* the package */
    if (keep)
    {
        /* the parts kept, but those written anew */
        for (int i = 0; i < zip_count( keep->zip ); i++)
        {
            const char *name = zip_name( keep->zip, i );
            char rels_name[300];
            const char *leaf = strrchr( main_part, '/' ) ? strrchr( main_part, '/' ) + 1 : main_part;
            BYTE *data;
            size_t size;

            snprintf( rels_name, sizeof(rels_name), "%s_rels/%s.rels", base, leaf );
            if (!strcmp( name, main_part ) || !strcmp( name, rels_name ) || !strcmp( name, "[Content_Types].xml" ) ||
                !strncmp( name, "word/media/awimage", 18 ) || !strcmp( name, "docProps/app.xml" ))
                continue;
            if (!strcmp( name + strlen( base ), "numbering.xml" ) && !strncmp( name, base, strlen( base ) ))
                continue;
            /* the document's own pictures are written anew: a picture stays
             * only when a header, footer or note still names it */
            if (strstr( name, "/media/" ))
            {
                const char *file = strrchr( name, '/' ) + 1;
                BOOL used = FALSE;

                for (int j = 0; j < zip_count( keep->zip ) && !used; j++)
                {
                    const char *other = zip_name( keep->zip, j );
                    BYTE *rels;

                    if (!strstr( other, ".rels" ) || !strcmp( other, rels_name )) continue;
                    if ((rels = zip_read( keep->zip, other, &size )))
                    {
                        used = strstr( (char *)rels, file ) != NULL;
                        free( rels );
                    }
                }
                if (!used) continue;
            }
            if ((data = zip_read( keep->zip, name, &size )))
            {
                zipw_add( w.zip, name, data, size, TRUE );
                free( data );
            }
        }
        kept_rels( &rels, keep, &has_numbering, &has_styles, &has_settings, &has_theme );
    }
    {
        char name[256];

        if (!has_styles)
        {
            snprintf( name, sizeof(name), "%sstyles.xml", base );
            zipw_add( w.zip, name, styles_xml, strlen( styles_xml ), TRUE );
            buf_str( &rels, "<Relationship Id=\"rIdAwStyles\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>" );
        }
        if (!has_theme)
        {
            snprintf( name, sizeof(name), "%stheme/theme1.xml", base );
            zipw_add( w.zip, name, theme_xml, strlen( theme_xml ), TRUE );
            buf_str( &rels, "<Relationship Id=\"rIdAwTheme\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/theme\" Target=\"theme/theme1.xml\"/>" );
        }
        if (!has_settings)
        {
            static const char settings[] =
                "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
                "<w:settings xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
                "<w:defaultTabStop w:val=\"708\"/><w:characterSpacingControl w:val=\"doNotCompress\"/>"
                "<w:compat><w:compatSetting w:name=\"compatibilityMode\" w:uri=\"http://schemas.microsoft.com/office/word\" w:val=\"15\"/></w:compat>"
                "</w:settings>";
            snprintf( name, sizeof(name), "%ssettings.xml", base );
            zipw_add( w.zip, name, settings, strlen( settings ), TRUE );
            buf_str( &rels, "<Relationship Id=\"rIdAwSettings\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/settings\" Target=\"settings.xml\"/>" );
        }
        numbering_part( &part );
        snprintf( name, sizeof(name), "%snumbering.xml", base );
        zipw_add( w.zip, name, part.data, part.len, TRUE );
        buf_free( &part );
        buf_str( &rels, "<Relationship Id=\"rIdAwNumbering\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/numbering\" Target=\"numbering.xml\"/>" );
    }
    if (w.rels.len) buf_add( &rels, w.rels.data, w.rels.len );

    zipw_add( w.zip, main_part, doc.data, doc.len, TRUE );
    {
        char name[300];
        struct buffer r = { 0 };
        const char *leaf = strrchr( main_part, '/' ) ? strrchr( main_part, '/' ) + 1 : main_part;

        buf_str( &r, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
                     "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">" );
        if (rels.len) buf_add( &r, rels.data, rels.len );
        buf_str( &r, "</Relationships>" );
        snprintf( name, sizeof(name), "%s_rels/%s.rels", base, leaf );
        zipw_add( w.zip, name, r.data, r.len, TRUE );
        buf_free( &r );
    }

    /* the content types: the package's own, with what this adds */
    {
        BYTE *old = NULL;
        size_t old_len;

        if (keep) old = zip_read( keep->zip, "[Content_Types].xml", &old_len );
        if (old)
        {
            char *end = strstr( (char *)old, "</Types>" );
            if (end) old_len = end - (char *)old;
            buf_add( &types, old, old_len );
            free( old );
        }
        else
        {
            buf_str( &types, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
                             "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
                             "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
                             "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
                             "<Override PartName=\"/docProps/core.xml\" ContentType=\"application/vnd.openxmlformats-package.core-properties+xml\"/>" );
            buf_printf( &types, "<Override PartName=\"/%s\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>", main_part );
            buf_printf( &types, "<Override PartName=\"/%sstyles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml\"/>", base );
            buf_printf( &types, "<Override PartName=\"/%ssettings.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.settings+xml\"/>", base );
            buf_printf( &types, "<Override PartName=\"/%stheme/theme1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.theme+xml\"/>", base );
        }
        if (!strstr( types.data, "Extension=\"png\"" ) && !strstr( types.data, "Extension=\"PNG\"" ))
            buf_str( &types, "<Default Extension=\"png\" ContentType=\"image/png\"/>" );
        if (!strstr( types.data, "numbering.xml\"" ))
            buf_printf( &types, "<Override PartName=\"/%snumbering.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml\"/>", base );
        if (!strstr( types.data, "/docProps/app.xml\"" ))
            buf_str( &types, "<Override PartName=\"/docProps/app.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.extended-properties+xml\"/>" );
        buf_str( &types, "</Types>" );
        zipw_add( w.zip, "[Content_Types].xml", types.data, types.len, TRUE );
    }
    if (!keep)
    {
        static const char root_rels[] =
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
            "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
            "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/>"
            "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties\" Target=\"docProps/core.xml\"/>"
            "<Relationship Id=\"rId3\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/extended-properties\" Target=\"docProps/app.xml\"/>"
            "</Relationships>";
        struct buffer core = { 0 };

        zipw_add( w.zip, "_rels/.rels", root_rels, strlen( root_rels ), TRUE );
        GetSystemTime( &now );
        snprintf( stamp, sizeof(stamp), "%04d-%02d-%02dT%02d:%02d:%02dZ", now.wYear, now.wMonth, now.wDay, now.wHour,
                  now.wMinute, now.wSecond );
        buf_printf( &core, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
                           "<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" "
                           "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:dcterms=\"http://purl.org/dc/terms/\" "
                           "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">"
                           "<dcterms:created xsi:type=\"dcterms:W3CDTF\">%s</dcterms:created>"
                           "<dcterms:modified xsi:type=\"dcterms:W3CDTF\">%s</dcterms:modified></cp:coreProperties>", stamp, stamp );
        zipw_add( w.zip, "docProps/core.xml", core.data, core.len, TRUE );
        buf_free( &core );
    }
    {
        static const char app[] =
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
            "<Properties xmlns=\"http://schemas.openxmlformats.org/officeDocument/2006/extended-properties\">"
            "<Application>Arctic WordPad</Application></Properties>";
        zipw_add( w.zip, "docProps/app.xml", app, strlen( app ), TRUE );
    }

    *out = zipw_finish( w.zip, out_len );
    buf_free( &doc );
    buf_free( &rels );
    buf_free( &types );
    buf_free( &w.body );
    buf_free( &w.para );
    buf_free( &w.cell );
    buf_free( &w.row );
    buf_free( &w.table_rows );
    buf_free( &w.rels );
    buf_free( &w.run_text );
    buf_free( &w.link_text );
    return *out != NULL;
}

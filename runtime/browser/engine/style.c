/*
 * Arctic web engine: the computed style of an element
 *
 * lexbor's style module did the hard part of the cascade: it matched every
 * rule to the elements and kept, per property, the declaration that wins by
 * specificity, !important and order. What is left is here: inheritance, the
 * user agent's own style per tag, and turning declared values into pixels
 * and colours.
 *
 * The user agent style is this table of tags and not a style sheet: lexbor
 * does not tell the origins of rules apart, so a sheet of ours could beat a
 * page's rule by specificity. Applied first, any declaration of the page
 * replaces it, as the cascade wants.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "arctichtml_private.h"

#define DEFAULT_FONT_SIZE 16.0f

/* the viewport, for vw and vh: set by the layout before it computes styles */
float style_viewport_width = 1024, style_viewport_height = 768;

static DWORD rgba( unsigned r, unsigned g, unsigned b, unsigned a )
{
    return (a << 24) | (r << 16) | (g << 8) | b;
}

/* --- named colours (CSS Color 4), sorted for bsearch --- */

static const struct { const char *name; DWORD rgb; } named_colors[] =
{
    { "aliceblue", 0xf0f8ff }, { "antiquewhite", 0xfaebd7 }, { "aqua", 0x00ffff }, { "aquamarine", 0x7fffd4 },
    { "azure", 0xf0ffff }, { "beige", 0xf5f5dc }, { "bisque", 0xffe4c4 }, { "black", 0x000000 },
    { "blanchedalmond", 0xffebcd }, { "blue", 0x0000ff }, { "blueviolet", 0x8a2be2 }, { "brown", 0xa52a2a },
    { "burlywood", 0xdeb887 }, { "cadetblue", 0x5f9ea0 }, { "chartreuse", 0x7fff00 }, { "chocolate", 0xd2691e },
    { "coral", 0xff7f50 }, { "cornflowerblue", 0x6495ed }, { "cornsilk", 0xfff8dc }, { "crimson", 0xdc143c },
    { "cyan", 0x00ffff }, { "darkblue", 0x00008b }, { "darkcyan", 0x008b8b }, { "darkgoldenrod", 0xb8860b },
    { "darkgray", 0xa9a9a9 }, { "darkgreen", 0x006400 }, { "darkgrey", 0xa9a9a9 }, { "darkkhaki", 0xbdb76b },
    { "darkmagenta", 0x8b008b }, { "darkolivegreen", 0x556b2f }, { "darkorange", 0xff8c00 },
    { "darkorchid", 0x9932cc }, { "darkred", 0x8b0000 }, { "darksalmon", 0xe9967a }, { "darkseagreen", 0x8fbc8f },
    { "darkslateblue", 0x483d8b }, { "darkslategray", 0x2f4f4f }, { "darkslategrey", 0x2f4f4f },
    { "darkturquoise", 0x00ced1 }, { "darkviolet", 0x9400d3 }, { "deeppink", 0xff1493 },
    { "deepskyblue", 0x00bfff }, { "dimgray", 0x696969 }, { "dimgrey", 0x696969 }, { "dodgerblue", 0x1e90ff },
    { "firebrick", 0xb22222 }, { "floralwhite", 0xfffaf0 }, { "forestgreen", 0x228b22 }, { "fuchsia", 0xff00ff },
    { "gainsboro", 0xdcdcdc }, { "ghostwhite", 0xf8f8ff }, { "gold", 0xffd700 }, { "goldenrod", 0xdaa520 },
    { "gray", 0x808080 }, { "green", 0x008000 }, { "greenyellow", 0xadff2f }, { "grey", 0x808080 },
    { "honeydew", 0xf0fff0 }, { "hotpink", 0xff69b4 }, { "indianred", 0xcd5c5c }, { "indigo", 0x4b0082 },
    { "ivory", 0xfffff0 }, { "khaki", 0xf0e68c }, { "lavender", 0xe6e6fa }, { "lavenderblush", 0xfff0f5 },
    { "lawngreen", 0x7cfc00 }, { "lemonchiffon", 0xfffacd }, { "lightblue", 0xadd8e6 }, { "lightcoral", 0xf08080 },
    { "lightcyan", 0xe0ffff }, { "lightgoldenrodyellow", 0xfafad2 }, { "lightgray", 0xd3d3d3 },
    { "lightgreen", 0x90ee90 }, { "lightgrey", 0xd3d3d3 }, { "lightpink", 0xffb6c1 }, { "lightsalmon", 0xffa07a },
    { "lightseagreen", 0x20b2aa }, { "lightskyblue", 0x87cefa }, { "lightslategray", 0x778899 },
    { "lightslategrey", 0x778899 }, { "lightsteelblue", 0xb0c4de }, { "lightyellow", 0xffffe0 },
    { "lime", 0x00ff00 }, { "limegreen", 0x32cd32 }, { "linen", 0xfaf0e6 }, { "magenta", 0xff00ff },
    { "maroon", 0x800000 }, { "mediumaquamarine", 0x66cdaa }, { "mediumblue", 0x0000cd },
    { "mediumorchid", 0xba55d3 }, { "mediumpurple", 0x9370db }, { "mediumseagreen", 0x3cb371 },
    { "mediumslateblue", 0x7b68ee }, { "mediumspringgreen", 0x00fa9a }, { "mediumturquoise", 0x48d1cc },
    { "mediumvioletred", 0xc71585 }, { "midnightblue", 0x191970 }, { "mintcream", 0xf5fffa },
    { "mistyrose", 0xffe4e1 }, { "moccasin", 0xffe4b5 }, { "navajowhite", 0xffdead }, { "navy", 0x000080 },
    { "oldlace", 0xfdf5e6 }, { "olive", 0x808000 }, { "olivedrab", 0x6b8e23 }, { "orange", 0xffa500 },
    { "orangered", 0xff4500 }, { "orchid", 0xda70d6 }, { "palegoldenrod", 0xeee8aa }, { "palegreen", 0x98fb98 },
    { "paleturquoise", 0xafeeee }, { "palevioletred", 0xdb7093 }, { "papayawhip", 0xffefd5 },
    { "peachpuff", 0xffdab9 }, { "peru", 0xcd853f }, { "pink", 0xffc0cb }, { "plum", 0xdda0dd },
    { "powderblue", 0xb0e0e6 }, { "purple", 0x800080 }, { "rebeccapurple", 0x663399 }, { "red", 0xff0000 },
    { "rosybrown", 0xbc8f8f }, { "royalblue", 0x4169e1 }, { "saddlebrown", 0x8b4513 }, { "salmon", 0xfa8072 },
    { "sandybrown", 0xf4a460 }, { "seagreen", 0x2e8b57 }, { "seashell", 0xfff5ee }, { "sienna", 0xa0522d },
    { "silver", 0xc0c0c0 }, { "skyblue", 0x87ceeb }, { "slateblue", 0x6a5acd }, { "slategray", 0x708090 },
    { "slategrey", 0x708090 }, { "snow", 0xfffafa }, { "springgreen", 0x00ff7f }, { "steelblue", 0x4682b4 },
    { "tan", 0xd2b48c }, { "teal", 0x008080 }, { "thistle", 0xd8bfd8 }, { "tomato", 0xff6347 },
    { "turquoise", 0x40e0d0 }, { "violet", 0xee82ee }, { "wheat", 0xf5deb3 }, { "white", 0xffffff },
    { "whitesmoke", 0xf5f5f5 }, { "yellow", 0xffff00 }, { "yellowgreen", 0x9acd32 },
};

static int compare_color( const void *key, const void *entry )
{
    const lxb_css_data_t *data = key;
    const char *name = *(const char *const *)entry;
    int diff = _strnicmp( (const char *)data->name, name, data->length );

    return diff ? diff : (name[data->length] ? -1 : 0);
}

static float channel( const lxb_css_value_number_percentage_t *value, float scale )
{
    switch (value->type)
    {
    case LXB_CSS_VALUE__NUMBER:     return (float)value->u.number.num;
    case LXB_CSS_VALUE__PERCENTAGE: return (float)value->u.percentage.num * scale / 100.0f;
    default:                        return 0;
    }
}

static unsigned clamp_byte( float value )
{
    return value <= 0 ? 0 : value >= 255 ? 255 : (unsigned)(value + 0.5f);
}

static float hue_to_rgb( float p, float q, float t )
{
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0f / 6) return p + (q - p) * 6 * t;
    if (t < 0.5f) return q;
    if (t < 2.0f / 3) return p + (q - p) * (2.0f / 3 - t) * 6;
    return p;
}

/* a colour of lexbor, or the current one when it is not one we know */
static DWORD color_value( const lxb_css_value_color_t *color, DWORD current )
{
    const lxb_css_data_t *data;

    switch (color->type)
    {
    case LXB_CSS_VALUE_HEX:
    {
        const lxb_css_value_color_hex_rgba_t *c = &color->u.hex.rgba;
        BOOL alpha = color->u.hex.type == LXB_CSS_PROPERTY_COLOR_HEX_TYPE_4 ||
                     color->u.hex.type == LXB_CSS_PROPERTY_COLOR_HEX_TYPE_8;
        /* #fff keeps a digit per channel: f is ff */
        unsigned scale = color->u.hex.type == LXB_CSS_PROPERTY_COLOR_HEX_TYPE_3 ||
                         color->u.hex.type == LXB_CSS_PROPERTY_COLOR_HEX_TYPE_4 ? 17 : 1;

        return rgba( c->r * scale, c->g * scale, c->b * scale, alpha ? c->a * scale : 255 );
    }
    case LXB_CSS_VALUE_RGB:
    case LXB_CSS_VALUE_RGBA:
    {
        const lxb_css_value_color_rgba_t *c = &color->u.rgb;
        float a = c->a.type == LXB_CSS_VALUE__NUMBER ? (float)c->a.u.number.num * 255 :
                  c->a.type == LXB_CSS_VALUE__PERCENTAGE ? (float)c->a.u.percentage.num * 2.55f : 255;

        return rgba( clamp_byte( channel( &c->r, 255 ) ), clamp_byte( channel( &c->g, 255 ) ),
                     clamp_byte( channel( &c->b, 255 ) ), clamp_byte( a ) );
    }
    case LXB_CSS_VALUE_HSL:
    case LXB_CSS_VALUE_HSLA:
    {
        const lxb_css_value_color_hsla_t *c = &color->u.hsl;
        float h = (float)(c->h.type == LXB_CSS_VALUE__NUMBER ? c->h.u.number.num : c->h.u.angle.num) / 360.0f;
        float s = (float)c->s.percentage.num / 100.0f, l = (float)c->l.percentage.num / 100.0f;
        float q = l < 0.5f ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
        float a = c->a.type == LXB_CSS_VALUE__NUMBER ? (float)c->a.u.number.num * 255 :
                  c->a.type == LXB_CSS_VALUE__PERCENTAGE ? (float)c->a.u.percentage.num * 2.55f : 255;

        h -= floorf( h );
        return rgba( clamp_byte( hue_to_rgb( p, q, h + 1.0f / 3 ) * 255 ), clamp_byte( hue_to_rgb( p, q, h ) * 255 ),
                     clamp_byte( hue_to_rgb( p, q, h - 1.0f / 3 ) * 255 ), clamp_byte( a ) );
    }
    case LXB_CSS_VALUE_TRANSPARENT:
        return 0;
    case LXB_CSS_VALUE_CURRENTCOLOR:
        return current;
    default:
        if ((data = lxb_css_value_by_id( color->type )))
        {
            const void *found = bsearch( data, named_colors, ARRAY_SIZE(named_colors), sizeof(named_colors[0]),
                                         compare_color );
            if (found) return 0xff000000 | ((const typeof(named_colors[0]) *)found)->rgb;
        }
        return current;
    }
}

/* --- lengths --- */

static float length_value( const lxb_css_value_length_t *length, float font_size )
{
    float num = (float)length->num;

    /* the field is one of lexbor's unit enums, the constants come from several */
    switch ((unsigned int)length->unit)
    {
    case LXB_CSS_UNIT_PX:  return num;
    case LXB_CSS_UNIT_EM:  return num * font_size;
    case LXB_CSS_UNIT_REM: return num * DEFAULT_FONT_SIZE;
    case LXB_CSS_UNIT_EX:
    case LXB_CSS_UNIT_CH:  return num * font_size * 0.5f;
    case LXB_CSS_UNIT_PT:  return num * 96.0f / 72.0f;
    case LXB_CSS_UNIT_PC:  return num * 16.0f;
    case LXB_CSS_UNIT_IN:  return num * 96.0f;
    case LXB_CSS_UNIT_CM:  return num * 96.0f / 2.54f;
    case LXB_CSS_UNIT_MM:  return num * 96.0f / 25.4f;
    case LXB_CSS_UNIT_Q:   return num * 96.0f / 101.6f;
    case LXB_CSS_UNIT_VW:  return num * style_viewport_width / 100.0f;
    case LXB_CSS_UNIT_VH:  return num * style_viewport_height / 100.0f;
    default:               return num;
    }
}

static BOOL length_percentage( const lxb_css_value_length_percentage_t *value, float font_size, struct length *out )
{
    switch (value->type)
    {
    case LXB_CSS_VALUE__LENGTH:
        out->unit = LEN_PX;
        out->value = length_value( &value->u.length, font_size );
        return TRUE;
    case LXB_CSS_VALUE__PERCENTAGE:
        out->unit = LEN_PERCENT;
        out->value = (float)value->u.percentage.num;
        return TRUE;
    case LXB_CSS_VALUE__NUMBER:     /* a bare 0 */
        out->unit = LEN_PX;
        out->value = (float)value->u.length.num;
        return TRUE;
    case LXB_CSS_VALUE_AUTO:
        out->unit = LEN_AUTO;
        out->value = 0;
        return TRUE;
    default:
        return FALSE;
    }
}

float length_px( const struct length *length, float containing )
{
    switch (length->unit)
    {
    case LEN_PX:      return length->value;
    case LEN_PERCENT: return length->value * containing / 100.0f;
    default:          return 0;
    }
}

static float border_width( const lxb_css_value_length_type_t *width, float font_size )
{
    switch (width->type)
    {
    case LXB_CSS_VALUE__LENGTH: return length_value( &width->length, font_size );
    case LXB_CSS_VALUE__NUMBER: return (float)width->length.num;     /* border: 0 */
    case LXB_CSS_VALUE_THIN:    return 1;
    case LXB_CSS_VALUE_THICK:   return 5;
    case LXB_CSS_VALUE_MEDIUM:
    default:                    return 3;
    }
}

/* --- the user agent's style --- */

enum { UA_INLINE, UA_BLOCK, UA_NONE, UA_LIST_ITEM };

struct ua
{
    unsigned int display : 2;
    unsigned int bold : 1, italic : 1, underline : 1, strike : 1, mono : 1, pre : 1, center : 1;
    float        font_scale;     /* 0: inherited */
    float        margin_v;       /* em, top and bottom */
    float        margin_h;       /* em, left and right */
    float        padding_left;   /* px */
};

static const struct ua *ua_for( lxb_tag_id_t tag )
{
    static const struct ua none = { UA_NONE }, block = { UA_BLOCK }, inline_ = { UA_INLINE };
    static const struct ua p = { UA_BLOCK, .margin_v = 1 }, blockquote = { UA_BLOCK, .margin_v = 1, .margin_h = 2.5f };
    static const struct ua h1 = { UA_BLOCK, .bold = 1, .font_scale = 2.0f, .margin_v = 0.67f };
    static const struct ua h2 = { UA_BLOCK, .bold = 1, .font_scale = 1.5f, .margin_v = 0.83f };
    static const struct ua h3 = { UA_BLOCK, .bold = 1, .font_scale = 1.17f, .margin_v = 1.0f };
    static const struct ua h4 = { UA_BLOCK, .bold = 1, .font_scale = 1.0f, .margin_v = 1.33f };
    static const struct ua h5 = { UA_BLOCK, .bold = 1, .font_scale = 0.83f, .margin_v = 1.67f };
    static const struct ua h6 = { UA_BLOCK, .bold = 1, .font_scale = 0.67f, .margin_v = 2.33f };
    static const struct ua list = { UA_BLOCK, .margin_v = 1, .padding_left = 40 };
    static const struct ua li = { UA_LIST_ITEM }, dd = { UA_BLOCK, .margin_h = 2.5f };
    static const struct ua pre = { UA_BLOCK, .mono = 1, .pre = 1, .margin_v = 1 };
    static const struct ua bold = { UA_INLINE, .bold = 1 }, italic = { UA_INLINE, .italic = 1 };
    static const struct ua underline = { UA_INLINE, .underline = 1 }, strike = { UA_INLINE, .strike = 1 };
    static const struct ua mono = { UA_INLINE, .mono = 1 }, small = { UA_INLINE, .font_scale = 0.83f };
    static const struct ua big = { UA_INLINE, .font_scale = 1.2f }, center = { UA_BLOCK, .center = 1 };
    static const struct ua th = { UA_BLOCK, .bold = 1, .center = 1 };

    switch (tag)
    {
    case LXB_TAG_HEAD: case LXB_TAG_SCRIPT: case LXB_TAG_STYLE: case LXB_TAG_TITLE: case LXB_TAG_META:
    case LXB_TAG_LINK: case LXB_TAG_TEMPLATE: case LXB_TAG_NOSCRIPT: case LXB_TAG_BASE:
        return &none;
    case LXB_TAG_HTML: case LXB_TAG_BODY: case LXB_TAG_DIV: case LXB_TAG_HEADER: case LXB_TAG_FOOTER:
    case LXB_TAG_NAV: case LXB_TAG_SECTION: case LXB_TAG_ARTICLE: case LXB_TAG_ASIDE: case LXB_TAG_MAIN:
    case LXB_TAG_FORM: case LXB_TAG_FIGCAPTION: case LXB_TAG_ADDRESS: case LXB_TAG_DT: case LXB_TAG_TABLE:
    case LXB_TAG_TR: case LXB_TAG_THEAD: case LXB_TAG_TBODY: case LXB_TAG_TFOOT: case LXB_TAG_TD:
    case LXB_TAG_CAPTION: case LXB_TAG_DETAILS: case LXB_TAG_SUMMARY: case LXB_TAG_HGROUP: case LXB_TAG_LEGEND:
    case LXB_TAG_FIELDSET: case LXB_TAG_HR:
        return &block;
    case LXB_TAG_P: case LXB_TAG_DL:
        return &p;
    case LXB_TAG_BLOCKQUOTE: case LXB_TAG_FIGURE:
        return &blockquote;
    case LXB_TAG_H1: return &h1;
    case LXB_TAG_H2: return &h2;
    case LXB_TAG_H3: return &h3;
    case LXB_TAG_H4: return &h4;
    case LXB_TAG_H5: return &h5;
    case LXB_TAG_H6: return &h6;
    case LXB_TAG_UL: case LXB_TAG_OL: case LXB_TAG_MENU:
        return &list;
    case LXB_TAG_LI: return &li;
    case LXB_TAG_DD: return &dd;
    case LXB_TAG_PRE: case LXB_TAG_LISTING: case LXB_TAG_XMP: case LXB_TAG_PLAINTEXT:
        return &pre;
    case LXB_TAG_CENTER: return &center;
    case LXB_TAG_TH: return &th;
    case LXB_TAG_B: case LXB_TAG_STRONG: return &bold;
    case LXB_TAG_I: case LXB_TAG_EM: case LXB_TAG_CITE: case LXB_TAG_VAR: case LXB_TAG_DFN:
        return &italic;
    case LXB_TAG_U: case LXB_TAG_INS: case LXB_TAG_A: return &underline;
    case LXB_TAG_S: case LXB_TAG_STRIKE: case LXB_TAG_DEL: return &strike;
    case LXB_TAG_CODE: case LXB_TAG_KBD: case LXB_TAG_SAMP: case LXB_TAG_TT: return &mono;
    case LXB_TAG_SMALL: case LXB_TAG_SUB: case LXB_TAG_SUP: return &small;
    case LXB_TAG_BIG: return &big;
    default: return &inline_;
    }
}

/* --- the author's style, from lexbor --- */

static const lxb_css_rule_declaration_t *declared( lxb_dom_element_t *element, uintptr_t id )
{
    return lxb_dom_element_style_by_id( element, id );
}

static void apply_font_size( lxb_dom_element_t *element, const struct style *parent, struct style *style )
{
    const lxb_css_rule_declaration_t *decl = declared( element, LXB_CSS_PROPERTY_FONT_SIZE );
    const lxb_css_property_font_size_t *size;
    float parent_size = parent ? parent->font_size : DEFAULT_FONT_SIZE;

    if (!decl) return;
    size = decl->u.font_size;
    switch (size->type)
    {
    case LXB_CSS_VALUE_XX_SMALL: style->font_size = 9; break;
    case LXB_CSS_VALUE_X_SMALL:  style->font_size = 10; break;
    case LXB_CSS_VALUE_SMALL:    style->font_size = 13; break;
    case LXB_CSS_VALUE_MEDIUM:   style->font_size = 16; break;
    case LXB_CSS_VALUE_LARGE:    style->font_size = 18; break;
    case LXB_CSS_VALUE_X_LARGE:  style->font_size = 24; break;
    case LXB_CSS_VALUE_XX_LARGE: style->font_size = 32; break;
    case LXB_CSS_VALUE_SMALLER:  style->font_size = parent_size / 1.2f; break;
    case LXB_CSS_VALUE_LARGER:   style->font_size = parent_size * 1.2f; break;
    case LXB_CSS_VALUE_INHERIT:
    case LXB_CSS_VALUE_UNSET:    style->font_size = parent_size; break;
    case LXB_CSS_VALUE_INITIAL:  style->font_size = DEFAULT_FONT_SIZE; break;
    default:
        /* lexbor says _LENGTH for a percentage too: the length's own type tells them apart */
        if (size->length.type == LXB_CSS_VALUE__LENGTH)
            style->font_size = length_value( &size->length.u.length, parent_size );
        else if (size->length.type == LXB_CSS_VALUE__PERCENTAGE)
            style->font_size = (float)size->length.u.percentage.num * parent_size / 100.0f;
        break;
    }
    if (style->font_size < 1) style->font_size = 1;
}

static void apply_font_family( lxb_dom_element_t *element, struct style *style )
{
    const lxb_css_rule_declaration_t *decl = declared( element, LXB_CSS_PROPERTY_FONT_FAMILY );

    if (!decl) return;
    for (const lxb_css_property_family_name_t *name = decl->u.font_family->first; name; name = name->next)
    {
        if (name->generic)
        {
            enum generic_family generic;

            switch (name->u.type)
            {
            case LXB_CSS_FONT_FAMILY_SANS_SERIF: case LXB_CSS_FONT_FAMILY_UI_SANS_SERIF: generic = GENERIC_SANS; break;
            case LXB_CSS_FONT_FAMILY_MONOSPACE: case LXB_CSS_FONT_FAMILY_UI_MONOSPACE: generic = GENERIC_MONO; break;
            case LXB_CSS_FONT_FAMILY_CURSIVE: generic = GENERIC_CURSIVE; break;
            case LXB_CSS_FONT_FAMILY_FANTASY: generic = GENERIC_FANTASY; break;
            case LXB_CSS_FONT_FAMILY_SYSTEM_UI: case LXB_CSS_FONT_FAMILY_UI_ROUNDED: generic = GENERIC_SYSTEM_UI; break;
            default: generic = GENERIC_SERIF; break;
            }
            font_generic_face( generic, style->font.face );
            return;
        }
        else
        {
            WCHAR *wide = utf8_to_wide( (const char *)name->u.str.data, name->u.str.length, NULL );
            BOOL found = wide && font_face_for( wide, style->font.face );

            free( wide );
            if (found) return;
        }
    }
}

/* lexbor keeps the sides a shorthand gave; the rest come from them, as
 * CSS says: "1px 2px" is 1px top and bottom, 2px right and left */
static void expand_sides( lxb_css_value_length_percentage_t sides[4] )
{
    if (!sides[1].type) sides[1] = sides[0];
    if (!sides[2].type) sides[2] = sides[0];
    if (!sides[3].type) sides[3] = sides[1];
}

static void apply_lengths( lxb_dom_element_t *element, struct style *style )
{
    static const uintptr_t margin_ids[4] = { LXB_CSS_PROPERTY_MARGIN_TOP, LXB_CSS_PROPERTY_MARGIN_RIGHT,
                                             LXB_CSS_PROPERTY_MARGIN_BOTTOM, LXB_CSS_PROPERTY_MARGIN_LEFT };
    static const uintptr_t padding_ids[4] = { LXB_CSS_PROPERTY_PADDING_TOP, LXB_CSS_PROPERTY_PADDING_RIGHT,
                                              LXB_CSS_PROPERTY_PADDING_BOTTOM, LXB_CSS_PROPERTY_PADDING_LEFT };
    float fs = style->font_size;
    const lxb_css_rule_declaration_t *decl;
    struct length len;

    if ((decl = declared( element, LXB_CSS_PROPERTY_MARGIN )))
    {
        const lxb_css_property_margin_t *m = decl->u.margin;
        lxb_css_value_length_percentage_t sides[4] = { m->top, m->right, m->bottom, m->left };

        expand_sides( sides );
        for (int i = 0; i < 4; i++) length_percentage( &sides[i], fs, &style->margin[i] );
    }
    for (int i = 0; i < 4; i++)
        if ((decl = declared( element, margin_ids[i] ))) length_percentage( decl->u.margin_top, fs, &style->margin[i] );

    if ((decl = declared( element, LXB_CSS_PROPERTY_PADDING )))
    {
        const lxb_css_property_padding_t *p = decl->u.padding;
        lxb_css_value_length_percentage_t sides[4] = { p->top, p->right, p->bottom, p->left };

        expand_sides( sides );
        for (int i = 0; i < 4; i++)
            if (length_percentage( &sides[i], fs, &len ) && len.unit == LEN_PX) style->padding[i] = len.value;
    }
    for (int i = 0; i < 4; i++)
        if ((decl = declared( element, padding_ids[i] )) && length_percentage( decl->u.padding_top, fs, &len ) &&
            len.unit == LEN_PX)
            style->padding[i] = len.value;

    if ((decl = declared( element, LXB_CSS_PROPERTY_WIDTH ))) length_percentage( decl->u.width, fs, &style->width );
    if ((decl = declared( element, LXB_CSS_PROPERTY_HEIGHT ))) length_percentage( decl->u.height, fs, &style->height );
    if ((decl = declared( element, LXB_CSS_PROPERTY_MIN_WIDTH )))
        length_percentage( decl->u.width /* the same type */, fs, &style->min_width );
    if ((decl = declared( element, LXB_CSS_PROPERTY_MAX_WIDTH )))
        length_percentage( decl->u.width, fs, &style->max_width );  /* none is not a length: stays auto */
    if (((decl = declared( element, LXB_CSS_PROPERTY_OVERFLOW_X )) && decl->u.overflow_x->type != LXB_CSS_VALUE_VISIBLE) ||
        ((decl = declared( element, LXB_CSS_PROPERTY_OVERFLOW_Y )) && decl->u.overflow_y->type != LXB_CSS_VALUE_VISIBLE))
        style->clips = TRUE;
    if ((decl = declared( element, LXB_CSS_PROPERTY_TOP ))) length_percentage( decl->u.top, fs, &style->inset[0] );
    if ((decl = declared( element, LXB_CSS_PROPERTY_RIGHT ))) length_percentage( decl->u.right, fs, &style->inset[1] );
    if ((decl = declared( element, LXB_CSS_PROPERTY_BOTTOM ))) length_percentage( decl->u.bottom, fs, &style->inset[2] );
    if ((decl = declared( element, LXB_CSS_PROPERTY_LEFT ))) length_percentage( decl->u.left, fs, &style->inset[3] );
    if ((decl = declared( element, LXB_CSS_PROPERTY_BOX_SIZING )))
        style->border_box = decl->u.box_sizing->type == LXB_CSS_VALUE_BORDER_BOX;
}

/* --- what lexbor has no property for: css.c names it --arctic-<name> --- */

static BOOL custom_value( lxb_dom_element_t *element, const char *name, const char **value, size_t *len )
{
    const lxb_css_rule_declaration_t *decl;
    const char *s, *end;

    if (!(decl = lxb_dom_element_style_by_name( element, (const lxb_char_t *)name, strlen( name ) )) ||
        decl->type != LXB_CSS_PROPERTY__CUSTOM)
        return FALSE;
    s = (const char *)decl->u.custom->value.data;
    end = s + decl->u.custom->value.length;
    while (s < end && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')) s++;
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r')) end--;
    if (s == end) return FALSE;
    *value = s;
    *len = end - s;
    return TRUE;
}

static BOOL text_is( const char *s, size_t len, const char *word )
{
    return strlen( word ) == len && !_strnicmp( s, word, len );
}

/* a length written as text: a number and a unit */
static BOOL parse_length( const char *s, size_t len, float font_size, float *px )
{
    char buffer[64], *unit;
    double num;

    if (!len || len >= sizeof(buffer)) return FALSE;
    memcpy( buffer, s, len );
    buffer[len] = 0;
    num = strtod( buffer, &unit );
    if (unit == buffer) return FALSE;
    if (!*unit || !_stricmp( unit, "px" )) *px = (float)num;
    else if (!_stricmp( unit, "em" )) *px = (float)num * font_size;
    else if (!_stricmp( unit, "rem" )) *px = (float)num * DEFAULT_FONT_SIZE;
    else if (!_stricmp( unit, "pt" )) *px = (float)num * 96.0f / 72.0f;
    else if (!_stricmp( unit, "vw" )) *px = (float)num * style_viewport_width / 100.0f;
    else if (!_stricmp( unit, "vh" )) *px = (float)num * style_viewport_height / 100.0f;
    else return FALSE;
    return TRUE;
}

/* a length or a percentage written as text */
static BOOL parse_length_percent( const char *s, size_t len, float font_size, struct length *out )
{
    float px;

    if (len && s[len - 1] == '%')
    {
        char buffer[32];

        if (len >= sizeof(buffer)) return FALSE;
        memcpy( buffer, s, len - 1 );
        buffer[len - 1] = 0;
        out->unit = LEN_PERCENT;
        out->value = (float)atof( buffer );
        return TRUE;
    }
    if (text_is( s, len, "auto" ) || text_is( s, len, "content" ))
    {
        out->unit = LEN_AUTO;
        out->value = 0;
        return TRUE;
    }
    if (!parse_length( s, len, font_size, &px )) return FALSE;
    out->unit = LEN_PX;
    out->value = px;
    return TRUE;
}

static enum flex_align parse_align( const char *s, size_t len )
{
    if (text_is( s, len, "stretch" ) || text_is( s, len, "normal" )) return FALIGN_STRETCH;
    if (text_is( s, len, "center" )) return FALIGN_CENTER;
    if (text_is( s, len, "flex-end" ) || text_is( s, len, "end" ) || text_is( s, len, "self-end" )) return FALIGN_END;
    if (text_is( s, len, "baseline" ) || text_is( s, len, "first baseline" )) return FALIGN_BASELINE;
    if (text_is( s, len, "auto" )) return FALIGN_AUTO;
    return FALIGN_START;
}

/* the words of a value, one at a time */
static BOOL next_word( const char **s, const char *end, const char **word, size_t *len )
{
    while (*s < end && (**s == ' ' || **s == '\t' || **s == '\n')) (*s)++;
    if (*s >= end) return FALSE;
    *word = *s;
    while (*s < end && **s != ' ' && **s != '\t' && **s != '\n') (*s)++;
    *len = *s - *word;
    return TRUE;
}

static void apply_flex( lxb_dom_element_t *element, struct style *style )
{
    const char *value, *end, *word;
    size_t len, word_len;

    if (custom_value( element, "--arctic-flex-flow", &value, &len ))
    {
        for (end = value + len; next_word( &value, end, &word, &word_len ); )
        {
            if (text_is( word, word_len, "row" )) style->flex_direction = FLEX_ROW;
            else if (text_is( word, word_len, "row-reverse" )) style->flex_direction = FLEX_ROW_REVERSE;
            else if (text_is( word, word_len, "column" )) style->flex_direction = FLEX_COLUMN;
            else if (text_is( word, word_len, "column-reverse" )) style->flex_direction = FLEX_COLUMN_REVERSE;
            else if (text_is( word, word_len, "wrap" ) || text_is( word, word_len, "wrap-reverse" )) style->flex_wrap = TRUE;
            else if (text_is( word, word_len, "nowrap" )) style->flex_wrap = FALSE;
        }
    }
    if (custom_value( element, "--arctic-flex-direction", &value, &len ))
    {
        if (text_is( value, len, "row-reverse" )) style->flex_direction = FLEX_ROW_REVERSE;
        else if (text_is( value, len, "column" )) style->flex_direction = FLEX_COLUMN;
        else if (text_is( value, len, "column-reverse" )) style->flex_direction = FLEX_COLUMN_REVERSE;
        else style->flex_direction = FLEX_ROW;
    }
    if (custom_value( element, "--arctic-flex-wrap", &value, &len ))
        style->flex_wrap = !text_is( value, len, "nowrap" );
    if (custom_value( element, "--arctic-justify-content", &value, &len ))
    {
        if (text_is( value, len, "center" )) style->justify = JUSTIFY_CENTER;
        else if (text_is( value, len, "flex-end" ) || text_is( value, len, "end" ) || text_is( value, len, "right" ))
            style->justify = JUSTIFY_END;
        else if (text_is( value, len, "space-between" )) style->justify = JUSTIFY_BETWEEN;
        else if (text_is( value, len, "space-around" )) style->justify = JUSTIFY_AROUND;
        else if (text_is( value, len, "space-evenly" )) style->justify = JUSTIFY_EVENLY;
        else style->justify = JUSTIFY_START;
    }
    if (custom_value( element, "--arctic-align-items", &value, &len ))
    {
        style->align_items = parse_align( value, len );
        if (style->align_items == FALIGN_AUTO) style->align_items = FALIGN_STRETCH;
    }
    if (custom_value( element, "--arctic-align-self", &value, &len )) style->align_self = parse_align( value, len );
    if (custom_value( element, "--arctic-gap", &value, &len ))
    {
        struct length row = { LEN_PX, 0 }, column;

        end = value + len;
        if (next_word( &value, end, &word, &word_len )) parse_length_percent( word, word_len, style->font_size, &row );
        column = row;
        if (next_word( &value, end, &word, &word_len )) parse_length_percent( word, word_len, style->font_size, &column );
        style->gap_row = row.unit == LEN_PX ? row.value : 0;
        style->gap_column = column.unit == LEN_PX ? column.value : 0;
    }
    if (custom_value( element, "--arctic-row-gap", &value, &len ))
    {
        struct length gap;
        if (parse_length_percent( value, len, style->font_size, &gap ) && gap.unit == LEN_PX) style->gap_row = gap.value;
    }
    if (custom_value( element, "--arctic-column-gap", &value, &len ))
    {
        struct length gap;
        if (parse_length_percent( value, len, style->font_size, &gap ) && gap.unit == LEN_PX) style->gap_column = gap.value;
    }

    /* flex: none | auto | <grow> [<shrink>] [<basis>] | <basis> */
    if (custom_value( element, "--arctic-flex", &value, &len ))
    {
        int numbers = 0;

        if (text_is( value, len, "none" )) { style->grow = 0; style->shrink = 0; style->basis.unit = LEN_AUTO; }
        else if (text_is( value, len, "auto" )) { style->grow = 1; style->shrink = 1; style->basis.unit = LEN_AUTO; }
        else if (text_is( value, len, "initial" )) { style->grow = 0; style->shrink = 1; style->basis.unit = LEN_AUTO; }
        else
        {
            BOOL basis = FALSE;

            style->grow = 1;    /* the defaults of the shorthand: 1 1 0 */
            style->shrink = 1;
            style->basis.unit = LEN_PX;
            style->basis.value = 0;
            for (end = value + len; next_word( &value, end, &word, &word_len ); )
            {
                char *stop, buffer[32];
                double number;

                if (word_len < sizeof(buffer))
                {
                    memcpy( buffer, word, word_len );
                    buffer[word_len] = 0;
                    number = strtod( buffer, &stop );
                    if (stop != buffer && !*stop && numbers < 2)
                    {
                        if (numbers++ == 0) style->grow = (float)number;
                        else style->shrink = (float)number;
                        continue;
                    }
                }
                if (!basis && parse_length_percent( word, word_len, style->font_size, &style->basis )) basis = TRUE;
            }
        }
    }
    if (custom_value( element, "--arctic-flex-grow", &value, &len )) style->grow = (float)atof( value );
    if (custom_value( element, "--arctic-flex-shrink", &value, &len )) style->shrink = (float)atof( value );
    if (custom_value( element, "--arctic-flex-basis", &value, &len ))
        parse_length_percent( value, len, style->font_size, &style->basis );
    if (custom_value( element, "--arctic-order", &value, &len )) style->order = atoi( value );
}

static void apply_list_style( lxb_dom_element_t *element, struct style *style )
{
    const char *value;
    size_t len;

    if (custom_value( element, "--arctic-vertical-align", &value, &len ))
    {
        if (text_is( value, len, "middle" )) style->valign = VALIGN_MIDDLE;
        else if (text_is( value, len, "top" ) || text_is( value, len, "text-top" )) style->valign = VALIGN_TOP;
        else if (text_is( value, len, "bottom" ) || text_is( value, len, "text-bottom" )) style->valign = VALIGN_BOTTOM;
        else style->valign = VALIGN_BASELINE;
    }

    if (custom_value( element, "--arctic-list-style-type", &value, &len ))
    {
        if (text_is( value, len, "none" )) style->list = LIST_NONE;
        else if (text_is( value, len, "disc" ) || text_is( value, len, "circle" ) || text_is( value, len, "square" ))
            style->list = LIST_DISC;
        else if (!text_is( value, len, "inherit" )) style->list = LIST_DECIMAL;
    }
}

/* the sides of a border as CSS has them: a width, and a style that is none
 * unless set; the width drawn is 0 while the style is none */
struct sides
{
    float width[4];
    BOOL  styled[4];
};

static void border_longhands( lxb_dom_element_t *element, float font_size, struct sides *sides )
{
    static const char *const width_names[4] = { "--arctic-border-top-width", "--arctic-border-right-width",
                                                "--arctic-border-bottom-width", "--arctic-border-left-width" };
    static const char *const style_names[4] = { "--arctic-border-top-style", "--arctic-border-right-style",
                                                "--arctic-border-bottom-style", "--arctic-border-left-style" };
    const char *value;
    size_t len;
    float px;

    /* taken as written after the shorthands, as they mostly are */
    for (int i = 0; i < 4; i++)
    {
        if (custom_value( element, width_names[i], &value, &len ))
        {
            if (text_is( value, len, "thin" )) sides->width[i] = 1;
            else if (text_is( value, len, "medium" )) sides->width[i] = 3;
            else if (text_is( value, len, "thick" )) sides->width[i] = 5;
            else if (parse_length( value, len, font_size, &px )) sides->width[i] = max( px, 0 );
        }
        if (custom_value( element, style_names[i], &value, &len ))
            sides->styled[i] = !text_is( value, len, "none" ) && !text_is( value, len, "hidden" );
    }
}

/* a border shorthand sets all three of its side: what it leaves out goes back to the initial */
static void apply_border( const lxb_css_property_border_t *border, struct style *style, float *width, BOOL *styled,
                          int side )
{
    *styled = border->style && border->style != LXB_CSS_VALUE_NONE && border->style != LXB_CSS_VALUE_HIDDEN;
    *width = border->width.type ? border_width( &border->width, style->font_size ) : 3;
    style->border_color[side] = border->color.type ? color_value( &border->color, style->color ) : style->color;
}

static void apply_borders( lxb_dom_element_t *element, struct style *style )
{
    static const uintptr_t side_ids[4] = { LXB_CSS_PROPERTY_BORDER_TOP, LXB_CSS_PROPERTY_BORDER_RIGHT,
                                           LXB_CSS_PROPERTY_BORDER_BOTTOM, LXB_CSS_PROPERTY_BORDER_LEFT };
    static const uintptr_t color_ids[4] = { LXB_CSS_PROPERTY_BORDER_TOP_COLOR, LXB_CSS_PROPERTY_BORDER_RIGHT_COLOR,
                                            LXB_CSS_PROPERTY_BORDER_BOTTOM_COLOR,
                                            LXB_CSS_PROPERTY_BORDER_LEFT_COLOR };
    const lxb_css_rule_declaration_t *decl;
    struct sides sides;

    /* what the user agent drew (<hr>) is a styled border */
    for (int i = 0; i < 4; i++)
    {
        sides.styled[i] = style->border[i] > 0;
        sides.width[i] = sides.styled[i] ? style->border[i] : 3;
    }
    if ((decl = declared( element, LXB_CSS_PROPERTY_BORDER )))
        for (int i = 0; i < 4; i++) apply_border( decl->u.border, style, &sides.width[i], &sides.styled[i], i );
    for (int i = 0; i < 4; i++)
        if ((decl = declared( element, side_ids[i] )))
            apply_border( decl->u.border_top, style, &sides.width[i], &sides.styled[i], i );
    for (int i = 0; i < 4; i++)
        if ((decl = declared( element, color_ids[i] )))
            style->border_color[i] = color_value( decl->u.border_top_color, style->color );
    border_longhands( element, style->font_size, &sides );
    for (int i = 0; i < 4; i++) style->border[i] = sides.styled[i] ? sides.width[i] : 0;
}

static void apply_text( lxb_dom_element_t *element, const struct style *parent, struct style *style )
{
    const lxb_css_rule_declaration_t *decl;

    if ((decl = declared( element, LXB_CSS_PROPERTY_FONT_WEIGHT )))
    {
        const lxb_css_property_font_weight_t *w = decl->u.font_weight;
        int inherited = parent ? parent->font.weight : 400;

        switch (w->type)
        {
        case LXB_CSS_VALUE_NORMAL:  style->font.weight = 400; break;
        case LXB_CSS_VALUE_BOLD:    style->font.weight = 700; break;
        case LXB_CSS_VALUE_BOLDER:  style->font.weight = inherited < 350 ? 400 : inherited < 550 ? 700 : 900; break;
        case LXB_CSS_VALUE_LIGHTER: style->font.weight = inherited < 550 ? 100 : inherited < 750 ? 400 : 700; break;
        case LXB_CSS_VALUE__NUMBER: style->font.weight = (int)w->number.num; break;
        default: break;
        }
    }
    if ((decl = declared( element, LXB_CSS_PROPERTY_FONT_STYLE )))
        style->font.italic = decl->u.font_style->type == LXB_CSS_VALUE_ITALIC ||
                             decl->u.font_style->type == LXB_CSS_VALUE_OBLIQUE;

    if ((decl = declared( element, LXB_CSS_PROPERTY_TEXT_ALIGN )))
    {
        switch (decl->u.text_align->type)
        {
        case LXB_CSS_VALUE_RIGHT: case LXB_CSS_VALUE_END: style->text_align = ALIGN_RIGHT; break;
        case LXB_CSS_VALUE_CENTER: style->text_align = ALIGN_CENTER; break;
        case LXB_CSS_VALUE_JUSTIFY: style->text_align = ALIGN_JUSTIFY; break;
        case LXB_CSS_VALUE_LEFT: case LXB_CSS_VALUE_START: style->text_align = ALIGN_LEFT; break;
        default: break;
        }
    }
    if ((decl = declared( element, LXB_CSS_PROPERTY_WHITE_SPACE )))
    {
        switch (decl->u.white_space->type)
        {
        case LXB_CSS_VALUE_NORMAL:   style->white_space = WS_NORMAL; break;
        case LXB_CSS_VALUE_NOWRAP:   style->white_space = WS_NOWRAP; break;
        case LXB_CSS_VALUE_PRE:      style->white_space = WS_PRE; break;
        case LXB_CSS_VALUE_PRE_WRAP: style->white_space = WS_PRE_WRAP; break;
        case LXB_CSS_VALUE_PRE_LINE: style->white_space = WS_PRE_LINE; break;
        default: break;
        }
    }
    if ((decl = declared( element, LXB_CSS_PROPERTY_LINE_HEIGHT )))
    {
        const lxb_css_property_line_height_t *lh = decl->u.line_height;

        switch (lh->type)
        {
        case LXB_CSS_VALUE_NORMAL:
            style->line_factor = 0;
            style->line_height = 0;
            break;
        case LXB_CSS_VALUE__NUMBER:
            style->line_factor = (float)lh->u.number.num;
            style->line_height = 0;
            break;
        case LXB_CSS_VALUE__LENGTH:
            style->line_factor = 0;
            style->line_height = length_value( &lh->u.length, style->font_size );
            break;
        case LXB_CSS_VALUE__PERCENTAGE:
            style->line_factor = 0;
            style->line_height = (float)lh->u.percentage.num * style->font_size / 100.0f;
            break;
        default: break;
        }
    }
    if ((decl = declared( element, LXB_CSS_PROPERTY_TEXT_DECORATION_LINE )))
    {
        const lxb_css_property_text_decoration_line_t *line = decl->u.text_decoration_line;

        style->font.underline = line->type != LXB_CSS_VALUE_NONE && line->underline;
        style->font.strike = line->type != LXB_CSS_VALUE_NONE && line->line_through;
    }
    if ((decl = declared( element, LXB_CSS_PROPERTY_TEXT_DECORATION )))
    {
        const lxb_css_property_text_decoration_line_t *line = &decl->u.text_decoration->line;

        style->font.underline = line->type != LXB_CSS_VALUE_NONE && line->underline;
        style->font.strike = line->type != LXB_CSS_VALUE_NONE && line->line_through;
    }
}

static void apply_display( lxb_dom_element_t *element, struct style *style )
{
    const lxb_css_rule_declaration_t *decl = declared( element, LXB_CSS_PROPERTY_DISPLAY );

    if (!decl) return;
    style->flex = decl->u.display->a == LXB_CSS_VALUE_FLEX || decl->u.display->a == LXB_CSS_VALUE_INLINE_FLEX;
    switch (decl->u.display->a)
    {
    case LXB_CSS_VALUE_NONE:         style->display = DISPLAY_NONE; break;
    case LXB_CSS_VALUE_INLINE:
    case LXB_CSS_VALUE_CONTENTS:     style->display = DISPLAY_INLINE; break;
    case LXB_CSS_VALUE_INLINE_BLOCK:
    case LXB_CSS_VALUE_INLINE_FLEX:
    case LXB_CSS_VALUE_INLINE_GRID:
    case LXB_CSS_VALUE_INLINE_TABLE: style->display = DISPLAY_INLINE_BLOCK; break;
    case LXB_CSS_VALUE_LIST_ITEM:    style->display = DISPLAY_LIST_ITEM; break;
    default:                         style->display = DISPLAY_BLOCK; break;   /* block, flex, grid, table...: stage 5 */
    }
}

/* --- form controls --- */

static BOOL is_control_tag( lxb_tag_id_t tag )
{
    return tag == LXB_TAG_INPUT || tag == LXB_TAG_SELECT || tag == LXB_TAG_TEXTAREA || tag == LXB_TAG_BUTTON ||
           tag == LXB_TAG_IFRAME;
}

/* what Chrome's style sheet gives the controls: their own font, a grey
 * frame, a white field or a grey button; the page's style comes after */
static void ua_control( lxb_dom_element_t *element, struct style *style )
{
    lxb_tag_id_t tag = lxb_dom_element_tag_id( element );
    enum control_kind kind = control_kind_of( element );
    size_t len;
    const char *type = (const char *)lxb_dom_element_get_attribute( element, (const lxb_char_t *)"type", 4, &len );

    if (tag == LXB_TAG_INPUT && type && len == 6 && !_strnicmp( type, "hidden", 6 ))
    {
        style->display = DISPLAY_NONE;
        return;
    }
    style->display = DISPLAY_INLINE_BLOCK;
    style->font_size = 13.333f;
    style->font.weight = 400;
    style->font.italic = style->font.underline = style->font.strike = FALSE;
    font_generic_face( tag == LXB_TAG_TEXTAREA ? GENERIC_MONO : GENERIC_SANS, style->font.face );
    style->color = 0xff000000;
    style->line_height = style->line_factor = 0;
    style->text_align = ALIGN_LEFT;
    style->white_space = tag == LXB_TAG_TEXTAREA ? WS_PRE_WRAP : WS_NOWRAP;
    style->list = LIST_NONE;
    style->background = 0xffffffff;
    for (int i = 0; i < 4; i++) style->border[i] = 1;

    switch (kind)
    {
    case CONTROL_CHECKBOX:
    case CONTROL_RADIO:
        style->width.unit = style->height.unit = LEN_PX;
        style->width.value = style->height.value = 13;
        style->border_box = TRUE;
        style->margin[0].value = style->margin[1].value = style->margin[2].value = 3;
        style->margin[3].value = 4;
        break;
    case CONTROL_SUBMIT:
    case CONTROL_BUTTON:
        style->padding[0] = style->padding[2] = 1;
        style->padding[1] = style->padding[3] = 6;
        style->background = 0xffefefef;
        style->text_align = ALIGN_CENTER;
        style->border_box = TRUE;
        break;
    case CONTROL_SELECT:
        style->padding[0] = style->padding[2] = 1;
        style->padding[1] = style->padding[3] = 4;
        style->border_box = TRUE;
        break;
    default:
        style->padding[0] = style->padding[2] = 2;
        style->padding[1] = style->padding[3] = 3;
        break;
    }
}

/* --- putting it together --- */

void style_initial( struct style *style )
{
    memset( style, 0, sizeof(*style) );
    style->display = DISPLAY_BLOCK;
    style->font_size = DEFAULT_FONT_SIZE;
    style->font.weight = 400;
    font_generic_face( GENERIC_SERIF, style->font.face );
    style->color = 0xff000000;
}

void style_compute( const struct style *parent, lxb_dom_element_t *element, struct style *style )
{
    const struct ua *ua = ua_for( lxb_dom_element_tag_id( element ) );
    const lxb_css_rule_declaration_t *decl;

    /* what is inherited, and the initial values of the rest */
    if (parent) *style = *parent;
    else style_initial( style );
    style->display = DISPLAY_INLINE;
    style->background = 0;
    style->out_of_flow = FALSE;
    style->width.unit = style->height.unit = style->min_width.unit = style->max_width.unit = LEN_AUTO;
    style->border_box = FALSE;
    style->valign = VALIGN_BASELINE;
    style->position = POSITION_STATIC;
    style->clips = FALSE;
    for (int i = 0; i < 4; i++) style->inset[i].unit = LEN_AUTO;
    style->flex = FALSE;
    style->flex_direction = FLEX_ROW;
    style->flex_wrap = FALSE;
    style->justify = JUSTIFY_START;
    style->align_items = FALIGN_STRETCH;
    style->align_self = FALIGN_AUTO;
    style->gap_row = style->gap_column = 0;
    style->grow = 0;
    style->shrink = 1;
    style->basis.unit = LEN_AUTO;
    style->order = 0;
    for (int i = 0; i < 4; i++)
    {
        style->margin[i].unit = LEN_PX;
        style->margin[i].value = 0;
        style->padding[i] = 0;
        style->border[i] = 0;
    }
    if (parent && style->line_factor) style->line_height = 0;  /* a factor goes on as a factor */

    /* the user agent: the font first, its margins are in the font's em */
    switch (ua->display)
    {
    case UA_NONE:      style->display = DISPLAY_NONE; break;
    case UA_BLOCK:     style->display = DISPLAY_BLOCK; break;
    case UA_LIST_ITEM: style->display = DISPLAY_LIST_ITEM; break;
    default:           break;
    }
    if (ua->bold) style->font.weight = 700;
    if (ua->italic) style->font.italic = TRUE;
    if (ua->underline) style->font.underline = TRUE;
    if (ua->strike) style->font.strike = TRUE;
    if (ua->pre) style->white_space = WS_PRE;
    if (ua->center) style->text_align = ALIGN_CENTER;
    if (ua->font_scale) style->font_size *= ua->font_scale;
    if (ua->mono)
    {
        font_generic_face( GENERIC_MONO, style->font.face );
        style->font_size *= 13.0f / 16.0f;      /* as browsers size monospace text */
    }
    switch (lxb_dom_element_tag_id( element ))
    {
    case LXB_TAG_BODY:
        for (int i = 0; i < 4; i++) style->margin[i].value = 8;
        break;
    case LXB_TAG_UL: case LXB_TAG_MENU:
        style->list = LIST_DISC;
        break;
    case LXB_TAG_OL:
        style->list = LIST_DECIMAL;
        break;
    case LXB_TAG_A:
        if (lxb_dom_element_has_attribute( element, (const lxb_char_t *)"href", 4 )) style->color = 0xff0000ee;
        else style->font.underline = FALSE;
        break;
    case LXB_TAG_MARK:
        style->background = 0xffffff00;
        break;
    case LXB_TAG_INPUT: case LXB_TAG_SELECT: case LXB_TAG_TEXTAREA: case LXB_TAG_BUTTON:
        ua_control( element, style );
        break;
    case LXB_TAG_IFRAME:
    {
        size_t len;
        const char *border = (const char *)lxb_dom_element_get_attribute( element, (const lxb_char_t *)"frameborder", 11, &len );

        style->display = DISPLAY_INLINE_BLOCK;
        if (!(border && len == 1 && border[0] == '0'))
            for (int i = 0; i < 4; i++) style->border[i] = 2;
        break;
    }
    case LXB_TAG_HR:
        style->border[0] = style->border[2] = 1;
        style->border_color[0] = 0xff9a9a9a;
        style->border_color[2] = 0xffeeeeee;
        style->margin[0].value = style->margin[2].value = 8;
        break;
    default:
        break;
    }

    /* the page's own font size, then what is measured in it */
    apply_font_size( element, parent, style );
    if (ua->margin_v) style->margin[0].value = style->margin[2].value = ua->margin_v * style->font_size;
    if (ua->margin_h) style->margin[1].value = style->margin[3].value = ua->margin_h * style->font_size;
    if (ua->padding_left) style->padding[3] = ua->padding_left;

    /* the page's style */
    if ((decl = declared( element, LXB_CSS_PROPERTY_COLOR ))) style->color = color_value( decl->u.color, style->color );
    /* a border is currentcolor unless said otherwise; <hr> has its own two */
    if (is_control_tag( lxb_dom_element_tag_id( element ) ))
        for (int i = 0; i < 4; i++) style->border_color[i] = 0xff767676;
    else if (lxb_dom_element_tag_id( element ) != LXB_TAG_HR)
        for (int i = 0; i < 4; i++) style->border_color[i] = style->color;
    if ((decl = declared( element, LXB_CSS_PROPERTY_BACKGROUND_COLOR )))
        style->background = color_value( decl->u.background_color, style->color );
    apply_display( element, style );
    if ((decl = declared( element, LXB_CSS_PROPERTY_VISIBILITY )))
        style->invisible = decl->u.visibility->type == LXB_CSS_VALUE_HIDDEN ||
                           decl->u.visibility->type == LXB_CSS_VALUE_COLLAPSE;
    if ((decl = declared( element, LXB_CSS_PROPERTY_OPACITY )) && decl->u.opacity->type == LXB_CSS_VALUE__NUMBER &&
        decl->u.opacity->u.number.num <= 0.001)
        style->transparent = TRUE;
    if ((decl = declared( element, LXB_CSS_PROPERTY_POSITION )))
    {
        switch (decl->u.position->type)
        {
        case LXB_CSS_VALUE_ABSOLUTE: style->position = POSITION_ABSOLUTE; break;
        case LXB_CSS_VALUE_FIXED:    style->position = POSITION_FIXED; break;
        case LXB_CSS_VALUE_RELATIVE:
        case LXB_CSS_VALUE_STICKY:   style->position = POSITION_RELATIVE; break;
        default:                     style->position = POSITION_STATIC; break;
        }
        style->out_of_flow = style->position == POSITION_ABSOLUTE || style->position == POSITION_FIXED;
    }
    apply_font_family( element, style );
    apply_text( element, parent, style );
    apply_list_style( element, style );
    apply_flex( element, style );
    apply_lengths( element, style );
    apply_borders( element, style );

    style->font.px = (int)(style->font_size + 0.5f);
    if (style->line_factor) style->line_height = style->line_factor * style->font_size;
}

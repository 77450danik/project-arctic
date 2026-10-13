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

        return rgba( c->r, c->g, c->b, alpha ? c->a : 255 );
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
    case LXB_CSS_VALUE_INHERIT:  style->font_size = parent_size; break;
    case LXB_CSS_VALUE__LENGTH:
        style->font_size = length_value( &size->length.u.length, parent_size );
        break;
    case LXB_CSS_VALUE__PERCENTAGE:
        style->font_size = (float)size->length.u.percentage.num * parent_size / 100.0f;
        break;
    default:
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
}

static void apply_border( const lxb_css_property_border_t *border, struct style *style, int side, float fs )
{
    if (border->style == LXB_CSS_VALUE_NONE || border->style == LXB_CSS_VALUE_HIDDEN)
    {
        style->border[side] = 0;
        return;
    }
    style->border[side] = border->width.type ? border_width( &border->width, fs ) :
                          border->style ? 3 : style->border[side];
    if (border->color.type) style->border_color[side] = color_value( &border->color, style->color );
}

static void apply_borders( lxb_dom_element_t *element, struct style *style )
{
    static const uintptr_t side_ids[4] = { LXB_CSS_PROPERTY_BORDER_TOP, LXB_CSS_PROPERTY_BORDER_RIGHT,
                                           LXB_CSS_PROPERTY_BORDER_BOTTOM, LXB_CSS_PROPERTY_BORDER_LEFT };
    static const uintptr_t color_ids[4] = { LXB_CSS_PROPERTY_BORDER_TOP_COLOR, LXB_CSS_PROPERTY_BORDER_RIGHT_COLOR,
                                            LXB_CSS_PROPERTY_BORDER_BOTTOM_COLOR,
                                            LXB_CSS_PROPERTY_BORDER_LEFT_COLOR };
    const lxb_css_rule_declaration_t *decl;

    if ((decl = declared( element, LXB_CSS_PROPERTY_BORDER )))
        for (int i = 0; i < 4; i++) apply_border( decl->u.border, style, i, style->font_size );
    for (int i = 0; i < 4; i++)
        if ((decl = declared( element, side_ids[i] ))) apply_border( decl->u.border_top, style, i, style->font_size );
    for (int i = 0; i < 4; i++)
        if ((decl = declared( element, color_ids[i] )))
            style->border_color[i] = color_value( decl->u.border_top_color, style->color );
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
    style->width.unit = style->height.unit = LEN_AUTO;
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
    if (lxb_dom_element_tag_id( element ) != LXB_TAG_HR)
        for (int i = 0; i < 4; i++) style->border_color[i] = style->color;
    if ((decl = declared( element, LXB_CSS_PROPERTY_BACKGROUND_COLOR )))
        style->background = color_value( decl->u.background_color, style->color );
    apply_display( element, style );
    apply_font_family( element, style );
    apply_text( element, parent, style );
    apply_lengths( element, style );
    apply_borders( element, style );

    style->font.px = (int)(style->font_size + 0.5f);
    if (style->line_factor) style->line_height = style->line_factor * style->font_size;
}

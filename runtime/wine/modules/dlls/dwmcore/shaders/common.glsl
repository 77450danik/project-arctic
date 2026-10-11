#version 450

/* what every draw is told: one rectangle, where it goes and how it looks */
layout(push_constant) uniform Push
{
    vec4  proj;      /* screen to clip space: xy * proj.xy + proj.zw */
    vec4  rect;      /* the rectangle drawn, screen coordinates */
    vec4  uv;        /* the part of the texture over it */
    vec4  clip;      /* the rounded rectangle outside which nothing is drawn */
    vec4  color;     /* a colour, the acrylic's tint, or the shadow's numbers */
    float radius;    /* of clip's corners, negative: no clip */
    float opacity;
    float mode;      /* texture: 0 opaque, 1 premultiplied, 2 keyed; acrylic: the grain; blur: the offset */
    float pad;
    vec2  half_px;   /* blur: half a texel of the source */
} pc;

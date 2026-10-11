layout(set = 0, binding = 0) uniform sampler2D u_tex;

/* the dual Kawase blur: a halving that blurs */
void main()
{
    vec2 o = pc.half_px * pc.mode;
    vec4 s = texture( u_tex, v_uv ) * 4.0;

    s += texture( u_tex, v_uv - o );
    s += texture( u_tex, v_uv + o );
    s += texture( u_tex, v_uv + vec2( o.x, -o.y ) );
    s += texture( u_tex, v_uv - vec2( o.x, -o.y ) );
    o_color = s / 8.0;
}

layout(set = 0, binding = 0) uniform sampler2D u_tex;

/* ... then a doubling that blurs */
void main()
{
    vec2 o = pc.half_px * pc.mode;
    vec4 s = texture( u_tex, v_uv + vec2( -o.x * 2.0, 0.0 ) );

    s += texture( u_tex, v_uv + vec2( -o.x, o.y ) ) * 2.0;
    s += texture( u_tex, v_uv + vec2( 0.0, o.y * 2.0 ) );
    s += texture( u_tex, v_uv + vec2( o.x, o.y ) ) * 2.0;
    s += texture( u_tex, v_uv + vec2( o.x * 2.0, 0.0 ) );
    s += texture( u_tex, v_uv + vec2( o.x, -o.y ) ) * 2.0;
    s += texture( u_tex, v_uv + vec2( 0.0, -o.y * 2.0 ) );
    s += texture( u_tex, v_uv + vec2( -o.x, -o.y ) ) * 2.0;
    o_color = s / 12.0;
}

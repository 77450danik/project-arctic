layout(set = 0, binding = 0) uniform sampler2D u_tex;

/* pc.mode 0: opaque, 1: premultiplied, 2: GDI over a backdrop (see LAYER_KEYED),
 * 3: drawn dark on white over a backdrop (LAYER_KEYED_LIGHT) */
void main()
{
    vec4 t = texture( u_tex, v_uv );
    float mode = pc.mode;

    /* a window with its title bar alone over a backdrop is opaque under it */
    if (mode > 1.5 && pc.key.x > 0.5 && v_pos.y >= pc.key.y) mode = 0.0;
    if (mode < 0.5) t.a = 1.0;
    else if (mode > 2.5)
    {
        if (t.a < 0.002) t.a = 1.0 - min( t.r, min( t.g, t.b ) );
        t.rgb = min( max( t.rgb - vec3( 1.0 - t.a ), vec3( 0.0 ) ), vec3( t.a ) );
    }
    else if (mode > 1.5)
    {
        if (t.a < 0.002) t.a = max( t.r, max( t.g, t.b ) );
        t.rgb = min( t.rgb, vec3( t.a ) );
    }
    o_color = t * (pc.opacity * coverage());
}

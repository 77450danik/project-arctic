layout(set = 0, binding = 0) uniform sampler2D u_tex;

/* the acrylic of Windows: the blur a little more saturated, the tint
 * (pc.color) over it, and a faint grain (pc.mode) */
void main()
{
    vec3 c = texture( u_tex, v_uv ).rgb;
    float l = dot( c, vec3( 0.2126, 0.7152, 0.0722 ) );

    c = clamp( mix( vec3( l ), c, 1.25 ), 0.0, 1.0 );
    c = mix( c, pc.color.rgb, pc.color.a );
    float n = fract( sin( dot( floor( v_pos ), vec2( 12.9898, 78.233 ) ) ) * 43758.5453 );
    c += (n - 0.5) * pc.mode;
    o_color = vec4( clamp( c, 0.0, 1.0 ), 1.0 ) * (pc.opacity * coverage());
}

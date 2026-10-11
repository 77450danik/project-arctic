layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec2 v_pos;
layout(location = 0) out vec4 o_color;

/* the distance from a rounded rectangle, negative inside */
float box( vec2 p, vec4 r, float radius )
{
    vec2 c = (r.xy + r.zw) * 0.5;
    vec2 h = (r.zw - r.xy) * 0.5;
    vec2 q = abs( p - c ) - h + vec2( radius );
    return length( max( q, 0.0 ) ) + min( max( q.x, q.y ), 0.0 ) - radius;
}

/* what lies outside the rounded rectangle pc.clip is cut off, with a soft edge */
float coverage()
{
    if (pc.radius < 0.0) return 1.0;
    return clamp( 0.5 - box( v_pos, pc.clip, pc.radius ), 0.0, 1.0 );
}

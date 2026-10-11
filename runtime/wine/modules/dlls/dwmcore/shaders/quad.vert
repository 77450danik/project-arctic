layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec2 v_pos;

void main()
{
    vec2 corner = vec2( gl_VertexIndex & 1, gl_VertexIndex >> 1 );
    vec2 pos = mix( pc.rect.xy, pc.rect.zw, corner );

    v_uv = mix( pc.uv.xy, pc.uv.zw, corner );
    v_pos = pos;
    gl_Position = vec4( pos * pc.proj.xy + pc.proj.zw, 0.0, 1.0 );
}

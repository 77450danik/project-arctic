/* the thin line around a window, on its outermost pixels */
void main()
{
    float d = box( v_pos, pc.clip, max( pc.radius, 0.0 ) );
    float ring = clamp( 0.5 - d, 0.0, 1.0 ) - clamp( -0.5 - d, 0.0, 1.0 );

    o_color = pc.color * (ring * pc.opacity);
}

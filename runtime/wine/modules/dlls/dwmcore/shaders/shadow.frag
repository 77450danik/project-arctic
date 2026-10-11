/* The shadow of Windows 11 under a window: a wide one dropped a little
 * (pc.color.x down, .y its sigma, .z its strength) and a close one all
 * around (.w its strength); none under the window. */
void main()
{
    float r = max( pc.radius, 0.0 );
    float d = box( v_pos, pc.clip, r );
    float dk = max( box( v_pos, pc.clip + vec4( 0.0, pc.color.x, 0.0, pc.color.x ), r ), 0.0 );
    float da = max( d, 0.0 );
    float a = pc.color.z * exp( -dk * dk / (2.0 * pc.color.y * pc.color.y) ) +
              pc.color.w * exp( -da * da / 32.0 );

    o_color = vec4( 0.0, 0.0, 0.0, min( a, 1.0 ) * clamp( d + 0.5, 0.0, 1.0 ) * pc.opacity );
}

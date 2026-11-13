/*
 * SBC, the codec every A2DP sink takes (Bluetooth A2DP specification,
 * appendix B): an encoder
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINEBTH_SBC_H
#define __WINEBTH_SBC_H

#include <stddef.h>
#include <stdint.h>

/* as the codec's information element has them (A2DP 4.3.2) */
enum sbc_frequency { SBC_FREQ_16000, SBC_FREQ_32000, SBC_FREQ_44100, SBC_FREQ_48000 };
enum sbc_mode { SBC_MODE_MONO, SBC_MODE_DUAL_CHANNEL, SBC_MODE_STEREO, SBC_MODE_JOINT_STEREO };
enum sbc_allocation { SBC_ALLOCATION_LOUDNESS, SBC_ALLOCATION_SNR };

/* Eight subbands only: every sink takes them (A2DP 4.3.2.4) */
struct sbc_encoder
{
    enum sbc_frequency frequency;
    enum sbc_mode mode;
    enum sbc_allocation allocation;
    int blocks;              /* 4, 8, 12, 16 */
    int subbands;            /* 8 */
    int bitpool;
    /* the analysis filter's last 80 samples a channel, newest first */
    float x[2][80];
};

void sbc_init( struct sbc_encoder *sbc, enum sbc_frequency frequency, enum sbc_mode mode,
               enum sbc_allocation allocation, int blocks, int subbands, int bitpool );
int sbc_channels( const struct sbc_encoder *sbc );
/* samples a channel in one frame */
int sbc_frame_samples( const struct sbc_encoder *sbc );
size_t sbc_frame_length( const struct sbc_encoder *sbc );
/* one frame of interleaved 16-bit samples, sbc_frame_samples() a channel;
 * returns the frame's length */
size_t sbc_encode( struct sbc_encoder *sbc, const int16_t *pcm, uint8_t *out );

/* for tests: the bits each subband gets, as the decoder works them out */
void sbc_allocate( const struct sbc_encoder *sbc, const int scale_factor[2][8], int bits[2][8] );
extern const float sbc_proto_8[80];

#endif

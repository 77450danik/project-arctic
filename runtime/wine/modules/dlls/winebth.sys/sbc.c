/*
 * SBC, the codec every A2DP sink takes: an encoder (A2DP specification,
 * appendix B "Technical specification of SBC")
 *
 * Eight subbands. Per block of eight samples a channel the analysis filter
 * windows the last 80 by the prototype Proto_8_80 (B.2.2, table 12.24),
 * folds them to 16 and turns those into the eight subbands with the
 * cosine matrix; per frame the scale factors, joint stereo where it saves
 * (B.2.4), the bit allocation of B.2.4.2 (which the decoder repeats, so it
 * must be exact), the quantized samples and the CRC-8 (x^8+x^4+x^3+x^2+1,
 * from 0x0F) over the header, the join bits and the scale factors.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#if 0
#pragma makedep unix
#endif

#include <math.h>
#include <string.h>

#include "sbc.h"

const float sbc_proto_8[80] =
{
     0.00000000e+00f,  1.56575398e-04f,  3.43256425e-04f,  5.54620202e-04f,
     8.23919506e-04f,  1.13992507e-03f,  1.47640169e-03f,  1.78371725e-03f,
     2.01182542e-03f,  2.10371989e-03f,  1.99454554e-03f,  1.61656283e-03f,
     9.02154502e-04f, -1.78805361e-04f, -1.64973098e-03f, -3.49717454e-03f,
     5.65949473e-03f,  8.02941163e-03f,  1.04584443e-02f,  1.27472335e-02f,
     1.46525263e-02f,  1.59045603e-02f,  1.62208471e-02f,  1.53184106e-02f,
     1.29371806e-02f,  8.85757540e-03f,  2.92408442e-03f, -4.91578024e-03f,
    -1.46404076e-02f, -2.61098752e-02f, -3.90751381e-02f, -5.31873032e-02f,
     6.79989431e-02f,  8.29847578e-02f,  9.75753918e-02f,  1.11196689e-01f,
     1.23264548e-01f,  1.33264415e-01f,  1.40753505e-01f,  1.45389847e-01f,
     1.46955068e-01f,  1.45389847e-01f,  1.40753505e-01f,  1.33264415e-01f,
     1.23264548e-01f,  1.11196689e-01f,  9.75753918e-02f,  8.29847578e-02f,
    -6.79989431e-02f, -5.31873032e-02f, -3.90751381e-02f, -2.61098752e-02f,
    -1.46404076e-02f, -4.91578024e-03f,  2.92408442e-03f,  8.85757540e-03f,
     1.29371806e-02f,  1.53184106e-02f,  1.62208471e-02f,  1.59045603e-02f,
     1.46525263e-02f,  1.27472335e-02f,  1.04584443e-02f,  8.02941163e-03f,
    -5.65949473e-03f, -3.49717454e-03f, -1.64973098e-03f, -1.78805361e-04f,
     9.02154502e-04f,  1.61656283e-03f,  1.99454554e-03f,  2.10371989e-03f,
     2.01182542e-03f,  1.78371725e-03f,  1.47640169e-03f,  1.13992507e-03f,
     8.23919506e-04f,  5.54620202e-04f,  3.43256425e-04f,  1.56575398e-04f,
};

/* the loudness offsets of eight subbands, by the sampling frequency (table 12.20) */
static const int offset8[4][8] =
{
    { -2, 0, 0, 0, 0, 0, 0, 1 },
    { -3, 0, 0, 0, 0, 0, 1, 2 },
    { -4, 0, 0, 0, 0, 0, 1, 2 },
    { -4, 0, 0, 0, 0, 0, 1, 2 },
};

static float matrix8[8][16];   /* cos((i + 0.5)(k - 4)pi/8) */

void sbc_init( struct sbc_encoder *sbc, enum sbc_frequency frequency, enum sbc_mode mode,
               enum sbc_allocation allocation, int blocks, int subbands, int bitpool )
{
    memset( sbc, 0, sizeof(*sbc) );
    sbc->frequency = frequency;
    sbc->mode = mode;
    sbc->allocation = allocation;
    sbc->blocks = blocks;
    sbc->subbands = subbands;
    sbc->bitpool = bitpool;
    if (!matrix8[0][4])
        for (int i = 0; i < 8; i++)
            for (int k = 0; k < 16; k++)
                matrix8[i][k] = cosf( (i + 0.5f) * (k - 4) * (float)M_PI / 8 );
}

int sbc_channels( const struct sbc_encoder *sbc )
{
    return sbc->mode == SBC_MODE_MONO ? 1 : 2;
}

int sbc_frame_samples( const struct sbc_encoder *sbc )
{
    return sbc->blocks * sbc->subbands;
}

size_t sbc_frame_length( const struct sbc_encoder *sbc )
{
    int channels = sbc_channels( sbc );
    size_t length = 4 + (4 * sbc->subbands * channels) / 8;

    if (sbc->mode == SBC_MODE_MONO || sbc->mode == SBC_MODE_DUAL_CHANNEL)
        return length + (sbc->blocks * channels * sbc->bitpool + 7) / 8;
    if (sbc->mode == SBC_MODE_STEREO)
        return length + (sbc->blocks * sbc->bitpool + 7) / 8;
    return length + (sbc->subbands + sbc->blocks * sbc->bitpool + 7) / 8;
}

/* eight new samples of a channel (every stride-th) through the analysis filter */
static void analyse( struct sbc_encoder *sbc, int ch, const int16_t *pcm, int stride, float out[8] )
{
    float *x = sbc->x[ch], y[16];

    memmove( x + 8, x, 72 * sizeof(*x) );
    for (int i = 7; i >= 0; i--, pcm += stride) x[i] = *pcm;
    for (int i = 0; i < 16; i++)
    {
        float sum = 0;
        for (int j = 0; j < 5; j++) sum += sbc_proto_8[i + 16 * j] * x[i + 16 * j];
        y[i] = sum;
    }
    for (int i = 0; i < 8; i++)
    {
        float sum = 0;
        for (int k = 0; k < 16; k++) sum += matrix8[i][k] * y[k];
        out[i] = sum;
    }
}

/* the smallest scale factor whose 2^(sf+1) is above the largest sample */
static int scale_factor_of( float largest )
{
    int sf = 0;

    while (sf < 15 && largest >= (float)(2 << sf)) sf++;
    return sf;
}

void sbc_allocate( const struct sbc_encoder *sbc, const int scale_factor[2][8], int bits[2][8] )
{
    int channels = sbc_channels( sbc ), subbands = sbc->subbands, bitpool = sbc->bitpool;
    int bitneed[2][8], max_bitneed, bitcount, slicecount, bitslice;
    int joined = sbc->mode == SBC_MODE_STEREO || sbc->mode == SBC_MODE_JOINT_STEREO;

    for (int ch = 0; ch < channels; ch++)
        for (int sb = 0; sb < subbands; sb++)
        {
            int sf = scale_factor[ch][sb];

            if (sbc->allocation == SBC_ALLOCATION_SNR) bitneed[ch][sb] = sf;
            else if (!sf) bitneed[ch][sb] = -5;
            else
            {
                int loudness = sf - offset8[sbc->frequency][sb];
                bitneed[ch][sb] = loudness > 0 ? loudness / 2 : loudness;
            }
        }

    /* mono and dual channel: each channel its own bitpool; stereo: both share it */
    for (int first = 0; first < (joined ? 1 : channels); first++)
    {
        int last = joined ? channels - 1 : first, ch, sb;

        max_bitneed = 0;
        for (ch = first; ch <= last; ch++)
            for (sb = 0; sb < subbands; sb++)
                if (bitneed[ch][sb] > max_bitneed) max_bitneed = bitneed[ch][sb];

        bitcount = 0;
        slicecount = 0;
        bitslice = max_bitneed + 1;
        do
        {
            bitslice--;
            bitcount += slicecount;
            slicecount = 0;
            for (ch = first; ch <= last; ch++)
                for (sb = 0; sb < subbands; sb++)
                {
                    if (bitneed[ch][sb] > bitslice + 1 && bitneed[ch][sb] < bitslice + 16) slicecount++;
                    else if (bitneed[ch][sb] == bitslice + 1) slicecount += 2;
                }
        } while (bitcount + slicecount < bitpool);
        if (bitcount + slicecount == bitpool)
        {
            bitcount += slicecount;
            bitslice--;
        }

        for (ch = first; ch <= last; ch++)
            for (sb = 0; sb < subbands; sb++)
            {
                if (bitneed[ch][sb] < bitslice + 2) bits[ch][sb] = 0;
                else bits[ch][sb] = bitneed[ch][sb] - bitslice < 16 ? bitneed[ch][sb] - bitslice : 16;
            }

        /* what is left: first to those already given some, then to any */
        ch = first;
        sb = 0;
        while (bitcount < bitpool && sb < subbands)
        {
            if (bits[ch][sb] >= 2 && bits[ch][sb] < 16)
            {
                bits[ch][sb]++;
                bitcount++;
            }
            else if (bitneed[ch][sb] == bitslice + 1 && bitpool > bitcount + 1)
            {
                bits[ch][sb] = 2;
                bitcount += 2;
            }
            if (ch == last) { ch = first; sb++; }
            else ch++;
        }
        ch = first;
        sb = 0;
        while (bitcount < bitpool && sb < subbands)
        {
            if (bits[ch][sb] < 16)
            {
                bits[ch][sb]++;
                bitcount++;
            }
            if (ch == last) { ch = first; sb++; }
            else ch++;
        }
    }
}

struct bit_writer
{
    uint8_t *data;
    size_t bit;
};

static void put_bits( struct bit_writer *w, unsigned int value, int count )
{
    while (count--)
    {
        if (!(w->bit & 7)) w->data[w->bit >> 3] = 0;
        if ((value >> count) & 1) w->data[w->bit >> 3] |= 0x80 >> (w->bit & 7);
        w->bit++;
    }
}

static uint8_t crc8( uint8_t crc, const uint8_t *data, size_t bits )
{
    for (size_t i = 0; i < bits; i++)
    {
        int bit = (data[i >> 3] >> (7 - (i & 7))) & 1;
        int top = crc >> 7;

        crc <<= 1;
        if (bit ^ top) crc ^= 0x1d;
    }
    return crc;
}

size_t sbc_encode( struct sbc_encoder *sbc, const int16_t *pcm, uint8_t *out )
{
    int channels = sbc_channels( sbc ), subbands = sbc->subbands, blocks = sbc->blocks;
    float samples[16][2][8];
    int scale_factor[2][8], bits[2][8];
    unsigned int join = 0;
    struct bit_writer w = { out, 0 };
    size_t header_bits;

    for (int blk = 0; blk < blocks; blk++)
        for (int ch = 0; ch < channels; ch++)
            analyse( sbc, ch, pcm + blk * subbands * channels + ch, channels, samples[blk][ch] );

    for (int ch = 0; ch < channels; ch++)
        for (int sb = 0; sb < subbands; sb++)
        {
            float largest = 0;
            for (int blk = 0; blk < blocks; blk++) largest = fmaxf( largest, fabsf( samples[blk][ch][sb] ) );
            scale_factor[ch][sb] = scale_factor_of( largest );
        }

    /* joint stereo: the sum and the difference where they cost less (all but the last subband) */
    if (sbc->mode == SBC_MODE_JOINT_STEREO)
    {
        for (int sb = 0; sb < subbands - 1; sb++)
        {
            float mid = 0, side = 0;
            int sf_mid, sf_side;

            for (int blk = 0; blk < blocks; blk++)
            {
                mid = fmaxf( mid, fabsf( (samples[blk][0][sb] + samples[blk][1][sb]) / 2 ) );
                side = fmaxf( side, fabsf( (samples[blk][0][sb] - samples[blk][1][sb]) / 2 ) );
            }
            sf_mid = scale_factor_of( mid );
            sf_side = scale_factor_of( side );
            if (sf_mid + sf_side >= scale_factor[0][sb] + scale_factor[1][sb]) continue;
            join |= 0x80 >> sb;
            scale_factor[0][sb] = sf_mid;
            scale_factor[1][sb] = sf_side;
            for (int blk = 0; blk < blocks; blk++)
            {
                float left = samples[blk][0][sb], right = samples[blk][1][sb];
                samples[blk][0][sb] = (left + right) / 2;
                samples[blk][1][sb] = (left - right) / 2;
            }
        }
    }

    sbc_allocate( sbc, scale_factor, bits );

    out[0] = 0x9c;
    out[1] = (sbc->frequency << 6) | ((blocks / 4 - 1) << 4) | (sbc->mode << 2) | (sbc->allocation << 1) |
             (subbands == 8);
    out[2] = sbc->bitpool;
    out[3] = 0;
    w.data = out + 4;
    if (sbc->mode == SBC_MODE_JOINT_STEREO) put_bits( &w, join >> (8 - subbands), subbands );
    for (int ch = 0; ch < channels; ch++)
        for (int sb = 0; sb < subbands; sb++)
            put_bits( &w, scale_factor[ch][sb], 4 );
    header_bits = w.bit;
    out[3] = crc8( crc8( 0x0f, out + 1, 16 ), out + 4, header_bits );

    for (int blk = 0; blk < blocks; blk++)
        for (int ch = 0; ch < channels; ch++)
            for (int sb = 0; sb < subbands; sb++)
            {
                int levels, q;

                if (!bits[ch][sb]) continue;
                levels = (1 << bits[ch][sb]) - 1;
                q = (int)((samples[blk][ch][sb] / (float)(2 << scale_factor[ch][sb]) + 1.0f) * levels / 2);
                if (q < 0) q = 0;
                if (q > levels) q = levels;
                put_bits( &w, q, bits[ch][sb] );
            }
    /* the decoder takes the frame's length from its header: what the bits
     * did not fill is padding */
    {
        size_t length = sbc_frame_length( sbc ), used = 4 + (w.bit + 7) / 8;
        if (used < length) memset( out + used, 0, length - used );
        return length;
    }
}

/*
 * H.264 luma quarter-pel motion compensation with VMX128 for the Xbox 360,
 * 16x16 and 8x8 blocks (4x4 and 2x2 keep the C code).
 *
 * The 6-tap filter is (c+d)*20 - (b+e)*5 + (a+f). Xenon's VMX128 has no vector
 * integer multiply, so *20 and *5 are shifts and adds. Horizontal and vertical
 * passes fit in 16-bit lanes; the centre (hv) position filters the unrounded
 * 16-bit horizontal results vertically in 32-bit lanes, like the C code.
 * Sub-pixel positions are combined exactly as in dsputil_internal.h (H264_MC).
 *
 * ff_xb_h264_qpel_init() runs a self-test against the C functions first and only
 * installs these if every block is identical.
 */
#include <ppcintrinsics.h>
#include <vectorintrinsics.h>
#include <string.h>
#include "libavcodec/dsputil.h"

#define LOADU(p) __vor( __lvlx( (p), 0 ), __lvrx( (p), 16 ) )
#define TMP_STRIDE 16

typedef struct
{
    __vector4 zero, k2, k4, k5, c16;          /* 16-bit lane constants */
    __vector4 w2, w4, w10, c512;              /* 32-bit lane constants */
} Consts;

static __inline void InitConsts( Consts* k )
{
    k->zero = __vspltisb( 0 );
    k->k2 = __vspltish( 2 );
    k->k4 = __vspltish( 4 );
    k->k5 = __vspltish( 5 );
    k->c16 = __vslh( __vspltish( 1 ), k->k4 );
    k->w2 = __vspltisw( 2 );
    k->w4 = __vspltisw( 4 );
    k->w10 = __vspltisw( 10 );
    k->c512 = __vslw( __vspltisw( 1 ), __vspltisw( 9 ) );
}

/* (c+d)*20 - (b+e)*5 + (a+f), 16-bit lanes. */
static __inline __vector4 Tap16( const Consts* k, __vector4 a, __vector4 b, __vector4 c, __vector4 d, __vector4 e,
                                  __vector4 f )
{
    __vector4 cd = __vadduhm( c, d ), be = __vadduhm( b, e ), af = __vadduhm( a, f );
    __vector4 x20 = __vadduhm( __vslh( cd, k->k4 ), __vslh( cd, k->k2 ) );
    __vector4 x5 = __vadduhm( __vslh( be, k->k2 ), be );
    return __vadduhm( __vsubuhm( x20, x5 ), af );
}

/* Same in 32-bit lanes. */
static __inline __vector4 Tap32( const Consts* k, __vector4 a, __vector4 b, __vector4 c, __vector4 d, __vector4 e,
                                  __vector4 f )
{
    __vector4 cd = __vadduwm( c, d ), be = __vadduwm( b, e ), af = __vadduwm( a, f );
    __vector4 x20 = __vadduwm( __vslw( cd, k->w4 ), __vslw( cd, k->w2 ) );
    __vector4 x5 = __vadduwm( __vslw( be, k->w2 ), be );
    return __vadduwm( __vsubuwm( x20, x5 ), af );
}

/* Horizontal 6-tap of one row, unrounded: first 8 outputs in *hi, next 8 in *lo. */
static __inline void HRow( const Consts* k, const uint8_t* src, int width, __vector4* hi, __vector4* lo )
{
    __vector4 v0 = LOADU( src - 2 ), v1 = LOADU( src + 14 );
    __vector4 s1 = __vsldoi( v0, v1, 1 ), s2 = __vsldoi( v0, v1, 2 ), s3 = __vsldoi( v0, v1, 3 );
    __vector4 s4 = __vsldoi( v0, v1, 4 ), s5 = __vsldoi( v0, v1, 5 );
    *hi = Tap16( k, __vmrghb( k->zero, v0 ), __vmrghb( k->zero, s1 ), __vmrghb( k->zero, s2 ),
                 __vmrghb( k->zero, s3 ), __vmrghb( k->zero, s4 ), __vmrghb( k->zero, s5 ) );
    if( width == 16 )
        *lo = Tap16( k, __vmrglb( k->zero, v0 ), __vmrglb( k->zero, s1 ), __vmrglb( k->zero, s2 ),
                     __vmrglb( k->zero, s3 ), __vmrglb( k->zero, s4 ), __vmrglb( k->zero, s5 ) );
    else
        *lo = *hi;
}

static __inline __vector4 Round5( const Consts* k, __vector4 hi, __vector4 lo )
{
    return __vpkshus( __vsrah( __vadduhm( hi, k->c16 ), k->k5 ), __vsrah( __vadduhm( lo, k->c16 ), k->k5 ) );
}

/* Filtered blocks go into aligned temporaries with a 16-byte stride. */
static void HPass( uint8_t* out, const uint8_t* src, int stride, int size )
{
    Consts k;
    int i;
    InitConsts( &k );
    for( i = 0; i < size; ++i, src += stride, out += TMP_STRIDE )
    {
        __vector4 hi, lo;
        HRow( &k, src, size, &hi, &lo );
        __stvx( Round5( &k, hi, lo ), out, 0 );
    }
}

static void VPass( uint8_t* out, const uint8_t* src, int stride, int size )
{
    Consts k;
    __vector4 hi[6], lo[6];
    int i, j;
    InitConsts( &k );
    src -= 2 * stride;
    for( j = 0; j < 5; ++j, src += stride )
    {
        __vector4 v = LOADU( src );
        hi[j] = __vmrghb( k.zero, v );
        lo[j] = __vmrglb( k.zero, v );
    }
    for( i = 0; i < size; ++i, src += stride, out += TMP_STRIDE )
    {
        __vector4 v = LOADU( src ), rh, rl;
        hi[5] = __vmrghb( k.zero, v );
        lo[5] = __vmrglb( k.zero, v );
        rh = Tap16( &k, hi[0], hi[1], hi[2], hi[3], hi[4], hi[5] );
        rl = size == 16 ? Tap16( &k, lo[0], lo[1], lo[2], lo[3], lo[4], lo[5] ) : rh;
        __stvx( Round5( &k, rh, rl ), out, 0 );
        for( j = 0; j < 5; ++j )
        {
            hi[j] = hi[j + 1];
            lo[j] = lo[j + 1];
        }
    }
}

static __inline __vector4 VCentre( const Consts* k, const __vector4* t )
{
    /* t[0..5]: 8 unrounded int16 horizontal results from 6 consecutive rows. */
    __vector4 h = Tap32( k, __vupkhsh( t[0] ), __vupkhsh( t[1] ), __vupkhsh( t[2] ), __vupkhsh( t[3] ),
                         __vupkhsh( t[4] ), __vupkhsh( t[5] ) );
    __vector4 l = Tap32( k, __vupklsh( t[0] ), __vupklsh( t[1] ), __vupklsh( t[2] ), __vupklsh( t[3] ),
                         __vupklsh( t[4] ), __vupklsh( t[5] ) );
    h = __vsraw( __vadduwm( h, k->c512 ), k->w10 );
    l = __vsraw( __vadduwm( l, k->c512 ), k->w10 );
    return __vpkswss( h, l );
}

static void HVPass( uint8_t* out, const uint8_t* src, int stride, int size )
{
    Consts k;
    __vector4 hi[21], lo[21];
    int i;
    InitConsts( &k );
    src -= 2 * stride;
    for( i = 0; i < size + 5; ++i, src += stride )
        HRow( &k, src, size, &hi[i], &lo[i] );
    for( i = 0; i < size; ++i, out += TMP_STRIDE )
    {
        __vector4 rh = VCentre( &k, &hi[i] );
        __vector4 rl = size == 16 ? VCentre( &k, &lo[i] ) : rh;
        __stvx( __vpkshus( rh, rl ), out, 0 );
    }
}

/* dst = op( dst, a [avg b] ) for one block; 8-wide blocks keep the bytes after them. */
static void Combine( uint8_t* dst, int stride, const uint8_t* a, int strideA, const uint8_t* b, int strideB,
                     int size, int avg )
{
    static const __declspec( align( 16 ) ) unsigned char m8[16] = { 255,255,255,255,255,255,255,255,0,0,0,0,0,0,0,0 };
    __vector4 mask = __lvx( m8, 0 );
    int i;
    for( i = 0; i < size; ++i, dst += stride, a += strideA, b += strideB )
    {
        __vector4 r = LOADU( a ), old;
        if( b )
            r = __vavgub( r, LOADU( b ) );
        if( size == 16 && !avg )
        {
            __stvlx( r, dst, 0 );
            __stvrx( r, dst, 16 );
            continue;
        }
        old = LOADU( dst );
        if( avg )
            r = __vavgub( old, r );
        if( size == 8 )
            r = __vsel( old, r, mask );
        __stvlx( r, dst, 0 );
        __stvrx( r, dst, 16 );
    }
    (void)b;
}

/* The 16 quarter-pel positions, mirroring H264_MC in dsputil_internal.h. */
static void QpelMc( uint8_t* dst, uint8_t* src, int stride, int size, int avg, int pos )
{
    __declspec( align( 16 ) ) uint8_t t1[16 * TMP_STRIDE], t2[16 * TMP_STRIDE];
    const int T = TMP_STRIDE;
    switch( pos )
    {
    case 0:  Combine( dst, stride, src, stride, NULL, 0, size, avg ); break;                                   /* mc00 */
    case 1:  HPass( t1, src, stride, size ); Combine( dst, stride, src, stride, t1, T, size, avg ); break;      /* mc10 */
    case 2:  HPass( t1, src, stride, size ); Combine( dst, stride, t1, T, NULL, 0, size, avg ); break;         /* mc20 */
    case 3:  HPass( t1, src, stride, size ); Combine( dst, stride, src + 1, stride, t1, T, size, avg ); break;  /* mc30 */
    case 4:  VPass( t1, src, stride, size ); Combine( dst, stride, src, stride, t1, T, size, avg ); break;      /* mc01 */
    case 5:  HPass( t1, src, stride, size ); VPass( t2, src, stride, size );
             Combine( dst, stride, t1, T, t2, T, size, avg ); break;                                          /* mc11 */
    case 6:  HPass( t1, src, stride, size ); HVPass( t2, src, stride, size );
             Combine( dst, stride, t1, T, t2, T, size, avg ); break;                                          /* mc21 */
    case 7:  HPass( t1, src, stride, size ); VPass( t2, src + 1, stride, size );
             Combine( dst, stride, t1, T, t2, T, size, avg ); break;                                          /* mc31 */
    case 8:  VPass( t1, src, stride, size ); Combine( dst, stride, t1, T, NULL, 0, size, avg ); break;         /* mc02 */
    case 9:  VPass( t1, src, stride, size ); HVPass( t2, src, stride, size );
             Combine( dst, stride, t1, T, t2, T, size, avg ); break;                                          /* mc12 */
    case 10: HVPass( t1, src, stride, size ); Combine( dst, stride, t1, T, NULL, 0, size, avg ); break;        /* mc22 */
    case 11: VPass( t1, src + 1, stride, size ); HVPass( t2, src, stride, size );
             Combine( dst, stride, t1, T, t2, T, size, avg ); break;                                          /* mc32 */
    case 12: VPass( t1, src, stride, size ); Combine( dst, stride, src + stride, stride, t1, T, size, avg ); break; /* mc03 */
    case 13: HPass( t1, src + stride, stride, size ); VPass( t2, src, stride, size );
             Combine( dst, stride, t1, T, t2, T, size, avg ); break;                                          /* mc13 */
    case 14: HPass( t1, src + stride, stride, size ); HVPass( t2, src, stride, size );
             Combine( dst, stride, t1, T, t2, T, size, avg ); break;                                          /* mc23 */
    case 15: HPass( t1, src + stride, stride, size ); VPass( t2, src + 1, stride, size );
             Combine( dst, stride, t1, T, t2, T, size, avg ); break;                                          /* mc33 */
    }
}

#define MC( OP, AVG, SIZE, POS ) \
    static void OP ## _qpel ## SIZE ## _ ## POS ## _xb( uint8_t* dst, uint8_t* src, int stride ) \
    { QpelMc( dst, src, stride, SIZE, AVG, POS ); }
#define MC16( OP, AVG, SIZE ) \
    MC( OP, AVG, SIZE, 0 ) MC( OP, AVG, SIZE, 1 ) MC( OP, AVG, SIZE, 2 ) MC( OP, AVG, SIZE, 3 ) \
    MC( OP, AVG, SIZE, 4 ) MC( OP, AVG, SIZE, 5 ) MC( OP, AVG, SIZE, 6 ) MC( OP, AVG, SIZE, 7 ) \
    MC( OP, AVG, SIZE, 8 ) MC( OP, AVG, SIZE, 9 ) MC( OP, AVG, SIZE, 10 ) MC( OP, AVG, SIZE, 11 ) \
    MC( OP, AVG, SIZE, 12 ) MC( OP, AVG, SIZE, 13 ) MC( OP, AVG, SIZE, 14 ) MC( OP, AVG, SIZE, 15 )
MC16( put, 0, 16 )
MC16( put, 0, 8 )
MC16( avg, 1, 16 )
MC16( avg, 1, 8 )

#define TAB( OP, SIZE ) { \
    OP ## _qpel ## SIZE ## _0_xb, OP ## _qpel ## SIZE ## _1_xb, OP ## _qpel ## SIZE ## _2_xb, OP ## _qpel ## SIZE ## _3_xb, \
    OP ## _qpel ## SIZE ## _4_xb, OP ## _qpel ## SIZE ## _5_xb, OP ## _qpel ## SIZE ## _6_xb, OP ## _qpel ## SIZE ## _7_xb, \
    OP ## _qpel ## SIZE ## _8_xb, OP ## _qpel ## SIZE ## _9_xb, OP ## _qpel ## SIZE ## _10_xb, OP ## _qpel ## SIZE ## _11_xb, \
    OP ## _qpel ## SIZE ## _12_xb, OP ## _qpel ## SIZE ## _13_xb, OP ## _qpel ## SIZE ## _14_xb, OP ## _qpel ## SIZE ## _15_xb }

static qpel_mc_func vmx_put[2][16] = { TAB( put, 16 ), TAB( put, 8 ) };
static qpel_mc_func vmx_avg[2][16] = { TAB( avg, 16 ), TAB( avg, 8 ) };
static qpel_mc_func c_put[2][16], c_avg[2][16];

static unsigned int q_seed = 777;
static int QRand( void )
{
    q_seed = q_seed * 1664525u + 1013904223u;
    return (int)( q_seed >> 16 );
}

/* Number of mismatching blocks against the C functions (0 = identical). */
int ff_xb_h264_qpel_selftest( void )
{
    enum { STRIDE = 64, ROWS = 32 };
    static __declspec( align( 16 ) ) uint8_t src[STRIDE * ROWS];
    static __declspec( align( 16 ) ) uint8_t d1[STRIDE * ROWS], d2[STRIDE * ROWS];
    int bad = 0, run, s, pos, avg, i;
    for( run = 0; run < 8; ++run )
    {
        for( i = 0; i < (int)sizeof( src ); ++i )
            src[i] = (uint8_t)( run == 0 ? 255 : ( run == 1 ? 0 : ( run == 2 ? ( i & 1 ) * 255 : QRand() ) ) );
        for( s = 0; s < 2; ++s )
            for( avg = 0; avg < 2; ++avg )
                for( pos = 0; pos < 16; ++pos )
                {
                    int soff = 3 * STRIDE + 5 + run, doff = 2 * STRIDE + ( run & 1 ? 8 : 3 );
                    for( i = 0; i < (int)sizeof( d1 ); ++i )
                        d1[i] = d2[i] = (uint8_t)QRand();
                    ( avg ? c_avg : c_put )[s][pos]( d1 + doff, src + soff, STRIDE );
                    ( avg ? vmx_avg : vmx_put )[s][pos]( d2 + doff, src + soff, STRIDE );
                    if( memcmp( d1, d2, sizeof( d1 ) ) )
                        ++bad;
                }
    }
    return bad;
}

int ff_xb_qpel_verdict = -2;   /* -2 untested, 0 VMX in use, >0 mismatches (C kept) */

void ff_xb_h264_qpel_init( DSPContext* c )
{
    int s, pos;
    if( ff_xb_qpel_verdict == -2 )
    {
        for( s = 0; s < 2; ++s )
            for( pos = 0; pos < 16; ++pos )
            {
                c_put[s][pos] = c->put_h264_qpel_pixels_tab[s][pos];
                c_avg[s][pos] = c->avg_h264_qpel_pixels_tab[s][pos];
            }
        ff_xb_qpel_verdict = ff_xb_h264_qpel_selftest();
    }
    if( ff_xb_qpel_verdict != 0 )
        return;
    for( s = 0; s < 2; ++s )
        for( pos = 0; pos < 16; ++pos )
        {
            c->put_h264_qpel_pixels_tab[s][pos] = vmx_put[s][pos];
            c->avg_h264_qpel_pixels_tab[s][pos] = vmx_avg[s][pos];
        }
}

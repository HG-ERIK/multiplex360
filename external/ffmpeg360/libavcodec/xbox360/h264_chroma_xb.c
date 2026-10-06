/*
 * H.264 chroma motion compensation with VMX128 for the Xbox 360.
 *
 * The C version computes, per pixel,
 *     (A*a + B*b + C*c + D*d + 32) >> 6,  A=(8-x)(8-y) B=x(8-y) C=(8-x)y D=xy
 * Xenon's VMX128 has no integer multiplies, so this uses the exactly equivalent
 * separable form with multiplies by 0..7 done as shifts and adds:
 *     h(row) = 8*a + x*(b - a)              (horizontal, per row)
 *     v      = 8*h(row) + y*(h(row+1) - h(row))
 *     out    = (v + 32) >> 6
 * Everything stays within 16-bit lanes (v <= 64*255), so results are bit-exact.
 *
 * ff_xb_h264_chroma_selftest() compares against the C functions on random data.
 */
#include <ppcintrinsics.h>
#include <vectorintrinsics.h>
#include <stdlib.h>
#include <string.h>
#include "libavcodec/dsputil.h"

#define LOADU(p) __vor( __lvlx( (p), 0 ), __lvrx( (p), 16 ) )

static __inline void storeu( __vector4 v, uint8_t* p )
{
    __stvlx( v, p, 0 );
    __stvrx( v, p, 16 );
}

/* v * k for k in 0..7 (16-bit lanes, modular). */
static __inline __vector4 mul07( __vector4 v, int k, __vector4 one, __vector4 two )
{
    __vector4 r = __vxor( v, v );
    if( k & 1 ) r = __vadduhm( r, v );
    if( k & 2 ) r = __vadduhm( r, __vslh( v, one ) );
    if( k & 4 ) r = __vadduhm( r, __vslh( v, two ) );
    return r;
}

/* Horizontal pass for one source row: 8*a + x*(b-a) for the first 8 pixels. */
static __inline __vector4 hpass( const uint8_t* src, int x, __vector4 zero, __vector4 one, __vector4 two,
                                 __vector4 three )
{
    __vector4 v = LOADU( src );
    __vector4 a = __vmrghb( zero, v );                       /* src[0..7] as u16 */
    __vector4 h = __vslh( a, three );
    if( x )
    {
        __vector4 b = __vmrghb( zero, __vsldoi( v, v, 1 ) ); /* src[1..8] */
        h = __vadduhm( h, mul07( __vsubuhm( b, a ), x, one, two ) );
    }
    return h;
}

/* Byte masks selecting the first n output bytes of a 16-byte store. */
static __inline __vector4 headmask( int n )
{
    static const __declspec( align( 16 ) ) unsigned char m8[16] = { 255,255,255,255,255,255,255,255,0,0,0,0,0,0,0,0 };
    static const __declspec( align( 16 ) ) unsigned char m4[16] = { 255,255,255,255,0,0,0,0,0,0,0,0,0,0,0,0 };
    return __lvx( n == 8 ? m8 : m4, 0 );
}

static __inline void chroma_mc( uint8_t* dst, uint8_t* src, int stride, int h, int x, int y, int width, int avg )
{
    const __vector4 zero = __vspltisb( 0 );
    const __vector4 one = __vspltish( 1 ), two = __vspltish( 2 ), three = __vspltish( 3 ), six = __vspltish( 6 );
    const __vector4 c32 = __vslh( one, __vspltish( 5 ) );
    const __vector4 mask = headmask( width );
    __vector4 h0 = hpass( src, x, zero, one, two, three );
    int i;
    for( i = 0; i < h; ++i )
    {
        __vector4 v, out, old, packed;
        if( y )
        {
            __vector4 h1 = hpass( src + stride, x, zero, one, two, three );
            v = __vadduhm( __vslh( h0, three ), mul07( __vsubuhm( h1, h0 ), y, one, two ) );
            h0 = h1;
        }
        else
        {
            v = __vslh( h0, three );
            if( i + 1 < h )
                h0 = hpass( src + stride, x, zero, one, two, three );
        }
        out = __vsrah( __vadduhm( v, c32 ), six );
        packed = __vpkshus( out, out );
        old = LOADU( dst );
        if( avg )
            packed = __vavgub( packed, old );
        storeu( __vsel( old, packed, mask ), dst );
        dst += stride;
        src += stride;
    }
}

static void put_h264_chroma_mc8_xb( uint8_t* dst, uint8_t* src, int stride, int h, int x, int y )
{
    chroma_mc( dst, src, stride, h, x, y, 8, 0 );
}
static void avg_h264_chroma_mc8_xb( uint8_t* dst, uint8_t* src, int stride, int h, int x, int y )
{
    chroma_mc( dst, src, stride, h, x, y, 8, 1 );
}
static void put_h264_chroma_mc4_xb( uint8_t* dst, uint8_t* src, int stride, int h, int x, int y )
{
    chroma_mc( dst, src, stride, h, x, y, 4, 0 );
}
static void avg_h264_chroma_mc4_xb( uint8_t* dst, uint8_t* src, int stride, int h, int x, int y )
{
    chroma_mc( dst, src, stride, h, x, y, 4, 1 );
}

/* Small LCG for test data (FFmpeg blocks rand()). */
static unsigned int xb_seed = 12345;
static int xb_rand( void )
{
    xb_seed = xb_seed * 1664525u + 1013904223u;
    return (int)( xb_seed >> 16 );
}

/* The C versions, kept for the self-test. */
static h264_chroma_mc_func c_put[2], c_avg[2];

int ff_xb_h264_chroma_selftest( void );
int ff_xb_chroma_verdict = -2;   /* -2 untested, 0 VMX in use, >0 mismatches (C kept) */

void ff_xb_h264_chroma_init( DSPContext* c )
{
    if( ff_xb_chroma_verdict == -2 )
    {
        c_put[0] = c->put_h264_chroma_pixels_tab[0];
        c_put[1] = c->put_h264_chroma_pixels_tab[1];
        c_avg[0] = c->avg_h264_chroma_pixels_tab[0];
        c_avg[1] = c->avg_h264_chroma_pixels_tab[1];
        ff_xb_chroma_verdict = ff_xb_h264_chroma_selftest();
    }
    if( ff_xb_chroma_verdict != 0 )
        return;   /* never use VMX that doesn't match the C code exactly */
    c->put_h264_chroma_pixels_tab[0] = put_h264_chroma_mc8_xb;
    c->put_h264_chroma_pixels_tab[1] = put_h264_chroma_mc4_xb;
    c->avg_h264_chroma_pixels_tab[0] = avg_h264_chroma_mc8_xb;
    c->avg_h264_chroma_pixels_tab[1] = avg_h264_chroma_mc4_xb;
}

/* Returns the number of mismatching blocks (0 = identical to the C code). */
int ff_xb_h264_chroma_selftest( void )
{
    enum { STRIDE = 64, ROWS = 24 };
    static __declspec( align( 16 ) ) uint8_t src[STRIDE * ROWS + 64];
    static __declspec( align( 16 ) ) uint8_t d1[STRIDE * ROWS + 64], d2[STRIDE * ROWS + 64];
    h264_chroma_mc_func vmx_put[2] = { put_h264_chroma_mc8_xb, put_h264_chroma_mc4_xb };
    h264_chroma_mc_func vmx_avg[2] = { avg_h264_chroma_mc8_xb, avg_h264_chroma_mc4_xb };
    int bad = 0, run, size, x, y, avg, hi, off, i;
    static const int heights[3] = { 2, 4, 8 };
    if( !c_put[0] )
        return -1;
    xb_seed = 12345;
    for( run = 0; run < 6; ++run )
    {
        for( i = 0; i < (int)sizeof( src ); ++i )
            src[i] = (uint8_t)( run == 0 ? 255 : ( run == 1 ? 0 : xb_rand() ) );
        for( size = 0; size < 2; ++size )
            for( avg = 0; avg < 2; ++avg )
                for( hi = 0; hi < 3; ++hi )
                    for( y = 0; y < 8; ++y )
                        for( x = 0; x < 8; ++x )
                        {
                            off = 3 + run;   /* unaligned source and destination */
                            for( i = 0; i < (int)sizeof( d1 ); ++i )
                                d1[i] = d2[i] = (uint8_t)xb_rand();
                            ( avg ? c_avg : c_put )[size]( d1 + off, src + 7 + off, STRIDE, heights[hi], x, y );
                            ( avg ? vmx_avg : vmx_put )[size]( d2 + off, src + 7 + off, STRIDE, heights[hi], x, y );
                            if( memcmp( d1, d2, sizeof( d1 ) ) )
                                ++bad;
                        }
    }
    return bad;
}

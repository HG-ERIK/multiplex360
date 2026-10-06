#include <xtl.h>

/****************************************************************************
 * IDCT transform:
 ****************************************************************************/
#define VEC_1D_DCT(vb0,vb1,vb2,vb3,va0,va1,va2,va3)               \
    /* 1st stage */                                               \
    vz0 = __vadduhm(vb0,vb2);       /* temp[0] = Y[0] + Y[2] */     \
    vz1 = __vsubuhm(vb0,vb2);       /* temp[1] = Y[0] - Y[2] */     \
    vz2 = __vsrah(vb1,__vpkshus(1));                          \
    vz2 = __vsubuhm(vz2,vb3);       /* temp[2] = Y[1].1/2 - Y[3] */ \
    vz3 = __vsrah (vb3,__vpkshus(1));                          \
    vz3 = __vadduhm(vb1,vz3);       /* temp[3] = Y[1] + Y[3].1/2 */ \
    /* 2nd stage: output */                                       \
    va0 = __vadduhm (vz0,vz3);       /* x[0] = temp[0] + temp[3] */  \
    va1 = __vadduhm (vz1,vz2);       /* x[1] = temp[1] + temp[2] */  \
    va2 = __vsubuhm (vz1,vz2);       /* x[2] = temp[1] - temp[2] */  \
    va3 = __vsubuhm(vz0,vz3)        /* x[3] = temp[0] - temp[3] */
	
	
#define VEC_TRANSPOSE_4(a0,a1,a2,a3,b0,b1,b2,b3) \
    b0 = __vmrghh( a0, a0 ); \
    b1 = __vmrghh( a1, a0 ); \
    b2 = __vmrghh( a2, a0 ); \
    b3 = __vmrghh( a3, a0 ); \
    a0 = __vmrghh( b0, b2 ); \
    a1 = __vmrglh( b0, b2 ); \
    a2 = __vmrghh( b1, b3 ); \
    a3 = __vmrglh( b1, b3 ); \
    b0 = __vmrghh( a0, a2 ); \
    b1 = __vmrglh( a0, a2 ); \
    b2 = __vmrghh( a1, a3 ); \
    b3 = __vmrglh( a1, a3 )

#define VEC_LOAD_U8_ADD_S16_STORE_U8(va)                      \
    vdst_orig = __lvx(dst, 0);                               \
    vdst = __vperm(vdst_orig, zero_u8v, vdst_mask);          \
    vdst_ss = __vmrghh(zero_u8v, vdst);         \
    va = __vadduhm(va, vdst_ss);                                \
    va_u8 = __vpkshus(va, zero_s16v);                        \
    va_u32 = __vspltw(va_u8, 0);                  \
    __stvewx(va_u32, (uint32_t*)dst, element);


static void ff_h264_idct_add_altivec(uint8_t *dst, DCTELEM *block, int stride)
{
    vec_s16 va0, va1, va2, va3;//s16
    vec_s16 vz0, vz1, vz2, vz3;//s16
    vec_s16 vtmp0, vtmp1, vtmp2, vtmp3;//s16
    vec_u8 va_u8;//u8
    vec_u32 va_u32;//u32
    vec_s16 vdst_ss;//s16
    const vec_u16 v6us = __vsplth(6);//u16
    vec_u8 vdst, vdst_orig;//u8
    vec_u8 vdst_mask = __lvsl(dst,0);//u8
    int element = ((unsigned long)dst & 0xf) >> 2;
    LOAD_ZERO;

    block[0] += 32;  /* add 32 as a DC-level for rounding */

    vtmp0 = __lvx(block,0);
    vtmp1 = __vsldoi(vtmp0, vtmp0, 8);
    vtmp2 = __lvx(block,16);
    vtmp3 = __vsldoi(vtmp2, vtmp2, 8);

    VEC_1D_DCT(vtmp0,vtmp1,vtmp2,vtmp3,va0,va1,va2,va3);
    VEC_TRANSPOSE_4(va0,va1,va2,va3,vtmp0,vtmp1,vtmp2,vtmp3);
    VEC_1D_DCT(vtmp0,vtmp1,vtmp2,vtmp3,va0,va1,va2,va3);

    va0 = __vsrah(va0,v6us);
    va1 = __vsrah(va1,v6us);
    va2 = __vsrah(va2,v6us);
    va3 = __vsrah(va3,v6us);

    VEC_LOAD_U8_ADD_S16_STORE_U8(va0);
    dst += stride;
    VEC_LOAD_U8_ADD_S16_STORE_U8(va1);
    dst += stride;
    VEC_LOAD_U8_ADD_S16_STORE_U8(va2);
    dst += stride;
    VEC_LOAD_U8_ADD_S16_STORE_U8(va3);
}



static av_always_inline void h264_idct_dc_add_internal(uint8_t *dst, DCTELEM *block, int stride, int size)
{
    vec_s16 dc16;
    vec_u8 dcplus, dcminus, v0, v1, v2, v3, aligner;
    LOAD_ZERO;
    DECLARE_ALIGNED(16, int, dc);
    int i;

    dc = (block[0] + 32) >> 6;
    dc16 = __vsplth(__lvewx(&dc, 0), 1);

    if (size == 4)
        dc16 = __vsldoi(dc16, zero_s16v, 8);
    dcplus = __vpkshus(dc16, zero_s16v);
    dcminus = __vpkshus(__vsubuhm(zero_s16v, dc16), zero_s16v);

    aligner = __lvsr(0, dst);
    dcplus = __perm(dcplus, dcplus, aligner);
    dcminus = __perm(dcminus, dcminus, aligner);

    for (i = 0; i < size; i += 4) {
        v0 = __lvx(dst+0*stride, 0);
        v1 = __lvx(dst+1*stride, 0);
        v2 = __lvx(dst+2*stride, 0);
        v3 = __lvx(dst+3*stride, 0);

        v0 = __vaddubs(v0, dcplus);
        v1 = __vaddubs(v1, dcplus);
        v2 = __vaddubs(v2, dcplus);
        v3 = __vaddubs(v3, dcplus);

        v0 = __vsububs(v0, dcminus);
        v1 = __vsububs(v1, dcminus);
        v2 = __vsububs(v2, dcminus);
        v3 = __vsububs(v3, dcminus);

        __stvx(v0, dst+0*stride, 0);
        __stvx(v1, dst+1*stride, 0);
        __stvx(v2, dst+2*stride, 0);
        __stvx(v3, dst+3*stride, 0);

        dst += 4*stride;
    }
}

static void h264_idct_dc_add_altivec(uint8_t *dst, DCTELEM *block, int stride)
{
    h264_idct_dc_add_internal(dst, block, stride, 4);
}

static void ff_h264_idct8_dc_add_altivec(uint8_t *dst, DCTELEM *block, int stride)
{
    h264_idct_dc_add_internal(dst, block, stride, 8);
}

static void ff_h264_idct_add16_altivec(uint8_t *dst, const int *block_offset, DCTELEM *block, int stride, const uint8_t nnzc[6*8]){
    int i;
    for(i=0; i<16; i++){
        int nnz = nnzc[ scan8[i] ];
        if(nnz){
            if(nnz==1 && block[i*16]) h264_idct_dc_add_altivec(dst + block_offset[i], block + i*16, stride);
            else                      ff_h264_idct_add_altivec(dst + block_offset[i], block + i*16, stride);
        }
    }
}

static void ff_h264_idct_add16intra_altivec(uint8_t *dst, const int *block_offset, DCTELEM *block, int stride, const uint8_t nnzc[6*8]){
    int i;
    for(i=0; i<16; i++){
        if(nnzc[ scan8[i] ]) ff_h264_idct_add_altivec(dst + block_offset[i], block + i*16, stride);
        else if(block[i*16]) h264_idct_dc_add_altivec(dst + block_offset[i], block + i*16, stride);
    }
}

static void ff_h264_idct8_add4_altivec(uint8_t *dst, const int *block_offset, DCTELEM *block, int stride, const uint8_t nnzc[6*8]){
    int i;
    for(i=0; i<16; i+=4){
        int nnz = nnzc[ scan8[i] ];
        if(nnz){
            if(nnz==1 && block[i*16]) ff_h264_idct8_dc_add_altivec(dst + block_offset[i], block + i*16, stride);
            else                      ff_h264_idct8_add_altivec   (dst + block_offset[i], block + i*16, stride);
        }
    }
}

static void ff_h264_idct_add8_altivec(uint8_t **dest, const int *block_offset, DCTELEM *block, int stride, const uint8_t nnzc[6*8]){
    int i;
    for(i=16; i<16+8; i++){
        if(nnzc[ scan8[i] ])
            ff_h264_idct_add_altivec(dest[(i&4)>>2] + block_offset[i], block + i*16, stride);
        else if(block[i*16])
            h264_idct_dc_add_altivec(dest[(i&4)>>2] + block_offset[i], block + i*16, stride);
    }
}



void idctInit(){
	c->h264_idct_add = ff_h264_idct_add_altivec;
	c->h264_idct_add8 = ff_h264_idct_add8_altivec;
	c->h264_idct_add16 = ff_h264_idct_add16_altivec;
	c->h264_idct_add16intra = ff_h264_idct_add16intra_altivec;
	c->h264_idct_dc_add= h264_idct_dc_add_altivec;
	c->h264_idct8_dc_add = ff_h264_idct8_dc_add_altivec;
	c->h264_idct8_add = ff_h264_idct8_add_altivec;
	c->h264_idct8_add4 = ff_h264_idct8_add4_altivec;
}
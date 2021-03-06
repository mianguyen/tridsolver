/*
 * Open source copyright declaration based on BSD open source template:
 * http://www.opensource.org/licenses/bsd-license.php
 *
 * This file is part of the scalar-tridiagonal solver distribution.
 *
 * Copyright (c) 2015, Endre László and others. Please see the AUTHORS file in
 * the main source directory for a full list of copyright holders.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * The name of Endre László may not be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY Endre László ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL Endre László BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

// Written by Endre Laszlo, University of Oxford, endre.laszlo@oerc.ox.ac.uk, 2013-2014 

#include "trid_common.h"
#include "trid_simd.h"
#include <assert.h>
#include "transpose.hpp"
#include "trid_cpu.h"

#define ROUND_DOWN(N,step) (((N)/(step))*step)

#ifdef __MIC__ // Or #ifdef __KNC__ - more general option, future proof, __INTEL_OFFLOAD is another option

__attribute__((target(mic)))
inline void load(SIMD_REG * __restrict__ dst, const FP * __restrict__ src, int n, int pad);

__attribute__((target(mic)))
inline void store(FP * __restrict__ dst, SIMD_REG * __restrict__ src, int n, int pad);

__attribute__((target(mic)))
void trid_x_transpose(const FP* __restrict a, const FP* __restrict b, const FP* __restrict c, FP* __restrict d, FP* __restrict u, int sys_size, int sys_pad, int stride);

__attribute__((target(mic)))
void trid_scalar_vec(const REAL* __restrict h_a, const REAL* __restrict h_b, const REAL* __restrict h_c, REAL* __restrict h_d, REAL* __restrict h_u, int N, int stride);

__attribute__((target(mic)))
void trid_scalar(const FP* __restrict a, const FP* __restrict b, const FP* __restrict c, FP* __restrict d, FP* __restrict u, int N, int stride);

#endif


inline void load(SIMD_REG * __restrict__ dst, const FP * __restrict__ src, int n, int pad) {
  __assume_aligned(src,SIMD_WIDTH);
  __assume_aligned(dst,SIMD_WIDTH);
  //  *(SIMD_REG*)&(u[i*N]) = *(SIMD_REG*)&(a[i*N]);
  for(int i=0; i<SIMD_VEC; i++) {
    //assert( ((long long)&(src[i*pad+n]) % SIMD_WIDTH) == 0);
    dst[i] = *(SIMD_REG*)&(src[i*pad+n]);
  }
}

inline void store(FP * __restrict__ dst, SIMD_REG * __restrict__ src, int n, int pad) {
  __assume_aligned(src,SIMD_WIDTH);
  __assume_aligned(dst,SIMD_WIDTH);
  //  *(SIMD_REG*)&(u[i*N]) = *(SIMD_REG*)&(a[i*N]);
  for(int i=0; i<SIMD_VEC; i++) {
    //assert( ((long long)&(dst[i*pad+n]) % SIMD_WIDTH) == 0);
    *(SIMD_REG*)&(dst[i*pad+n]) = src[i];
  }
}

#ifdef __MIC__ 
  #if FPPREC == 0 
     #define LOAD(reg,array,n,N) load(reg,array,n,N); transpose16x16_intrinsic(reg); 
     #define STORE(array,reg,n,N) transpose16x16_intrinsic(reg); store(array,reg,n,N);
  #elif FPPREC == 1 
     #define LOAD(reg,array,n,N) load(reg,array,n,N); transpose8x8_intrinsic(reg); 
     #define STORE(array,reg,n,N) transpose8x8_intrinsic(reg); store(array,reg,n,N);
  #endif
#elif __AVX__
  #if FPPREC == 0
     #define LOAD(reg,array,n,N) load(reg,array,n,N); transpose8x8_intrinsic(reg); 
     #define STORE(array,reg,n,N) transpose8x8_intrinsic(reg); store(array,reg,n,N);
  #elif FPPREC == 1
     #define LOAD(reg,array,n,N) load(reg,array,n,N); transpose4x4_intrinsic(reg); 
     #define STORE(array,reg,n,N) transpose4x4_intrinsic(reg); store(array,reg,n,N);
  #endif
#endif

//
// tridiagonal-x solver
//
//__attribute__((vector(linear(a),linear(b),linear(c),linear(d),linear(u))))
//inline void trid_x_transpose(FP* __restrict a, FP* __restrict b, FP* __restrict c, FP* __restrict d, FP* __restrict u, int sys_size, int sys_pad, int stride) {
void trid_x_transpose(const FP* __restrict a, const FP* __restrict b, const FP* __restrict c, FP* __restrict d, FP* __restrict u, int sys_size, int sys_pad, int stride) {

  __assume_aligned(a,SIMD_WIDTH);
  __assume_aligned(b,SIMD_WIDTH);
  __assume_aligned(c,SIMD_WIDTH);
  __assume_aligned(d,SIMD_WIDTH);

  assert( (((long long)a)%SIMD_WIDTH) == 0);

  int   i, ind = 0;
  SIMD_REG aa;  
  SIMD_REG bb;
  SIMD_REG cc;
  SIMD_REG dd;

  SIMD_REG tmp1;
  SIMD_REG tmp2;

  SIMD_REG a_reg[SIMD_VEC];  
  SIMD_REG b_reg[SIMD_VEC];
  SIMD_REG c_reg[SIMD_VEC];
  SIMD_REG d_reg[SIMD_VEC];

  SIMD_REG tmp_reg[SIMD_VEC];

  SIMD_REG c2[N_MAX];
  SIMD_REG d2[N_MAX];

  //
  // forward pass
  //
  int   n = 0;
  SIMD_REG ones = SIMD_SET1_P(1.0F);

  LOAD(a_reg,a,n,sys_pad);
  LOAD(b_reg,b,n,sys_pad);
  LOAD(c_reg,c,n,sys_pad);
  LOAD(d_reg,d,n,sys_pad);

  bb = b_reg[0];
  #if FPPREC == 0
    bb = SIMD_RCP_P(bb);
  #elif FPPREC == 1
    bb = SIMD_DIV_P(ones,bb);
  #endif
  cc = c_reg[0];
  cc = SIMD_MUL_P(bb,cc);
  dd = d_reg[0];
  dd = SIMD_MUL_P(bb,dd);
  c2[0] = cc;
  d2[0] = dd;
  
  //d_reg[0] = dd;

  for(i=1; i<SIMD_VEC; i++) {
    aa    = a_reg[i];
    #ifdef __MIC__
      bb    = SIMD_FNMADD_P(aa,cc,b_reg[i]);
      dd    = SIMD_FNMADD_P(aa,dd,d_reg[i]);
    #else
      bb    = SIMD_SUB_P(b_reg[i], SIMD_MUL_P(aa,cc) );
      dd    = SIMD_SUB_P(d_reg[i], SIMD_MUL_P(aa,dd) );
    #endif
    #if FPPREC == 0
      bb    = SIMD_RCP_P(bb);
    #elif FPPREC == 1
      bb    = SIMD_DIV_P(ones,bb);
    #endif
    cc    = SIMD_MUL_P(bb,c_reg[i]);
    dd    = SIMD_MUL_P(bb,dd);
    c2[n+i] = cc;
    d2[n+i] = dd;

    //d_reg[i] = dd;
  }
  //STORE(u,d_reg,n,sys_pad);

  for(n=SIMD_VEC; n < (sys_size/SIMD_VEC)*SIMD_VEC; n+=SIMD_VEC) {
    LOAD(a_reg,a,n,sys_pad);
    LOAD(b_reg,b,n,sys_pad);
    LOAD(c_reg,c,n,sys_pad);
    LOAD(d_reg,d,n,sys_pad);
    for(i=0; i<SIMD_VEC; i++) {
      aa    = a_reg[i];
      #ifdef __MIC__
        bb    = SIMD_FNMADD_P(aa,cc,b_reg[i]);
        dd    = SIMD_FNMADD_P(aa,dd,d_reg[i]);
      #else
        bb    = SIMD_SUB_P(b_reg[i], SIMD_MUL_P(aa,cc) );
        dd    = SIMD_SUB_P(d_reg[i], SIMD_MUL_P(aa,dd) );
      #endif
      #if FPPREC == 0
        bb    = SIMD_RCP_P(bb);
      #elif FPPREC == 1
        bb    = SIMD_DIV_P(ones,bb);
      #endif
      cc    = SIMD_MUL_P(bb,c_reg[i]);
      dd    = SIMD_MUL_P(bb,dd);
      c2[n+i] = cc;
      d2[n+i] = dd;
      
    //tmp1 = cc;
    //tmp2 = dd;
    //d_reg[i] = dd;
    //printf("i = %d   dd[0-15] = %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f \n",i,((FP*)&dd)[0],((FP*)&dd)[1],((FP*)&dd)[2],((FP*)&dd)[3],((FP*)&dd)[4],((FP*)&dd)[5],((FP*)&dd)[6],((FP*)&dd)[7],((FP*)&dd)[8],((FP*)&dd)[9],((FP*)&dd)[10],((FP*)&dd)[11],((FP*)&dd)[12],((FP*)&dd)[13],((FP*)&dd)[14],((FP*)&dd)[15]);
    }
    //printf("n = %d\n",n);
    //STORE(u,d_reg,n,sys_pad);
  }

  if(sys_size != sys_pad) {
    //n=n-SIMD_VEC;
    n  = (sys_size/SIMD_VEC)*SIMD_VEC;
    //printf("n = %d\n",n);
    //cc = c2[n-1];//i];//ones;//tmp1;
    //dd = d2[n-1];//i];//ones;//tmp2;
    //tmp_reg[0] = tmp;
    //STORE(u,tmp_reg,n,sys_pad);
    //d_reg[0] = c2[n+0];//cc;//bb;//cc;//dd;
    //STORE(u,d_reg,n,sys_pad);
    LOAD(a_reg,a,n,sys_pad);
    LOAD(b_reg,b,n,sys_pad);
    LOAD(c_reg,c,n,sys_pad);
    LOAD(d_reg,d,n,sys_pad);
    for(i=0; (n+i) < sys_size; i++) {
      //d_reg[i] = c2[n+i];//cc;//bb;//cc;//dd;
      //STORE(u,d_reg,n,sys_pad);
      aa    = a_reg[i];
      #ifdef __MIC__
        bb    = SIMD_FNMADD_P(aa,cc,b_reg[i]);
        dd    = SIMD_FNMADD_P(aa,dd,d_reg[i]);
      #else
        bb    = SIMD_SUB_P(b_reg[i], SIMD_MUL_P(aa,cc) );
        dd    = SIMD_SUB_P(d_reg[i], SIMD_MUL_P(aa,dd) );
      #endif
      #if FPPREC == 0
        bb    = SIMD_RCP_P(bb);
      #elif FPPREC == 1
        bb    = SIMD_DIV_P(ones,bb);
      #endif
      cc    = SIMD_MUL_P(bb,c_reg[i]);
      dd    = SIMD_MUL_P(bb,dd);
      c2[n+i] = cc;
      d2[n+i] = dd;
      
      //d_reg[i] = dd;
    }
    //STORE(u,d_reg,n,sys_pad);
   // STORE(u,d_reg,n,sys_pad);
    //d_reg[SIMD_VEC-1] = dd;
    d_reg[i-1] = dd;
    //n -= SIMD_VEC;
    //for(i=VEC-sys_off-2; i>=0; i--) {
    for(i=i-2; i>=0; i--) {
      //for(i=sys_off-2; i>=0; i--) {
      //if(i==VEC-sys_off-1) l_d[i] = dd;
      //if(i<VEC-sys_off-1) {
      dd     = SIMD_SUB_P(d2[n+i], SIMD_MUL_P(c2[n+i],dd) );
//    printf("i = %d   dd[0-15] = %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f \n",i,((FP*)&dd)[0],((FP*)&dd)[1],((FP*)&dd)[2],((FP*)&dd)[3],((FP*)&dd)[4],((FP*)&dd)[5],((FP*)&dd)[6],((FP*)&dd)[7],((FP*)&dd)[8],((FP*)&dd)[9],((FP*)&dd)[10],((FP*)&dd)[11],((FP*)&dd)[12],((FP*)&dd)[13],((FP*)&dd)[14],((FP*)&dd)[15]);
      //  dd      = d2[n+i] - c2[n+i]*dd;
      d_reg[i] = dd;
      //l_d[i]  = n+i;
      //}
    }
    STORE(d,d_reg,n,sys_pad);
    //STORE(u,d_reg,n,sys_pad);
  } else {

    //
    // reverse pass
    //
    d_reg[SIMD_VEC-1] = dd;
    n -= SIMD_VEC;
    //printf("n = %d\n",n);
    //for(i=VEC-sys_off-2; i>=0; i--) {
    for(i=SIMD_VEC-2; i>=0; i--) {
      //for(i=sys_off-2; i>=0; i--) {
      //if(i==VEC-sys_off-1) l_d[i] = dd;
      //if(i<VEC-sys_off-1) {
      dd     = SIMD_SUB_P(d2[n+i], SIMD_MUL_P(c2[n+i],dd) );
      //  dd      = d2[n+i] - c2[n+i]*dd;
      d_reg[i] = dd;
      //l_d[i]  = n+i;
      //}
    }
    //STORE(u,d_reg,n,sys_pad);
    STORE(d,d_reg,n,sys_pad);
  }

  //for(n=sys_size-2*VEC; n>=0; n-=VEC) {
  //for(n=n-SIMD_VEC; n>=0; n-=SIMD_VEC) {
  for(n=(sys_size/SIMD_VEC)*SIMD_VEC - SIMD_VEC; n>=0; n-=SIMD_VEC) {
    for(i=(SIMD_VEC-1); i>=0; i--) {
      dd     = SIMD_SUB_P(d2[n+i], SIMD_MUL_P(c2[n+i],dd) );
      //dd     = d2[n+i] - c2[n+i]*dd;
      d_reg[i] = dd;
      //l_d[i] = n+i;
    }
    //STORE(u,d_reg,n,sys_pad);
    STORE(d,d_reg,n,sys_pad);
  }
}

//
// tridiagonal solver
//
template<typename REAL, typename VECTOR, int INC>
//inline void trid_scalar_vec(REAL* __restrict h_a, REAL* __restrict h_b, REAL* __restrict h_c, REAL* __restrict h_d, REAL* __restrict h_u, int N, int stride) {
void trid_scalar_vec(const REAL* __restrict h_a, const REAL* __restrict h_b, const REAL* __restrict h_c, REAL* __restrict h_d, REAL* __restrict h_u, int N, int stride) {

  int i, ind = 0;
  VECTOR aa, bb, cc, dd, c2[N_MAX], d2[N_MAX];

  VECTOR* __restrict a = (VECTOR*) h_a;
  VECTOR* __restrict b = (VECTOR*) h_b;
  VECTOR* __restrict c = (VECTOR*) h_c;
  VECTOR* __restrict d = (VECTOR*) h_d;
  VECTOR* __restrict u = (VECTOR*) h_u;

//    b[0] = a[0];

  VECTOR ones(1.0f);

  //
  // forward pass
  //
  //bb    = 1.0f / b[0];
  bb    = ones / b[0];
  cc    = bb*c[0];
  dd    = bb*d[0];
  c2[0] = cc;
  d2[0] = dd;

  //u[0] = a[0];
  for(i=1; i<N; i++) {
    ind   = ind + stride;
    aa    = a[ind];
    bb    = b[ind] - aa*cc;
    dd    = d[ind] - aa*dd;
    //bb    = 1.0f/bb;
    bb    = ones / bb;
    cc    = bb*c[ind];
    dd    = bb*dd;
    c2[i] = cc;
    d2[i] = dd;
//    u[ind] = a[ind];
  }
  //
  // reverse pass
  //
  if(INC) u[ind] += dd;
  else    d[ind]  = dd;
//  u[ind] = dd;
  for(i=N-2; i>=0; i--) {
    ind    = ind - stride;
    dd     = d2[i] - c2[i]*dd;
    if(INC) u[ind] += dd;
    else    d[ind]  = dd;
//    u[ind] = ones;
  }
}

//
// tridiagonal solver
//
//inline void trid_scalar(FP* __restrict a, FP* __restrict b, FP* __restrict c, FP* __restrict d, FP* __restrict u, int N, int stride) {
void trid_scalar(const FP* __restrict a, const FP* __restrict b, const FP* __restrict c, FP* __restrict d, FP* __restrict u, int N, int stride) {
  int   i, ind = 0;
  FP aa, bb, cc, dd, c2[N_MAX], d2[N_MAX];
  //
  // forward pass
  //
  bb    = 1.0F/b[0];
  cc    = bb*c[0];
  dd    = bb*d[0];
  c2[0] = cc;
  d2[0] = dd;

  //u[0] = a[0];
  for(i=1; i<N; i++) {
    ind   = ind + stride;
    aa    = a[ind];
    bb    = b[ind] - aa*cc;
    dd    = d[ind] - aa*dd;
    bb    = 1.0F/bb;
    cc    = bb*c[ind];
    dd    = bb*dd;
    c2[i] = cc;
    d2[i] = dd;
//    u[ind] = a[ind];
  }
  //
  // reverse pass
  //
  d[ind] = dd;
//  u[ind] = dd;
  for(i=N-2; i>=0; i--) {
    ind    = ind - stride;
    dd     = d2[i] - c2[i]*dd;
    d[ind] = dd;
//    u[ind] = dd;
  }
}

//
// Function for selecting the proper setup for solve in a specific dimension
//
void tridMultiDimBatchSolve(const FP* a, const FP* b, const FP* c, FP* d, FP* u, int ndim, int solvedim, int *dims, int *pads) {
  //int sys_n = cumdims[ndim]/dims[solvedim]; // Number of systems to be solved

  if(solvedim == 0) {
    int sys_stride = 1;       // Stride between the consecutive elements of a system
    int sys_size   = dims[0]; // Size (length) of a system
    int sys_pads   = pads[0]; // Padded sizes along each ndim number of dimensions
    int sys_n_lin  = dims[1]*dims[2]; // = cumdims[solve] // Number of systems to be solved
    
    //if((sys_pads % SIMD_VEC) == 0) {
      #pragma omp parallel for collapse(2)
      for(int k=0; k<dims[2]; k++) {
        for(int j=0; j<ROUND_DOWN(dims[1],SIMD_VEC); j+=SIMD_VEC) {
          int ind = k*pads[0]*dims[1] + j*pads[0];
          trid_x_transpose(&a[ind], &b[ind], &c[ind], &d[ind], &u[ind], sys_size, sys_pads, sys_stride);
        }
      }
      if(ROUND_DOWN(dims[1],SIMD_VEC) < dims[1]) { // If there is leftover, fork threads an compute it
        #pragma omp parallel for collapse(2)
        for(int k=0; k<dims[2]; k++) {
          for(int j=ROUND_DOWN(dims[0],SIMD_VEC); j<dims[0]; j++) {
            int ind = k*pads[0]*dims[1] + j*pads[0];
            trid_scalar(&a[ind], &b[ind], &c[ind], &d[ind], &u[ind], sys_size, sys_stride);
          }
        }
      }
    //}
    //else {
    //  #pragma omp parallel for collapse(2)
    //  for(int k=0; k<dims[2]; k++) {
    //    for(int j=dims[0],SIMD_VEC); j<dims[0]; j++) {
    //      int ind = k*pads[0]*dims[1] + j*pads[0];
    //      trid_scalar(&a[ind], &b[ind], &c[ind], &d[ind], &u[ind], sys_size, sys_stride);
    //    }
    //  }
    //} 
  }
  //else if(solvedim==1) {
  //  int sys_stride = dims[0]; // Stride between the consecutive elements of a system
  //  int sys_size   = dims[1]; // Size (length) of a system
  //  int sys_pads   = pads[1]; // Padded sizes along each ndim number of dimensions
  //  int sys_n_lin  = dims[0]*dims[2]; // = cumdims[solve] // Number of systems to be solved
  //
  //  trid_scalar_vec<FP,VECTOR,0>(a, b, c, d, u, sys_size, sys_stride);

  //} else if(solvedim==2) {
  //  int numTrids = dims[0]*dims[1];
  //  int length   = dims[2];
  //  int stride1  = dims[0]*dims[1];
  //  int stride2  = 1;
  //  int subBatchSize   = dims[0]*dims[1];
  //  int subBatchStride = 0;
  //    solveBatchedTrid<REAL,INC>(numTrids, length, stride1, stride2, subBatchSize, subBatchStride, d_a, d_b, d_c, d_d, d_u);
  //} else {
  //  // Test if data is aligned
  //  long isaligned = 0;
  //  isaligned  = (long)d_a % CUDA_ALIGN_BYTE;            // Check if base pointers are aligned
  //  isaligned += (long)d_b % CUDA_ALIGN_BYTE;
  //  isaligned += (long)d_c % CUDA_ALIGN_BYTE;
  //  isaligned += (long)d_d % CUDA_ALIGN_BYTE;
  //  if(d_u != NULL) isaligned += (long)d_u % CUDA_ALIGN_BYTE;
  //  isaligned += (dims[0]*sizeof(REAL)) % CUDA_ALIGN_BYTE; // Check if X-dimension allows alignment

  //  if(isaligned==0) { // If any of the above is non-zero, vector loads can not be used
  //    if(sizeof(REAL) == 4) {
  //      // Kernel launch configuration
  //      int sys_n_float4 = sys_n/4;
  //      int blockdimx_float4 = 128; // Has to be the multiple of 32(or maybe 4??)
  //      int blockdimy_float4 = 1;
  //      int dimgrid_float4   = 1 + (sys_n_float4-1)/blockdimx_float4; // can go up to 65535
  //      int dimgridx_float4  = dimgrid_float4 % 65536;         // can go up to max 65535 on Fermi
  //      int dimgridy_float4  = 1 + dimgrid_float4 / 65536;
  //      dim3 dimGrid_float4(dimgridx_float4, dimgridy_float4);
  //      dim3 dimBlock_float4(blockdimx_float4,blockdimy_float4);

  //      // Setup dimension and padding according to float4 loads/stores
  //      dims[0] = dims[0]/4;
  //      pads[0] = dims[0];
  //      //trid_set_consts(ndim, dims, pads);
  //      initTridMultiDimBatchSolve(ndim, dims, pads);

  //      trid_strided_multidim<float,float4,INC><<<dimGrid_float4, dimBlock_float4>>>((float4*)d_a, (float4*)d_b, (float4*)d_c, (float4*)d_d, (float4*)d_u, ndim, solvedim, sys_n_float4);

  //      dims[0] = dims[0]*4;
  //      pads[0] = dims[0];
  //      //trid_set_consts(ndim, dims, pads);
  //      initTridMultiDimBatchSolve(ndim, dims, pads);
  //    } else if(sizeof(REAL) == 8) {
  //      // Kernel launch configuration
  //      int sys_n_double2 = sys_n/2;
  //      int blockdimx_double2 = 128; // Has to be the multiple of 32(or maybe 4??)
  //      int blockdimy_double2 = 1;
  //      int dimgrid_double2  = 1 + (sys_n_double2-1)/blockdimx_double2; // can go up to 65535
  //      int dimgridx_double2  = dimgrid_double2 % 65536;         // can go up to max 65535 on Fermi
  //      int dimgridy_double2  = 1 + dimgrid_double2 / 65536;
  //      dim3 dimGrid_double2(dimgridx_double2, dimgridy_double2);
  //      dim3 dimBlock_double2(blockdimx_double2,blockdimy_double2);
  //      // Setup dimension and padding according to double2 loads/stores
  //      dims[0] = dims[0]/2;
  //      pads[0] = dims[0];
  //      //trid_set_consts(ndim, dims, pads);
  //      initTridMultiDimBatchSolve(ndim, dims, pads);

  //      trid_strided_multidim<double,double2,INC><<<dimGrid_double2, dimBlock_double2>>>((double2*)d_a, (double2*)d_b, (double2*)d_c, (double2*)d_d, (double2*)d_u, ndim, solvedim, sys_n_double2);

  //      dims[0] = dims[0]*2;
  //      pads[0] = dims[0];
  //      //trid_set_consts(ndim, dims, pads);
  //      initTridMultiDimBatchSolve(ndim, dims, pads);
  //    }
  //  } else {
  //    // Kernel launch configuration
  //    int blockdimx = 128; // Has to be the multiple of 32(or maybe 4??)
  //    int blockdimy = 1;
  //    int dimgrid   = 1 + (sys_n-1)/blockdimx; // can go up to 65535
  //    int dimgridx  = dimgrid % 65536;         // can go up to max 65535 on Fermi
  //    int dimgridy  = 1 + dimgrid / 65536;
  //    dim3 dimGrid(dimgridx, dimgridy);
  //    dim3 dimBlock(blockdimx,blockdimy);

  //    trid_strided_multidim<REAL,REAL,INC><<<dimGrid, dimBlock>>>(d_a, d_b, d_c, d_d, d_u, ndim, solvedim, sys_n);
  //  }
  //}
  //}
}


#if FPPREC == 0


tridStatus_t tridSmtsvStridedBatch(const float *a, const float *b, const float *c, float *d, float* u, int ndim, int solvedim, int *dims, int *pads) {
  tridMultiDimBatchSolve(a, b, c, d, NULL, ndim, solvedim, dims, pads);
  return TRID_STATUS_SUCCESS;
}

//tridStatus_t tridSmtsvStridedBatchInc(const float *a, const float *b, const float *c, float *d, float* u, int ndim, int solvedim, int *dims, int *pads, int *opts, int sync) {
//  tridMultiDimBatchSolve<float,1>(a, b, c, d, u, ndim, solvedim, dims, pads, opts, 1);
//  return TRID_STATUS_SUCCESS;
//}
//
//int* get_opts() {return opts;}





void trid_scalarS(float* __restrict a, float* __restrict b, float* __restrict c, float* __restrict d, float* __restrict u, int N, int stride) {
  
  trid_scalar(a, b, c, d, u, N, stride);
  
}

void trid_x_transposeS(float* __restrict a, float* __restrict b, float* __restrict c, float* __restrict d, float* __restrict u, int sys_size, int sys_pad, int stride) {

  trid_x_transpose(a, b, c, d, u, sys_size, sys_pad, stride);

}

void trid_scalar_vecS(float* __restrict a, float* __restrict b, float* __restrict c, float* __restrict d, float* __restrict u, int N, int stride) {

  trid_scalar_vec<FP,VECTOR,0>(a, b, c, d, u, N, stride);

}

void trid_scalar_vecSInc(float* __restrict a, float* __restrict b, float* __restrict c, float* __restrict d, float* __restrict u, int N, int stride) {

  trid_scalar_vec<FP,VECTOR,1>(a, b, c, d, u, N, stride);

}

#elif FPPREC == 1

tridStatus_t tridDmtsvStridedBatch(const double *a, const double *b, const double *c, double *d, double* u, int ndim, int solvedim, int *dims, int *pads) {
  tridMultiDimBatchSolve(a, b, c, d, NULL, ndim, solvedim, dims, pads);
  return TRID_STATUS_SUCCESS;
}

void trid_scalarD(double* __restrict a, double* __restrict b, double* __restrict c, double* __restrict d, double* __restrict u, int N, int stride) {
  
  trid_scalar(a, b, c, d, u, N, stride);

}

void trid_x_transposeD(double* __restrict a, double* __restrict b, double* __restrict c, double* __restrict d, double* __restrict u, int sys_size, int sys_pad, int stride) {

  trid_x_transpose(a, b, c, d, u, sys_size, sys_pad, stride);

}

void trid_scalar_vecD(double* __restrict a, double* __restrict b, double* __restrict c, double* __restrict d, double* __restrict u, int N, int stride) {

  trid_scalar_vec<FP,VECTOR,0>(a, b, c, d, u, N, stride);

}

void trid_scalar_vecDInc(double* __restrict a, double* __restrict b, double* __restrict c, double* __restrict d, double* __restrict u, int N, int stride) {

  trid_scalar_vec<FP,VECTOR,1>(a, b, c, d, u, N, stride);

}
#endif






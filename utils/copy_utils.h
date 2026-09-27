#include <pthread.h>
#include <assert.h>
#include <string.h>

#ifndef _COPYUTILS_H_
#define _COPYUTILS_H_


#define memcpy1 memcpy


#ifdef __ARM__
#include <arm_neon.h>

#define memcpy1 memcpy_neon_aligned

void memcpy_neon_8bytes(uint8_t* region2, const uint8_t* region1, size_t length);
void memcpy_neon_16bytes(uint8_t* region2, const uint8_t* region1, size_t length);
void memcpy_neon_32bytes(uint8_t* region2, const uint8_t* region1, size_t length);

void memcpy_neon_aligned(void* dst, const void * src, size_t length);

// From https://stackoverflow.com/questions/34888683/arm-neon-memcpy-optimized-for-uncached-memory
// and https://stackoverflow.com/questions/61210517/memcpy-for-arm-uncached-memory-for-arm64
//#ifdef __ARM__
// void my_copy(volatile void *dst, volatile const void *src, int sz){
//     if (sz & 63) {
//         sz = (sz & -64) + 64;
//     }
//     asm volatile ("NEONCopyPLD: \n"
//                   "sub %[dst], %[dst], #64 \n"
//                   "1: \n"
//                   "ldnp q0, q1, [%[src]] \n"
//                   "ldnp q2, q3, [%[src], #32] \n"
//                   "add %[dst], %[dst], #64 \n"
//                   "subs %[sz], %[sz], #64 \n"
//                   "add %[src], %[src], #64 \n"
//                   "stnp q0, q1, [%[dst]] \n"
//                   "stnp q2, q3, [%[dst], #32] \n"
//                   "b.gt 1b \n"
//             : [dst]"+r"(dst), [src]"+r"(src), [sz]"+r"(sz) : : "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "cc", "memory");
// }
// https://wx.comake.online/doc/doc/SigmaStarDocs-SSC9341_Ispahan-ULS00V040-20210913/customer/faq/i6b0/system/i6b0/neon.html
// https://community.nxp.com/t5/i-MX-Processors/iMX6-EIM-transfer-speed-with-using-NEON-vld-vst-instructions/m-p/312256
void __attribute__ ((noinline)) memcpy_neon_pld(void *dest, const void *src, size_t n);
//#endif


extern "C"{
// The memcpymove-v7l.S impl
void *mempcpy(void * __restrict s1, const void * __restrict s2, size_t n);
// memcpy from arm repo
//void *__memcpy_aarch64(void * __restrict s1, const void * __restrict s2, size_t n);
//void *__memcpy_aarch64_simd(void * __restrict s1, const void * __restrict s2, size_t n);
//void *__memcpy_aarch64_sve(void * __restrict s1, const void * __restrict s2, size_t n);
//void *__memcpy_aarch64_sve (void *__restrict, const void *__restrict, size_t);
};
#endif

void simple_memcpy (char *dst, const char *src, size_t n);

struct memcpy_args_t {
    void* src;
    void* dst;
    int len;
};
void* memcpy_data_function(void* args_uncast);

void memcpy_threaded(void* dest,void* src, int len,int n_threads);

#endif
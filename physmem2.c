/* physmem2 -- threaded physical-memory pattern tester with pagemap reporting.
 *
 * Copyright (c) 2026 Rory Blair
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
/* v2: compiler barriers instead of volatile -- verify loops must really load,
 * but stay vectorized so memory bandwidth (the condition the fault needs) is preserved.
 * Adds a random-access pass to defeat prefetch and spread row activations. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#define BARRIER() __asm__ __volatile__("" ::: "memory")

static int pagemap_fd; static size_t PS;
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static long total_err = 0;

static uint64_t v2p(void *v){
    uint64_t val; off_t off = ((uintptr_t)v / PS) * 8;
    if (pread(pagemap_fd, &val, 8, off) != 8) return 0;
    if (!(val & (1ULL<<63))) return 0;
    return (val & ((1ULL<<55)-1)) * PS + ((uintptr_t)v % PS);
}
static void report(int id, uint64_t *p, uint64_t got, uint64_t exp, const char *ph){
    uint64_t phys = v2p(p), x = got ^ exp;
    pthread_mutex_lock(&lk);
    if (++total_err <= 300)
      printf("ERR t=%02d phase=%s paddr=0x%012lx exp=0x%016lx got=0x%016lx xor=0x%016lx nbits=%d\n",
             id, ph, phys, exp, got, x, __builtin_popcountll(x));
    fflush(stdout);
    pthread_mutex_unlock(&lk);
}
typedef struct { int id; size_t bytes; int passes; } arg_t;
static void *worker(void *a){
    arg_t *A=a; size_t n=A->bytes/8;
    uint64_t *b = mmap(NULL, A->bytes, PROT_READ|PROT_WRITE,
                       MAP_PRIVATE|MAP_ANONYMOUS|MAP_LOCKED|MAP_POPULATE, -1, 0);
    if (b==MAP_FAILED){ fprintf(stderr,"t%d mmap failed\n",A->id); return NULL; }
    uint64_t pats[]={0xAAAAAAAAAAAAAAAAULL,0x5555555555555555ULL,0xFFFFFFFFFFFFFFFFULL,
                     0x0ULL,0x0F0F0F0F0F0F0F0FULL,0xCCCCCCCCCCCCCCCCULL};
    uint64_t rs = 88172645463325252ULL + A->id*2654435761ULL;
    for(int pass=0; pass<A->passes; pass++){
      for(int k=0;k<6;k++){
        uint64_t p=pats[k], inv=~p;
        for(size_t i=0;i<n;i++) b[i]=p;            BARRIER();
        for(size_t i=0;i<n;i++) if(b[i]!=p) report(A->id,&b[i],b[i],p,"fill");
        BARRIER();
        for(size_t i=0;i<n;i++){ if(b[i]!=p) report(A->id,&b[i],b[i],p,"minv-up"); b[i]=inv; }
        BARRIER();
        for(size_t i=n;i-->0;){ if(b[i]!=inv) report(A->id,&b[i],b[i],inv,"minv-dn"); b[i]=p; }
        BARRIER();
        /* random-access pass: defeats prefetch, spreads row activations */
        for(size_t j=0;j<n/4;j++){
          rs^=rs<<13; rs^=rs>>7; rs^=rs<<17;
          size_t i = rs % n;
          if(b[i]!=p) report(A->id,&b[i],b[i],p,"rand");
        }
        BARRIER();
        for(size_t i=0;i<n;i++) if(b[i]!=p) report(A->id,&b[i],b[i],p,"final");
      }
    }
    munmap(b,A->bytes); return NULL;
}
int main(int argc,char**argv){
    int nt=argc>1?atoi(argv[1]):24; size_t mb=argc>2?atol(argv[2]):1024; int ps=argc>3?atoi(argv[3]):3;
    PS=sysconf(_SC_PAGESIZE);
    if((pagemap_fd=open("/proc/self/pagemap",O_RDONLY))<0){perror("pagemap");return 1;}
    printf("threads=%d bytes/thread=%zuMB passes=%d total=%zuGB\n",nt,mb,ps,(size_t)nt*mb/1024);
    fflush(stdout);
    pthread_t *th=calloc(nt,sizeof(*th)); arg_t *ar=calloc(nt,sizeof(*ar));
    for(int i=0;i<nt;i++){ar[i]=(arg_t){i,mb*1024*1024,ps};pthread_create(&th[i],NULL,worker,&ar[i]);}
    for(int i=0;i<nt;i++) pthread_join(th[i],NULL);
    printf("TOTAL ERRORS: %ld\n",total_err); return 0;
}

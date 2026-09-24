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
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#define BARRIER() __asm__ __volatile__("" ::: "memory")

static int pagemap_fd; static size_t PS;
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static long total_err = 0;
/* A thread that could not get its memory tested nothing. Counting that separately
 * is what keeps "no errors" from meaning "nothing ran" -- see the exit status below. */
static long failed_threads = 0;
/* Flips per data bit, over ALL errors (not just the 300 printed). One 64-bit word is one beat of
 * the module's DQ bus, so bit i is DQ i and bit/8 is the byte lane -- one x8 chip per rank.
 * Row = decoded rank 0-3; row 4 = no rank rule given, or the address could not be read. */
static long bit_err[5][64];
/* Address bits common to every error with a known address: the raw material for finding a
 * rank rule on a new bench (fail a known-bad chip, see which bits never change). */
static uint64_t pa_and = ~0ULL, pa_or = 0; static long pa_known = 0;

/* Rank decoding. The controller picks rank from the physical address, by a rule this program
 * cannot read, so the rule is supplied: "above" = count of ascending boundaries at or below the
 * address (rank interleaving off: rank 1 is the upper part), "mask" = rank bit i is the parity
 * of paddr & mask i (a calibrated bit, or the XOR hash controllers usually use). */
enum { RULE_NONE, RULE_SINGLE, RULE_ABOVE, RULE_MASK };
static int rule = RULE_NONE, rule_n = 0; static uint64_t rule_v[3];
static char ic[4][8][32]; static int have_layout = 0;

static int rank_of(uint64_t pa){
    if (rule == RULE_SINGLE) return 0;
    if (rule == RULE_NONE || !pa) return -1;
    int r = 0;
    for (int i = 0; i < rule_n; i++)
      if (rule == RULE_ABOVE) r += pa >= rule_v[i];
      else r |= __builtin_parityll(pa & rule_v[i]) << i;
    return r;
}
static int parse_rule(char *s){
    char *p = s, *e;
    rule_n = 0;
    if (!strcmp(s, "single")) { rule = RULE_SINGLE; return 0; }
    if (!strncmp(s, "above:", 6)) { rule = RULE_ABOVE; p += 6; }
    else if (!strncmp(s, "mask:", 5)) { rule = RULE_MASK; p += 5; }
    else return -1;
    int max = rule == RULE_ABOVE ? 3 : 2;       /* either way, at most 4 ranks */
    for (;;) {
      if (rule_n == max) return -1;
      rule_v[rule_n++] = strtoull(p, &e, 0);
      if (e == p || !rule_v[rule_n-1]) return -1;
      if (!*e) break;
      if (*e != ',') return -1;
      p = e + 1;
    }
    for (int i = 1; rule == RULE_ABOVE && i < rule_n; i++) if (rule_v[i] <= rule_v[i-1]) return -1;
    return 0;
}
/* Layout: "rank lane designator" per line, '#' comments. x16 parts list the same designator
 * on both of their lanes. Designators are the module maker's; nothing here assumes a raw card. */
static int load_layout(const char *path){
    FILE *f = fopen(path, "r"); char line[256]; int ln = 0, n = 0;
    if (!f) { perror(path); return -1; }
    while (fgets(line, sizeof line, f)) {
      int r, l; char d[32]; ln++;
      char *s = line; while (*s == ' ' || *s == '\t') s++;
      if (*s == '#' || *s == '\n' || !*s) continue;
      if (sscanf(s, "%d %d %31s", &r, &l, d) != 3 || r < 0 || r > 3 || l < 0 || l > 7) {
        fprintf(stderr, "%s:%d: expected \"rank(0-3) lane(0-7) designator\"\n", path, ln);
        fclose(f); return -1;
      }
      strcpy(ic[r][l], d); n++;
    }
    fclose(f); have_layout = 1; return n;
}

static uint64_t v2p(void *v){
    uint64_t val; off_t off = ((uintptr_t)v / PS) * 8;
    if (pread(pagemap_fd, &val, 8, off) != 8) return 0;
    if (!(val & (1ULL<<63))) return 0;
    return (val & ((1ULL<<55)-1)) * PS + ((uintptr_t)v % PS);
}
static void report(int id, uint64_t *p, uint64_t got, uint64_t exp, const char *ph){
    uint64_t phys = v2p(p), x = got ^ exp;
    int r = rank_of(phys);
    char dq[64*3+1] = "", ln[8*2+1] = "", ex[8*34+32] = ""; int nd = 0, nl = 0, ne = 0;
    for (int i = 0; i < 64; i++) if (x >> i & 1)
      nd += snprintf(dq + nd, sizeof dq - nd, "%s%d", nd ? "," : "", i);
    for (int l = 0; l < 8; l++) if (x >> (l*8) & 0xff)
      nl += snprintf(ln + nl, sizeof ln - nl, "%s%d", nl ? "," : "", l);
    if (rule != RULE_NONE) {
      ne += r < 0 ? snprintf(ex, sizeof ex, " rank=?") : snprintf(ex, sizeof ex, " rank=%d", r);
      for (int l = 0, k = 0; have_layout && l < 8; l++) if (x >> (l*8) & 0xff)
        ne += snprintf(ex + ne, sizeof ex - ne, "%s%s", k++ ? "," : " ic=",
                       r >= 0 && ic[r][l][0] ? ic[r][l] : "?");
    }
    pthread_mutex_lock(&lk);
    for (int i = 0; i < 64; i++) if (x >> i & 1) bit_err[r < 0 ? 4 : r][i]++;
    if (phys) { pa_and &= phys; pa_or |= phys; pa_known++; }
    if (++total_err <= 300)
      printf("ERR t=%02d phase=%s paddr=0x%012lx exp=0x%016lx got=0x%016lx xor=0x%016lx nbits=%d lane=%s dq=%s%s\n",
             id, ph, phys, exp, got, x, __builtin_popcountll(x), ln, dq, ex);
    fflush(stdout);
    pthread_mutex_unlock(&lk);
}
typedef struct { int id; size_t bytes; int passes; } arg_t;
static void *worker(void *a){
    arg_t *A=a; size_t n=A->bytes/8;
    uint64_t *b = mmap(NULL, A->bytes, PROT_READ|PROT_WRITE,
                       MAP_PRIVATE|MAP_ANONYMOUS|MAP_LOCKED|MAP_POPULATE, -1, 0);
    if (b==MAP_FAILED){
      fprintf(stderr,"t%d mmap failed: could not lock %zu bytes\n",A->id,A->bytes);
      pthread_mutex_lock(&lk); failed_threads++; pthread_mutex_unlock(&lk);
      return NULL;
    }
    uint64_t pats[]={0xAAAAAAAAAAAAAAAAULL,0x5555555555555555ULL,0xFFFFFFFFFFFFFFFFULL,
                     0x0ULL,0x0F0F0F0F0F0F0F0FULL,0xCCCCCCCCCCCCCCCCULL};
    uint64_t rs = 88172645463325252ULL + A->id*2654435761ULL;
    for(int pass=0; pass<A->passes; pass++){
      for(int k=0;k<6;k++){
        uint64_t p=pats[k], inv=~p;
        for(size_t i=0;i<n;i++) b[i]=p;
        BARRIER();
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
    int o; const char *layout=NULL;
    while((o=getopt(argc,argv,"r:l:"))!=-1){
      if(o=='r' && !parse_rule(optarg)) continue;
      if(o=='l'){ layout=optarg; continue; }
      fprintf(stderr,"usage: %s [-r single|above:ADDR[,ADDR,ADDR]|mask:M[,M]] [-l layout] [threads] [MB/thread] [passes]\n",argv[0]);
      return 2;
    }
    if(layout && rule==RULE_NONE){ fprintf(stderr,"-l needs -r: a designator is per rank\n"); return 2; }
    if(layout && load_layout(layout)<0) return 2;
    argc-=optind-1; argv+=optind-1;
    int nt=argc>1?atoi(argv[1]):24; size_t mb=argc>2?atol(argv[2]):1024; int ps=argc>3?atoi(argv[3]):3;
    PS=sysconf(_SC_PAGESIZE);
    if((pagemap_fd=open("/proc/self/pagemap",O_RDONLY))<0){perror("pagemap");return 2;}
    printf("threads=%d bytes/thread=%zuMB passes=%d total=%zuGB\n",nt,mb,ps,(size_t)nt*mb/1024);
    fflush(stdout);
    pthread_t *th=calloc(nt,sizeof(*th)); arg_t *ar=calloc(nt,sizeof(*ar));
    for(int i=0;i<nt;i++){ar[i]=(arg_t){i,mb*1024*1024,ps};pthread_create(&th[i],NULL,worker,&ar[i]);}
    for(int i=0;i<nt;i++) pthread_join(th[i],NULL);
    /* Per-lane summary before the total, so TOTAL ERRORS stays the last stdout line. */
    for(int r=0;r<5;r++) for(int l=0;l<8;l++){
      long s=0; for(int i=l*8;i<l*8+8;i++) s+=bit_err[r][i];
      if(!s) continue;
      printf("LANE %d",l);
      if(rule!=RULE_NONE){ if(r<4) printf(" RANK %d",r); else printf(" RANK ?"); }
      printf(" (DQ%d-%d)",l*8,l*8+7);
      if(have_layout) printf(" IC %s", r<4 && ic[r][l][0] ? ic[r][l] : "?");
      printf(": %ld bit flips:",s);
      for(int i=l*8;i<l*8+8;i++) if(bit_err[r][i]) printf(" DQ%d=%ld",i,bit_err[r][i]);
      printf("\n");
    }
    if(pa_known){
      uint64_t span = pa_or ? ~0ULL >> __builtin_clzll(pa_or) : 0;
      printf("PADDR bits set in every error: 0x%012lx  clear in every error: 0x%012lx  (%ld errors with a known address)\n",
             pa_and, ~pa_or & span, pa_known);
    }
    printf("TOTAL ERRORS: %ld\n",total_err);
    fflush(stdout);
    if(failed_threads){
      /* Incomplete beats clean: some memory was never tested, so 0 errors is not a verdict. */
      fprintf(stderr,"INCOMPLETE: %ld of %d thread(s) could not allocate; "
                     "the memory they would have tested was not tested\n",failed_threads,nt);
      return 2;
    }
    return total_err ? 1 : 0;
}

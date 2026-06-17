/* Offline validation of the DMRA "Late Entry MI" conveyance used by the stock TYT.
 * The 32-bit MI (+ CRC4) is Golay(24,12)-encoded and stuffed into ambe_fr[3][0..3]
 * (4 bits) of each of the 3 AMBE codewords, across voice frames vc=1..6.
 *
 * This test reproduces the ENCODE (TX side, ours) and then runs DSD-FME's EXACT
 * decode logic (dmr_le.c dmr_late_entry_mi) to prove the MI round-trips. Golay
 * matrices / encode / decode and crc4 are copied verbatim from dsd-fme so this is
 * ground-truth, not a re-derivation.
 *
 * The ENCODE under test is the REAL firmware dmr_le_mi_build() (linked from
 * application/source/crypto/dmr_aes.c), so this is a true regression on shipped code.
 *
 * Build: gcc -O2 -Iapplication/include -Iapplication/include/crypto -o /tmp/test_le_mi \
 *          tests/test_le_mi.c application/source/crypto/dmr_aes.c && /tmp/test_le_mi
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "dmr_aes.h"   /* real dmr_le_mi_build() under test */

/* ---- verbatim from dsd-fme src/fec.c ---- */
static const unsigned char G[24*12] = {
  1,0,0,0,0,0,0,0,0,0,0,0, 1,1,0,0,0,1,1,1,0,1,0,1,
  0,1,0,0,0,0,0,0,0,0,0,0, 0,1,1,0,0,0,1,1,1,0,1,1,
  0,0,1,0,0,0,0,0,0,0,0,0, 1,1,1,1,0,1,1,0,1,0,0,0,
  0,0,0,1,0,0,0,0,0,0,0,0, 0,1,1,1,1,0,1,1,0,1,0,0,
  0,0,0,0,1,0,0,0,0,0,0,0, 0,0,1,1,1,1,0,1,1,0,1,0,
  0,0,0,0,0,1,0,0,0,0,0,0, 1,1,0,1,1,0,0,1,1,0,0,1,
  0,0,0,0,0,0,1,0,0,0,0,0, 0,1,1,0,1,1,0,0,1,1,0,1,
  0,0,0,0,0,0,0,1,0,0,0,0, 0,0,1,1,0,1,1,0,0,1,1,1,
  0,0,0,0,0,0,0,0,1,0,0,0, 1,1,0,1,1,1,0,0,0,1,1,0,
  0,0,0,0,0,0,0,0,0,1,0,0, 1,0,1,0,1,0,0,1,0,1,1,1,
  0,0,0,0,0,0,0,0,0,0,1,0, 1,0,0,1,0,0,1,1,1,1,1,0,
  0,0,0,0,0,0,0,0,0,0,0,1, 1,0,0,0,1,1,1,0,1,0,1,1,
};
static const unsigned char H[24*12] = {
  1,0,1,0,0,1,0,0,1,1,1,1, 1,0,0,0,0,0,0,0,0,0,0,0,
  1,1,1,1,0,1,1,0,1,0,0,0, 0,1,0,0,0,0,0,0,0,0,0,0,
  0,1,1,1,1,0,1,1,0,1,0,0, 0,0,1,0,0,0,0,0,0,0,0,0,
  0,0,1,1,1,1,0,1,1,0,1,0, 0,0,0,1,0,0,0,0,0,0,0,0,
  0,0,0,1,1,1,1,0,1,1,0,1, 0,0,0,0,1,0,0,0,0,0,0,0,
  1,0,1,0,1,0,1,1,1,0,0,1, 0,0,0,0,0,1,0,0,0,0,0,0,
  1,1,1,1,0,0,0,1,0,0,1,1, 0,0,0,0,0,0,1,0,0,0,0,0,
  1,1,0,1,1,1,0,0,0,1,1,0, 0,0,0,0,0,0,0,1,0,0,0,0,
  0,1,1,0,1,1,1,0,0,0,1,1, 0,0,0,0,0,0,0,0,1,0,0,0,
  1,0,0,1,0,0,1,1,1,1,1,0, 0,0,0,0,0,0,0,0,0,1,0,0,
  0,1,0,0,1,0,0,1,1,1,1,1, 0,0,0,0,0,0,0,0,0,0,1,0,
  1,1,0,0,0,1,1,1,0,1,0,1, 0,0,0,0,0,0,0,0,0,0,0,1,
};
static int Golay_24_12_decode(unsigned char *rx){
  unsigned int syn=0; int is;
  for(is=0;is<12;is++){ int s=0,k; for(k=0;k<24;k++) s+=rx[k]*H[24*is+k]; syn+=(s%2)<<(11-is); }
  if(syn>0) return 0; /* error-free test: any syndrome = fail */
  return 1;
}
static uint8_t crc4(uint8_t bits[], unsigned int len){
  uint8_t crc=0,buf[256],poly[5]={1,0,0,1,1}; unsigned int K=4,i,j;
  if(len+K>sizeof(buf)) return 0; memset(buf,0,sizeof(buf));
  for(i=0;i<len;i++) buf[i]=bits[i];
  for(i=0;i<len;i++) if(buf[i]) for(j=0;j<K+1;j++) buf[i+j]^=poly[j];
  for(i=0;i<K;i++) crc=(crc<<1)+buf[len+i];
  return crc^0xF;
}
static uint64_t ConvertBitIntoBytes(uint8_t *b, uint32_t n){
  uint64_t v=0; uint32_t i; for(i=0;i<n;i++){ v<<=1; v|=b[i]&1; } return v;
}

/* ENCODE under test = the real firmware dmr_le_mi_build() (from dmr_aes.c). */

/* ---- DECODE: verbatim logic from dsd-fme dmr_le.c dmr_late_entry_mi ---- */
static int decode_le_mi(uint8_t frag[7][3], uint32_t *mi_out){
  uint64_t mi_test = ((uint64_t)frag[1][0]<<32)|((uint64_t)frag[2][0]<<28)|((uint64_t)frag[3][0]<<24)|
                     ((uint64_t)frag[1][1]<<20)|((uint64_t)frag[2][1]<<16)|((uint64_t)frag[3][1]<<12)|
                     ((uint64_t)frag[1][2]<<8 )|((uint64_t)frag[2][2]<<4 )|((uint64_t)frag[3][2]<<0);
  uint64_t go_test = ((uint64_t)frag[4][0]<<32)|((uint64_t)frag[5][0]<<28)|((uint64_t)frag[6][0]<<24)|
                     ((uint64_t)frag[4][1]<<20)|((uint64_t)frag[5][1]<<16)|((uint64_t)frag[6][1]<<12)|
                     ((uint64_t)frag[4][2]<<8 )|((uint64_t)frag[5][2]<<4 )|((uint64_t)frag[6][2]<<0);
  int g[3], i, j; uint8_t mi_go_bits[24], mi_bits[36];
  uint64_t mi_corrected=0, go_corrected=0;
  for(j=0;j<3;j++){
    for(i=0;i<12;i++){
      mi_go_bits[i]    = (( mi_test << (i+j*12) ) & 0x800000000ULL) >> 35;
      mi_go_bits[i+12] = (( go_test << (i+j*12) ) & 0x800000000ULL) >> 35;
    }
    g[j] = Golay_24_12_decode(mi_go_bits);
    for(i=0;i<12;i++){
      mi_corrected=(mi_corrected<<1)|mi_go_bits[i];
      go_corrected=(go_corrected<<1)|mi_go_bits[i+12];
      mi_bits[i+(j*12)]=mi_go_bits[i];
    }
  }
  uint32_t mi_final=(mi_corrected>>4)&0xFFFFFFFF;
  uint8_t crc_ext=(uint8_t)ConvertBitIntoBytes(&mi_bits[32],4);
  uint8_t crc_cmp=crc4(mi_bits,32);
  *mi_out=mi_final;
  return g[0]&&g[1]&&g[2] && (crc_ext==crc_cmp);
}

int main(void){
  uint32_t tests[]={0x174E6042u,0x00000000u,0xFFFFFFFFu,0xDEADBEEFu,0x12345678u,0xA5335998u,0x0BADF00Du};
  int n=sizeof(tests)/sizeof(tests[0]), fails=0;
  for(int t=0;t<n;t++){
    uint8_t frag[7][3]={{0}}; uint32_t got=0;
    dmr_le_mi_build(tests[t],frag);
    int ok=decode_le_mi(frag,&got);
    int pass=ok && got==tests[t];
    printf("MI %08X -> decode %08X  CRC/Golay %s  %s\n",
           tests[t], got, ok?"OK":"BAD", pass?"PASS":"FAIL");
    if(!pass) fails++;
  }
  /* pseudo-random sweep */
  uint32_t r=0x2545F491u;
  for(int t=0;t<200000;t++){
    r=r*1664525u+1013904223u;
    uint8_t frag[7][3]={{0}}; uint32_t got=0;
    dmr_le_mi_build(r,frag);
    if(!(decode_le_mi(frag,&got)&&got==r)) fails++;
  }
  printf("%s  (200007 vectors)\n", fails?"=== FAILURES ===":"ALL PASS");
  return fails?1:0;
}

/* Apple's documented bv4 LZ4 framing. No Apple implementation is used. */
#include <compression.h>
#include <lz4.h>
#include <limits.h>
#include <string.h>
static uint32_t get32(const uint8_t *p) { return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static void put32(uint8_t *p,uint32_t n) { for(int i=0;i<4;i++)p[i]=(uint8_t)(n>>(8*i)); }
size_t compression_encode_scratch_buffer_size(compression_algorithm a) { (void)a;return 1; }
size_t compression_decode_scratch_buffer_size(compression_algorithm a) { (void)a;return 1; }
size_t compression_encode_buffer(uint8_t *dst,size_t cap,const uint8_t *src,size_t n,void *scratch,compression_algorithm a) {
 (void)scratch;if(a!=COMPRESSION_LZ4||n>INT_MAX||cap<16||cap-16>INT_MAX)return 0;
 int compressed=LZ4_compress_default((const char *)src,(char *)dst+12,(int)n,(int)(cap-16));
 if(compressed<=0)return 0;
 memcpy(dst,"bv41",4);put32(dst+4,(uint32_t)n);put32(dst+8,(uint32_t)compressed);
 memcpy(dst+12+compressed,"bv4$",4);return (size_t)compressed+16;
}
size_t compression_decode_buffer(uint8_t *dst,size_t cap,const uint8_t *src,size_t n,void *scratch,compression_algorithm a) {
 (void)scratch;if(a!=COMPRESSION_LZ4)return 0;
 size_t at=0,out=0;
 while(n-at>=4) {
  const uint8_t *p=src+at;
  if(!memcmp(p,"bv4$",4))return at+4==n?out:0;
  int compressed=!memcmp(p,"bv41",4);
  size_t header=compressed?12:8;
  if((!compressed&&memcmp(p,"bv4-",4))||n-at<header)return 0;
  uint32_t raw=get32(p+4),packed=compressed?get32(p+8):raw;
  if(raw>cap-out||raw>INT_MAX||packed>INT_MAX||packed>n-at-header)return 0;
  if(compressed) {
   size_t dictionary=out<65536?out:65536;
   int got=LZ4_decompress_safe_usingDict((const char *)p+header,(char *)dst+out,(int)packed,(int)raw,(const char *)dst+out-dictionary,(int)dictionary);
   if(got!=(int)raw)return 0;
  } else memcpy(dst+out,p+header,raw);
  out+=raw;at+=header+packed;
 }
 return 0;
}

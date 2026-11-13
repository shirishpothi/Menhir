#include <compression.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __APPLE__
#include <dlfcn.h>
#endif
int main(void) {
 size_t n=4u<<20;uint8_t *raw=malloc(n),*packed=malloc(n+65536),*out=malloc(n);
 if(!raw||!packed||!out)return 2;
 for(size_t i=0;i<n;i++)raw[i]=(uint8_t)((i/37)^i);
 size_t got=compression_encode_buffer(packed,n+65536,raw,n,NULL,COMPRESSION_LZ4);
 if(!got||compression_decode_buffer(out,n,packed,got,NULL,COMPRESSION_LZ4)!=n||memcmp(raw,out,n))return 1;
 if(compression_decode_buffer(out,n,packed,got-1,NULL,COMPRESSION_LZ4))return 1;
 printf("bv4 LZ4 round trip: %zu -> %zu bytes; truncated input rejected\n",n,got);
#ifdef __APPLE__
 void *lib=dlopen("/usr/lib/libcompression.dylib",RTLD_NOW|RTLD_LOCAL);if(!lib)return 1;
 typedef size_t (*Codec)(uint8_t *,size_t,const uint8_t *,size_t,void *,compression_algorithm);
 Codec encode=(Codec)dlsym(lib,"compression_encode_buffer"),decode=(Codec)dlsym(lib,"compression_decode_buffer");
 if(!encode||!decode||decode(out,n,packed,got,NULL,COMPRESSION_LZ4)!=n||memcmp(raw,out,n))return 1;
 got=encode(packed,n+65536,raw,n,NULL,COMPRESSION_LZ4);
 if(!got||compression_decode_buffer(out,n,packed,got,NULL,COMPRESSION_LZ4)!=n||memcmp(raw,out,n))return 1;
 printf("Apple libcompression interoperability: PASS in both directions\n");dlclose(lib);
#endif
 free(raw);free(packed);free(out);return 0;
}

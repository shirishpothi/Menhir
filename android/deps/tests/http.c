/* Bounded Apple CDN transfer: abort at 1 MiB, resume a 4 MiB range, compare
 * every byte with a fresh range. Also verify signing-server TLS and fail closed
 * with an empty trust directory. No IPSW file is written. */
#include "menhir_fetch.h"
#include <curl/curl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LIMIT (4u<<20)
typedef struct { unsigned char *data;size_t used,stop; } Data;
static size_t receive(char *p,size_t s,size_t n,void *v) {
 Data *d=v;size_t bytes=s*n;
 if(bytes>LIMIT-d->used)return 0;
 if(d->stop && d->used+bytes>d->stop)return 0;
 memcpy(d->data+d->used,p,bytes);d->used+=bytes;return bytes;
}
void menhir_curl_trust(CURL *c);
static CURLcode get(const char *url,const char *range,Data *d,const char *ca,long *status) {
 CURL *c=curl_easy_init();char error[CURL_ERROR_SIZE]={0};curl_easy_setopt(c,CURLOPT_ERRORBUFFER,error);
 if(strcmp(ca,"/data/local/tmp/menhir-empty-ca"))menhir_curl_trust(c);
 curl_easy_setopt(c,CURLOPT_URL,url);
 curl_easy_setopt(c,CURLOPT_PROTOCOLS_STR,"https");
 curl_easy_setopt(c,CURLOPT_SSL_VERIFYPEER,1L);curl_easy_setopt(c,CURLOPT_SSL_VERIFYHOST,2L);
 curl_easy_setopt(c,CURLOPT_CAPATH,ca);
 if(!strcmp(ca,"/data/local/tmp/menhir-empty-ca"))curl_easy_setopt(c,CURLOPT_CAINFO,NULL);
 curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,20L);curl_easy_setopt(c,CURLOPT_TIMEOUT,120L);
 if(range)curl_easy_setopt(c,CURLOPT_RANGE,range);
 curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,receive);curl_easy_setopt(c,CURLOPT_WRITEDATA,d);
 CURLcode r=curl_easy_perform(c);curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,status);curl_easy_cleanup(c);if(r)fprintf(stderr,"curl: %s\n",error);return r;
}
int main(int argc,char **argv) {
 const char *ca=argc>1?argv[1]:"/apex/com.android.conscrypt/cacerts";
 curl_global_init(CURL_GLOBAL_DEFAULT);
 Data a={calloc(1,LIMIT),0,1u<<20},b={calloc(1,LIMIT),0,0};long status=0;
 if(!a.data||!b.data)return 2;
 CURLcode r=get("https://gs.apple.com/",NULL,&b,ca,&status);
 printf("Signing server verified TLS: curl=%d HTTP=%ld\n",r,status);if(r)return 1;
 b.used=0;r=get("https://gs.apple.com/",NULL,&b,"/data/local/tmp/menhir-empty-ca",&status);
 printf("Empty trust rejected: curl=%d\n",r);if(r==CURLE_OK)return 1;
 const MenhirRelease *catalog=menhir_releases(NULL);const char *url=catalog[1].url;
 r=get(url,"0-4194303",&a,ca,&status);
 printf("Interrupted range: curl=%d HTTP=%ld saved=%zu\n",r,status,a.used);
 if(r!=CURLE_WRITE_ERROR||status!=206||!a.used)return 1;
 char range[80];snprintf(range,sizeof range,"%zu-4194303",a.used);a.stop=0;
 r=get(url,range,&a,ca,&status);
 printf("Resumed range: curl=%d HTTP=%ld bytes=%zu\n",r,status,a.used);
 if(r||status!=206||a.used!=LIMIT)return 1;
 b.used=0;r=get(url,"0-4194303",&b,ca,&status);
 int same=!r&&status==206&&b.used==LIMIT&&!memcmp(a.data,b.data,LIMIT);
 printf("Fresh range comparison: %s\n",same?"PASS":"FAIL");
 free(a.data);free(b.data);return same?0:1;
}

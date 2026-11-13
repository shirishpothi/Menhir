/* Native Apple downloads. No Apple payload is distributed with Menhir. */
#include "menhir_fetch.h"
#include <curl/curl.h>
#include <mbedtls/sha256.h>
#include <lzfse.h>
#include <lzvn_decode_base.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <inttypes.h>

struct MenhirFetch {
    pthread_t thread;
    pthread_mutex_t lock;
    atomic_int cancel;
    MenhirFetchProgress progress;
    char *url, *path, *ca;
    int rom;
    uint64_t base;
};
static const MenhirRelease releases[] = {
    {"22G120", "macOS Ventura 13.6", "https://updates.cdn-apple.com/2023FallFCS/fullrestores/042-55833/C0830847-A2F8-458F-B680-967991820931/UniversalMac_13.6_22G120_Restore.ipsw", UINT64_C(12297) << 20},
    {"23G93", "macOS Sonoma 14.6.1", "https://updates.cdn-apple.com/2024SummerFCS/fullrestores/062-52859/932E0A8F-6644-4759-82DA-F8FA8DEA806A/UniversalMac_14.6.1_23G93_Restore.ipsw", UINT64_C(14106) << 20},
    {"24G90", "macOS Sequoia 15.6.1", "https://updates.cdn-apple.com/2025SummerFCS/fullrestores/093-10809/CFD6DD38-DAF0-40DA-854F-31AAD1294C6F/UniversalMac_15.6.1_24G90_Restore.ipsw", UINT64_C(16035) << 20},
    {"25G83", "macOS Tahoe 26.6.2", "https://updates.cdn-apple.com/2026SummerFCS/fullrestores/140-75212/A2A24B94-1FC1-45A3-93F7-C51B02AF1F4D/UniversalMac_26.6.2_25G83_Restore.ipsw", UINT64_C(18856) << 20},
    {"26B123", "macOS 27.2", "https://updates.cdn-apple.com/2026FallFCS/c9671975-52db-4fb5-9af2-00217169ba8d/UniversalMac_27.2_26B123_Restore.ipsw", UINT64_C(24409) << 20},
};
const MenhirRelease *menhir_releases(size_t *count) { if(count) *count=sizeof releases/sizeof *releases; return releases; }
static pthread_once_t curl_once = PTHREAD_ONCE_INIT;
static void init_curl(void) { curl_global_init(CURL_GLOBAL_DEFAULT); }
static int progress(void *p,curl_off_t total,curl_off_t now,curl_off_t up,curl_off_t sent) {
    (void)up;(void)sent;
    MenhirFetch *j=p;
    pthread_mutex_lock(&j->lock);
    j->progress.downloaded=j->base+(uint64_t)now;
    j->progress.total=total>0?j->base+(uint64_t)total:0;
    pthread_mutex_unlock(&j->lock);
    return atomic_load(&j->cancel);
}
void menhir_curl_trust(CURL *c);
static CURL *easy(MenhirFetch *j) {
    pthread_once(&curl_once,init_curl);
    CURL *c=curl_easy_init();
    if(!c) return NULL;
    menhir_curl_trust(c);
    curl_easy_setopt(c,CURLOPT_URL,j->url);
    curl_easy_setopt(c,CURLOPT_PROTOCOLS_STR,"https");
    curl_easy_setopt(c,CURLOPT_REDIR_PROTOCOLS_STR,"https");
    curl_easy_setopt(c,CURLOPT_FOLLOWLOCATION,1L);
    curl_easy_setopt(c,CURLOPT_MAXREDIRS,5L);
    curl_easy_setopt(c,CURLOPT_SSL_VERIFYPEER,1L);
    curl_easy_setopt(c,CURLOPT_SSL_VERIFYHOST,2L);
    if(j->ca) curl_easy_setopt(c,CURLOPT_CAPATH,j->ca);
#ifdef __ANDROID__
    else curl_easy_setopt(c,CURLOPT_CAPATH,access("/apex/com.android.conscrypt/cacerts",R_OK)==0?
                              "/apex/com.android.conscrypt/cacerts":"/system/etc/security/cacerts");
#endif
    curl_easy_setopt(c,CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,30L);
    curl_easy_setopt(c,CURLOPT_LOW_SPEED_LIMIT,1L);
    curl_easy_setopt(c,CURLOPT_LOW_SPEED_TIME,120L);
    curl_easy_setopt(c,CURLOPT_XFERINFOFUNCTION,progress);
    curl_easy_setopt(c,CURLOPT_XFERINFODATA,j);
    curl_easy_setopt(c,CURLOPT_NOPROGRESS,0L);
    return c;
}
#define ROM_SIZE 214904
#define BLOCK_SIZE 859848
static const unsigned char digest[32]={0xfd,0x49,0x73,0xe4,0x2e,0x60,0x97,0x51,0x83,0x0d,0xf6,0xf3,0xdd,0x2d,0x2b,0x2a,0xd2,0x30,0x9f,0xe3,0x8f,0x0c,0xa0,0xf5,0xab,0xa0,0x2d,0xcb,0xef,0xe4,0x5f,0x91};
static int genuine(const unsigned char *p,size_t n) {
    unsigned char hash[32];
    return n==ROM_SIZE && mbedtls_sha256(p,n,hash,0)==0 && !memcmp(hash,digest,32);
}
typedef struct { unsigned char *data; size_t used,cap; uint64_t start,end,total; int valid_range; long status; } Buffer;
static size_t buffer_write(char *p,size_t size,size_t n,void *opaque) {
    Buffer *b=opaque;
    if(size && n>SIZE_MAX/size) return 0;
    size_t len=size*n;
    if(len>b->cap-b->used) return 0;
    memcpy(b->data+b->used,p,len); b->used+=len; return len;
}
static size_t range_header(char *p,size_t size,size_t n,void *opaque) {
    Buffer *b=opaque; size_t len=size*n;
    if(len>=5 && !strncasecmp(p,"HTTP/",5)) {
        char line[160];size_t k=len<sizeof line-1?len:sizeof line-1;
        memcpy(line,p,k);line[k]=0;b->valid_range=0;b->status=0;
        sscanf(line,"%*s %ld",&b->status);
    }
    if(len>14 && !strncasecmp(p,"Content-Range:",14)) {
        char s[160]; size_t k=len<sizeof(s)-1?len:sizeof(s)-1; memcpy(s,p,k);s[k]=0;
        b->valid_range=sscanf(s+14," bytes %"SCNu64"-%"SCNu64"/%"SCNu64,&b->start,&b->end,&b->total)==3;
    }
    return len;
}
static uint32_t le32(const unsigned char *p) { return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static int unpack(Buffer *b,unsigned char *rom) {
    unsigned char *whole=malloc(1048576);
    if(!whole) return -1;
    int result=-1;
    if(lzfse_decode_buffer(whole,1048576,b->data,b->used,NULL)!=1048576) goto done;
    const unsigned char *fork=whole+876544;
    uint32_t table=le32(fork); size_t out=0;
    if(table<8 || table%4 || table>116335) goto done;
    for(uint32_t i=0;i<table/4-1;i++) {
        uint32_t start=le32(fork+i*4),end=le32(fork+(i+1)*4);
        if(start<table || start>=end || end>116335 || out>=ROM_SIZE) goto done;
        size_t want=ROM_SIZE-out; if(want>65536) want=65536;
        if(fork[start]==6) {
            if(end-start-1!=want) goto done;
            memcpy(rom+out,fork+start+1,want);
        } else {
            lzvn_decoder_state s={0};
            s.src=fork+start; s.src_end=fork+end;
            s.dst=s.dst_begin=rom+out; s.dst_end=rom+out+want;
            lzvn_decode(&s);
            if(s.dst!=rom+out+want || !s.end_of_stream) goto done;
        }
        out+=want;
    }
    result=genuine(rom,out)?0:-1;
done: free(whole); return result;
}
static int fetch_rom(MenhirFetch *j,const char *part,char *err,size_t cap) {
    unsigned char *rom=malloc(ROM_SIZE); if(!rom) return -1;
    FILE *f=fopen(j->path,"rb");
    if(f) { size_t n=fread(rom,1,ROM_SIZE,f); int extra=fgetc(f); fclose(f); if(extra==EOF && genuine(rom,n)) { free(rom); return 0; } }
    Buffer b={.data=malloc(BLOCK_SIZE),.cap=BLOCK_SIZE};
    CURL *c=easy(j); int rc=-1;
    if(!b.data||!c) goto done;
    curl_easy_setopt(c,CURLOPT_RANGE,"5349700747-5350560594");
    curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,buffer_write);
    curl_easy_setopt(c,CURLOPT_WRITEDATA,&b);
    curl_easy_setopt(c,CURLOPT_HEADERFUNCTION,range_header);
    curl_easy_setopt(c,CURLOPT_HEADERDATA,&b);
    CURLcode r=curl_easy_perform(c); long status=0;curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);
    if(r || status!=206 || !b.valid_range || b.start!=5349700747ULL || b.end!=5350560594ULL || b.used!=BLOCK_SIZE) {
        snprintf(err,cap,"ROM range: HTTP %ld, %s, %zu bytes",status,curl_easy_strerror(r),b.used); goto done;
    }
    if(unpack(&b,rom)) { snprintf(err,cap,"ROM decompression or SHA-256 verification failed");goto done; }
    f=fopen(part,"wb"); if(!f) goto done;
    int ok=fwrite(rom,1,ROM_SIZE,f)==ROM_SIZE && fflush(f)==0 && fsync(fileno(f))==0;
    if(fclose(f)) ok=0;
    if(ok && !atomic_load(&j->cancel)) rc=rename(part,j->path);
done:
    if(c) curl_easy_cleanup(c);
    free(b.data);free(rom);return rc;
}
typedef struct { FILE *file;Buffer *headers;uint64_t offset; } ImageSink;
static size_t file_write(char *p,size_t s,size_t n,void *opaque) {
    ImageSink *sink=opaque;Buffer *h=sink->headers;
    /* Reject a wrong range before it can poison a resumable partial file. */
    if(h->status==206) {
        if(!h->valid_range || h->start!=sink->offset)return 0;
    } else if(h->status!=200 || sink->offset)return 0;
    return fwrite(p,1,s*n,sink->file);
}
static int fetch_image(MenhirFetch *j,const char *part,char *err,size_t cap) {
    struct stat cached;
    if(stat(j->path,&cached)==0 && S_ISREG(cached.st_mode) && cached.st_size>0) {
        pthread_mutex_lock(&j->lock);
        j->progress.downloaded=j->progress.total=(uint64_t)cached.st_size;
        pthread_mutex_unlock(&j->lock);
        return 0;
    }
    FILE *f=fopen(part,"ab+"); if(!f) return -1;
    int rc=-1;
    for(int attempt=0;attempt<2;attempt++) {
        if(fseeko(f,0,SEEK_END)) break;
        off_t end=ftello(f); if(end<0) break;
        j->base=(uint64_t)end;
        CURL *c=easy(j); if(!c) break;
        Buffer headers={0};
        ImageSink sink={f,&headers,(uint64_t)end};
        curl_easy_setopt(c,CURLOPT_RESUME_FROM_LARGE,(curl_off_t)end);
        curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,file_write);
        curl_easy_setopt(c,CURLOPT_WRITEDATA,&sink);
        curl_easy_setopt(c,CURLOPT_HEADERFUNCTION,range_header);
        curl_easy_setopt(c,CURLOPT_HEADERDATA,&headers);
        curl_easy_setopt(c,CURLOPT_FAILONERROR,1L);
        CURLcode r=curl_easy_perform(c); long status=0;curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);
        curl_easy_cleanup(c);
        if(end>0 && status==200 && r==CURLE_RANGE_ERROR && !atomic_load(&j->cancel)) {
            if(fflush(f) || ftruncate(fileno(f),0)) break;
            continue;
        }
        if(r || (status!=200 && status!=206) || (status==206 && (!headers.valid_range || headers.start!=(uint64_t)end))) {
            snprintf(err,cap,"IPSW: HTTP %ld, %s",status,curl_easy_strerror(r));break;
        }
        if(fflush(f)==0 && fsync(fileno(f))==0) rc=0;
        break;
    }
    if(fclose(f)) rc=-1;
    if(!rc && !atomic_load(&j->cancel)) rc=rename(part,j->path);
    return rc;
}
static void *worker(void *opaque) {
    MenhirFetch *j=opaque;char err[256]="Download or file operation failed";
    char *part=malloc(strlen(j->path)+6);int rc=-1;
    if(part) { sprintf(part,"%s.part",j->path);rc=j->rom?fetch_rom(j,part,err,sizeof err):fetch_image(j,part,err,sizeof err);free(part); }
    pthread_mutex_lock(&j->lock);
    j->progress.state=atomic_load(&j->cancel)?MENHIR_FETCH_CANCELLED:rc?MENHIR_FETCH_FAILED:MENHIR_FETCH_DONE;
    if(rc) snprintf(j->progress.error,sizeof j->progress.error,"%s",err);
    pthread_mutex_unlock(&j->lock);return NULL;
}
static MenhirFetch *start(const char *url,const char *path,const char *ca,int rom) {
    if(!url||strncmp(url,"https://",8)||!path||!*path) return NULL;
    MenhirFetch *j=calloc(1,sizeof *j);if(!j)return NULL;
    j->url=strdup(url);j->path=strdup(path);j->ca=ca?strdup(ca):NULL;j->rom=rom;
    atomic_init(&j->cancel,0);
    if(!j->url||!j->path||(ca&&!j->ca)||pthread_mutex_init(&j->lock,NULL)) { free(j->url);free(j->path);free(j->ca);free(j);return NULL; }
    if(pthread_create(&j->thread,NULL,worker,j)) { pthread_mutex_destroy(&j->lock);free(j->url);free(j->path);free(j->ca);free(j);return NULL; }
    return j;
}
MenhirFetch *menhir_fetch_rom(const char *path,const char *ca) { return start(releases[1].url,path,ca,1); }
MenhirFetch *menhir_fetch_ipsw(const char *url,const char *path,const char *ca) { return start(url,path,ca,0); }
void menhir_fetch_poll(MenhirFetch *j,MenhirFetchProgress *out) { if(!j||!out)return;pthread_mutex_lock(&j->lock);*out=j->progress;pthread_mutex_unlock(&j->lock); }
void menhir_fetch_cancel(MenhirFetch *j) { if(j)atomic_store(&j->cancel,1); }
void menhir_fetch_free(MenhirFetch *j) { if(!j)return;menhir_fetch_cancel(j);pthread_join(j->thread,NULL);pthread_mutex_destroy(&j->lock);free(j->url);free(j->path);free(j->ca);free(j); }

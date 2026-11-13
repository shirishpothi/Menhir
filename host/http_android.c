/* Verified native curl, with Apple's signing root scoped to its TSS hosts. */
#include <curl/curl.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include "../android/deps/certs/apple_root.h"
static pthread_once_t once=PTHREAD_ONCE_INIT;
static mbedtls_x509_crt apple;
static int root_ok;
static void init(void) {
 curl_global_init(CURL_GLOBAL_DEFAULT);
 mbedtls_x509_crt_init(&apple);
 root_ok=mbedtls_x509_crt_parse_der(&apple,apple_root_der,sizeof apple_root_der)==0;
}
static CURLcode signing_root(CURL *curl,void *ctx,void *opaque) {
 (void)opaque;char *url=NULL,*host=NULL;CURLcode rc=CURLE_OK;
 curl_easy_getinfo(curl,CURLINFO_EFFECTIVE_URL,&url);
 CURLU *u=curl_url();if(!u)return CURLE_OUT_OF_MEMORY;
 if(url && !curl_url_set(u,CURLUPART_URL,url,0) && !curl_url_get(u,CURLUPART_HOST,&host,0)) {
  if(!strcmp(host,"gs.apple.com") || !strcmp(host,"gs.apple.com.akadns.net") || !strcmp(host,"gs.apple.com.v.aaplimg.com")) {
   if(!root_ok)rc=CURLE_SSL_CACERT_BADFILE;
   else mbedtls_ssl_conf_ca_chain(ctx,&apple,NULL);
  }
 }
 curl_free(host);curl_url_cleanup(u);return rc;
}
/* Internal helper shared by fetch jobs and command-line tests. */
void menhir_curl_trust(CURL *c) {
 pthread_once(&once,init);
 curl_easy_setopt(c,CURLOPT_SSL_VERIFYPEER,1L);
 curl_easy_setopt(c,CURLOPT_SSL_VERIFYHOST,2L);
 curl_easy_setopt(c,CURLOPT_PROTOCOLS_STR,"https");
 curl_easy_setopt(c,CURLOPT_REDIR_PROTOCOLS_STR,"https");
 curl_easy_setopt(c,CURLOPT_SSL_CTX_FUNCTION,signing_root);
#ifdef __ANDROID__
 curl_easy_setopt(c,CURLOPT_CAPATH,access("/apex/com.android.conscrypt/cacerts",R_OK)==0?
                     "/apex/com.android.conscrypt/cacerts":"/system/etc/security/cacerts");
#endif
}
CURL *menhir_restore_curl_init(void) {
 pthread_once(&once,init);
 CURL *c=curl_easy_init();if(c)menhir_curl_trust(c);return c;
}

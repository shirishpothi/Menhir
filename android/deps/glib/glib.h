/* Menhir's small POSIX implementation of the GLib surface used by libslirp.
 * GPL-2.0-or-later. Process forwarding is deliberately unavailable. */
#ifndef MENHIR_GLIB_H
#define MENHIR_GLIB_H
#include <assert.h>
#include <stddef.h>
#include <signal.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#ifndef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#endif
#define G_OS_UNIX 1
#define GLIB_CHECK_VERSION(a,b,c) 1
#define GLIB_SIZEOF_VOID_P 8
#define G_BIG_ENDIAN 4321
#define G_LITTLE_ENDIAN 1234
#define G_BYTE_ORDER G_LITTLE_ENDIAN
#define G_N_ELEMENTS(a) (sizeof(a)/sizeof((a)[0]))
#define G_STATIC_ASSERT(x) _Static_assert(x, #x)
#define G_GNUC_PRINTF(a,b) __attribute__((format(printf,a,b)))
#define G_GNUC_UNUSED __attribute__((unused))
#define G_LIKELY(x) (x)
#define G_UNLIKELY(x) (x)
#define TRUE 1
#define FALSE 0
#define g_assert assert
#define g_assert_not_reached() abort()
#define g_warn_if_reached() g_warning("unreachable")
#define g_warn_if_fail(x) do { if (!(x)) g_warning("%s", #x); } while(0)
#define g_return_if_fail(x) do { if (!(x)) return; } while(0)
#define g_return_val_if_fail(x,v) do { if (!(x)) return (v); } while(0)
#define GUINT16_FROM_BE ntohs
#define GUINT16_TO_BE htons
#define GUINT32_FROM_BE ntohl
#define GUINT32_TO_BE htonl
#define GINT16_FROM_BE(x) ((int16_t)ntohs(x))
#define GINT16_TO_BE(x) ((int16_t)htons(x))
#define GINT32_FROM_BE(x) ((int32_t)ntohl(x))
#define GINT32_TO_BE(x) ((int32_t)htonl(x))
typedef int gboolean;
typedef int gint;
typedef char gchar;
typedef void *gpointer;
typedef char **GStrv;
typedef int GPid;
typedef int GSpawnFlags;
typedef void (*GSpawnChildSetupFunc)(void *);
typedef struct { char *message; } GError;
typedef struct { const char *key; unsigned value; } GDebugKey;
typedef struct { int unused; } GRand;
typedef struct { char *str; size_t len; } GString;
#define G_SPAWN_SEARCH_PATH 1
static inline void *g_malloc(size_t n) { void *p=malloc(n?n:1); if(!p) abort(); return p; }
static inline void *g_malloc0(size_t n) { void *p=calloc(1,n?n:1); if(!p) abort(); return p; }
static inline void *g_realloc(void *p,size_t n) { p=realloc(p,n?n:1); if(!p) abort(); return p; }
#define g_new(t,n) ((t*)g_malloc(sizeof(t)*(n)))
#define g_new0(t,n) ((t*)g_malloc0(sizeof(t)*(n)))
#define g_free free
static inline char *g_strdup(const char *s) { return s?strdup(s):NULL; }
#define g_getenv getenv
#define g_strerror strerror
#define g_ascii_strcasecmp strcasecmp
#define g_snprintf snprintf
#define g_vsnprintf vsnprintf
static inline void g_warning(const char *fmt,...) { va_list a; va_start(a,fmt); vfprintf(stderr,fmt,a); fputc('\n',stderr); va_end(a); }
#define g_debug(...) ((void)0)
#define g_critical g_warning
#define g_error(...) do { g_warning(__VA_ARGS__); abort(); } while(0)
static inline size_t g_strlcpy(char *d,const char *s,size_t n) { size_t l=strlen(s); if(n) { size_t c=l<n-1?l:n-1; memcpy(d,s,c); d[c]=0; } return l; }
static inline bool g_str_has_prefix(const char *s,const char *p) { return !strncmp(s,p,strlen(p)); }
static inline char *g_strstr_len(const char *s,ssize_t n,const char *p) { if(n<0) return strstr(s,p); size_t l=strlen(p); for(size_t i=0;i+l<=(size_t)n && s[i];i++) if(!memcmp(s+i,p,l)) return (char*)s+i; return NULL; }
static inline unsigned g_strv_length(char **v) { unsigned n=0; while(v&&v[n]) n++; return n; }
static inline void g_strfreev(char **v) { if(v) { for(int i=0;v[i];i++) free(v[i]); free(v); } }
static inline GRand *g_rand_new(void) { return g_new0(GRand,1); }
#define g_rand_free free
static inline int32_t g_rand_int_range(GRand *r,int32_t a,int32_t b) { (void)r; return a+(int32_t)arc4random_uniform((uint32_t)(b-a)); }
static inline unsigned g_parse_debug_string(const char *s,const GDebugKey *k,unsigned n) { unsigned v=0; for(unsigned i=0;i<n;i++) if(strstr(s,k[i].key)||!strcmp(s,"all")) v|=k[i].value; return v; }
static inline GString *g_string_new(const char *s) { GString *v=g_new0(GString,1); v->str=strdup(s?s:""); v->len=strlen(v->str); return v; }
static inline void g_string_append_printf(GString *s,const char *fmt,...) { va_list a,b; va_start(a,fmt); va_copy(b,a); int n=vsnprintf(NULL,0,fmt,b); va_end(b); if(n>=0) { s->str=g_realloc(s->str,s->len+n+1); vsnprintf(s->str+s->len,n+1,fmt,a); s->len+=n; } va_end(a); }
static inline char *g_string_free(GString *s,int release) { char *p=s->str; if(release) { free(p); p=NULL; } free(s); return p; }
static inline void g_error_free(GError *e) { if(e) { free(e->message); free(e); } }
static inline gboolean g_shell_parse_argv(const char *s,int *n,char ***v,GError **e) { (void)s;(void)n;(void)v; if(e) { *e=g_new0(GError,1); (*e)->message=strdup("Menhir disables process forwarding"); } return FALSE; }
static inline gboolean g_spawn_async_with_fds(const char *w,char **v,char **env,GSpawnFlags f,GSpawnChildSetupFunc cb,void *u,GPid *p,int in,int out,int err,GError **e) { (void)w;(void)v;(void)env;(void)f;(void)cb;(void)u;(void)p;(void)in;(void)out;(void)err;(void)e; return FALSE; }
#endif

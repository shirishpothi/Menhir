#ifndef MENHIR_FETCH_H
#define MENHIR_FETCH_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* A job owns one worker. Poll from any thread; free joins the worker.
 * Paths must be in app-private storage. Use ~/.menhir/cache on desktop.
 * Only one job may write a given destination at a time. */
typedef struct MenhirFetch MenhirFetch;
typedef enum { MENHIR_FETCH_RUNNING, MENHIR_FETCH_DONE, MENHIR_FETCH_FAILED,
               MENHIR_FETCH_CANCELLED } MenhirFetchState;
typedef struct {
    MenhirFetchState state;
    uint64_t downloaded, total; /* total is zero until known */
    char error[256];
} MenhirFetchProgress;
typedef struct {
    const char *build, *name, *url;
    uint64_t approximate_bytes;
} MenhirRelease;
/* Borrowed static catalogue, copied from upstream Installer.swift. */
const MenhirRelease *menhir_releases(size_t *count);
/* Return NULL on invalid arguments/allocation/thread failure. Destination must
 * have an existing parent. Writes destination.part, fsyncs, then renames.
 * Each IPSW destination must identify one immutable URL (use the build ID).
 * IPSW reuses a completed file or resumes its .part; it has no upstream hash.
 * ROM verifies SHA-256 even for cached files.
 * ca_directory: NULL uses Android system roots; desktop callers supply a CA
 * directory (or build curl with a default bundle). HTTPS is mandatory. */
MenhirFetch *menhir_fetch_rom(const char *destination, const char *ca_directory);
MenhirFetch *menhir_fetch_ipsw(const char *https_url, const char *destination,
                              const char *ca_directory);
void menhir_fetch_poll(MenhirFetch *job, MenhirFetchProgress *out);
void menhir_fetch_cancel(MenhirFetch *job);
/* Cancels unfinished work, joins, then frees. Partial IPSWs remain resumable. */
void menhir_fetch_free(MenhirFetch *job);
#ifdef __cplusplus
}
#endif
#endif

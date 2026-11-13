/* A pretend host/menhir_fetch.h, for the fake-engine build (CMake: MENHIR_FAKE_FETCH). It downloads nothing.
   A boot ROM job writes a small placeholder file; an IPSW job counts up to the release's approximate size
   in about ten seconds, keeps how far it got in destination.part so that a cancelled job goes on from
   there next time (as the real one resumes), then writes a small placeholder and renames it into place.
   The real implementation is the services agent's, from menhir::http. */
#include "menhir_fetch.h"

#include <android/log.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "menhir-fake", __VA_ARGS__)

struct MenhirFetch {
    pthread_t thread;
    pthread_mutex_t lock;
    MenhirFetchProgress progress;
    _Atomic int cancel;
    int rom;
    char url[1024], destination[1024];
};

static const MenhirRelease releases[] = {
    {"22G120", "macOS Ventura 13.6",
     "https://updates.cdn-apple.com/2023FallFCS/fullrestores/042-55833/C0830847-A2F8-458F-B680-967991820931/UniversalMac_13.6_22G120_Restore.ipsw",
     12297ull << 20},
    {"23G93", "macOS Sonoma 14.6.1",
     "https://updates.cdn-apple.com/2024SummerFCS/fullrestores/062-52859/932E0A8F-6644-4759-82DA-F8FA8DEA806A/UniversalMac_14.6.1_23G93_Restore.ipsw",
     14106ull << 20},
    {"24G90", "macOS Sequoia 15.6.1",
     "https://updates.cdn-apple.com/2025SummerFCS/fullrestores/093-10809/CFD6DD38-DAF0-40DA-854F-31AAD1294C6F/UniversalMac_15.6.1_24G90_Restore.ipsw",
     16035ull << 20},
    {"25G83", "macOS Tahoe 26.6.2",
     "https://updates.cdn-apple.com/2026SummerFCS/fullrestores/140-75212/A2A24B94-1FC1-45A3-93F7-C51B02AF1F4D/UniversalMac_26.6.2_25G83_Restore.ipsw",
     18856ull << 20},
    {"26B123", "macOS 27.2",
     "https://updates.cdn-apple.com/2026FallFCS/c9671975-52db-4fb5-9af2-00217169ba8d/UniversalMac_27.2_26B123_Restore.ipsw",
     24409ull << 20},
};

const MenhirRelease *menhir_releases(size_t *count) {
    *count = sizeof releases / sizeof releases[0];
    return releases;
}

static uint64_t size_for(const char *url) {
    for (size_t i = 0; i < sizeof releases / sizeof releases[0]; i++)
        if (strcmp(releases[i].url, url) == 0) return releases[i].approximate_bytes;
    return 1ull << 30;
}

static void finish(MenhirFetch *job, MenhirFetchState state, const char *error) {
    pthread_mutex_lock(&job->lock);
    job->progress.state = state;
    if (error) snprintf(job->progress.error, sizeof job->progress.error, "%s", error);
    pthread_mutex_unlock(&job->lock);
}

static void *run(void *arg) {
    MenhirFetch *job = arg;
    char part[1100];
    snprintf(part, sizeof part, "%s.part", job->destination);
    uint64_t total = job->rom ? 214904 : size_for(job->url), have = 0;
    FILE *f = fopen(part, "r");
    if (f) {
        unsigned long long n = 0;
        if (fscanf(f, "%llu", &n) == 1 && n <= total) have = n;
        fclose(f);
    }
    if (have) LOG("fetch: going on from %llu of %llu", (unsigned long long)have, (unsigned long long)total);
    pthread_mutex_lock(&job->lock);
    job->progress.total = total;
    job->progress.downloaded = have;
    pthread_mutex_unlock(&job->lock);
    uint64_t step = total / 100 + 1;
    while (have < total) {
        if (job->cancel) {
            f = fopen(part, "w");
            if (f) fprintf(f, "%llu\n", (unsigned long long)have), fclose(f);
            LOG("fetch: cancelled at %llu", (unsigned long long)have);
            finish(job, MENHIR_FETCH_CANCELLED, "Cancelled.");
            return NULL;
        }
        struct timespec ts = {0, 100 * 1000000L};
        nanosleep(&ts, NULL);
        have = have + step > total ? total : have + step;
        pthread_mutex_lock(&job->lock);
        job->progress.downloaded = have;
        pthread_mutex_unlock(&job->lock);
    }
    f = fopen(part, "w");
    if (!f) {
        finish(job, MENHIR_FETCH_FAILED, "Cannot write the download.");
        return NULL;
    }
    fprintf(f, "menhir fake %s, not a real %s\n", job->rom ? "boot ROM" : "restore image", job->rom ? "ROM" : "IPSW");
    fclose(f);
    if (rename(part, job->destination) != 0) {
        finish(job, MENHIR_FETCH_FAILED, "Cannot move the download into place.");
        return NULL;
    }
    LOG("fetch: done, %s", job->destination);
    finish(job, MENHIR_FETCH_DONE, NULL);
    return NULL;
}

static MenhirFetch *start(int rom, const char *url, const char *destination) {
    if (!destination || (!rom && (!url || strncmp(url, "https://", 8) != 0))) return NULL;
    MenhirFetch *job = calloc(1, sizeof *job);
    if (!job) return NULL;
    pthread_mutex_init(&job->lock, NULL);
    job->rom = rom;
    snprintf(job->url, sizeof job->url, "%s", url ? url : "");
    snprintf(job->destination, sizeof job->destination, "%s", destination);
    job->progress.state = MENHIR_FETCH_RUNNING;
    if (rom && access(destination, F_OK) == 0) {
        /* Cached. The real one checks the hash first. */
        job->progress.state = MENHIR_FETCH_DONE;
        job->progress.downloaded = job->progress.total = 214904;
        job->thread = 0;
        return job;
    }
    if (pthread_create(&job->thread, NULL, run, job) != 0) {
        free(job);
        return NULL;
    }
    return job;
}

MenhirFetch *menhir_fetch_rom(const char *destination, const char *ca_directory) {
    (void)ca_directory;
    return start(1, NULL, destination);
}

MenhirFetch *menhir_fetch_ipsw(const char *https_url, const char *destination, const char *ca_directory) {
    (void)ca_directory;
    return start(0, https_url, destination);
}

void menhir_fetch_poll(MenhirFetch *job, MenhirFetchProgress *out) {
    pthread_mutex_lock(&job->lock);
    *out = job->progress;
    pthread_mutex_unlock(&job->lock);
}

void menhir_fetch_cancel(MenhirFetch *job) { job->cancel = 1; }

void menhir_fetch_free(MenhirFetch *job) {
    if (!job) return;
    job->cancel = 1;
    if (job->thread) pthread_join(job->thread, NULL);
    pthread_mutex_destroy(&job->lock);
    free(job);
}

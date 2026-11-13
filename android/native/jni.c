/* JNI bindings: app.menhir.Engine (android/app/src/main/java/app/menhir/Engine.kt) onto henge-vm.h and
   host/menhir_fetch.h. Thin on purpose: every function is one call, and errors come back as a String
   (null for success) so the Kotlin side never has to size a buffer. */
#include <jni.h>

#include <android/native_window_jni.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "henge-vm.h"
#include "menhir_fetch.h"

#define FN(ret, name) JNIEXPORT ret JNICALL Java_app_menhir_Engine_##name

typedef struct {
    JNIEnv *env;
    const char *s[8];
    jstring j[8];
    int n;
} Strings;

static const char *str(Strings *b, jstring j) {
    if (!j) return NULL;
    const char *s = (*b->env)->GetStringUTFChars(b->env, j, NULL);
    b->j[b->n] = j;
    b->s[b->n++] = s;
    return s;
}

static void release(Strings *b) {
    for (int i = 0; i < b->n; i++) (*b->env)->ReleaseStringUTFChars(b->env, b->j[i], b->s[i]);
}

static jstring result(JNIEnv *env, int rc, const char *err) {
    if (rc == 0) return NULL;
    return (*env)->NewStringUTF(env, err[0] ? err : "It did not work, and the engine did not say why.");
}

FN(jstring, boot)(JNIEnv *env, jclass c, jstring rom, jstring disk, jstring aux, jlong ecid, jlong seed, jlong ram,
                  jint cpus) {
    (void)c;
    Strings b = {.env = env};
    char err[256] = "";
    int rc = henge_vm_boot(str(&b, rom), str(&b, disk), str(&b, aux), (uint64_t)ecid, (uint64_t)seed, (uint64_t)ram,
                           cpus, err, sizeof err);
    release(&b);
    return result(env, rc, err);
}

FN(jstring, install)(JNIEnv *env, jclass c, jstring rom, jstring disk, jstring aux, jlong ecid, jstring ipsw,
                     jstring workdir, jlong ram, jint cpus) {
    (void)c;
    Strings b = {.env = env};
    char err[256] = "";
    int rc = henge_vm_install(str(&b, rom), str(&b, disk), str(&b, aux), (uint64_t)ecid, str(&b, ipsw),
                              str(&b, workdir), (uint64_t)ram, cpus, err, sizeof err);
    release(&b);
    return result(env, rc, err);
}

FN(jint, installStep)(JNIEnv *env, jclass c) { (void)env, (void)c; return henge_install_step(); }
FN(jdouble, installProgress)(JNIEnv *env, jclass c) { (void)env, (void)c; return henge_install_progress(); }

FN(jboolean, makeAux)(JNIEnv *env, jclass c, jstring path) {
    (void)c;
    Strings b = {.env = env};
    int rc = henge_make_aux(str(&b, path));
    release(&b);
    return rc == 0;
}

FN(jstring, pause)(JNIEnv *env, jclass c) {
    (void)c;
    char err[256] = "";
    return result(env, henge_vm_pause(err, sizeof err), err);
}

FN(jstring, save)(JNIEnv *env, jclass c, jstring path, jstring ident) {
    (void)c;
    Strings b = {.env = env};
    char err[256] = "";
    int rc = henge_vm_save(str(&b, path), str(&b, ident), err, sizeof err);
    release(&b);
    return result(env, rc, err);
}

FN(jboolean, continueRunning)(JNIEnv *env, jclass c) { (void)env, (void)c; return henge_vm_continue() == 0; }

FN(jstring, resume)(JNIEnv *env, jclass c, jstring rom, jstring disk, jstring aux, jlong ecid, jlong seed, jlong ram,
                    jint cpus, jstring path, jstring ident) {
    (void)c;
    Strings b = {.env = env};
    char err[256] = "";
    int rc = henge_vm_resume(str(&b, rom), str(&b, disk), str(&b, aux), (uint64_t)ecid, (uint64_t)seed, (uint64_t)ram,
                             cpus, str(&b, path), str(&b, ident), err, sizeof err);
    release(&b);
    return result(env, rc, err);
}

/* When the saved Mac at path was saved (seconds since 1970), or -1 if it is not a whole one for ident. */
FN(jlong, savedAt)(JNIEnv *env, jclass c, jstring path, jstring ident) {
    (void)c;
    Strings b = {.env = env};
    uint64_t bytes = 0;
    int64_t at = 0;
    int rc = henge_vm_saved_ok(str(&b, path), str(&b, ident), &bytes, &at);
    release(&b);
    return rc == 0 ? at : -1;
}

FN(jint, state)(JNIEnv *env, jclass c) { (void)env, (void)c; return henge_vm_state(); }
FN(jlong, generation)(JNIEnv *env, jclass c) { (void)env, (void)c; return (jlong)henge_vm_generation(); }
FN(jboolean, canStart)(JNIEnv *env, jclass c) { (void)env, (void)c; return henge_vm_can_start() != 0; }
FN(jstring, exitReason)(JNIEnv *env, jclass c) { (void)c; return (*env)->NewStringUTF(env, henge_vm_exit_reason()); }
FN(jint, exitOutcome)(JNIEnv *env, jclass c) { (void)env, (void)c; return henge_vm_exit_outcome(); }
FN(void, stop)(JNIEnv *env, jclass c) { (void)env, (void)c; henge_vm_stop(); }
FN(jlong, blocks)(JNIEnv *env, jclass c) { (void)env, (void)c; return (jlong)henge_vm_blocks(); }
FN(jlong, netFrames)(JNIEnv *env, jclass c, jboolean sent) { (void)env, (void)c; return (jlong)henge_vm_net_frames(sent); }
FN(jint, cpusOn)(JNIEnv *env, jclass c) { (void)env, (void)c; return henge_vm_cpus_on(); }

FN(void, share)(JNIEnv *env, jclass c, jstring folder, jstring socket, jstring agent) {
    (void)c;
    Strings b = {.env = env};
    henge_vm_share(str(&b, folder), str(&b, socket), str(&b, agent));
    release(&b);
}

/* UTF-8 bytes, for the same reason as takeClipboard. */
FN(void, setClipboard)(JNIEnv *env, jclass c, jbyteArray text) {
    (void)c;
    jsize n = (*env)->GetArrayLength(env, text);
    char *s = malloc((size_t)n + 1);
    if (!s) return;
    (*env)->GetByteArrayRegion(env, text, 0, n, (jbyte *)s);
    s[n] = 0;
    henge_share_set_clipboard(s);
    free(s);
}

/* Bytes, not a String: the guest's text is UTF-8, which NewStringUTF does not take (it wants modified UTF-8). */
FN(jbyteArray, takeClipboard)(JNIEnv *env, jclass c) {
    (void)c;
    char *text = henge_share_take_clipboard();
    if (!text) return NULL;
    jsize n = (jsize)strlen(text);
    jbyteArray a = (*env)->NewByteArray(env, n);
    (*env)->SetByteArrayRegion(env, a, 0, n, (const jbyte *)text);
    free(text);
    return a;
}

FN(jlong, agentSeen)(JNIEnv *env, jclass c) { (void)env, (void)c; return henge_share_agent_seen(); }

FN(void, setDisplay)(JNIEnv *env, jclass c, jint w, jint h) { (void)env, (void)c; henge_vm_set_display((uint32_t)w, (uint32_t)h); }
FN(void, setDisplayMm)(JNIEnv *env, jclass c, jint w, jint h) {
    (void)env, (void)c;
    henge_vm_set_display_mm((uint32_t)w, (uint32_t)h);
}

FN(void, key)(JNIEnv *env, jclass c, jint code, jboolean down) { (void)env, (void)c; henge_vm_key((uint16_t)code, down); }
FN(void, keyHid)(JNIEnv *env, jclass c, jint usage, jboolean down) {
    (void)env, (void)c;
    henge_vm_key_hid((uint8_t)usage, down);
}
FN(void, mouseMove)(JNIEnv *env, jclass c, jint dx, jint dy) { (void)env, (void)c; henge_vm_mouse_move(dx, dy); }
FN(void, mouseButton)(JNIEnv *env, jclass c, jint button, jboolean down) {
    (void)env, (void)c;
    henge_vm_mouse_button(button, down);
}
FN(void, mouseScroll)(JNIEnv *env, jclass c, jint dx, jint dy) { (void)env, (void)c; henge_vm_mouse_scroll(dx, dy); }
FN(void, touch)(JNIEnv *env, jclass c, jint finger, jint phase, jint x, jint y, jint ts) {
    (void)env, (void)c;
    henge_vm_touch(finger, phase, x, y, (uint32_t)ts);
}
FN(void, touchFrame)(JNIEnv *env, jclass c, jint ts) { (void)env, (void)c; henge_vm_touch_frame((uint32_t)ts); }
FN(void, touchButton)(JNIEnv *env, jclass c, jboolean down) { (void)env, (void)c; henge_vm_touch_button(down); }

FN(jintArray, pointer)(JNIEnv *env, jclass c) {
    (void)c;
    int x, y;
    if (henge_vm_pointer(&x, &y) != 0) return NULL;
    jintArray a = (*env)->NewIntArray(env, 2);
    jint v[2] = {x, y};
    (*env)->SetIntArrayRegion(env, a, 0, 2, v);
    return a;
}

/* The window this side holds for the engine: released once the engine has the next one. */
static ANativeWindow *held_window;
static pthread_mutex_t window_lock = PTHREAD_MUTEX_INITIALIZER;

FN(void, setDisplaySurface)(JNIEnv *env, jclass c, jobject surface) {
    (void)c;
    ANativeWindow *window = surface ? ANativeWindow_fromSurface(env, surface) : NULL;
    pthread_mutex_lock(&window_lock);
    henge_vm_set_display_surface(window);
    if (held_window) ANativeWindow_release(held_window);
    held_window = window;
    pthread_mutex_unlock(&window_lock);
}

FN(jlong, gpuFrames)(JNIEnv *env, jclass c) { (void)env, (void)c; return (jlong)henge_vm_gpu_frames(); }
FN(jlong, availableMemory)(JNIEnv *env, jclass c) { (void)env, (void)c; return (jlong)henge_available_memory(); }
FN(jlong, largestMappingMb)(JNIEnv *env, jclass c) { (void)env, (void)c; return (jlong)henge_largest_mapping_mb(); }

/* Downloads (host/menhir_fetch.h). A job is a handle the Kotlin side frees exactly once. */
FN(jlong, fetchRom)(JNIEnv *env, jclass c, jstring destination) {
    (void)c;
    Strings b = {.env = env};
    MenhirFetch *job = menhir_fetch_rom(str(&b, destination), NULL);
    release(&b);
    return (jlong)(intptr_t)job;
}

FN(jlong, fetchIpsw)(JNIEnv *env, jclass c, jstring url, jstring destination) {
    (void)c;
    Strings b = {.env = env};
    MenhirFetch *job = menhir_fetch_ipsw(str(&b, url), str(&b, destination), NULL);
    release(&b);
    return (jlong)(intptr_t)job;
}

/* Fills out with {state, downloaded, total} and returns the error text, if any. */
FN(jstring, fetchPoll)(JNIEnv *env, jclass c, jlong job, jlongArray out) {
    (void)c;
    MenhirFetchProgress p;
    memset(&p, 0, sizeof p);
    menhir_fetch_poll((MenhirFetch *)(intptr_t)job, &p);
    jlong v[3] = {p.state, (jlong)p.downloaded, (jlong)p.total};
    (*env)->SetLongArrayRegion(env, out, 0, 3, v);
    p.error[sizeof p.error - 1] = 0;
    return p.error[0] ? (*env)->NewStringUTF(env, p.error) : NULL;
}

FN(void, fetchCancel)(JNIEnv *env, jclass c, jlong job) { (void)env, (void)c; menhir_fetch_cancel((MenhirFetch *)(intptr_t)job); }
FN(void, fetchFree)(JNIEnv *env, jclass c, jlong job) { (void)env, (void)c; menhir_fetch_free((MenhirFetch *)(intptr_t)job); }

FN(jboolean, isFake)(JNIEnv *env, jclass c) {
    (void)env, (void)c;
#ifdef MENHIR_FAKE_ENGINE
    return JNI_TRUE;
#else
    return JNI_FALSE;
#endif
}

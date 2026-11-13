/* Shell diagnostic only, enabled with MENHIR_GFX_WINDOW_TEST. */
#include <jni.h>
#include <android/native_window_jni.h>

extern int pvg_replay_main(int argc, char **argv);
ANativeWindow *pvg_replay_window;

JNIEXPORT jint JNICALL Java_app_menhir_gfx_ReplayWindow_replay(JNIEnv *env, jclass cls, jobject surface, jstring path) {
    (void)cls;
    const char *recording = (*env)->GetStringUTFChars(env, path, NULL);
    pvg_replay_window = ANativeWindow_fromSurface(env, surface);
    if (!recording || !pvg_replay_window) return 2;
    char *argv[] = {"pvg_replay_window", "--free", "--frames", "1", (char *)recording};
    int result = pvg_replay_main(5, argv);
    ANativeWindow_release(pvg_replay_window);
    pvg_replay_window = NULL;
    (*env)->ReleaseStringUTFChars(env, path, recording);
    return result;
}

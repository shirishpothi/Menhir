package app.menhir.gfx;

// Shell-only diagnostic. app_process can use SurfaceControl without installing an APK.
public final class ReplayWindow {
    private static native int replay(android.view.Surface surface, String recording);
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        Class<?> sc = Class.forName("android.view.SurfaceControl");
        Class<?> builder = Class.forName("android.view.SurfaceControl$Builder");
        Object b = builder.getConstructor().newInstance();
        builder.getMethod("setName", String.class).invoke(b, "Menhir graphics replay");
        builder.getMethod("setBufferSize", int.class, int.class).invoke(b, 3000, 2120);
        Object control = builder.getMethod("build").invoke(b);
        Class<?> transaction = Class.forName("android.view.SurfaceControl$Transaction");
        Object tx = transaction.getConstructor().newInstance();
        transaction.getMethod("setLayer", sc, int.class).invoke(tx, control, 1000000);
        transaction.getMethod("show", sc).invoke(tx, control);
        transaction.getMethod("apply").invoke(tx);
        android.view.Surface surface = android.view.Surface.class.getConstructor(sc).newInstance(control);
        try {
            int result = replay(surface, args[1]);
            if (result != 0) throw new IllegalStateException("Replay failed: " + result);
        } finally {
            surface.release();
            transaction.getMethod("remove", sc).invoke(tx, control);
            transaction.getMethod("apply").invoke(tx);
            sc.getMethod("release").invoke(control);
        }
    }
}

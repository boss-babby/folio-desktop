package ai.folio.reader;

/** Standalone JVM regression for NativeActivity window-vs-content safe area. */
public final class SystemBarGeometryTest {
    private static void equal(int expected, int actual, String reason) {
        if (expected != actual) throw new AssertionError(reason + ": " + actual + " != " + expected);
    }

    public static void main(String[] args) {
        // On the affected device a visible bar occupies screen Y=0..72,
        // but NativeActivity's window surface begins at screen Y=0.
        equal(72, SystemBarGeometry.uncovered(72, 0), "window surface under clock");
        // Child content below the bar is NOT the window surface. Previously
        // measuring a nonexistent SurfaceView caused zero native offset.
        equal(0, SystemBarGeometry.uncovered(72, 72), "already fitted content");
        equal(0, SystemBarGeometry.uncovered(0, 0), "no visible status bar");
        equal(28, SystemBarGeometry.uncovered(72, 44), "partly fitted window");
        equal(84, SystemBarGeometry.uncovered(72, -12), "surface under cutout");
        System.out.println("SystemBarGeometry: 5 checks passed");
    }
}

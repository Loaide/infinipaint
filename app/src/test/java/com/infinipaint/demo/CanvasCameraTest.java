package com.infinipaint.demo;

import org.junit.Test;
import static org.junit.Assert.*;

public class CanvasCameraTest {
    private final CanvasCamera camera = new CanvasCamera();

    @Test public void screenWorldRoundTripAtExtremeOriginsAndBothZoomLimits() {
        for (double z : new double[]{0.1, 1.345, 16}) {
            double origin = Math.scalb(1.0, 40);
            camera.restore(-origin, origin, z);
            double tolerancePx = Math.ulp(origin) * z;
            // Drag that screen pixel to (0,0): its world point is now the camera origin.
            assertTrue(camera.track(937.432, 741.11, 0, 0, 1));
            assertEquals(937.432, (camera.panX() + origin) * z, tolerancePx);
            assertEquals(741.11, (camera.panY() - origin) * z, tolerancePx);
            assertTrue(camera.track(0, 0, 937.432, 741.11, 1));
            assertEquals(-origin, camera.panX(), Math.ulp(origin));
            assertEquals(origin, camera.panY(), Math.ulp(origin));
        }
    }

    @Test public void resizedViewportRetainsOriginAndChangesVisibleExtent() {
        camera.restore(120, -42, 2);
        CanvasCamera landscape = new CanvasCamera();
        assertTrue(landscape.restore(camera.panX(), camera.panY(), camera.zoom()));
        assertEquals(120, landscape.panX(), 0);
        assertEquals(-42, landscape.panY(), 0);
        // Drag each viewport's bottom-right corner to its top-left.
        assertTrue(camera.track(800, 1200, 0, 0, 1));
        assertTrue(landscape.track(1200, 800, 0, 0, 1));
        assertEquals(camera.panX() + 200, landscape.panX(), 0);
        assertEquals(camera.panY() - 200, landscape.panY(), 0);
    }

    @Test public void invalidRestoreIsAtomic() {
        camera.restore(12, -7, 0.5);
        for (double z : new double[]{0, -1, 16.1, Double.NaN, Double.POSITIVE_INFINITY}) {
            assertFalse(camera.restore(0, 0, z));
        }
        assertFalse(camera.restore(Double.NaN, 0, 1));
        assertFalse(camera.restore(0, Double.NEGATIVE_INFINITY, 1));
        assertEquals(12, camera.panX(), 0);
        assertEquals(-7, camera.panY(), 0);
        assertEquals(0.5, camera.zoom(), 0);
    }
}

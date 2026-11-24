package com.infinipaint.demo;

import org.junit.Test;
import static org.junit.Assert.*;

public class FingerNavigationTest {
    private final CanvasCamera camera = new CanvasCamera();
    // Synthetic pointer lists in Android's per-event order: [id,x,y] repeated.
    private final FingerNavigation<double[]> nav = new FingerNavigation<>(camera,
            new FingerNavigation.Points<double[]>() {
                public int indexOf(double[] e, int id) {
                    for (int i = 0; i < e.length; i += 3) if (e[i] == id) return i;
                    return -1;
                }
                public double x(double[] e, int i) { return e[i + 1]; }
                public double y(double[] e, int i) { return e[i + 2]; }
            });
    private static double[] event(double... points) { return points; }

    // Independent projection oracle for the state produced by the navigation path.
    private double worldX(double sx) { return camera.panX() + sx / camera.zoom(); }
    private double worldY(double sy) { return camera.panY() + sy / camera.zoom(); }

    @Test public void panKeepsGrabbedWorldPointUnderFingerAtEveryScale() {
        for (double z : new double[]{0.1, 2, 16}) {
            nav.cancel();
            camera.restore(10, -35, z);
            nav.down(event(7, 50, 75), 7);
            double wx = worldX(50), wy = worldY(75);
            assertTrue(nav.move(event(7, 60, 45)));
            assertEquals(wx, worldX(60), 1e-11);
            assertEquals(wy, worldY(45), 1e-11);
            assertFalse(nav.move(event(7, 60, 45)));
        }
    }

    @Test public void movingPinchUsesMovingAnchorAndIdsNotIndices() {
        nav.down(event(11, 0, 20), 11);
        nav.down(event(11, 10, 20, 4, 30, 20), 4);
        double wx = worldX(20), wy = worldY(20);
        nav.move(event(4, 80, 50, 11, 40, 50));
        assertEquals(2, camera.zoom(), 0);
        assertEquals(wx, worldX(60), 0);
        assertEquals(wy, worldY(50), 0);
        nav.move(event(11, 50, 35, 4, 90, 35));
        assertEquals(2, camera.zoom(), 0);
        assertEquals(wx, worldX(70), 0);
        assertEquals(wy, worldY(35), 0);
    }

    @Test public void clampAtBothEndsAndRecoverAfterCoincidentContacts() {
        nav.down(event(3, 100, 20), 3);
        nav.down(event(3, 100, 20, 9, 100, 20), 9);
        for (int i = 0; i < 100; i++) {
            nav.move(event(9, 1300, 30, 3, -900, 30));
            assertEquals(16, camera.zoom(), 0);
            assertEquals(100, worldX(200), 1e-10);
            nav.move(event(9, 100, 20, 3, 100, 20));
            assertEquals(0.1, camera.zoom(), 0);
            assertEquals(100, worldX(100), 1e-10);
            assertEquals(20, worldY(20), 1e-10);
        }
    }

    @Test public void eitherTrackedFingerContinuesWithoutAdoptingUntrackedThird() {
        for (int survivor : new int[]{1, 2}) {
            nav.cancel();
            camera.restore(0, 0, 1);
            double[] both = event(1, 0, 0, 2, 20, 0);
            nav.down(both, 1);
            nav.down(both, 2);
            double[] up = event(3, 500, 500, survivor, 45, 55, 3 - survivor, 25, 55);
            nav.down(up, 3);
            nav.up(up, 3 - survivor);
            assertFalse(nav.pinching());
            assertFalse(nav.move(event(survivor, 45, 55, 3, 700, 700)));
            assertTrue(nav.move(event(3, 900, 900, survivor, 48, 51)));
            assertEquals(-3, camera.panX(), 0);
            assertEquals(4, camera.panY(), 0);
            nav.up(event(survivor, 48, 51), survivor);
            assertFalse(nav.move(event(3, 910, 910)));
        }
    }

    @Test public void cancelledMissingAndNonFiniteContactsRequireFreshDown() {
        double[] pair = event(1, 0, 0, 2, 10, 0);
        nav.down(pair, 1);
        nav.down(pair, 2);
        nav.cancel();
        assertFalse(nav.move(pair));
        nav.down(pair, 1);
        nav.down(pair, 2);
        assertFalse(nav.move(event(2, 10, 0)));
        assertFalse(nav.move(pair));
        nav.down(pair, 1);
        assertFalse(nav.move(event(1, Double.NaN, 0)));
        assertFalse(nav.move(event(1, 500, 500)));
        nav.down(event(1, 500, 500), 1);
        assertTrue(nav.move(event(1, 501, 502)));
        assertEquals(-1, camera.panX(), 0);
        assertEquals(-2, camera.panY(), 0);
    }
}

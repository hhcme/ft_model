import type { CadController } from './CadController';

/**
 * Attaches mouse and touch gesture handlers to a canvas element,
 * translating them into CAD view operations (pan, zoom, fit-to-extents).
 *
 * Returns a cleanup function that removes all listeners.
 *
 * Gesture mapping:
 *   - Mouse drag          -> pan
 *   - Mouse wheel         -> zoom (1.1x in, 0.9x out) around cursor
 *   - Double-click        -> fitToExtents
 *   - Touch drag (1 finger) -> pan
 *   - Pinch (2 fingers)   -> zoom around pinch center
 *   - Touch double-tap    -> fitToExtents
 */
export function attachGestures(
  canvas: HTMLCanvasElement,
  controller: CadController,
): () => void {
  // ----- Mouse state -----
  let isDragging = false;
  let lastMouseX = 0;
  let lastMouseY = 0;

  // ----- Touch state -----
  let lastTouchDist = 0;
  let lastTouchCenterX = 0;
  let lastTouchCenterY = 0;
  let lastTapTime = 0;

  // ------ Mouse handlers ------

  function onMouseDown(e: MouseEvent): void {
    if (e.button !== 0) return; // left button only
    isDragging = true;
    lastMouseX = e.clientX;
    lastMouseY = e.clientY;
    e.preventDefault();
  }

  function onMouseMove(e: MouseEvent): void {
    if (!isDragging) return;
    const dx = e.clientX - lastMouseX;
    const dy = e.clientY - lastMouseY;
    lastMouseX = e.clientX;
    lastMouseY = e.clientY;

    controller.pan(dx, dy);
    e.preventDefault();
  }

  function onMouseUp(_e: MouseEvent): void {
    isDragging = false;
  }

  function onWheel(e: WheelEvent): void {
    e.preventDefault();

    const rect = canvas.getBoundingClientRect();
    const pivotX = e.clientX - rect.left;
    const pivotY = e.clientY - rect.top;

    // deltaY > 0 = scroll down = zoom out; < 0 = zoom in
    const factor = e.deltaY > 0 ? 0.9 : 1.1;
    controller.zoom(factor, pivotX, pivotY);
  }

  function onDblClick(_e: MouseEvent): void {
    controller.fitToExtents();
  }

  // ------ Touch handlers ------

  function onTouchStart(e: TouchEvent): void {
    if (e.touches.length === 1) {
      // Check for double-tap
      const now = Date.now();
      if (now - lastTapTime < 300) {
        controller.fitToExtents();
        lastTapTime = 0;
        e.preventDefault();
        return;
      }
      lastTapTime = now;

      lastMouseX = e.touches[0].clientX;
      lastMouseY = e.touches[0].clientY;
    } else if (e.touches.length === 2) {
      // Start pinch
      const dx = e.touches[1].clientX - e.touches[0].clientX;
      const dy = e.touches[1].clientY - e.touches[0].clientY;
      lastTouchDist = Math.sqrt(dx * dx + dy * dy);
      lastTouchCenterX = (e.touches[0].clientX + e.touches[1].clientX) / 2;
      lastTouchCenterY = (e.touches[0].clientY + e.touches[1].clientY) / 2;
    }
    e.preventDefault();
  }

  function onTouchMove(e: TouchEvent): void {
    if (e.touches.length === 1) {
      // Single-finger pan
      const dx = e.touches[0].clientX - lastMouseX;
      const dy = e.touches[0].clientY - lastMouseY;
      lastMouseX = e.touches[0].clientX;
      lastMouseY = e.touches[0].clientY;
      controller.pan(dx, dy);
    } else if (e.touches.length === 2) {
      // Pinch zoom
      const dx = e.touches[1].clientX - e.touches[0].clientX;
      const dy = e.touches[1].clientY - e.touches[0].clientY;
      const dist = Math.sqrt(dx * dx + dy * dy);

      if (lastTouchDist > 0) {
        const factor = dist / lastTouchDist;
        const rect = canvas.getBoundingClientRect();
        const pivotX = lastTouchCenterX - rect.left;
        const pivotY = lastTouchCenterY - rect.top;
        controller.zoom(factor, pivotX, pivotY);
      }

      lastTouchDist = dist;
      lastTouchCenterX = (e.touches[0].clientX + e.touches[1].clientX) / 2;
      lastTouchCenterY = (e.touches[0].clientY + e.touches[1].clientY) / 2;
    }
    e.preventDefault();
  }

  function onTouchEnd(_e: TouchEvent): void {
    lastTouchDist = 0;
  }

  // ------ Attach ------

  canvas.addEventListener('mousedown', onMouseDown);
  window.addEventListener('mousemove', onMouseMove);
  window.addEventListener('mouseup', onMouseUp);
  canvas.addEventListener('wheel', onWheel, { passive: false });
  canvas.addEventListener('dblclick', onDblClick);
  canvas.addEventListener('touchstart', onTouchStart, { passive: false });
  canvas.addEventListener('touchmove', onTouchMove, { passive: false });
  canvas.addEventListener('touchend', onTouchEnd);

  // ------ Cleanup ------

  return () => {
    canvas.removeEventListener('mousedown', onMouseDown);
    window.removeEventListener('mousemove', onMouseMove);
    window.removeEventListener('mouseup', onMouseUp);
    canvas.removeEventListener('wheel', onWheel);
    canvas.removeEventListener('dblclick', onDblClick);
    canvas.removeEventListener('touchstart', onTouchStart);
    canvas.removeEventListener('touchmove', onTouchMove);
    canvas.removeEventListener('touchend', onTouchEnd);
  };
}

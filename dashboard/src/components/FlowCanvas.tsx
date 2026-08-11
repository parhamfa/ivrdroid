import { Maximize2, Minus, Move, Plus } from "lucide-react";
import {
  useCallback,
  useEffect,
  useLayoutEffect,
  useRef,
  useState,
  type KeyboardEvent as ReactKeyboardEvent,
  type PointerEvent as ReactPointerEvent,
  type ReactNode,
} from "react";

type Viewport = {
  x: number;
  y: number;
  zoom: number;
};

type CanvasMeasurements = {
  viewportWidth: number;
  viewportHeight: number;
  contentWidth: number;
  contentHeight: number;
};

type DragState = {
  pointerId: number;
  startX: number;
  startY: number;
  originX: number;
  originY: number;
};

const MIN_ZOOM = 0.25;
const MIN_FIT_ZOOM = 0.7;
const MAX_ZOOM = 1.5;
const ZOOM_STEP = 0.1;
const FIT_PADDING = 56;
const CONTENT_EDGE = 72;
const KEYBOARD_PAN_STEP = 64;
const INTERACTIVE_TARGETS = "button, a, input, select, textarea, [role='button'], .flow-node, .flow-reference, .flow-canvas__toolbar";

function clamp(value: number, minimum: number, maximum: number) {
  return Math.min(maximum, Math.max(minimum, value));
}

function clampAxis(position: number, viewportSize: number, contentSize: number) {
  if (contentSize <= viewportSize) {
    const centered = (viewportSize - contentSize) / 2;
    return clamp(position, centered - CONTENT_EDGE, centered + CONTENT_EDGE);
  }
  return clamp(position, viewportSize - contentSize - CONTENT_EDGE, CONTENT_EDGE);
}

function clampViewport(viewport: Viewport, measurements: CanvasMeasurements): Viewport {
  const contentWidth = measurements.contentWidth * viewport.zoom;
  const contentHeight = measurements.contentHeight * viewport.zoom;
  return {
    ...viewport,
    x: clampAxis(viewport.x, measurements.viewportWidth, contentWidth),
    y: clampAxis(viewport.y, measurements.viewportHeight, contentHeight),
  };
}

export function FlowCanvas({ children }: { children: ReactNode }) {
  const canvasRef = useRef<HTMLDivElement>(null);
  const stageRef = useRef<HTMLDivElement>(null);
  const viewportRef = useRef<Viewport>({ x: 0, y: 0, zoom: 1 });
  const dragRef = useRef<DragState | null>(null);
  const initialFitComplete = useRef(false);
  const [viewport, setViewport] = useState<Viewport>(viewportRef.current);
  const [dragging, setDragging] = useState(false);

  const measure = useCallback((): CanvasMeasurements | null => {
    const canvas = canvasRef.current;
    const stage = stageRef.current;
    if (!canvas || !stage) return null;
    const viewportWidth = canvas.clientWidth;
    const viewportHeight = canvas.clientHeight;
    const contentWidth = stage.offsetWidth || stage.scrollWidth;
    const contentHeight = stage.offsetHeight || stage.scrollHeight;
    if (!viewportWidth || !viewportHeight || !contentWidth || !contentHeight) return null;
    return { viewportWidth, viewportHeight, contentWidth, contentHeight };
  }, []);

  const updateViewport = useCallback((next: Viewport | ((current: Viewport) => Viewport)) => {
    setViewport((current) => {
      const proposed = typeof next === "function" ? next(current) : next;
      const measurements = measure();
      const bounded = measurements ? clampViewport(proposed, measurements) : proposed;
      viewportRef.current = bounded;
      return bounded;
    });
  }, [measure]);

  const fitView = useCallback((minimumZoom = MIN_FIT_ZOOM) => {
    const measurements = measure();
    if (!measurements) return;
    const availableWidth = Math.max(1, measurements.viewportWidth - FIT_PADDING * 2);
    const availableHeight = Math.max(1, measurements.viewportHeight - FIT_PADDING * 2);
    const zoom = clamp(
      Math.min(
        availableWidth / measurements.contentWidth,
        availableHeight / measurements.contentHeight,
      ),
      minimumZoom,
      1,
    );
    updateViewport({
      zoom,
      x: (measurements.viewportWidth - measurements.contentWidth * zoom) / 2,
      y: (measurements.viewportHeight - measurements.contentHeight * zoom) / 2,
    });
    initialFitComplete.current = true;
  }, [measure, updateViewport]);

  const zoomAt = useCallback((zoom: number, clientX?: number, clientY?: number) => {
    const canvas = canvasRef.current;
    const measurements = measure();
    if (!canvas || !measurements) return;
    const current = viewportRef.current;
    const boundedZoom = clamp(zoom, MIN_ZOOM, MAX_ZOOM);
    const rect = canvas.getBoundingClientRect();
    const anchorX = clientX === undefined ? measurements.viewportWidth / 2 : clientX - rect.left;
    const anchorY = clientY === undefined ? measurements.viewportHeight / 2 : clientY - rect.top;
    const contentX = (anchorX - current.x) / current.zoom;
    const contentY = (anchorY - current.y) / current.zoom;
    updateViewport({
      zoom: boundedZoom,
      x: anchorX - contentX * boundedZoom,
      y: anchorY - contentY * boundedZoom,
    });
  }, [measure, updateViewport]);

  const resetView = useCallback(() => {
    const measurements = measure();
    if (!measurements) return;
    updateViewport({
      zoom: 1,
      x: (measurements.viewportWidth - measurements.contentWidth) / 2,
      y: CONTENT_EDGE,
    });
  }, [measure, updateViewport]);

  const panBy = useCallback((deltaX: number, deltaY: number) => {
    updateViewport((current) => ({ ...current, x: current.x + deltaX, y: current.y + deltaY }));
  }, [updateViewport]);

  useLayoutEffect(() => {
    const frame = window.requestAnimationFrame(() => fitView());
    return () => window.cancelAnimationFrame(frame);
  }, [fitView]);

  useEffect(() => {
    const canvas = canvasRef.current;
    const stage = stageRef.current;
    if (!canvas || !stage || typeof ResizeObserver === "undefined") return;
    const observer = new ResizeObserver(() => {
      if (!initialFitComplete.current) {
        fitView();
        return;
      }
      updateViewport((current) => current);
    });
    observer.observe(canvas);
    observer.observe(stage);
    return () => observer.disconnect();
  }, [fitView, updateViewport]);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const onWheel = (event: WheelEvent) => {
      event.preventDefault();
      const lineScale = event.deltaMode === WheelEvent.DOM_DELTA_LINE ? 16 : 1;
      const pageScale = event.deltaMode === WheelEvent.DOM_DELTA_PAGE ? canvas.clientHeight : 1;
      const scale = event.deltaMode === WheelEvent.DOM_DELTA_PIXEL ? 1 : lineScale * pageScale;
      if (event.ctrlKey || event.metaKey) {
        const factor = Math.exp(-event.deltaY * scale * 0.0025);
        zoomAt(viewportRef.current.zoom * factor, event.clientX, event.clientY);
        return;
      }
      const horizontal = event.shiftKey && event.deltaX === 0 ? event.deltaY : event.deltaX;
      const vertical = event.shiftKey && event.deltaX === 0 ? 0 : event.deltaY;
      panBy(-horizontal * scale, -vertical * scale);
    };
    canvas.addEventListener("wheel", onWheel, { passive: false });
    return () => canvas.removeEventListener("wheel", onWheel);
  }, [panBy, zoomAt]);

  const startPan = (event: ReactPointerEvent<HTMLDivElement>) => {
    if (event.button !== 0 || event.isPrimary === false) return;
    const target = event.target;
    if (target instanceof Element && target.closest(INTERACTIVE_TARGETS)) return;
    event.preventDefault();
    event.currentTarget.focus({ preventScroll: true });
    event.currentTarget.setPointerCapture?.(event.pointerId);
    const current = viewportRef.current;
    dragRef.current = {
      pointerId: event.pointerId,
      startX: event.clientX,
      startY: event.clientY,
      originX: current.x,
      originY: current.y,
    };
    setDragging(true);
  };

  const continuePan = (event: ReactPointerEvent<HTMLDivElement>) => {
    const drag = dragRef.current;
    if (!drag || drag.pointerId !== event.pointerId) return;
    updateViewport((current) => ({
      ...current,
      x: drag.originX + event.clientX - drag.startX,
      y: drag.originY + event.clientY - drag.startY,
    }));
  };

  const stopPan = (event: ReactPointerEvent<HTMLDivElement>) => {
    if (dragRef.current?.pointerId !== event.pointerId) return;
    if (event.currentTarget.hasPointerCapture?.(event.pointerId)) {
      event.currentTarget.releasePointerCapture(event.pointerId);
    }
    dragRef.current = null;
    setDragging(false);
  };

  const losePan = (event: ReactPointerEvent<HTMLDivElement>) => {
    if (dragRef.current?.pointerId !== event.pointerId) return;
    dragRef.current = null;
    setDragging(false);
  };

  const onKeyDown = (event: ReactKeyboardEvent<HTMLDivElement>) => {
    if (event.target !== event.currentTarget) return;
    if (event.key === "ArrowLeft") panBy(KEYBOARD_PAN_STEP, 0);
    else if (event.key === "ArrowRight") panBy(-KEYBOARD_PAN_STEP, 0);
    else if (event.key === "ArrowUp") panBy(0, KEYBOARD_PAN_STEP);
    else if (event.key === "ArrowDown") panBy(0, -KEYBOARD_PAN_STEP);
    else if (event.key === "+" || event.key === "=") zoomAt(viewportRef.current.zoom + ZOOM_STEP);
    else if (event.key === "-" || event.key === "_") zoomAt(viewportRef.current.zoom - ZOOM_STEP);
    else if (event.key === "0") resetView();
    else if (event.key.toLowerCase() === "f") fitView(MIN_ZOOM);
    else return;
    event.preventDefault();
  };

  const zoomPercent = Math.round(viewport.zoom * 100);

  return (
    <div
      ref={canvasRef}
      className={`flow-canvas ${dragging ? "flow-canvas--dragging" : ""}`}
      role="region"
      aria-label="IVR flow canvas"
      aria-describedby="flow-canvas-help"
      tabIndex={0}
      onPointerDown={startPan}
      onPointerMove={continuePan}
      onPointerUp={stopPan}
      onPointerCancel={stopPan}
      onLostPointerCapture={losePan}
      onKeyDown={onKeyDown}
    >
      <div
        ref={stageRef}
        className="flow-canvas__stage"
        style={{ transform: `translate3d(${viewport.x}px, ${viewport.y}px, 0) scale(${viewport.zoom})` }}
      >
        {children}
      </div>

      <div id="flow-canvas-help" className="flow-canvas__help">
        <Move size={15} /> Drag to pan <span>·</span> Scroll to move <span>·</span> Ctrl/⌘ + scroll to zoom
      </div>
      <div className="flow-canvas__toolbar" role="toolbar" aria-label="Canvas navigation">
        <button
          type="button"
          className="flow-canvas__control"
          onClick={() => zoomAt(viewportRef.current.zoom - ZOOM_STEP)}
          disabled={viewport.zoom <= MIN_ZOOM}
          aria-label="Zoom out"
          title="Zoom out (−)"
        >
          <Minus size={17} />
        </button>
        <button
          type="button"
          className="flow-canvas__zoom"
          onClick={resetView}
          aria-label={`Reset zoom to 100%. Current zoom ${zoomPercent}%`}
          title="Reset to 100% (0)"
        >
          <span aria-live="polite">{zoomPercent}%</span>
        </button>
        <button
          type="button"
          className="flow-canvas__control"
          onClick={() => zoomAt(viewportRef.current.zoom + ZOOM_STEP)}
          disabled={viewport.zoom >= MAX_ZOOM}
          aria-label="Zoom in"
          title="Zoom in (+)"
        >
          <Plus size={17} />
        </button>
        <span className="flow-canvas__toolbar-divider" />
        <button
          type="button"
          className="flow-canvas__control"
          onClick={() => fitView(MIN_ZOOM)}
          aria-label="Fit flow to view"
          title="Fit flow to view (F)"
        >
          <Maximize2 size={17} />
        </button>
      </div>
    </div>
  );
}

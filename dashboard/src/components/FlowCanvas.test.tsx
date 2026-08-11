import { cleanup, fireEvent, render, screen } from "@testing-library/react";
import { afterEach, beforeAll, describe, expect, it } from "vitest";
import { FlowCanvas } from "./FlowCanvas";

class TestPointerEvent extends MouseEvent {
  readonly pointerId: number;
  readonly isPrimary: boolean;

  constructor(type: string, init: PointerEventInit = {}) {
    super(type, init);
    this.pointerId = init.pointerId ?? 1;
    this.isPrimary = init.isPrimary ?? true;
  }
}

beforeAll(() => {
  Object.defineProperty(window, "PointerEvent", { configurable: true, value: TestPointerEvent });
});
afterEach(cleanup);

function setCanvasDimensions() {
  const canvas = screen.getByRole("region", { name: "IVR flow canvas" });
  const stage = canvas.querySelector<HTMLElement>(".flow-canvas__stage");
  if (!stage) throw new Error("Flow stage was not rendered");
  Object.defineProperties(canvas, {
    clientWidth: { configurable: true, value: 1000 },
    clientHeight: { configurable: true, value: 600 },
  });
  Object.defineProperties(stage, {
    offsetWidth: { configurable: true, value: 2000 },
    offsetHeight: { configurable: true, value: 800 },
  });
  return { canvas, stage };
}

describe("FlowCanvas", () => {
  it("fits a wide flow, zooms, and resets to 100 percent", () => {
    render(<FlowCanvas><div>Flow tree</div></FlowCanvas>);
    setCanvasDimensions();

    fireEvent.click(screen.getByRole("button", { name: "Fit flow to view" }));
    expect(screen.getByText("44%")).toBeTruthy();

    fireEvent.click(screen.getByRole("button", { name: "Zoom in" }));
    expect(screen.getByText("54%")).toBeTruthy();

    fireEvent.click(screen.getByRole("button", { name: /Reset zoom to 100%/ }));
    expect(screen.getByText("100%")).toBeTruthy();
  });

  it("pans the stage by dragging the canvas background", () => {
    render(<FlowCanvas><div>Flow tree</div></FlowCanvas>);
    const { canvas, stage } = setCanvasDimensions();
    fireEvent.click(screen.getByRole("button", { name: /Reset zoom to 100%/ }));

    fireEvent.pointerDown(canvas, { button: 0, clientX: 400, clientY: 250, pointerId: 7 });
    fireEvent.pointerMove(canvas, { clientX: 500, clientY: 290, pointerId: 7 });
    fireEvent.pointerUp(canvas, { clientX: 500, clientY: 290, pointerId: 7 });

    expect(stage.style.transform).toContain("translate3d(-400px, 72px, 0)");
    expect(canvas.classList.contains("flow-canvas--dragging")).toBe(false);
  });

  it("keeps flow nodes clickable instead of treating them as pan handles", () => {
    render(<FlowCanvas><article className="flow-node">Menu node</article></FlowCanvas>);
    const { canvas, stage } = setCanvasDimensions();
    fireEvent.click(screen.getByRole("button", { name: /Reset zoom to 100%/ }));
    const node = screen.getByText("Menu node");

    fireEvent.pointerDown(node, { button: 0, clientX: 400, clientY: 250, pointerId: 8 });
    fireEvent.pointerMove(canvas, { clientX: 500, clientY: 290, pointerId: 8 });
    fireEvent.pointerUp(canvas, { clientX: 500, clientY: 290, pointerId: 8 });

    expect(stage.style.transform).toContain("translate3d(-500px, 72px, 0)");
  });

  it("supports keyboard pan, zoom, reset, and fit shortcuts", () => {
    render(<FlowCanvas><div>Flow tree</div></FlowCanvas>);
    const { canvas } = setCanvasDimensions();
    canvas.focus();

    fireEvent.keyDown(canvas, { key: "f" });
    expect(screen.getByText("44%")).toBeTruthy();
    fireEvent.keyDown(canvas, { key: "+" });
    expect(screen.getByText("54%")).toBeTruthy();
    fireEvent.keyDown(canvas, { key: "0" });
    expect(screen.getByText("100%")).toBeTruthy();
    fireEvent.keyDown(canvas, { key: "ArrowRight" });
    expect(screen.getByText("100%")).toBeTruthy();
  });
});

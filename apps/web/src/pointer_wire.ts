const MAGIC = 0xd1;
const VERSION = 1;
const OP_MOVE = 1;
const OP_WHEEL = 2;

function clampUnit(value: number): number {
  if (!Number.isFinite(value)) return 0;
  return Math.max(0, Math.min(1, value));
}

function quantizeUnit(value: number): number {
  return Math.round(clampUnit(value) * 0xffff);
}

export function encodePointerMove(x: number, y: number): ArrayBuffer {
  const buffer = new ArrayBuffer(7);
  const view = new DataView(buffer);
  view.setUint8(0, MAGIC);
  view.setUint8(1, VERSION);
  view.setUint8(2, OP_MOVE);
  view.setUint16(3, quantizeUnit(x), true);
  view.setUint16(5, quantizeUnit(y), true);
  return buffer;
}

export function encodePointerWheel(delta: number): ArrayBuffer {
  const buffer = new ArrayBuffer(5);
  const view = new DataView(buffer);
  const rounded = Number.isFinite(delta) ? Math.round(delta) : 0;
  const clamped = Math.max(-0x8000, Math.min(0x7fff, rounded));
  view.setUint8(0, MAGIC);
  view.setUint8(1, VERSION);
  view.setUint8(2, OP_WHEEL);
  view.setInt16(3, clamped, true);
  return buffer;
}

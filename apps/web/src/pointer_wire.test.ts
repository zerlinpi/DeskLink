import { describe, expect, it } from "vitest";
import { encodePointerMove, encodePointerWheel } from "./pointer_wire";

describe("pointer binary wire protocol", () => {
  it("encodes normalized pointer coordinates in seven bytes", () => {
    expect([...new Uint8Array(encodePointerMove(0, 1))]).toEqual([
      0xd1, 1, 1, 0x00, 0x00, 0xff, 0xff,
    ]);
    expect([...new Uint8Array(encodePointerMove(0.5, 0.25))]).toEqual([
      0xd1, 1, 1, 0x00, 0x80, 0x00, 0x40,
    ]);
  });

  it("clamps invalid or out-of-range coordinates", () => {
    expect([...new Uint8Array(encodePointerMove(-2, 9))].slice(3)).toEqual([
      0x00, 0x00, 0xff, 0xff,
    ]);
    expect([...new Uint8Array(encodePointerMove(Number.NaN, Number.POSITIVE_INFINITY))].slice(3)).toEqual([
      0x00, 0x00, 0x00, 0x00,
    ]);
  });

  it("encodes wheel deltas as signed little-endian int16", () => {
    expect([...new Uint8Array(encodePointerWheel(120))]).toEqual([
      0xd1, 1, 2, 0x78, 0x00,
    ]);
    expect([...new Uint8Array(encodePointerWheel(-600))]).toEqual([
      0xd1, 1, 2, 0xa8, 0xfd,
    ]);
    expect([...new Uint8Array(encodePointerWheel(100_000))].slice(3)).toEqual([
      0xff, 0x7f,
    ]);
  });
});

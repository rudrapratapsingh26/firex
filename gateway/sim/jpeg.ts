/**
 * The smallest honest JPEG this repository can produce without a codec: a
 * baseline, greyscale image of one flat grey level, with Huffman tables that
 * hold only the two symbols it uses. Browsers and image viewers decode it.
 * The simulator uses a different grey (and a comment) per frame, so every
 * frame has its own SHA-256, as a real camera's would.
 */

class Bits {
  private readonly bytes: number[] = [];
  private current = 0;
  private count = 0;

  put(value: number, length: number): void {
    for (let i = length - 1; i >= 0; i--) {
      this.current = (this.current << 1) | ((value >> i) & 1);
      if (++this.count === 8) this.flush();
    }
  }

  private flush(): void {
    this.bytes.push(this.current);
    if (this.current === 0xff) this.bytes.push(0x00); // byte stuffing
    this.current = 0;
    this.count = 0;
  }

  finish(): number[] {
    while (this.count !== 0) this.put(1, 1); // pad with ones
    return this.bytes;
  }
}

function segment(marker: number, body: number[]): number[] {
  const length = body.length + 2;
  return [0xff, marker, length >> 8, length & 0xff, ...body];
}

/** width and height are multiples of 8; grey is 0-255. */
export function flatGreyJpeg(width: number, height: number, grey: number, comment = ''): Buffer {
  const blocks = (width / 8) * (height / 8);
  const dc = Math.round(grey) - 128; // quantised by 8 below: the DC coefficient of a flat block is 8 x the level
  const magnitude = Math.abs(dc);
  const category = magnitude === 0 ? 0 : Math.floor(Math.log2(magnitude)) + 1;

  const quant = new Array(64).fill(1);
  quant[0] = 8;

  const out: number[] = [0xff, 0xd8];
  if (comment) out.push(...segment(0xfe, [...Buffer.from(comment, 'utf8')]));
  out.push(...segment(0xdb, [0x00, ...quant]));
  out.push(...segment(0xc0, [8, height >> 8, height & 0xff, width >> 8, width & 0xff, 1, 1, 0x11, 0]));

  /* DC table 0: two 2-bit codes, 00 = category 0, 01 = the image's category.
   * AC table 0: one 2-bit code, 00 = end of block. */
  const dcSymbols = category === 0 ? [0] : [0, category];
  const dcCounts = [0, dcSymbols.length, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
  out.push(...segment(0xc4, [0x00, ...dcCounts, ...dcSymbols]));
  out.push(...segment(0xc4, [0x10, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00]));
  out.push(...segment(0xda, [1, 1, 0x00, 0, 63, 0]));

  const bits = new Bits();
  for (let b = 0; b < blocks; b++) {
    if (b === 0 && category > 0) {
      bits.put(0b01, 2);
      bits.put(dc > 0 ? dc : (dc - 1) & ((1 << category) - 1), category);
    } else {
      bits.put(0b00, 2); // no DC change from the previous block
    }
    bits.put(0b00, 2); // end of block: every AC coefficient is zero
  }
  out.push(...bits.finish(), 0xff, 0xd9);
  return Buffer.from(out);
}

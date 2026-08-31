import assert from "node:assert/strict";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import { writeFileSync } from "node:fs";
import { PNG } from "pngjs";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..");

const COLORS = {
  R: [255, 0, 0, 255],
  G: [0, 255, 0, 255],
  B: [0, 0, 255, 255],
  Y: [255, 255, 0, 255],
  C: [0, 255, 255, 255],
  M: [255, 0, 255, 255],
  W: [255, 255, 255, 255],
};

const HERO = [
  "RRRRGGGG",
  "RCCMGWWG",
  "RCBMGMYG",
  "BYWMCCCG",
  "BBYMMCCW",
  "BBBBYYYY",
];
const COIN = [
  "WYYYW",
  "YRRRY",
  "YGCYY",
  "YGBCY",
  "YMMMY",
  "YYYYW",
  "WWYWW",
];

function encodePng(width, height, rgba) {
  assert.equal(rgba.length, width * height * 4);
  const png = new PNG({ width, height });
  rgba.copy(png.data);
  return PNG.sync.write(png);
}

function pixels(pattern) {
  const width = pattern[0].length;
  assert(pattern.every((row) => row.length === width));
  return {
    width,
    height: pattern.length,
    rgba: Buffer.from(pattern.flatMap((row) => [...row].flatMap((key) => COLORS[key]))),
  };
}

function drawNearest(canvas, source, left, top, scale) {
  for (let sy = 0; sy < source.height; ++sy) {
    for (let sx = 0; sx < source.width; ++sx) {
      const sourceOffset = (sy * source.width + sx) * 4;
      for (let dy = 0; dy < scale; ++dy) {
        for (let dx = 0; dx < scale; ++dx) {
          const x = left + sx * scale + dx;
          const y = top + sy * scale + dy;
          const targetOffset = (y * canvas.width + x) * 4;
          source.rgba.copy(canvas.rgba, targetOffset, sourceOffset, sourceOffset + 4);
        }
      }
    }
  }
}

function output(relative, bytes) {
  writeFileSync(join(ROOT, relative), bytes);
}

const hero = pixels(HERO);
const coin = pixels(COIN);
const canvas = {
  width: 256,
  height: 144,
  rgba: Buffer.alloc(256 * 144 * 4),
};
for (let offset = 3; offset < canvas.rgba.length; offset += 4) {
  canvas.rgba[offset] = 255;
}
drawNearest(canvas, hero, 56, 28, 8);
drawNearest(canvas, coin, 164, 52, 8);

output("sprites/hero.png", encodePng(hero.width, hero.height, hero.rgba));
output("sprites/coin.png", encodePng(coin.width, coin.height, coin.rgba));
output("tests/expected.png", encodePng(canvas.width, canvas.height, canvas.rgba));

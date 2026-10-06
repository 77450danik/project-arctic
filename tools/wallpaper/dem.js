// The terrain of the live wallpaper: Copernicus GLO-30 around Isfjorden,
// merged from its 1-degree tiles and resampled to a square metric grid
// centred on Longyearbyen (local east/north metres, equirectangular at the
// centre's latitude, good to a few metres over 40 km).
//
//   node dem.js <dem folder> <out folder> [size_km] [samples]
//
// Writes heights.f32 (samples x samples, metres, row 0 north), dem.json
// (centre, extent, cell) and hillshade.png to choose the camera from.

import fs from 'fs';
import path from 'path';
import { fromFile } from 'geotiff';
import { PNG } from 'pngjs';

const [demDir, outDir, sizeKm = '40', samplesArg = '2049'] = process.argv.slice(2);
const CENTRE = { lat: 78.2232, lon: 15.6469 };   // Longyearbyen
const size = Number(sizeKm) * 1000, N = Number(samplesArg);
const M_LAT = 111132.954 - 559.822 * Math.cos(2 * CENTRE.lat * Math.PI / 180);
const M_LON = 111412.84 * Math.cos(CENTRE.lat * Math.PI / 180);

const tiles = [];
for (const f of fs.readdirSync(demDir).filter(f => f.endsWith('.tif'))) {
    const tiff = await fromFile(path.join(demDir, f));
    const img = await tiff.getImage();
    const [x0, , , y0] = img.getBoundingBox();   // west, south, east, north
    const bbox = img.getBoundingBox();
    const data = (await img.readRasters({ interleave: true }));
    tiles.push({ w: img.getWidth(), h: img.getHeight(), west: bbox[0], south: bbox[1], east: bbox[2], north: bbox[3], data });
    console.log(f, img.getWidth(), 'x', img.getHeight(), bbox.map(v => v.toFixed(3)).join(' '));
}

function sample(lat, lon) {
    for (const t of tiles) {
        if (lon < t.west || lon >= t.east || lat <= t.south || lat > t.north) continue;
        const fx = (lon - t.west) / (t.east - t.west) * t.w - 0.5;
        const fy = (t.north - lat) / (t.north - t.south) * t.h - 0.5;
        const x = Math.max(0, Math.min(t.w - 2, Math.floor(fx))), y = Math.max(0, Math.min(t.h - 2, Math.floor(fy)));
        const ax = Math.min(1, Math.max(0, fx - x)), ay = Math.min(1, Math.max(0, fy - y));
        const g = (i, j) => t.data[(y + j) * t.w + x + i];
        return (g(0, 0) * (1 - ax) + g(1, 0) * ax) * (1 - ay) + (g(0, 1) * (1 - ax) + g(1, 1) * ax) * ay;
    }
    return 0;   // the sea, beyond the tiles
}

const heights = new Float32Array(N * N);
let min = Infinity, max = -Infinity;
for (let j = 0; j < N; j++) {
    const north = size / 2 - j * size / (N - 1);
    for (let i = 0; i < N; i++) {
        const east = -size / 2 + i * size / (N - 1);
        const h = sample(CENTRE.lat + north / M_LAT, CENTRE.lon + east / M_LON);
        heights[j * N + i] = h;
        if (h < min) min = h;
        if (h > max) max = h;
    }
}
fs.mkdirSync(outDir, { recursive: true });
fs.writeFileSync(path.join(outDir, 'heights.f32'), Buffer.from(heights.buffer));
fs.writeFileSync(path.join(outDir, 'dem.json'), JSON.stringify({ centre: CENTRE, size, samples: N, cell: size / (N - 1), min, max }, null, 1));

// a hillshade lit from the north-west, the sea in blue
const png = new PNG({ width: N, height: N }), cell = size / (N - 1);
for (let j = 0; j < N; j++) for (let i = 0; i < N; i++) {
    const h = (a, b) => heights[Math.max(0, Math.min(N - 1, b)) * N + Math.max(0, Math.min(N - 1, a))];
    const dx = (h(i + 1, j) - h(i - 1, j)) / (2 * cell), dy = (h(i, j + 1) - h(i, j - 1)) / (2 * cell);
    const nx = -dx, ny = dy, nz = 1, len = Math.hypot(nx, ny, nz);
    const shade = Math.max(0, (nx * -0.5 + ny * 0.5 + nz * 0.7) / len / 1.0);
    const o = (j * N + i) * 4, sea = h(i, j) <= 0.5;
    const v = Math.round(40 + 215 * shade * (0.6 + 0.4 * Math.min(1, h(i, j) / 900)));
    png.data[o] = sea ? 30 : v; png.data[o + 1] = sea ? 60 : v; png.data[o + 2] = sea ? 110 : v; png.data[o + 3] = 255;
}
fs.writeFileSync(path.join(outDir, 'hillshade.png'), PNG.sync.write(png));
console.log('heights', min.toFixed(1), '..', max.toFixed(1), 'm, cell', cell.toFixed(1), 'm');

#!/usr/bin/env node
// BBB-AIRGAP: convert the QR drawn by docs/sign/index.html into a grayscale frame readable by
// the emulator camera. Measure whether the device accepts the page's actual payload. Run the
// page's own script block on a mock DOM and reconstruct the frame from canvas rectangles,
// avoiding assumptions about equivalence to another encoder.
//
// Run: node pijade/tools/sign_qr_frame.js <page.html> <output.gray> [path] [message]
// Defaults use the page's self-test path and message.
//
// Frame format matches pijade/tools/epoch_qr.py write_gray() (320x240, 8-bit grayscale, centered
// code, dark modules 0x00, background 0xff). Both use the dimensions expected by jadectl.py;
// mismatches fail before writing. Do not call epoch_qr.py directly: it imports cbor2 and qrcode,
// while this measurement has no third-party dependencies.
'use strict';
const fs = require('fs');
const vm = require('vm');

const CAMERA_W = 320;
const CAMERA_H = 240;

const [, , pagePath, framePath, pathArg, messageArg] = process.argv;
if (!pagePath || !framePath) {
    console.error('usage: sign_qr_frame.js <page.html> <output.gray> [path] [message]');
    process.exit(2);
}
const PATH = pathArg !== undefined ? pathArg : "m/44'/0'/0'/0/0";
const MESSAGE = messageArg !== undefined ? messageArg : 'hello';

const html = fs.readFileSync(pagePath, 'utf8');
const start = html.indexOf('<script>');
const end = html.indexOf('</script>');
if (start < 0 || end < 0 || end < start) {
    console.error('script block not found in page');
    process.exit(2);
}
const code = html.slice(start + '<script>'.length, end);

// --- mock DOM ---------------------------------------------------------------------------------
let paints = [];
let canvasW = 0;
let canvasH = 0;
let fill = '#000';
const ctx = {
    set fillStyle(v) { fill = v; },
    get fillStyle() { return fill; },
    fillRect(x, y, w, h) { paints.push({ x, y, w, h, color: fill }); },
};
const elements = {};
function element(id) {
    if (!elements[id]) {
        elements[id] = {
            id, value: '', textContent: '', hidden: false,
            getContext() { return ctx; },
            addEventListener() {},
        };
    }
    return elements[id];
}
element('path').value = PATH;
element('message').value = MESSAGE;
// Setting a real canvas width also clears it; reproduce that behavior so a second QR draw
// cannot leave old rectangles in the frame.
Object.defineProperty(element('canvas'), 'width', {
    set(v) { canvasW = v; paints = []; }, get() { return canvasW; },
});
Object.defineProperty(element('canvas'), 'height', {
    set(v) { canvasH = v; }, get() { return canvasH; },
});

const sandbox = { document: { getElementById: element }, TextEncoder, console };
vm.createContext(sandbox);
vm.runInContext(code, sandbox, { filename: pagePath });

if (!elements.error.hidden) {
    console.error('page reported an error: ' + elements.error.textContent);
    process.exit(1);
}

// --- module matrix from painted rectangles -----------------------------------------------------
const background = paints.filter((b) => b.color === '#fff');
if (background.length !== 1 || background[0].w !== canvasW || background[0].h !== canvasH) {
    console.error('background is not a single white rectangle');
    process.exit(1);
}
const dark = paints.filter((b) => b.color === '#000');
if (!dark.length) { console.error('no dark modules drawn'); process.exit(1); }
const scale = dark[0].w;
if (dark.some((b) => b.w !== scale || b.h !== scale)) {
    console.error('module dimensions differ');
    process.exit(1);
}
if (canvasW !== canvasH || canvasW % scale !== 0) {
    console.error('canvas is not divisible by scale');
    process.exit(1);
}
const side = canvasW / scale;                 // side in modules, including quiet zone
const matrix = Array.from({ length: side }, () => new Uint8Array(side));
for (const b of dark) {
    if (b.x % scale || b.y % scale) { console.error('module does not align to grid'); process.exit(1); }
    matrix[b.y / scale][b.x / scale] = 1;
}

// --- grayscale frame -----------------------------------------------------------------------------------
const cameraScale = Math.floor(CAMERA_H / side);
if (cameraScale < 1) { console.error('code exceeds camera size: ' + side + ' modules'); process.exit(1); }
const sidePx = side * cameraScale;
const ofsX = Math.floor((CAMERA_W - sidePx) / 2);
const ofsY = Math.floor((CAMERA_H - sidePx) / 2);
const frame = Buffer.alloc(CAMERA_W * CAMERA_H, 0xff);
for (let y = 0; y < side; y++) {
    for (let x = 0; x < side; x++) {
        if (!matrix[y][x]) continue;
        for (let dy = 0; dy < cameraScale; dy++) {
            const start2 = (ofsY + y * cameraScale + dy) * CAMERA_W + ofsX + x * cameraScale;
            frame.fill(0x00, start2, start2 + cameraScale);
        }
    }
}
if (frame.length !== CAMERA_W * CAMERA_H) { console.error('incorrect frame size'); process.exit(1); }
fs.writeFileSync(framePath, frame);

console.log('string : ' + elements.payload.textContent);
console.log('measure : ' + elements.measure.textContent);
console.log('frame : ' + framePath + ', ' + frame.length + ' bytes, code ' + sidePx + 'x' + sidePx +
    ' px, top left (' + ofsX + ',' + ofsY + ')');

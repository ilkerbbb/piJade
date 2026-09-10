'use strict';
// BBB-AIRGAP: the pieces shared by the two tools that turn a published help page into a camera
// frame (sign_qr_frame.js, clock_qr_frame.js). Both run the page's own script on a mock DOM and
// rebuild the code from the rectangles it paints, so neither assumes the page agrees with another
// encoder. The scale rule lives here because it is a property of the scanner, not of either page:
// when it was duplicated per tool, one tool was changed and the others silently kept drawing
// codes the device could not read.
const fs = require('fs');
const vm = require('vm');

const CAMERA_W = 640;
const CAMERA_H = 480;

// quirc reads only the central square of the frame (SCAN_MARGIN, main/qrscan.c:13) and identifies
// nothing at 8 px per module or above (measured 2026-09-10 on build_linux: every QR version from
// 1 to 10 decodes at 3..7 px, none at 12 px), so the code is capped well below that rather than
// stretched to fill the frame.
const SCAN_WINDOW = Math.min(CAMERA_W, CAMERA_H) - 20;
const MAX_SCALE = 6;

// A canvas context starts black, and returns to black whenever the canvas is resized.
const DEFAULT_FILL = '#000';

// Exit 1 means the page was read but is not usable; exit 2 means the tool was called wrong.
function fail(message, code) {
    console.error(message);
    process.exit(code === undefined ? 1 : code);
}

// Run the page's single <script> block against a mock DOM.
// opts.values: element id -> value assigned before the script runs (form inputs).
// opts.sandbox: extra globals the page needs (Date, setInterval, ...).
function runPage(pagePath, opts) {
    const options = opts || {};
    const html = fs.readFileSync(pagePath, 'utf8');
    const start = html.indexOf('<script>');
    const end = html.indexOf('</script>');
    if (start < 0 || end < 0 || end < start) {
        fail('script block not found in page', 2);
    }
    const code = html.slice(start + '<script>'.length, end);

    const state = { paints: [], canvasW: 0, canvasH: 0 };
    let fill = DEFAULT_FILL;
    const ctx = {
        set fillStyle(v) { fill = v; },
        get fillStyle() { return fill; },
        fillRect(x, y, w, h) { state.paints.push({ x, y, w, h, color: fill }); },
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
    for (const [id, value] of Object.entries(options.values || {})) {
        element(id).value = value;
    }
    // Setting either real canvas dimension clears the bitmap AND resets the drawing state, so a
    // second draw cannot leave old rectangles or an old fill colour behind. Reproduce both halves:
    // a mock that only clears on width would accept a page the browser draws differently.
    function resetCanvas() { state.paints = []; fill = DEFAULT_FILL; }
    Object.defineProperty(element('canvas'), 'width', {
        set(v) { state.canvasW = v; resetCanvas(); }, get() { return state.canvasW; },
    });
    Object.defineProperty(element('canvas'), 'height', {
        set(v) { state.canvasH = v; resetCanvas(); }, get() { return state.canvasH; },
    });

    const sandbox = Object.assign(
        { document: { getElementById: element }, TextEncoder, console },
        options.sandbox || {});
    vm.createContext(sandbox);
    vm.runInContext(code, sandbox, { filename: pagePath });
    return { elements, state };
}

// Rebuild the module matrix from the painted rectangles, including the page's quiet zone.
function matrixFromPaints(state) {
    const { paints, canvasW, canvasH } = state;
    // Order matters, not just membership: a rectangle painted AFTER the modules can cover them on
    // the real page, and a reconstruction that only filtered by colour would still report a code
    // the visitor never sees. Rather than model what covers what, require exactly the shape both
    // pages draw: one white rectangle over the whole canvas first, then black modules and nothing
    // else. That is stricter than the covering rule on purpose - a page that repaints an already
    // white cell is rejected too, because the shape is no longer the one measured here.
    if (!paints.length) { fail('nothing was drawn'); }
    const background = paints[0];
    if (background.color !== '#fff' || background.x !== 0 || background.y !== 0
        || background.w !== canvasW || background.h !== canvasH) {
        fail('first paint is not a white rectangle covering the canvas');
    }
    const dark = paints.slice(1);
    const other = dark.find((b) => b.color !== '#000');
    if (other) { fail('unexpected paint colour after the background: ' + other.color); }
    if (!dark.length) { fail('no dark modules drawn'); }
    const scale = dark[0].w;
    if (dark.some((b) => b.w !== scale || b.h !== scale)) {
        fail('module dimensions differ');
    }
    if (canvasW !== canvasH || canvasW % scale !== 0) {
        fail('canvas is not divisible by scale');
    }
    const side = canvasW / scale;              // side in modules, including quiet zone
    const matrix = Array.from({ length: side }, () => new Uint8Array(side));
    for (const b of dark) {
        if (b.x % scale || b.y % scale) { fail('module does not align to grid'); }
        matrix[b.y / scale][b.x / scale] = 1;
    }
    return { matrix, side };
}

// Frame format matches pijade/tools/epoch_qr.py write_gray(): 640x480 8-bit grayscale, centered
// code, dark modules 0x00, background 0xff.
function writeFrame(matrix, side, framePath) {
    const cameraScale = Math.min(MAX_SCALE, Math.floor(SCAN_WINDOW / side));
    if (cameraScale < 1) { fail('code exceeds the scan window: ' + side + ' modules'); }
    const sidePx = side * cameraScale;
    const ofsX = Math.floor((CAMERA_W - sidePx) / 2);
    const ofsY = Math.floor((CAMERA_H - sidePx) / 2);
    const frame = Buffer.alloc(CAMERA_W * CAMERA_H, 0xff);
    for (let y = 0; y < side; y++) {
        for (let x = 0; x < side; x++) {
            if (!matrix[y][x]) continue;
            for (let dy = 0; dy < cameraScale; dy++) {
                const start = (ofsY + y * cameraScale + dy) * CAMERA_W + ofsX + x * cameraScale;
                frame.fill(0x00, start, start + cameraScale);
            }
        }
    }
    if (frame.length !== CAMERA_W * CAMERA_H) { fail('incorrect frame size'); }
    // Write beside the destination and rename, so a failed write cannot leave a truncated frame
    // that the next step would happily read as a measurement. The temporary name carries the pid
    // and is created exclusively, so two runs writing the same output cannot publish each other's
    // half-written buffer; a failure removes the temporary file instead of leaving it behind.
    const tmpPath = framePath + '.' + process.pid + '.tmp';
    let fd;
    try {
        fd = fs.openSync(tmpPath, 'wx');
        fs.writeFileSync(fd, frame);
        fs.closeSync(fd);
        fd = undefined;
        fs.renameSync(tmpPath, framePath);
    } catch (err) {
        if (fd !== undefined) { try { fs.closeSync(fd); } catch (ignored) { /* keep err */ } }
        try { fs.unlinkSync(tmpPath); } catch (ignored) { /* keep err */ }
        throw err;
    }
    return { cameraScale, sidePx, ofsX, ofsY, bytes: frame.length };
}

module.exports = { runPage, matrixFromPaints, writeFrame };

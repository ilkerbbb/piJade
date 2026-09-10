#!/usr/bin/env node
// BBB-AIRGAP: guard the reconstruction in qr_frame.js against the failure this round actually
// produced: a page-to-frame tool that keeps reporting success while the frame it writes is not
// what the page draws, or is not something the scanner can read. Each case runs the shared module
// against a synthetic page and asserts the outcome, so a future change to the mock DOM or to the
// scale rule fails here instead of in a device round.
//
// Run: node pijade/tools/qr_frame_test.js
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const { execFileSync } = require('child_process');

const MODULE = path.join(__dirname, 'qr_frame.js');
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'qrframe-'));

function page(body) {
    return '<html><body><canvas id="canvas"></canvas><script>\n' + body + '\n</script></body></html>';
}

// Each case runs in its own process because the module reports failure with process.exit.
function run(name, body, outName) {
    const pagePath = path.join(dir, name + '.html');
    fs.writeFileSync(pagePath, page(body));
    const framePath = path.join(dir, (outName || name) + '.gray');
    const runner = path.join(dir, name + '.run.js');
    fs.writeFileSync(runner, [
        'const q = require(' + JSON.stringify(MODULE) + ');',
        'const r = q.runPage(' + JSON.stringify(pagePath) + ', {});',
        'const m = q.matrixFromPaints(r.state);',
        'const p = q.writeFrame(m.matrix, m.side, ' + JSON.stringify(framePath) + ');',
        'console.log(JSON.stringify({ side: m.side, scale: p.cameraScale, sidePx: p.sidePx }));',
    ].join('\n'));
    try {
        const out = execFileSync('node', [runner], { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
        return { code: 0, out: out.trim(), framePath };
    } catch (e) {
        return { code: e.status, out: ((e.stdout || '') + (e.stderr || '')).trim(), framePath };
    }
}

// The third block sits at (3,9) rather than the finder-pattern (0,9) on purpose: three blocks in
// the corners are symmetric across the diagonal, so a reconstruction or a draw that swapped x and
// y would rebuild the same frame and this test would not notice. Measured 2026-09-10: with the
// symmetric layout both transpose mutations passed all twelve checks.
const HEALTHY = `
    const c = document.getElementById("canvas");
    c.width = 12; c.height = 12;
    const g = c.getContext("2d");
    g.fillStyle = "#fff"; g.fillRect(0,0,12,12);
    g.fillStyle = "#000"; g.fillRect(0,0,3,3); g.fillRect(9,0,3,3); g.fillRect(3,9,3,3);`;

// A white rectangle painted after the modules hides them on the real page.
const COVERED = HEALTHY + `
    g.fillStyle = "#fff"; g.fillRect(0,0,12,12);`;

// Anything that is neither the background nor a module means the page is not what we think.
const FOREIGN = HEALTHY + `
    g.fillStyle = "#f00"; g.fillRect(6,6,3,3);`;

// Assigning either dimension clears a real canvas and resets the fill to black, so what follows
// is a module painted on nothing: there is no background left to find.
const CLEARED_BY_HEIGHT = HEALTHY + `
    c.height = 12;
    g.fillRect(0,0,3,3);`;

// Assigning a dimension resets fillStyle to black as well as clearing the bitmap, so the rectangle
// below is painted BLACK even though white was the last colour set. If only the bitmap were
// cleared, the reconstruction would see a white background and no modules; if only the fill were
// reset, it would see the earlier background plus a full-canvas module. Each mutation reports a
// different message, so the expected message is what makes this case discriminating.
const FILL_RESET_BY_WIDTH = HEALTHY + `
    g.fillStyle = "#fff";
    c.width = 12;
    g.fillRect(0,0,12,12);`;

let failures = 0;
function check(label, ok, detail) {
    console.log((ok ? 'PASS ' : 'FAIL ') + label + (ok ? '' : '  <- ' + detail));
    if (!ok) failures++;
}

const healthy = run('healthy', HEALTHY);
check('healthy page writes a frame', healthy.code === 0 && fs.existsSync(healthy.framePath)
    && fs.statSync(healthy.framePath).size === 640 * 480, 'exit ' + healthy.code + ' ' + healthy.out);
check('healthy page stays under the scale cap',
    healthy.code === 0 && JSON.parse(healthy.out).scale <= 6, healthy.out);

// Size and exit status alone cannot tell a correct frame from an all-white one, so build the frame
// this page must produce and compare it byte for byte. The expectation is written out here rather
// than taken from the module: a test that asks the code under test what it should have done
// cannot catch the code doing nothing.
//
// Geometry, derived from the page above: every fillRect is one module, so the 3 px rectangles on a
// 12 px canvas make a 4 module code with three modules set. 4 modules at the 6 px cap is 24 px,
// centred in 640x480 puts the top left at (308, 228).
const EXPECT_SIDE = 4;
const EXPECT_SCALE = 6;
const EXPECT_MODULES = [[0, 0], [3, 0], [1, 3]];   // module coordinates, not pixels; asymmetric
function expectedFrame() {
    const px = EXPECT_SIDE * EXPECT_SCALE;
    const ofsX = (640 - px) / 2;
    const ofsY = (480 - px) / 2;
    const buf = Buffer.alloc(640 * 480, 0xff);
    for (const [mx, my] of EXPECT_MODULES) {
        for (let y = 0; y < EXPECT_SCALE; y++) {
            const row = ofsY + my * EXPECT_SCALE + y;
            const col = ofsX + mx * EXPECT_SCALE;
            buf.fill(0x00, row * 640 + col, row * 640 + col + EXPECT_SCALE);
        }
    }
    return buf;
}
const expected = expectedFrame();
const written = healthy.code === 0 && fs.existsSync(healthy.framePath)
    ? fs.readFileSync(healthy.framePath) : Buffer.alloc(0);
check('healthy frame matches the page pixel for pixel', Buffer.compare(written, expected) === 0,
    'dark pixels written ' + written.filter((v) => v === 0).length
    + ', expected ' + expected.filter((v) => v === 0).length);

for (const [label, body, expect] of [
    ['modules covered by a later background', COVERED, 'unexpected paint colour'],
    ['a colour that is neither module nor background', FOREIGN, 'unexpected paint colour'],
    ['canvas cleared through the height setter', CLEARED_BY_HEIGHT, 'first paint is not a white rectangle'],
    ['fill reset by the width setter', FILL_RESET_BY_WIDTH, 'first paint is not a white rectangle'],
]) {
    const r = run(label.replace(/\W+/g, '_'), body, 'rejected');
    check('rejects: ' + label, r.code === 1 && r.out.includes(expect), 'exit ' + r.code + ' ' + r.out);
    check('rejects: ' + label + ' (no frame written)', !fs.existsSync(r.framePath), 'a frame was written');
}

// A code too dense for the scan window has no readable scale at all.
const DENSE = `
    const c = document.getElementById("canvas");
    c.width = 600; c.height = 600;
    const g = c.getContext("2d");
    g.fillStyle = "#fff"; g.fillRect(0,0,600,600);
    g.fillStyle = "#000"; g.fillRect(0,0,1,1);`;
const dense = run('dense', DENSE, 'dense_out');
check('rejects: code denser than the scan window',
    dense.code === 1 && dense.out.includes('exceeds the scan window'), 'exit ' + dense.code + ' ' + dense.out);

fs.rmSync(dir, { recursive: true, force: true });
console.log(failures ? 'qr_frame_test: ' + failures + ' FAILED' : 'qr_frame_test: PASS');
process.exit(failures ? 1 : 0);

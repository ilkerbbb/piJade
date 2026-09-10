#!/usr/bin/env node
// BBB-AIRGAP: convert the QR drawn by docs/clock/index.html into a grayscale frame readable by
// the emulator camera, the clock-page counterpart of sign_qr_frame.js. Without it the clock page
// was the only published surface whose own drawing was never fed to the device: the page's
// self-test proves the string it encodes, not the code it paints.
//
// Run: node pijade/tools/clock_qr_frame.js <page.html> <output.gray> [epoch]
// The default epoch is the page's own self-test epoch, so the frame is reproducible; the page
// otherwise reads the wall clock.
'use strict';
const qrf = require('./qr_frame.js');

const [, , pagePath, framePath, epochArg] = process.argv;
if (!pagePath || !framePath) {
    console.error('usage: clock_qr_frame.js <page.html> <output.gray> [epoch]');
    process.exit(2);
}
const EPOCH = epochArg !== undefined ? Number(epochArg) : 1789000000;
if (!Number.isInteger(EPOCH) || EPOCH <= 0) {
    console.error('epoch must be a positive integer');
    process.exit(2);
}

// The page draws whatever Date.now() says and then redraws every second. Freeze the clock so the
// frame is reproducible, and drop the timer: one draw is what gets measured.
class FrozenDate extends Date {
    static now() { return EPOCH * 1000; }
}
const { elements, state } = qrf.runPage(pagePath, {
    sandbox: { Date: FrozenDate, setInterval() {} },
});

// The page replaces the canvas with a message when its own self-test fails; that must not be
// reported as a frame.
if (elements.clock.textContent === 'ERROR') {
    console.error("page self-test failed: " + elements['code-box'].textContent);
    process.exit(1);
}

const { matrix, side } = qrf.matrixFromPaints(state);
const placed = qrf.writeFrame(matrix, side, framePath);

console.log('epoch : ' + EPOCH + ' (' + elements.clock.textContent + ')');
console.log('frame : ' + framePath + ', ' + placed.bytes + ' bytes, code ' + placed.sidePx + 'x' +
    placed.sidePx + ' px, top left (' + placed.ofsX + ',' + placed.ofsY + ')');

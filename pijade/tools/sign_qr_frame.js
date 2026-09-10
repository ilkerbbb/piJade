#!/usr/bin/env node
// BBB-AIRGAP: convert the QR drawn by docs/sign/index.html into a grayscale frame readable by
// the emulator camera. Measure whether the device accepts the page's actual payload. The page's
// own script runs on a mock DOM and the frame is rebuilt from the canvas rectangles, avoiding
// assumptions about equivalence to another encoder.
//
// Run: node pijade/tools/sign_qr_frame.js <page.html> <output.gray> [path] [message]
// Defaults use the page's self-test path and message.
//
// The mock DOM, the matrix reconstruction and the frame geometry live in qr_frame.js, shared with
// clock_qr_frame.js. Do not call epoch_qr.py instead: it imports cbor2 and qrcode, while this
// measurement has no third-party dependencies.
'use strict';
const qrf = require('./qr_frame.js');

const [, , pagePath, framePath, pathArg, messageArg] = process.argv;
if (!pagePath || !framePath) {
    console.error('usage: sign_qr_frame.js <page.html> <output.gray> [path] [message]');
    process.exit(2);
}
const PATH = pathArg !== undefined ? pathArg : "m/44'/0'/0'/0/0";
const MESSAGE = messageArg !== undefined ? messageArg : 'hello';

const { elements, state } = qrf.runPage(pagePath, { values: { path: PATH, message: MESSAGE } });
if (!elements.error.hidden) {
    console.error('page reported an error: ' + elements.error.textContent);
    process.exit(1);
}

const { matrix, side } = qrf.matrixFromPaints(state);
const placed = qrf.writeFrame(matrix, side, framePath);

console.log('string : ' + elements.payload.textContent);
console.log('measure : ' + elements.measure.textContent);
console.log('frame : ' + framePath + ', ' + placed.bytes + ' bytes, code ' + placed.sidePx + 'x' +
    placed.sidePx + ' px, top left (' + placed.ofsX + ',' + placed.ofsY + ')');

#!/usr/bin/env node
// Acceptance test for the live camera on docs/sign/index.html.
//
// The other test (sign_verify_test.js) drives the page through its fields: it can check that a
// photograph fills the signature in, but a headless browser has no camera, so the path from
// getUserMedia through jsQR to the signature field is invisible to it. Measured 2026-09-10:
// removing the input event the camera dispatches after a successful read leaves that test green.
//
// So this one gives Chrome a camera. --use-file-for-fake-video-capture plays a Y4M file as a real
// capture device, and the Y4M here carries the golden signature QR as the device draws it on its
// own screen. Everything after the camera is the page's own code, unmodified: the same decoder,
// the same field, the same verification.
//
// Usage: sign_camera_test.js [page.html]
// CHROME=/path/to/chrome overrides the browser.

'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const zlib = require('zlib');
const { spawn } = require('child_process');

const REPO = path.resolve(__dirname, '..', '..');
const pagePath = process.argv[2] || path.join(REPO, 'docs', 'sign', 'index.html');
const goldenPath = path.join(REPO, 'test_data', 'sign_message_golden.json');
const qrPath = path.join(REPO, 'test_data', 'sign_message_golden_qr.png');
const CHROME = process.env.CHROME || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';

// The emulator's own camera contract, so the frame the page is handed is the size a real one is.
const CAM_W = 640;
const CAM_H = 480;
const TIMEOUT_MS = 90000;

// The run has to prove it reached the end, not merely that nothing it reached failed. A `return`
// or a throw partway through run() would otherwise leave the report short and green: every check
// that did run passed, and the ones that never ran cannot fail. So each check records its name and
// the reporter compares the whole list against the one below, in order.
const EXPECTED_CHECKS = [
    'the page came up with its self-test passing',
    'the fake capture device delivers frames to the page',
    'the camera fills the signature field with what it read',
    'the scan closes its own panel once it has an answer',
    'the camera is released rather than left running',
    'what the camera read is verified without another click',
    'a tab that goes away stops the camera with it',
    'a page that leaves puts its panel away as well as its camera',
    'a grant that arrives after the reader cancelled is stopped, not opened',
    'a grant that arrives after the tab hid is stopped, not opened',
    'what the reader types ends a scan that is still looking',
    'choosing a photograph ends a scan that is still looking',
    'a camera that stops on its own hands the button back and says so',
    'starting a scan supersedes a photograph still being read',
    'a running scan leaves neither launch button live to be pressed',
    'the address button fills the address field and leaves the signature alone',
    'the test module finished',
];

let failures = 0;
const done = [];
function check(name, ok, detail) {
    console.log((ok ? 'PASS  ' : 'FAIL  ') + name + (detail ? '   ' + detail : ''));
    done.push(name);
    if (!ok) { failures++; }
}

/* ---- the golden code, as pixels ------------------------------------------ */

// A truecolour, non-interlaced PNG at 8 bits a channel: the one shape test_data holds. Reading it
// here rather than depending on an image library keeps this test runnable wherever node is.
function readPng(file) {
    const data = fs.readFileSync(file);
    if (data.readUInt32BE(0) !== 0x89504e47) throw new Error('not a PNG');
    let pos = 8, width = 0, height = 0, idat = [];
    while (pos < data.length) {
        const len = data.readUInt32BE(pos);
        const type = data.toString('ascii', pos + 4, pos + 8);
        const body = data.subarray(pos + 8, pos + 8 + len);
        if (type === 'IHDR') {
            width = body.readUInt32BE(0);
            height = body.readUInt32BE(4);
            if (body[8] !== 8 || body[9] !== 2 || body[12] !== 0) {
                throw new Error('expected an 8-bit truecolour, non-interlaced PNG');
            }
        } else if (type === 'IDAT') {
            idat.push(body);
        }
        pos += 12 + len;
    }
    const raw = zlib.inflateSync(Buffer.concat(idat));
    const bpp = 3;
    const stride = width * bpp;
    const out = Buffer.alloc(width * height);          // one grey byte a pixel
    const line = Buffer.alloc(stride);
    const prev = Buffer.alloc(stride);
    for (let y = 0; y < height; y++) {
        const filter = raw[y * (stride + 1)];
        raw.copy(line, 0, y * (stride + 1) + 1, y * (stride + 1) + 1 + stride);
        for (let i = 0; i < stride; i++) {
            const a = i >= bpp ? line[i - bpp] : 0;
            const b = prev[i];
            const c = i >= bpp ? prev[i - bpp] : 0;
            let v = line[i];
            if (filter === 1) v += a;
            else if (filter === 2) v += b;
            else if (filter === 3) v += (a + b) >> 1;
            else if (filter === 4) {
                const p = a + b - c;
                const pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
                v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
            } else if (filter !== 0) throw new Error('unknown PNG filter ' + filter);
            line[i] = v & 0xff;
        }
        for (let x = 0; x < width; x++) {
            // The code is black on white, so any channel carries it; the green one is taken.
            out[y * width + x] = line[x * bpp + 1];
        }
        line.copy(prev);
    }
    return { width, height, gray: out };
}

// A camera does not see the code alone: it sees a screen inside a room. The code is placed at the
// size a phone held over the device would give it, on a mid-grey ground rather than white, so the
// decoder has to find the code rather than be handed a page-sized one.
function frameWithCode(code, sidePx) {
    const frame = Buffer.alloc(CAM_W * CAM_H, 128);
    const x0 = Math.floor((CAM_W - sidePx) / 2);
    const y0 = Math.floor((CAM_H - sidePx) / 2);
    for (let y = 0; y < sidePx; y++) {
        const sy = Math.min(code.height - 1, Math.floor(y * code.height / sidePx));
        for (let x = 0; x < sidePx; x++) {
            const sx = Math.min(code.width - 1, Math.floor(x * code.width / sidePx));
            frame[(y0 + y) * CAM_W + x0 + x] = code.gray[sy * code.width + sx];
        }
    }
    return frame;
}

function writeY4m(frame, file, frames) {
    const chroma = Buffer.alloc((CAM_W / 2) * (CAM_H / 2), 128);   // grey: no colour to carry
    const parts = [Buffer.from(`YUV4MPEG2 W${CAM_W} H${CAM_H} F10:1 Ip A1:1 C420mpeg2\n`)];
    for (let i = 0; i < frames; i++) {
        parts.push(Buffer.from('FRAME\n'), frame, chroma, chroma);
    }
    fs.writeFileSync(file, Buffer.concat(parts));
}

/* ---- driving the browser -------------------------------------------------- */

function sleep(ms) { return new Promise(resolve => setTimeout(resolve, ms)); }

// Waits for an outcome the browser produces on its own schedule, rather than for a fixed span. A
// fixed sleep passes on a quiet machine and then reports, on a loaded one, a defect the page does
// not have: measured 2026-09-10 in sign_verify_test.js, where a fixed 900 ms failed with the
// camera request still in flight. A probe that throws counts as not ready, because a page in the
// middle of navigating answers that way before it answers at all.
async function waitFor(ready, ms) {
    const giveUp = Date.now() + ms;
    while (Date.now() < giveUp) {
        try { if (await ready()) return true; } catch (e) { /* not there yet */ }
        await sleep(25);
    }
    return false;
}

async function devtoolsPort(dir, deadline) {
    const file = path.join(dir, 'DevToolsActivePort');
    while (Date.now() < deadline) {
        try {
            const line = fs.readFileSync(file, 'utf8').split('\n')[0].trim();
            if (line) return Number(line);
        } catch (e) { /* not written yet */ }
        await sleep(150);
    }
    throw new Error('Chrome never reported a debugging port');
}

async function connect(port, deadline) {
    while (Date.now() < deadline) {
        try {
            // Bounded by the same deadline as the loop around it: a browser that accepts the
            // connection and then stalls would otherwise park this await past the deadline the
            // loop is checking, and the run would never reach its report or its cleanup.
            const list = await (await fetch(`http://127.0.0.1:${port}/json/list`,
                { signal: AbortSignal.timeout(deadline - Date.now()) })).json();
            const page = list.find(t => t.type === 'page');
            if (page) return page.webSocketDebuggerUrl;
        } catch (e) { /* not listening yet */ }
        await sleep(150);
    }
    throw new Error('Chrome never offered a page target');
}

async function run() {
    if (!fs.existsSync(CHROME)) {
        console.error('Chrome not found at ' + CHROME + '; set CHROME to its path.');
        process.exit(2);
    }
    const golden = JSON.parse(fs.readFileSync(goldenPath, 'utf8'));
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'pijade-sign-camera-'));
    const y4m = path.join(dir, 'camera.y4m');
    const profile = path.join(dir, 'profile');

    const code = readPng(qrPath);
    // 360 px across a 480 px frame: the code fills three quarters of the shorter side, which is
    // what holding a phone over a 240 px screen gives.
    writeY4m(frameWithCode(code, 360), y4m, 40);

    const chrome = spawn(CHROME, ['--headless=new', '--disable-gpu', '--no-sandbox',
        '--remote-debugging-port=0', '--user-data-dir=' + profile,
        '--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream',
        '--use-file-for-fake-video-capture=' + y4m, 'about:blank'], { stdio: 'ignore' });

    const deadline = Date.now() + TIMEOUT_MS;
    let ws = null;
    try {
        const port = await devtoolsPort(profile, deadline);
        ws = new WebSocket(await connect(port, deadline));
        let id = 0;
        const waiting = new Map();
        // A command that is never answered has to fail rather than hang: a stalled renderer, or a
        // browser that went away, would otherwise leave this run parked forever inside an await,
        // never reaching the report and its completeness check. Both a per-command deadline and the
        // socket closing turn silence into an error, which run()'s catch reports as a failure.
        const CMD_MS = 20000;
        const fail = why => {
            for (const [n, entry] of waiting) { entry.reject(new Error(why)); waiting.delete(n); }
        };
        const send = (method, params = {}) => new Promise((resolve, reject) => {
            const n = ++id;
            const timer = setTimeout(() => {
                waiting.delete(n);
                reject(new Error('devtools did not answer ' + method + ' within ' + CMD_MS + 'ms'));
            }, CMD_MS);
            waiting.set(n, { resolve: r => { clearTimeout(timer); resolve(r); },
                             reject: e => { clearTimeout(timer); reject(e); } });
            ws.send(JSON.stringify({ id: n, method, params }));
        });
        ws.onmessage = e => {
            const msg = JSON.parse(e.data);
            if (msg.id && waiting.has(msg.id)) {
                const entry = waiting.get(msg.id);
                waiting.delete(msg.id);
                entry.resolve(msg.result);
            }
        };
        ws.onclose = () => fail('the devtools connection closed while a command was outstanding');
        // The handshake is the last startup step outside the per-command timers below, so it
        // carries the startup deadline itself; without it a socket that opens but never completes
        // leaves the run parked with no report and no cleanup.
        await new Promise((resolve, reject) => {
            const timer = setTimeout(() => reject(new Error('the devtools handshake did not finish'
                + ' before the ' + (TIMEOUT_MS / 1000) + ' second startup deadline')),
                Math.max(0, deadline - Date.now()));
            ws.onopen = () => { clearTimeout(timer); resolve(); };
            ws.onerror = () => { clearTimeout(timer); reject(new Error('the devtools socket failed'
                + ' to open')); };
        });
        const evaluate = expression => send('Runtime.evaluate', { expression, returnByValue: true })
            .then(r => (r && r.result) ? r.result.value : undefined);

        await send('Page.enable');
        await send('Emulation.setDeviceMetricsOverride',
            { width: 390, height: 900, deviceScaleFactor: 2, mobile: true });
        await send('Page.navigate', { url: 'file://' + pagePath });
        // window.piJadeVerify is the last thing the page publishes: its module is deferred, so it
        // runs after every classic script, the camera's among them. A filled payload would not do,
        // because the script that fills it runs first and the scan button would have no handler
        // yet; the one click this harness gives it is never retried.
        const ready = await waitFor(() => evaluate("!!window.piJadeVerify"), 20000);

        check('the page came up with its self-test passing',
            ready && await evaluate("document.getElementById('payload').textContent.length > 0"),
            ready ? await evaluate("document.getElementById('error').textContent.slice(0, 40)")
                  : 'the page never finished loading');

        // Every grant the page is given is kept here, so the lifecycle checks can look at the real
        // track instead of racing the page for it: a scan that decodes its first frame clears
        // srcObject before a poll a few hundred milliseconds later can see anything at all. The
        // wrapper hands the page exactly what the browser handed the wrapper, so the capture and
        // decode path under test is the real one; __gumDelay is how the late-grant check below
        // makes the browser answer after the reader has already cancelled.
        await evaluate(`(() => {
            const real = navigator.mediaDevices.getUserMedia.bind(navigator.mediaDevices);
            window.__grants = [];
            window.__gumDelay = 0;
            navigator.mediaDevices.getUserMedia = function (c) {
                return new Promise(function (resolve, reject) {
                    setTimeout(function () {
                        real(c).then(function (s) { window.__grants.push(s); resolve(s); }, reject);
                    }, window.__gumDelay);
                });
            };
        })()`);

        // The message has to be the one that was signed; the signature is what the camera brings.
        await evaluate(`(() => { const m = document.getElementById('message');
            m.value = ${JSON.stringify(golden.message)}; m.dispatchEvent(new Event('input')); })()`);
        const emptyBefore = await evaluate("document.getElementById('signature').value === ''");

        await evaluate("document.getElementById('scan-btn').click()");
        let sawVideo = false;
        let scanned = '';
        while (Date.now() < deadline) {
            await sleep(300);
            if (!sawVideo) {
                // Either the video is running or it has already finished its work; a scan fast
                // enough to decode and close between two polls still proves frames arrived.
                sawVideo = await evaluate("document.getElementById('scan-video').videoWidth > 0"
                    + " || document.getElementById('signature').value !== ''");
            }
            scanned = await evaluate("document.getElementById('signature').value");
            if (scanned) break;
        }

        // A scan that reaches the camera is one the browser answered: waiting on the grant is both
        // the cheapest signal and the one the checks below actually need.
        async function scanUntilGranted(n) {
            await evaluate("document.getElementById('scan-btn').click()");
            for (let i = 0; i < 20; i++) {
                if (await evaluate("window.__grants.length") >= n) return true;
                await sleep(250);
            }
            return false;
        }
        const trackState = i => evaluate("window.__grants.length > " + i
            + " ? window.__grants[" + i + "].getVideoTracks()[0].readyState : '(no grant)'");

        check('the fake capture device delivers frames to the page', sawVideo);
        check('the camera fills the signature field with what it read',
            emptyBefore && scanned === golden.signature_base64,
            scanned ? scanned.slice(0, 24) + '...' : '(nothing was read)');
        check('the scan closes its own panel once it has an answer',
            await evaluate("document.getElementById('scan-panel').hidden === true"));
        // Clearing srcObject on its own only proves the reference was dropped, not that the device
        // was stopped. Measured 2026-09-10: a module that only dropped it passed without this.
        check('the camera is released rather than left running',
            await evaluate("document.getElementById('scan-video').srcObject === null")
            && await evaluate("window.__grants.length === 1"
                + " && window.__grants[0].getVideoTracks()[0].readyState === 'ended'"),
            'track is ' + await evaluate("window.__grants.length"
                + " ? window.__grants[0].getVideoTracks()[0].readyState : '(no grant)'"));

        // Verification runs off the input event the camera dispatched, on the page's own schedule.
        // The page opens on m/44', and since ROADMAP item 69 the purpose in the path picks ONE
        // address to list rather than all three, so the legacy address is the one that has to
        // appear and the other two are the ones that must not. Measured 2026-09-11: this check
        // still demanded all three and had been failing since that change went in, unnoticed
        // because this test needs a real browser and a fake capture device to run at all.
        await waitFor(async () => (await evaluate(
            "document.getElementById('verify-out').textContent")).indexOf(golden.p2pkh) >= 0, 8000);
        const out = await evaluate("document.getElementById('verify-out').textContent");
        check('what the camera read is verified without another click',
            out.includes(golden.p2pkh) && !out.includes(golden.p2wpkh)
            && !out.includes(golden.p2sh_p2wpkh)
            && await evaluate("document.getElementById('verify-error').hidden === true"),
            out.replace(/\s+/g, ' ').slice(0, 44));

        // A camera left running behind a hidden tab keeps recording a room nobody is watching. The
        // scan stops the moment it reads something, so this needs a camera that finds nothing:
        // the decoder is stubbed out for the length of this check and then put back. The hidden
        // state itself is simulated, since a headless tab cannot be sent to the background.
        await evaluate("window.__realJsQR = jsQR; window.jsQR = function () { return null; }");
        const second = await scanUntilGranted(2);
        await evaluate("Object.defineProperty(document, 'hidden', "
            + "{ configurable: true, get: function () { return true; } });"
            + "document.dispatchEvent(new Event('visibilitychange'))");
        await sleep(300);
        check('a tab that goes away stops the camera with it',
            second && await evaluate("document.getElementById('scan-panel').hidden === true")
            && await evaluate("window.__grants[1].getVideoTracks()[0].readyState === 'ended'"),
            'track is ' + await trackState(1));

        // A page can leave without being closed: a browser may keep it alive on the back button and
        // restore it later. What is on screen when it comes back is whatever was on screen when it
        // left, so the panel has to be put away here too, not only the camera. The decoder is still
        // stubbed to find nothing, so the scan started below stays open until pagehide arrives.
        await evaluate("delete document.hidden");
        const third = await scanUntilGranted(3);
        await evaluate("window.dispatchEvent(new Event('pagehide'))");
        await sleep(300);
        check('a page that leaves puts its panel away as well as its camera',
            third && await evaluate("document.getElementById('scan-panel').hidden === true")
            && await evaluate("window.__grants[2].getVideoTracks()[0].readyState === 'ended'"),
            'panel is ' + await evaluate("document.getElementById('scan-panel').hidden ? 'closed' : 'still open'"));

        // The browser can still be asking for the camera when the reader gives up. Whatever it
        // hands over after that belongs to nobody: attaching it would open a camera behind a closed
        // panel, which is the same privacy defect as one left running. The grant is delayed here so
        // Cancel lands first, which is the order a slow permission prompt produces on its own.
        await evaluate("window.__gumDelay = 1200");
        await evaluate("document.getElementById('scan-btn').click()");
        await sleep(200);
        await evaluate("document.getElementById('scan-stop').click()");
        const cancelledEarly = await evaluate("document.getElementById('scan-panel').hidden === true");
        for (let i = 0; i < 20 && await evaluate("window.__grants.length") < 4; i++) await sleep(250);
        await sleep(300);
        check('a grant that arrives after the reader cancelled is stopped, not opened',
            cancelledEarly && await evaluate("window.__grants.length === 4")
            && await evaluate("window.__grants[3].getVideoTracks()[0].readyState === 'ended'")
            && await evaluate("document.getElementById('scan-video').srcObject === null")
            && await evaluate("document.getElementById('scan-panel').hidden === true")
            && await evaluate("document.getElementById('scan-btn').disabled === false"),
            'late track is ' + await trackState(3));

        // The same race against the tab rather than the button. A request still pending has no
        // stream yet, so a hidden-tab handler that waits for a live one lets the grant land behind
        // a tab nobody is looking at. Measured 2026-09-10: requiring a stream there passed every
        // other check in this file, which is why this one exists.
        await evaluate("document.getElementById('scan-btn').click()");
        await sleep(200);
        await evaluate("Object.defineProperty(document, 'hidden', "
            + "{ configurable: true, get: function () { return true; } });"
            + "document.dispatchEvent(new Event('visibilitychange'))");
        const hidEarly = await evaluate("document.getElementById('scan-panel').hidden === true");
        for (let i = 0; i < 20 && await evaluate("window.__grants.length") < 5; i++) await sleep(250);
        await sleep(300);
        check('a grant that arrives after the tab hid is stopped, not opened',
            hidEarly && await evaluate("window.__grants.length === 5")
            && await evaluate("window.__grants[4].getVideoTracks()[0].readyState === 'ended'")
            && await evaluate("document.getElementById('scan-video').srcObject === null")
            && await evaluate("document.getElementById('scan-panel').hidden === true"),
            'late track is ' + await trackState(4));
        await evaluate("delete document.hidden");
        await evaluate("window.__gumDelay = 0");

        // A scan that is still looking must give way to the reader: an answer they typed, pasted or
        // photographed is newer than anything the camera is about to find, and a code recognised a
        // moment later would otherwise overwrite it. Input.insertText reaches the page as a real
        // keystroke, so this exercises the same input event the Verify button also produces.
        // The field still holds the signature the camera read earlier, so it is emptied first. A
        // script assigning .value fires no input event, so nothing but the field changes here; the
        // typing below is what this check is about.
        await evaluate("document.getElementById('signature').value = ''");
        const sixth = await scanUntilGranted(6);
        await evaluate("document.getElementById('signature').focus()");
        await send('Input.insertText', { text: 'reader typed this' });
        await sleep(300);
        check('what the reader types ends a scan that is still looking',
            sixth && await evaluate("document.getElementById('scan-panel').hidden === true")
            && await evaluate("window.__grants[5].getVideoTracks()[0].readyState === 'ended'")
            && await evaluate("document.getElementById('signature').value === 'reader typed this'"),
            'field holds ' + await evaluate("JSON.stringify(document.getElementById('signature').value)"));

        // The same for the photograph, which is the other way an answer arrives while a scan runs.
        const seventh = await scanUntilGranted(7);
        await send('DOM.enable');
        const doc = await send('DOM.getDocument');
        const node = await send('DOM.querySelector', { nodeId: doc.root.nodeId, selector: '#photo' });
        await send('DOM.setFileInputFiles', { files: [qrPath], nodeId: node.nodeId });
        await sleep(400);
        // What the photograph decodes to is not the question here, and cannot be: the decoder is
        // still stubbed out so the camera finds nothing. The question is only whether choosing one
        // ends the scan. What a photograph reads is measured in sign_verify_test.js.
        check('choosing a photograph ends a scan that is still looking',
            seventh && await evaluate("document.getElementById('scan-panel').hidden === true")
            && await evaluate("window.__grants[6].getVideoTracks()[0].readyState === 'ended'"),
            'photo track is ' + await trackState(6)
            + ', panel ' + await evaluate("document.getElementById('scan-panel').hidden ? 'closed' : 'open'"));

        // A camera that goes away by itself, unplugged or revoked, ends its track without any of the
        // startup promises hearing about it. The event is dispatched here because a test cannot
        // unplug a webcam; what is being measured is whether the module listens for it at all.
        const eighth = await scanUntilGranted(8);
        await evaluate("window.__grants[7].getVideoTracks()[0].dispatchEvent(new Event('ended'))");
        await sleep(300);
        check('a camera that stops on its own hands the button back and says so',
            eighth && await evaluate("document.getElementById('scan-btn').disabled === false")
            && await evaluate("document.getElementById('scan-video').srcObject === null")
            && await evaluate("document.getElementById('scan-status').textContent")
                .then(t => (t || '').includes('Choose photo')),
            'status says ' + await evaluate("JSON.stringify(document.getElementById('scan-status').textContent)"));
        await evaluate("document.getElementById('scan-stop').click()");

        // The reverse of the photograph check above: a photograph still being read is older than a
        // scan started after it, and it ends by calling the page's verify path directly rather than
        // through an input event, so the close listener never sees it. The page's own way of saying
        // "what came before is superseded" is an input event on this field, so that event is what
        // this counts.
        await evaluate("window.__inputs = 0; document.getElementById('signature')"
            + ".addEventListener('input', function () { window.__inputs++; })");
        /* Counted in the same expression as the click, so only the handler's own synchronous
         * announcement can have arrived: a frame the camera goes on to read would announce itself
         * too, but no frame can be read while this script is still running. */
        const announced = await evaluate("(function () { document.getElementById('scan-btn').click();"
            + " return window.__inputs; })()");
        check('starting a scan supersedes a photograph still being read', announced >= 1,
            'the field saw ' + announced + ' input event(s) in the click itself');
        await evaluate("document.getElementById('scan-stop').click()");

        // One camera serves two fields, and which field it fills is fixed when the scan starts:
        // the guard at the top of startScan cannot change it once a stream is running. Measured
        // 2026-09-11 (Codex review of commit 7b9cc1ef): the other button stayed live, its press
        // returned at that guard without a word, and the code presented next landed in the field
        // of the button pressed first. Both buttons are out of reach while the panel holds the
        // camera, and both come back when it lets go. jsQR is still stubbed here, so the scans
        // below stay open instead of decoding and closing themselves.
        await evaluate("document.getElementById('scan-btn').click()");
        const bothDownForSignature = await waitFor(async () =>
            await evaluate("document.getElementById('scan-btn').disabled"
                + " && document.getElementById('expected-scan').disabled"), 5000);
        await evaluate("document.getElementById('scan-stop').click()");
        const bothBack = await evaluate("document.getElementById('scan-btn').disabled === false"
            + " && document.getElementById('expected-scan').disabled === false");
        await evaluate("document.getElementById('expected-scan').click()");
        const bothDownForAddress = await waitFor(async () =>
            await evaluate("document.getElementById('scan-btn').disabled"
                + " && document.getElementById('expected-scan').disabled"), 5000);
        await evaluate("document.getElementById('scan-stop').click()");
        check('a running scan leaves neither launch button live to be pressed',
            bothDownForSignature && bothBack && bothDownForAddress,
            'signature scan ' + bothDownForSignature + ', released ' + bothBack
            + ', address scan ' + bothDownForAddress);

        await evaluate("window.jsQR = window.__realJsQR");

        // ROADMAP item 74: the address field has a camera button of its own, and which field a
        // read lands in is decided when the scan starts. The capture device here carries a
        // signature rather than an address, which is exactly what makes the check sharp: what was
        // read has to land in the address field anyway, because the button pressed was that
        // field's. A module that kept writing to the signature field would fail both halves.
        await evaluate("document.getElementById('signature').value = '';"
            + " document.getElementById('expected').value = '';");
        await evaluate("document.getElementById('expected-scan').click()");
        const landed = await waitFor(async () =>
            await evaluate("document.getElementById('expected').value") === golden.signature_base64, 15000);
        check('the address button fills the address field and leaves the signature alone',
            landed && await evaluate("document.getElementById('signature').value") === '',
            'address field holds ' + JSON.stringify(
                (await evaluate("document.getElementById('expected').value")).slice(0, 24)));
        await evaluate("document.getElementById('scan-stop').click()");

        check('the test module finished', true);
    } finally {
        if (ws) { try { ws.close(); } catch (e) { /* already gone */ } }
        chrome.kill('SIGKILL');
        // The browser is still flushing its profile as it dies, so the directory can refuse to go
        // on the first attempt. Tidying up is not a measurement: a leftover directory under the
        // system temp path must never turn a good run into a failure. Same reasoning, and the same
        // shape, as the cleanup in sign_verify_test.js.
        try {
            fs.rmSync(dir, { recursive: true, force: true, maxRetries: 10, retryDelay: 100 });
        } catch (err) { /* the operating system clears its own temp directory */ }
    }
}

run().then(() => {
    const complete = done.length === EXPECTED_CHECKS.length
        && done.every((name, i) => name === EXPECTED_CHECKS[i]);
    if (!complete) {
        failures++;
        console.log('FAIL  the report was complete and in order   ran ' + done.length
            + ' of ' + EXPECTED_CHECKS.length
            + (done.length < EXPECTED_CHECKS.length
                ? ', missing: ' + EXPECTED_CHECKS.filter(n => done.indexOf(n) < 0).join(', ')
                : ''));
    } else {
        console.log('PASS  the report was complete and in order');
    }
    console.log('FAILURES: ' + failures);
    process.exit(failures ? 1 : 0);
}).catch(err => {
    check('the test itself ran', false, String(err && err.message));
    console.log('FAILURES: ' + failures);
    process.exit(1);
});

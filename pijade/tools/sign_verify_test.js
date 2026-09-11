#!/usr/bin/env node
// BBB-AIRGAP: prove that the signature reader in docs/sign/index.html gives the right answer, in a
// real browser, from a real device screen. The page is loaded as a file:// URL, which is how a
// saved copy is used, and the test drives the page's own recoverAddresses() rather than a
// reimplementation of it: a copy would pass while the page was broken.
//
// Run: node pijade/tools/sign_verify_test.js [page.html]
// CHROME=/path/to/chrome overrides the browser.
//
// What is measured:
//   1. every embedded third-party block still hashes to the pinned value (nothing edited in place)
//   2. the compose script is still the page's first <script>, which sign_qr_frame.js depends on
//   3. the golden signature recovers the golden public key and all three addresses
//   4. malformed signatures are refused, and a wrong message gives different addresses
//   5. the device's own signature screen decodes back to the golden signature, upright and rotated
'use strict';
const { spawn } = require('child_process');
const crypto = require('crypto');
const fs = require('fs');
const http = require('http');
const os = require('os');
const path = require('path');

const REPO = path.resolve(__dirname, '..', '..');
const pagePath = process.argv[2] || path.join(REPO, 'docs', 'sign', 'index.html');
const goldenPath = path.join(REPO, 'test_data', 'sign_message_golden.json');
const qrPath = path.join(REPO, 'test_data', 'sign_message_golden_qr.png');

const CHROME = process.env.CHROME || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';

// Hashes of the embedded third-party code, from the sources and dates in the page's headers. These
// are the whole point of the marker comments in the page: an edit inside a vendored block is a
// silent fork of somebody else's audited code, so it has to fail here rather than ship.
const PINNED = {
    'qrcodegen': '2511bc17f40a3c41d4a0578995db956b38997334d3d20113a5d4dc5c49c69480',
    'crypto-js-core': '2638bdd7224472f801890636d42df16c144ca46c84229ef3087190d21f935daf',
    'crypto-js-sha256': 'd8074f23cbd27aa6c2f231b8bdad34bbd8d49c7e47ffa0d7cd07a2c4d1d3c798',
    'crypto-js-ripemd160': '3d7eea732acb8f27ba7be5ec54cc1c5ea198be2f7b12ae78cf03ce3b87d78da1',
    'jsQR': 'bc40c8a15196236b2314db0856f72ca0b49980cd5413b8c852a7349f5fee0859',
    'noble-secp256k1': 'ba5aadb469e9208f2da1c9878ab6bb93d2c894f84e6c7ebf9dbf9462741b9fb7',
};

// Every check the page-side module is expected to report, in the order it reports them. The list
// is what makes an incomplete run visible: the module marks itself finished as its last act, but
// a `return` in the middle would still reach that marker with checks silently missing. Adding a
// check to the harness means adding its name here, and forgetting fails the run rather than
// quietly shrinking it.
const EXPECTED_CHECKS = [
    'page self-test',
    'golden public key',
    'golden P2PKH',
    'golden P2SH-P2WPKH',
    'golden P2WPKH',
    'a changed message changes the addresses',
    'a flipped signature byte does not still match',
    'refuses an uncompressed header',
    'refuses a BIP137 header',
    'refuses a short signature',
    'refuses text that is not base64',
    'oversized signature text is refused before base64 decoding',
    'signature and address markup stays inert',
    'device screen decodes to the golden signature',
    'rotated and noisy screen still decodes',
    'oversized photographs are refused before image decoding',
    'large photographs are downscaled before QR decoding',
    'under a purpose-44 path the page shows the legacy address alone',
    'under a path with no purpose the page lists all three addresses',
    'the preset buttons carry one path each',
    'a preset writes its path and the result follows it',
    'the preset in use is the only one marked',
    'a path typed by hand clears the marks',
    'typing a preset path back marks it again',
    'Verify with an empty signature says what is missing',
    'an edit alone does not light the result up',
    'Verify lights the result up',
    'an upper-case bech32 address still matches',
    'only the matching row is marked',
    'a match on another script type than the path names says so',
    'the verdict does not claim the signature came from that path',
    'a foreign address is reported as no match',
    'a photograph of the screen fills the signature in',
    'a photo with no code in it says so',
    'a failed photo clears the answer it replaces',
    'the old answer does not return when another field is edited',
    'a photograph does not overwrite what was typed after it',
    'a superseded read leaves no notice behind',
    'a failed photo does not overwrite what was typed after it',
    'a second photo wins over an earlier successful read',
    'a second photo wins over an earlier failed read',
    'mixed-case bech32 is refused, not lower-cased into a match',
    'a letter that is not ASCII is refused, not folded into an address',
    'trailing whitespace is part of the message, not trimmed away',
    'an empty message is refused rather than verified',
    '192 bytes is refused, 191 is not',
    'the limit counts bytes, not characters',
    'the code panel opens and closes on its own button',
    'the verify button runs the same check an edit does',
    'a camera that will not start hands the button back',
    'the address field carries a scan button of its own',
    'the address button opens the same panel, named for the address',
    'a camera that will not start hands the address button back',
    'the address reader is not sent to Choose photo, which fills the signature',
    'the test module finished',
];

let failures = 0;
function check(name, ok, detail) {
    console.log((ok ? 'PASS  ' : 'FAIL  ') + name + (detail ? '   ' + detail : ''));
    if (!ok) { failures++; }
}

const page = fs.readFileSync(pagePath, 'utf8');
const golden = JSON.parse(fs.readFileSync(goldenPath, 'utf8'));

// ---- 1. embedded blocks -----------------------------------------------------------------------
for (const [name, want] of Object.entries(PINNED)) {
    const begin = page.indexOf('/* piJade-vendor-begin ' + name + ' ');
    const end = page.indexOf('/* piJade-vendor-end ' + name + ' */');
    if (begin === -1 || end === -1 || end < begin) {
        check('embedded ' + name, false, 'markers not found in the page');
        continue;
    }
    // The block is what lies between the end of the begin marker's line and the end marker; the
    // assembler wrote a newline after each, so both are stripped back off here.
    const from = page.indexOf('\n', begin) + 1;
    const got = crypto.createHash('sha256').update(page.slice(from, end - 1), 'utf8').digest('hex');
    check('embedded ' + name, got === want, got === want ? '' : 'sha256 ' + got);
}

// The guide describes this device and this page and no other wallet by name (Ilker, 2026-09-11);
// the walkthrough it replaced was written around one, which left a reader of the page following
// instructions for software they may not run.
{
    const named = page.match(/sparrow/gi);
    check('the page names no other wallet', named === null,
        named ? named.length + ' mentions' : '');
}

// ---- 2. script order --------------------------------------------------------------------------
// sign_qr_frame.js takes the page's first <script> block and runs it on a mock DOM. If a vendored
// block ever moves above the compose script, that tool silently measures the wrong code.
{
    const first = page.indexOf('<script>');
    const firstEnd = page.indexOf('</script>');
    const block = page.slice(first, firstEnd);
    check('compose script is still the first <script>',
        first !== -1 && block.includes('function buildPayload') && block.includes('qrcodegen'));
}

// ---- 3-5. behaviour, in a browser -------------------------------------------------------------
if (!fs.existsSync(CHROME)) {
    console.error('Chrome not found at ' + CHROME + '; set CHROME to its path.');
    process.exit(2);
}

const qrDataUrl = 'data:image/png;base64,' + fs.readFileSync(qrPath).toString('base64');

// The assertions run as one more module appended to a copy of the real page, so everything above
// them is the page exactly as it ships.
// The page reports back over a loopback socket instead of leaving its answer in the DOM for
// --dump-dom to collect. Measured 2026-09-10: under Chrome's virtual clock the DOM is dumped
// while image decoding is still in flight, so three runs in ten of an unchanged page came back
// truncated. A run that ends when the page says it has ended cannot do that.
const harness = `
<script>
// Installed as a classic script, which runs while the page is still parsing and so is in place
// before any module executes: an error thrown by the page itself is a result, not a silence.
window.__pageErrors = [];
window.addEventListener('error', e => window.__pageErrors.push(e.message));
window.addEventListener('unhandledrejection', e => window.__pageErrors.push('rejected: ' + e.reason));
</script>
<script type="module">
const golden = ${JSON.stringify(golden)};
const results = [];
let blankBlob;
// Collect checks as they run; report sends them after completion or an escaping exception.
function check(name, ok, detail) { results.push({ name, ok: !!ok, detail: detail || '' }); }
function report() {
  for (const message of window.__pageErrors) { results.push({ name: 'the page threw nothing', ok: false, detail: message }); }
  results.push({ name: 'the test module finished', ok: true, detail: '' });
  // no-cors because this page's origin is a file, which the loopback server is not: the request
  // still arrives, and its reply is of no interest here.
  fetch('\${REPORT_URL}', { method: 'POST', mode: 'no-cors', body: JSON.stringify(results) });
}
function refuses(name, message, signature, fragment) {
  try {
    window.piJadeVerify.recoverAddresses(message, signature);
    check(name, false, 'accepted it');
  } catch (e) {
    check(name, e.message.includes(fragment), e.message);
  }
}
async function blobFromDataUrl(url) { return await (await fetch(url)).blob(); }
async function run() {
  if (!window.piJadeVerify) { check('page exposes its verifier', false, 'self-test failed or blocked'); return; }
  check('page self-test', window.piJadeVerify.selfTest());
  const el = id => document.getElementById(id);
  const type = (id, value) => { el(id).value = value; el(id).dispatchEvent(new Event('input')); };

  const r = window.piJadeVerify.recoverAddresses(golden.message, golden.signature_base64);
  check('golden public key', r.pubkeyHex === golden.pubkey_hex, r.pubkeyHex);
  check('golden P2PKH', r.p2pkh === golden.p2pkh, r.p2pkh);
  check('golden P2SH-P2WPKH', r.p2shP2wpkh === golden.p2sh_p2wpkh, r.p2shP2wpkh);
  check('golden P2WPKH', r.p2wpkh === golden.p2wpkh, r.p2wpkh);

  // A different message recovers a different key from the same signature. That is what makes the
  // message field load-bearing rather than decoration.
  const other = window.piJadeVerify.recoverAddresses(golden.message + '!', golden.signature_base64);
  check('a changed message changes the addresses', other.p2pkh !== r.p2pkh, other.p2pkh);

  // One flipped bit in the signature.
  const raw = atob(golden.signature_base64).split('');
  raw[40] = String.fromCharCode(raw[40].charCodeAt(0) ^ 1);
  const flipped = btoa(raw.join(''));
  let flippedSame = false;
  try { flippedSame = window.piJadeVerify.recoverAddresses(golden.message, flipped).p2pkh === r.p2pkh; }
  catch (e) { flippedSame = false; }
  check('a flipped signature byte does not still match', !flippedSame);

  refuses('refuses an uncompressed header', golden.message, btoa(String.fromCharCode(27) + atob(golden.signature_base64).slice(1)), 'uncompressed');
  refuses('refuses a BIP137 header', golden.message, btoa(String.fromCharCode(39) + atob(golden.signature_base64).slice(1)), 'BIP137');
  refuses('refuses a short signature', golden.message, btoa(atob(golden.signature_base64).slice(0, 64)), '64');
  refuses('refuses text that is not base64', golden.message, 'not a signature!!', 'base64');

  // The raw string must be bounded before atob allocates a decoded copy, including when the
  // excess is only whitespace: trimming in the DOM handler must not bypass the bound.
  const padded = window.piJadeVerify.recoverAddresses(golden.message,
    golden.signature_base64.padEnd(1024, ' '));
  const originalAtob = window.atob;
  let base64Calls = 0, oversizedRefused = false;
  window.atob = text => { base64Calls++; return originalAtob(text); };
  try {
    try { window.piJadeVerify.recoverAddresses(golden.message, 'A'.repeat(1025)); }
    catch (e) { oversizedRefused = e.message.includes('1024'); }
    type('message', golden.message);
    type('signature', golden.signature_base64.padEnd(1025, ' '));
    check('oversized signature text is refused before base64 decoding',
      oversizedRefused && base64Calls === 0 && padded.p2wpkh === golden.p2wpkh
        && !el('verify-error').hidden && el('verify-out').hidden);
  } finally { window.atob = originalAtob; }

  const markup = '<img src=x onerror="window.__signatureMarkup=true">';
  type('signature', markup);
  const signatureRefused = !el('verify-error').hidden && el('verify-out').hidden;
  type('signature', golden.signature_base64);
  type('expected', markup);
  check('signature and address markup stays inert',
    signatureRefused && !el('verify-out').hidden
      && el('verify-out').textContent.includes(markup)
      && el('verify-out').querySelectorAll('img, script').length === 0
      && el('verify-error').querySelectorAll('img, script').length === 0
      && !window.__signatureMarkup);

  // The device's own screen, through the same reader the page uses.
  const blob = await blobFromDataUrl(${JSON.stringify(qrDataUrl)});
  const decoded = await window.piJadeVerify.decodeSignatureFromFile(blob);
  check('device screen decodes to the golden signature', decoded === golden.signature_base64, String(decoded).slice(0, 24) + '...');

  // The same image upside down with noise on it: a photograph is never pixel perfect, and the
  // reader must not depend on the code arriving the right way up.
  const bitmap = await createImageBitmap(blob);
  const c = document.createElement('canvas');
  // Drawn at the device's own 240 px, which is both the realistic size and cheap enough that the
  // noise loop below stays well inside the browser's time budget.
  c.width = 240; c.height = 240;
  const g = c.getContext('2d', { willReadFrequently: true });
  g.translate(c.width / 2, c.height / 2);
  g.rotate(Math.PI);
  g.drawImage(bitmap, -c.width / 2, -c.height / 2, c.width, c.height);
  const img = g.getImageData(0, 0, c.width, c.height);
  for (let i = 0; i < img.data.length; i += 4) {
    const n = (Math.random() * 60) | 0;
    img.data[i] = Math.min(255, Math.max(0, img.data[i] + n - 30));
    img.data[i + 1] = img.data[i]; img.data[i + 2] = img.data[i];
  }
  g.putImageData(img, 0, 0);
  // toDataURL, not toBlob: measured 2026-09-10, toBlob's callback never fires under the
  // browser's virtual clock, so the run stops here in about half of its attempts. toDataURL
  // returns synchronously and the fetch that follows is a request the clock does wait for.
  const noisy = await blobFromDataUrl(c.toDataURL('image/png'));
  const decoded2 = await window.piJadeVerify.decodeSignatureFromFile(noisy);
  check('rotated and noisy screen still decodes', decoded2 === golden.signature_base64, String(decoded2).slice(0, 24) + '...');
  bitmap.close();

  // Observe the decoder boundary directly. A missing cap must fail even though the device's
  // small golden screen would decode successfully without either resource limit.
  const originalBitmap = window.createImageBitmap;
  let imageCalls = 0, largePhotoRefused = false;
  window.createImageBitmap = async () => { imageCalls++; throw new Error('unexpected decode'); };
  try {
    try { await window.piJadeVerify.decodeSignatureFromFile({ size: 30 * 1024 * 1024 + 1 }); }
    catch (e) { largePhotoRefused = e instanceof RangeError; }
    check('oversized photographs are refused before image decoding', largePhotoRefused && imageCalls === 0);
  } finally { window.createImageBitmap = originalBitmap; }

  c.width = 2800; c.height = 2000;
  const largePhoto = await blobFromDataUrl(c.toDataURL('image/png'));
  const originalJsQR = window.jsQR;
  let decodeSize;
  window.jsQR = (data, w, h) => { decodeSize = [w, h, data.length]; return null; };
  try {
    await window.piJadeVerify.decodeSignatureFromFile(largePhoto);
    check('large photographs are downscaled before QR decoding',
      !!decodeSize && decodeSize[0] === 1400 && decodeSize[1] === 1000
        && decodeSize[2] === 1400 * 1000 * 4);
  } finally { window.jsQR = originalJsQR; }

  // The page itself, not just the function underneath it. Typing into the fields is what a reader
  // actually does, and the match, the no-match and the missing-message paths each render something
  // different; a verifier that is right while the page shows the wrong thing is still wrong.
  type('message', golden.message);
  type('signature', golden.signature_base64);
  type('expected', '');
  // The path names the script type (ROADMAP item 69): under 44' the page shows the legacy address
  // and nothing else, so a reader is not sent looking for a native segwit address their wallet
  // never had. A path the page cannot read that way lists all three, as it always did.
  type('path', golden.path);
  const one = el('verify-out').textContent;
  check('under a purpose-44 path the page shows the legacy address alone',
    !el('verify-out').hidden && one.includes(golden.p2pkh) && !one.includes(golden.p2sh_p2wpkh)
      && !one.includes(golden.p2wpkh) && el('verify-out').querySelectorAll('.addr').length === 1,
    one.slice(0, 60));
  type('path', "m/0/0");
  const listed = el('verify-out').textContent;
  check('under a path with no purpose the page lists all three addresses',
    !el('verify-out').hidden && listed.includes(golden.p2pkh) && listed.includes(golden.p2sh_p2wpkh)
      && listed.includes(golden.p2wpkh) && el('verify-out').querySelectorAll('.addr').length === 3);
  type('path', golden.path);

  // ROADMAP item 69d: the reader picks the address type instead of recalling a path. A preset has
  // to do exactly what typing does, since both the code above and the result below hang off the
  // field's input event, and the mark has to follow the field rather than the last click, or a
  // path edited by hand would still show a type as chosen.
  const presets = Array.from(document.querySelectorAll('.preset'));
  const marked = () => presets.filter(b => b.getAttribute('aria-pressed') === 'true')
    .map(b => b.dataset.path);
  check('the preset buttons carry one path each',
    presets.length === 3 && presets.map(b => b.dataset.path).join(' ')
      === "m/44'/0'/0'/0/0 m/49'/0'/0'/0/0 m/84'/0'/0'/0/0",
    presets.map(b => b.dataset.path).join(' '));
  presets[2].click();
  const native = el('verify-out').textContent;
  check('a preset writes its path and the result follows it',
    el('path').value === "m/84'/0'/0'/0/0" && !el('verify-out').hidden
      && native.includes(golden.p2wpkh) && !native.includes(golden.p2pkh)
      && el('verify-out').querySelectorAll('.addr').length === 1,
    el('path').value + ' ' + native.slice(0, 40));
  check('the preset in use is the only one marked',
    marked().length === 1 && marked()[0] === "m/84'/0'/0'/0/0", marked().join(' '));
  type('path', "m/0/0");
  check('a path typed by hand clears the marks', marked().length === 0, marked().join(' '));
  type('path', golden.path);
  check('typing a preset path back marks it again',
    marked().length === 1 && marked()[0] === golden.path, marked().join(' '));

  // The Verify button has to be seen to do something, even though every edit already verified:
  // with nothing to check it says so, with a result it lights the result up.
  type('signature', '');
  el('verify-btn').click();
  check('Verify with an empty signature says what is missing',
    !el('verify-error').hidden && el('verify-error').textContent.includes('Nothing to verify'),
    el('verify-error').textContent.slice(0, 40));
  type('signature', golden.signature_base64);
  check('an edit alone does not light the result up', !el('verify-out').classList.contains('flash'));
  el('verify-btn').click();
  check('Verify lights the result up',
    !el('verify-out').hidden && el('verify-out').classList.contains('flash')
      && el('verify-out').textContent.includes(golden.p2pkh));

  // Upper case, the way an address is often printed or copied out of a receipt.
  type('expected', golden.p2wpkh.toUpperCase());
  const matchRow = el('verify-out').querySelector('.addr.match');
  check('an upper-case bech32 address still matches',
    el('verify-out').textContent.includes('Match.') && !!matchRow && matchRow.textContent.includes(golden.p2wpkh),
    el('verify-out').querySelector('.verdict').textContent.slice(0, 40));
  check('only the matching row is marked', el('verify-out').querySelectorAll('.addr.match').length === 1);
  // The match is real, the key is the same; but the path says 44', so the page says which address
  // that path's wallet would show instead of leaving the reader to wonder why the two differ.
  check('a match on another script type than the path names says so',
    el('verify-out').querySelector('.verdict').textContent.includes('where a wallet would show the legacy address instead'),
    el('verify-out').querySelector('.verdict').textContent.slice(-80));
  // The path is the field's text, not something the signature carries; a verdict that claimed the
  // key belongs to the path would be an unprovable claim (Codex review, 2026-09-11).
  check('the verdict does not claim the signature came from that path',
    el('verify-out').querySelector('.verdict').textContent.includes('does not say which path made it'),
    el('verify-out').querySelector('.verdict').textContent.slice(0, 60));

  type('expected', '1BvBMSEYstWetqTFn5Au4m4GFg7xJaNVN2');
  check('a foreign address is reported as no match',
    el('verify-out').textContent.includes('No match.')
      && el('verify-out').querySelectorAll('.addr.match').length === 0,
    el('verify-out').querySelector('.verdict').textContent.slice(0, 40));

  // The file input itself, driven the way a reader drives it. Assigning a DataTransfer's files is
  // the only way to put a file on an input from script, and it exercises the change handler, the
  // decode and the two messages it can end with.
  const asFile = (blobIn, name) => {
    const transfer = new DataTransfer();
    transfer.items.add(new File([blobIn], name, { type: 'image/png' }));
    return transfer.files;
  };
  const settle = () => new Promise(res => setTimeout(res, 0));
  type('signature', '');
  el('photo').files = asFile(blob, 'signature.png');
  el('photo').dispatchEvent(new Event('change'));
  for (let i = 0; i < 50 && !el('signature').value; i++) { await settle(); }
  // The address on screen is the legacy one: the path field still reads the golden 44' path.
  check('a photograph of the screen fills the signature in',
    el('signature').value === golden.signature_base64
      && el('verify-out').textContent.includes(golden.p2pkh),
    el('signature').value.slice(0, 24) + '...');

  const blank = document.createElement('canvas');
  blank.width = 200; blank.height = 200;
  const bg = blank.getContext('2d');
  bg.fillStyle = '#fff';
  bg.fillRect(0, 0, 200, 200);
  blankBlob = await blobFromDataUrl(blank.toDataURL('image/png'));
  el('verify-error').hidden = true;
  el('photo').files = asFile(blankBlob, 'blank.png');
  el('photo').dispatchEvent(new Event('change'));
  for (let i = 0; i < 50 && el('verify-error').hidden; i++) { await settle(); }
  check('a photo with no code in it says so',
    !el('verify-error').hidden && el('verify-error').textContent.includes('No QR code'),
    el('verify-error').textContent.slice(0, 40));

  // A stale answer is worse than no answer: it looks like a statement about what is on screen now.
  type('signature', golden.signature_base64);
  type('expected', golden.p2wpkh);
  const hadMatch = el('verify-out').textContent.includes('Match.');
  el('photo').files = asFile(blankBlob, 'blank.png');
  el('photo').dispatchEvent(new Event('change'));
  for (let i = 0; i < 50 && el('signature').value; i++) { await settle(); }
  check('a failed photo clears the answer it replaces',
    hadMatch && el('signature').value === '' && el('verify-out').hidden,
    'signature=' + JSON.stringify(el('signature').value));
  // And it must not come back when the reader touches another field.
  type('expected', golden.p2wpkh);
  check('the old answer does not return when another field is edited',
    el('verify-out').hidden && !el('verify-out').textContent.includes('Match.'));

  // Typing while a photograph is still being read wins: the read belongs to an earlier input.
  el('photo').files = asFile(blob, 'signature.png');
  el('photo').dispatchEvent(new Event('change'));
  type('signature', 'typed over the pending read');
  for (let i = 0; i < 30; i++) { await settle(); }
  check('a photograph does not overwrite what was typed after it',
    el('signature').value === 'typed over the pending read', el('signature').value.slice(0, 30));
  // The read is over and its result was discarded, so its "reading..." line has to go with it.
  // Left behind, it says the page is still working on a photograph that no longer has an answer.
  check('a superseded read leaves no notice behind', el('verify-note').hidden,
    el('verify-note').textContent.slice(0, 40));

  // The same race on the other branch: a read that ends in an error, not in a code. The failure
  // message belongs to an input the reader has already moved on from, so it must stay silent.
  el('photo').files = asFile(new Blob(['not an image at all'], { type: 'image/png' }), 'broken.png');
  el('photo').dispatchEvent(new Event('change'));
  type('signature', 'typed over the failing read');
  for (let i = 0; i < 30; i++) { await settle(); }
  check('a failed photo does not overwrite what was typed after it',
    el('signature').value === 'typed over the failing read'
      && !el('verify-error').textContent.includes('could not be read as an image'),
    el('signature').value.slice(0, 30));

  // Settle two image reads in reverse order, explicitly, so passing does not depend on a
  // particular machine finishing an old decode within a handful of timer ticks.
  async function racePhotos(failFirst) {
    const pending = [];
    const winner = await originalBitmap(blob);
    const loser = failFirst ? null : await originalBitmap(blankBlob);
    type('expected', golden.p2wpkh);
    window.createImageBitmap = () => new Promise((resolve, reject) => pending.push({ resolve, reject }));
    try {
      el('photo').files = asFile(blankBlob, 'first.png');
      el('photo').dispatchEvent(new Event('change'));
      el('photo').files = asFile(blob, 'second.png');
      el('photo').dispatchEvent(new Event('change'));
      if (pending.length !== 2) return false;
      pending[1].resolve(winner);
      await settle();
      const won = el('signature').value === golden.signature_base64
        && !el('verify-out').hidden && el('verify-out').textContent.includes('Match.');
      if (failFirst) pending[0].reject(new Error('old read failed'));
      else pending[0].resolve(loser);
      await settle();
      return won && el('signature').value === golden.signature_base64
        && !el('verify-out').hidden && el('verify-out').textContent.includes('Match.')
        && el('verify-error').hidden && el('verify-note').hidden;
    } finally {
      window.createImageBitmap = originalBitmap;
      winner.close();
      if (loser) loser.close();
    }
  }
  check('a second photo wins over an earlier successful read', await racePhotos(false));
  check('a second photo wins over an earlier failed read', await racePhotos(true));

  // Mixed-case bech32 is not an address; lower-casing it would manufacture a Match.
  type('signature', golden.signature_base64);
  type('expected', 'bC1' + golden.p2wpkh.slice(3));
  check('mixed-case bech32 is refused, not lower-cased into a match',
    !el('verify-error').hidden && el('verify-error').textContent.includes('BIP173')
      && el('verify-out').hidden,
    el('verify-error').textContent.slice(0, 40));

  // The same attack one level down: a character that is not ASCII at all but that JavaScript
  // lower-cases onto an ASCII letter. The Kelvin sign U+212A is its own upper case and lowers to
  // "k", so a case test based on toUpperCase/toLowerCase would pass it and then hand the
  // comparison a different address than the reader typed. Any message recovers some key, so this
  // uses one whose address contains a "k" to swap; that requirement is asserted, not assumed.
  const foldable = window.piJadeVerify.recoverAddresses(golden.message + '!', golden.signature_base64).p2wpkh;
  type('message', golden.message + '!');
  type('expected', foldable.toUpperCase().replace('K', '\\u212A'));
  check('a letter that is not ASCII is refused, not folded into an address',
    foldable.includes('k') && !el('verify-error').hidden
      && el('verify-error').textContent.includes('BIP173') && el('verify-out').hidden,
    foldable.includes('k') ? el('verify-error').textContent.slice(0, 40) : 'no k to fold in ' + foldable);

  // Whitespace is part of the message the device hashed. A page that trimmed it would report a
  // Match for text nobody signed, and every other check here uses a message trimming leaves alone.
  type('message', golden.message + ' ');
  type('expected', golden.p2wpkh);
  const spaceVerdict = el('verify-out').textContent;
  type('message', golden.message + '\\n');
  check('trailing whitespace is part of the message, not trimmed away',
    el('verify-error').hidden && spaceVerdict.includes('No match')
      && el('verify-out').textContent.includes('No match'),
    spaceVerdict.slice(0, 40));
  type('message', golden.message);

  type('expected', '');
  type('signature', golden.signature_base64);
  type('message', '');
  check('an empty message is refused rather than verified',
    !el('verify-error').hidden && el('verify-out').hidden, el('verify-error').textContent.slice(0, 48));

  // The compose side refuses 192 bytes and up because the device truncates their display.
  type('message', 'e'.repeat(191));
  const at191 = el('error').hidden;
  type('message', 'e'.repeat(192));
  check('192 bytes is refused, 191 is not',
    at191 && !el('error').hidden && el('error').textContent.includes('192 bytes'),
    el('error').textContent.slice(0, 48));
  // Bytes, not characters: 48 four-byte emoji are 192 bytes.
  type('message', '\u{1F600}'.repeat(48));
  check('the limit counts bytes, not characters', !el('error').hidden, el('measure').textContent.slice(0, 40));

  // The panels and buttons the camera module adds. That module is a closure, so what can be
  // checked from here is its contract with the page: which element it shows, and what it leaves
  // behind when the camera refuses to start.
  type('message', 'hello');
  const shutAtStart = el('qr-panel').hidden;
  el('qr-btn').click();
  const openAfterClick = !el('qr-panel').hidden;
  el('qr-btn').click();
  check('the code panel opens and closes on its own button',
    shutAtStart && openAfterClick && el('qr-panel').hidden,
    'button reads ' + el('qr-btn').textContent);

  // Verification runs on every edit already. The button has to reach that same path rather than a
  // second one of its own, so the fields are filled without dispatching anything and the button is
  // left to do the work. The previous answer is cleared through the ordinary path first: measured
  // 2026-09-10, without that clearing a button wired to nothing still passed, because what the
  // check read was the answer already on the screen. Clearing hides the panel without emptying it,
  // so the answer has to be on screen as well as correct: measured the same day, reading only the
  // text let a button wired to nothing pass on the hidden leftovers of the previous check.
  type('signature', '');
  const clearedFirst = el('verify-out').hidden;
  el('expected').value = '';
  el('message').value = golden.message;
  el('signature').value = golden.signature_base64;
  el('verify-btn').click();
  check('the verify button runs the same check an edit does',
    clearedFirst && !el('verify-out').hidden
    && el('verify-out').textContent.includes(golden.p2pkh) && el('verify-error').hidden,
    el('verify-out').textContent.slice(0, 40));

  // No camera answers a headless browser. The reader has to be told, and left able to try again or
  // reach for the photograph instead; a button that stayed disabled would strand them.
  // The refusal arrives on the browser's own schedule, so this waits for the outcome and not for
  // a fixed span: a fixed 900 ms was measured failing on a loaded machine with the status still
  // reading "asking for the camera", which is the request in flight rather than a defect.
  el('scan-btn').click();
  const handedBack = () => el('scan-status').textContent.includes('Choose photo')
    && !el('scan-btn').disabled;
  const giveUp = Date.now() + 8000;
  while (!handedBack() && Date.now() < giveUp) {
    await new Promise(resolve => setTimeout(resolve, 25));
  }
  check('a camera that will not start hands the button back', handedBack(),
    el('scan-status').textContent.slice(0, 56));

  // ROADMAP item 74: the device draws the address it is showing as a code, so the same camera can
  // fill the address field. The button sits inside that field rather than in the row of actions,
  // which is where a reader looks for it. What is measured here is the wiring: the button exists,
  // it opens the one panel the page has, the panel says which field it is filling, and a camera
  // that refuses hands this button back too. Where the read lands is measured in
  // sign_camera_test.js, which is the test with a camera.
  const addrBtn = el('expected-scan');
  check('the address field carries a scan button of its own',
    !!addrBtn && addrBtn.closest('.infield') === el('expected').closest('.infield')
      && addrBtn.getAttribute('aria-label').length > 0,
    addrBtn ? addrBtn.getAttribute('aria-label') : 'no button');
  addrBtn.click();
  check('the address button opens the same panel, named for the address',
    !el('scan-panel').hidden && el('scan-heading').textContent.includes('address'),
    el('scan-heading').textContent);
  // The way out of a refused camera is not the same for the two fields: Choose photo fills the
  // signature and nothing else, so sending an address reader there would put their address in the
  // wrong field. The address reader is told to type instead, and that is measured, not assumed.
  const addrHandedBack = () => el('scan-status').textContent.includes('type the address')
    && !addrBtn.disabled;
  const addrGiveUp = Date.now() + 8000;
  while (!addrHandedBack() && Date.now() < addrGiveUp) {
    await new Promise(resolve => setTimeout(resolve, 25));
  }
  check('a camera that will not start hands the address button back', addrHandedBack(),
    el('scan-status').textContent.slice(0, 56));
  check('the address reader is not sent to Choose photo, which fills the signature',
    !el('scan-status').textContent.includes('Choose photo'),
    el('scan-status').textContent.slice(0, 56));
}
run().catch(e => check('the test itself ran', false, String(e && e.message))).then(report);
</script>
`;

const REPORT_PATH = '/results';
const TIMEOUT_MS = 120000;

// Everything below is one attempt to get an answer out of a browser and then stop it again.
function runInBrowser(pageHtml) {
    return new Promise((resolve, reject) => {
        let settled = false;
        const finish = (fn, value) => { if (!settled) { settled = true; fn(value); } };

        const server = http.createServer((req, res) => {
            if (req.method !== 'POST' || req.url !== REPORT_PATH) { res.writeHead(404).end(); return; }
            let body = '';
            req.on('data', chunk => {
                body += chunk;
                // A report is a few kilobytes; anything larger is not one.
                if (body.length > 1024 * 1024) { req.destroy(); }
            });
            req.on('end', () => { res.writeHead(204).end(); finish(resolve, body); });
        });
        server.on('error', err => finish(reject, err));
        server.listen(0, '127.0.0.1', () => {
            const url = 'http://127.0.0.1:' + server.address().port + REPORT_PATH;
            const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'pijade-sign-verify-'));
            const file = path.join(dir, 'page.html');
            fs.writeFileSync(file, pageHtml.replace('${REPORT_URL}', url));
            const chrome = spawn(CHROME, ['--headless=new', '--disable-gpu', '--no-sandbox',
                '--no-first-run', '--no-default-browser-check', '--user-data-dir=' + path.join(dir, 'profile'),
                'file://' + file], { stdio: 'ignore' });
            const timer = setTimeout(
                () => finish(reject, new Error('the browser did not report back within ' + (TIMEOUT_MS / 1000) + ' s')),
                TIMEOUT_MS);
            chrome.on('error', err => finish(reject, err));
            const cleanUp = () => {
                clearTimeout(timer);
                chrome.kill('SIGKILL');
                server.close();
                // The browser is still flushing its profile as it dies, so the directory can
                // refuse to go on the first attempt. Tidying up is not a measurement: a leftover
                // directory under the system temp path must never turn a good run into a failure.
                try {
                    fs.rmSync(dir, { recursive: true, force: true, maxRetries: 10, retryDelay: 100 });
                } catch (err) { /* the operating system clears its own temp directory */ }
            };
            // Whichever way this ends, the browser and the socket go with it.
            const wrap = fn => value => { cleanUp(); fn(value); };
            const originalResolve = resolve, originalReject = reject;
            resolve = wrap(originalResolve);
            reject = wrap(originalReject);
        });
    });
}

async function main() {
    let body;
    try {
        body = await runInBrowser(page.replace('</body>', harness + '</body>'));
    } catch (err) {
        check('the browser reported back', false, err.message);
    }
    if (body !== undefined) {
        let parsed;
        try {
            parsed = JSON.parse(body);
        } catch (err) {
            check('the browser reported back', false, 'the report was not readable: ' + body.slice(0, 80));
        }
        if (!Array.isArray(parsed)) {
            check('the browser reported back', false, 'the report was not a list of checks');
        } else {
            const valid = parsed.every(r => r && typeof r.name === 'string'
                && typeof r.ok === 'boolean' && typeof r.detail === 'string');
            check('the report contains valid checks', valid);
            if (valid) {
                for (const r of parsed) { check(r.name, r.ok, r.detail); }
                const names = parsed.map(r => r.name);
                const complete = names.length === EXPECTED_CHECKS.length
                    && names.every((name, i) => name === EXPECTED_CHECKS[i]);
                check('the report was complete and in order', complete,
                    complete ? '' : JSON.stringify(names));
            }
        }
    }
    console.log('FAILURES: ' + failures);
    process.exit(failures ? 1 : 0);
}

main();

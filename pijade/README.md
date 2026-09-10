# piJade

A fork of the Blockstream Jade firmware, prepared to run as an airgapped hardware wallet on a
Raspberry Pi Zero W.

> **Warning: this is experimental work.** The device is not used with real funds. Testing is done
> on testnet or with an empty wallet only. The code in this repository has not been through an
> independent security audit.

## Goal

Run existing SeedSigner hardware as a Jade by swapping the card:

- Raspberry Pi Zero W, with the WiFi and Bluetooth traces physically cut
- Waveshare 1.3" LCD HAT, 240x240
- Camera
- The only cable reaching the device is power; everything else goes in and out as QR codes

The existing SeedSigner card is left untouched; piJade is installed on a separate SD card.

## What is in place

**Seed creation from user entropy.** When a new wallet is created the entropy source can be
chosen:

| Source | Behaviour |
|---|---|
| Device | Upstream Jade behaviour; the device RNG |
| Dice Rolls | 50 rolls for 12 words, 99 for 24; the roll sequence is hashed exactly the way SeedSigner hashes it, so a seed made here can be reproduced on independent hardware |
| Camera | 25 frames, each different from the last, hashed together with the device id and the clock counter |

The camera branch is guarded against a stuck sensor by two layers: frames that are flat inside and
frames identical to one already accepted do not count. Without that guard, a camera that stopped
producing frames would have collapsed the entropy silently into a function of the device id and
the clock counter.

## The clock and OTP codes

A Pi Zero W has no battery-backed clock. The device starts at the date the image was built every
time it is powered on, and forgets the time when the plug is pulled. Jade itself works the same
way; there the companion app sends the time on every connection, and here that channel does not
exist because the radios are cut.

The only thing the clock affects is **time-based OTP codes** (TOTP). Signing, PSBTs, address
derivation and SeedQR do not depend on the clock; there is no need to set it for those.

**Setting the clock**

1. On your phone or computer open `ilkerbbb.github.io/piJade/clock`; the device shows that address
   on screen when `Set Clock` is picked. The same page is kept in this repository as
   `docs/clock/index.html` and can be opened from the file as well. Once loaded the page sends
   nothing anywhere; it reads the time from the machine you opened it on, so make sure that one is
   right first.
2. On piJade go to `Options > OTP > Set Clock`.
3. Hold the camera over the code on the page. The device shows the date it is about to set and
   asks for confirmation; after `Yes` it shows "Time set successfully" and the date. The device
   then returns to the home screen, and the OTP menu is entered from there.

The clock that gets set lives only as long as the device stays powered. It resets when the device
is switched off and has to be set again on the next boot.

**If a code is asked for before the clock is set** the device produces no code and says "Clock not
set". That is deliberate: a TOTP code from a device that does not know the time would be silently
wrong.

For anyone who prefers the command line, `pijade/tools/epoch_qr.py` builds the same QR code
(Python, needs `cbor2` and `qrcode`).

## Signing a message

`docs/sign/index.html`, published at `ilkerbbb.github.io/piJade/sign`, describes the round trip
with Sparrow and carries a generator that builds the request QR code for wallets that cannot draw
one. The device shows that address on the `?` button of the Sign Message screen.

## Relationship with upstream

This fork is deliberately kept small so that a new upstream Jade release can be taken without
friction: new logic goes into new files, and Jade's own files hold only the call sites. The
inventory of divergences, the update procedure and the post-update verification list are in
[`UPSTREAM.md`](UPSTREAM.md)

Upstream: https://github.com/Blockstream/Jade

## Licence

Blockstream Jade is MIT licensed and this fork is under the same licence. The `LICENSE` file at
the root of the repository is untouched.

`docs/clock/index.html` and `docs/sign/index.html` each embed Project Nayuki's QR Code generator
library (MIT) unmodified, which is what lets the pages work with no network. The source, the
download date and the file digest are written inside each page.

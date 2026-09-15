# piJade

Blockstream Jade's firmware, running on a Raspberry Pi Zero W whose radios have been physically
removed, with a Waveshare 1.3" 240x240 LCD HAT and a camera. The only cable reaching the device
is power. Everything else travels by QR code: the device reads with its camera and answers on its
screen.

> **Warning: this is experimental work.** The device is not used with real funds; testing is done
> on testnet or with an empty wallet. The code in this repository has not had an independent
> security audit.

This document describes what the fork itself brings and how the device is used. Jade's own
features, and the build document for Jade's hardware, stay with upstream; the root `README.md`
carries that document unchanged. Every departure from upstream is recorded, file by file, in
`UPSTREAM.md`.

## The hardware

| Part | What is used |
|---|---|
| Board | Raspberry Pi Zero W with the WiFi and Bluetooth antenna circuitry physically removed |
| Screen and buttons | Waveshare 1.3" LCD HAT, 240x240, three buttons and a joystick |
| Camera | A camera the host opens as `/dev/video0` |
| Storage | A microSD card, which holds the operating system, the piJade binaries and the settings file |

There is no secure element and no secure boot. A Pi Zero W has neither, so the card is readable
by anyone who takes it; what that means for stored secrets is written out in
`SECURITY-AUDIT-2026-09-03.md`.

## Two ways to hold a wallet

The distinction runs through everything below, so it comes first.

A **temporary wallet** lives in RAM only. It is loaded by scanning a SeedQR, or by typing the
words, and it is gone when the device is powered off or when Log Out is pressed. Nothing about it
is written to the card. This is the SeedSigner working style, and it needs nothing but the device.

A **PIN-unlocked wallet** is the Jade working style: the seed is kept on the card encrypted, and
the key to decrypt it is held by a remote server that releases it only when the right PIN is
entered. On Jade that exchange goes over the cable; here there is no cable, so it goes by QR. The
device shows a QR code, a phone or a computer with internet carries it to the server, and the
reply comes back as a QR code the device reads. **Unlocking a PIN-protected wallet therefore needs
an internet-connected companion device**, and the resistance to card cloning comes from the
server's own monotonic counter, not from the Pi. A temporary wallet needs none of that.

## Seed creation from user entropy

When a new wallet is created, the source of the entropy is chosen from a four-row menu.

| Source | Behaviour |
|---|---|
| Device | Upstream Jade behaviour; the device CSPRNG alone |
| Dice Rolls | 50 rolls for 12 words, 99 for 24; the roll sequence is hashed exactly the way SeedSigner hashes it, so a seed made here can be reproduced on independent hardware |
| Camera | Camera frames are hashed into a chain whose final input is the device CSPRNG, so the floor of the result is the CSPRNG and the camera is a layer on top of it |
| Combined | Dice rolls first, then the camera chain that ends in the device CSPRNG; the two digests are hashed together |

Without a camera the Camera row is not built at all and the menu has three rows, the last of
which is named `Dice + Device`: it mixes what it can and says so, rather than keeping a name that
promises a source it cannot reach.

The camera path collects at least 50 frames, and that number is a floor rather than a target: once
it is reached, collection continues and the user ends it. Three gates stand in front of every
frame, so a sensor that has stopped producing usable images cannot quietly collapse the chain:

- a frame whose lightest and darkest pixels are too close together is flat, and is thrown away;
- a frame identical to one of the last fifty accepted, which is what a frozen sensor hands back, is thrown away;
- a frame too similar to the previous accepted one is thrown away, which also means the count
  advances with the user's movement rather than with the sensor's frame rate.

The screen shows a progress bar that fills as frames are accepted, and the label changes to
`Exit to finish` once the floor is reached; the accepted and rejected counts, with the reason for
each rejection, go to the log rather than to the screen. Neither shows a bit count: the entropy of
a hash output cannot be measured, and a live figure would be an invented reassurance. A real per-frame figure can only come from characterising the sensor on the
device, which has not been done; the rejection threshold is deliberately conservative until it is.

## Restoring a seed

`Restore Wallet` offers four rows.

| Row | What it does |
|---|---|
| 12 Words | Type twelve words |
| 24 Words | Type twenty-four words |
| Scan QR | Read a compact SeedQR, a standard SeedQR, a BC-UR `crypto-bip39` phrase, or the words as plain text |
| Split Backup | Reassemble a seed that was split into parts |

Words are spelled out unless the setup was taken through `Advanced Setup`, which puts an `Entry
Method` screen in front of the keyboard with a second row, `Index Number`: each word is then given
by its position in the BIP39 list, which is faster and is what a numeric backup gives you. A
temporary restore takes the advanced path without asking, so that screen appears there by
default.

`Split Backup` leads to two schemes:

- **SeedXOR**, which reassembles a seed from parts that are each themselves a valid mnemonic. At
  least two are needed and the device does not cap how many may be entered, because it cannot know
  how many the user wrote down; splitting, which the fork also does, offers two, three or four.
- **SLIP-39**, which reassembles a seed from Shamir shares of 20 or 33 words, entered by word or
  scanned as QR codes. The fork reads SLIP-39; it does not produce shares.

A restored wallet can be kept for the session or, with a PIN, written to the card.

## Backing up a seed

Every backup screen shows the words, so the row that offers it appears only for a wallet whose
words passed through this session. A wallet that was unlocked with a PIN comes back from the card
without the entropy those screens draw from, so for that wallet the row is not offered at all
rather than failing when pressed.

The backup screen draws a SeedQR the device's own camera can read back, in either of SeedSigner's
two formats: the compact one, which is the raw entropy, or the standard one, which is four digits
per word. There is a zoom step for copying the grid by hand, and the flow ends by offering to read
the drawn code back through the camera and check it against the wallet it came from. The separate
`Verify Backup` row is a different check and uses no camera: it asks for words from the phrase.

## Reading and writing QR codes

The camera reads single-frame QR codes, animated BC-UR sequences, and BBQr sequences. A completed
BBQr transfer is routed on the file type its own header declares: a serialised PSBT is signed, and
a Unicode text payload, which is how Coldcard exports a multisig setup file, goes to the same
reader as any other text. A file type the device cannot use is refused by name, rather than pushed
into a parser that would reject it with a vaguer message.

An address scanned on its own is checked for ownership. The search covers accounts 0, 1 and 2 plus
whichever account the last xpub export used, on both the receive and the change branch, so the
person holding a receipt does not have to know which branch it came from.

Exports that reveal more than they appear to carry a warning screen before the QR code is drawn:
an xpub lets whoever scans it see every address and payment of that wallet forever, and a multisig
wallet record carries the same exposure. The warnings can be switched off in the settings for
someone who knows what they are doing.

## The clock and OTP codes

Upstream Jade gets the time from the computer it is plugged into. This device is never plugged
into one, so it asks for the time by QR code. A helper page draws the QR:

- [set the clock](https://ilkerbbb.github.io/piJade/clock)

The page works offline once loaded, and reads the current time from the browser it is open in. It
redraws the QR every second, and the device does not ask before accepting what it reads, because a
yes/no question took longer than that and left the clock behind the time just shown. A value that
cannot be rendered as a date is refused before the clock is touched; one that can is set, and the
date the clock now holds is shown back on a screen that waits for `Continue`.

## Signing a message

Message signing, which upstream drives over the cable, is also reached by QR code. A helper page
turns a message and a derivation path into the QR the device reads:

- [sign a message](https://ilkerbbb.github.io/piJade/sign)

The device shows the message and the path, and the signature comes back on the screen as a QR
code.

## The duress PIN

A second PIN can be set which, when entered, erases the stored wallet. It is not stored in the
clear: the card holds a salt and a one-way verifier, and the settings screen shows only whether a
duress PIN is set.

**This does not make the duress PIN deniable against someone who takes the card.** Six digits is
about twenty bits; an attacker with the card can try all of them on their own machine, which also
answers the question of whether a duress PIN exists at all. What the change removes is plaintext
storage, and nothing more. Upstream Jade gets deniability here from the ESP32's encrypted flash;
a Pi Zero W has no equivalent, and this is one of the places where that shows.

## Installing

The device does not run an installer and never needs a keyboard or a monitor. A card is written
once, and after that a new version is the binaries copied over on a Mac.

The chain has three steps, and each script carries its exact invocation in its own header comment,
which is where the commands are kept so they cannot drift from the code:

| Step | Script | What it produces |
|---|---|---|
| 1 | `pijade/images/bootstrap-builder.sh` | The ARMv6 build environment, built from the Raspberry Pi OS image itself, plus a record of the fingerprints of everything it produced |
| 2 | `pijade/images/build-armv6.sh` | `pijade-armv6.tar.gz`, the binaries with their SHA-256 |
| 3 | `pijade/images/prepare-image.sh` | A Raspberry Pi OS image with those binaries embedded, ready to write to a card |

Step 1 verifies the fingerprint of the source Raspberry Pi OS image before it starts and refuses
to continue if it does not match, and it records the fingerprints of everything it produces, which
is what makes the build environment itself checkable. Step 3 refuses to run unless the package
hash it is given matches the package it finds; passing that hash by hand, from the build log, is
what makes the check an authenticity check rather than only a staleness check. Step 3's container
is pinned by digest; step 2's is the environment step 1 built, named by the release it was built
from.

Three binaries are placed on the FAT partition, in `/boot/firmware/pijade/`, rather than in the
Linux filesystem, and the settings file sits on that same partition. Two of them are the device
itself, `pijade-host` and `libjade.so`; the third, `pijade-t44-bench`, is a measurement tool that
exits without printing a line unless the marker file `/boot/firmware/pijade-t44.enable` is there,
so a normal boot still runs it but it returns straight away. macOS can read and write FAT but cannot read ext4, so putting them
there is what makes the update path work: insert the card in a Mac, replace the binaries, and
eject. The root filesystem is mounted read-only, and
the service that runs at boot has core dumps disabled, because a crash dump would be written while
the keys are in memory.

The base image is the ARM hard-float Raspberry Pi OS Lite build; the exact release is pinned in
the bootstrap script.

## Relationship with upstream

The fork tracks Blockstream Jade and takes its updates. Every change made here is marked in the
source with a `BBB-AIRGAP:` comment saying why, and `UPSTREAM.md` lists them file by file together
with the discipline for merging upstream changes into them. Anything measured against SeedSigner,
menu row by menu row, is in `SEEDSIGNER-COMPARISON.md`; the threat model and what is deliberately
not claimed are in `SECURITY-AUDIT-2026-09-03.md`.

## Licence

Blockstream Jade is MIT licensed and this fork is under the same licence. The `LICENSE` file at
the root of the repository is untouched, and so is the `COPYING` file upstream ships beside it;
upstream's own wording is that the collection is subject to GPL3 while individual source
components can be used under their specific licences.

Third-party code brought in by the fork keeps its own licence, recorded where it sits:

| What | Licence |
|---|---|
| `pijade/tools/ur/` | BSD-2-Clause Plus Patent; the vendored commit and its source are recorded in `VENDORED.md` beside it |
| SLIP-39 test vectors under `pijade/tools/fixtures/` | MIT, from SatoshiLabs; the licence file sits next to them |
| The SLIP-39 and Shamir routines under `main/` | MIT, taken from headers that carry it explicitly, with the origin written at the top of each file |
| The QR generator embedded in the helper pages | MIT, Project Nayuki, unmodified; the source, download date and file digest are written inside each page |

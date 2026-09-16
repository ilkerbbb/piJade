# How piJade compares

Several devices exist to keep a Bitcoin key away from a networked computer, and they disagree
about how. This document puts those disagreements in one place: what reaches the device and
through which opening, how a seed gets in, how one is made here, and what each device will not do.
Below the tables, the row-by-row reading of SeedSigner that this document began as is kept in
full, because SeedSigner is the closest relative and the only other device this fork has read line
by line.

**Every cell was read from the source of the device it names.** Where the reading left a question
open the cell says `not measured`, which is a different statement from `no`. Nothing here comes
from a product page or from memory.

## The devices, and what was read of each

| Device | Repository | What was read | At | Licence |
|---|---|---|---|---|
| **piJade** | this one | this tree | `bbb-airgap` | MIT |
| Blockstream Jade | `Blockstream/Jade` | the other side of the fork point | `9c097297`, 2026-08-23 | MIT |
| SeedSigner | `SeedSigner/seedsigner` | `src/` | `85cd9a0`, 2026-09-04 | MIT |
| Coldcard | `Coldcard/firmware` | `shared/` | `948dc10`, 2026-08-29 | MIT with a Commons Clause |
| Passport | `Foundation-Devices/passport2` | `ports/stm32/boards/Passport/` | `1fea63c8`, 2026-09-14, v2.4.0 | mostly GPL-3.0 |
| Trezor | `trezor/trezor-firmware` | `core/` and `crypto/` | `7ab83a2`, 2026-09-11 | GPLv3, except `crypto/` which is MIT |

SeedSigner appears at two commits, and the gap between them was measured rather than waved away:
the row-by-row section below was read at `d70b322`, an ancestor of the `85cd9a0` this table was
measured at, and the fifteen commits between them change two files under `src/`,
`models/psbt_parser.py` and `views/psbt_views.py`. Neither is the source of a cell above.

The repository column is there because of what it cost to get the Passport row right. This
document first read `Foundation-Devices/passport-firmware`, whose name and history make it look
like the project's tree; its `main` branch ends at 2022-08-02 and the firmware has been developed
since in `passport2`. Every Passport cell measured against the old repository described a release
four years old, and five of them were wrong. Nothing in the old tree says it has been superseded,
and the dates only look wrong when they sit next to the other five columns, which is why they are
now in the table.

Two of the six are assembled from parts bought separately. SeedSigner's own README lists a
Raspberry Pi Zero and a Waveshare 1.3 inch 240x240 LCD (`README.md:89-91`); those are the two
boards this fork runs on as well. The other four are firmware for a device its maker builds.

The licence column is not decoration, because it decides what this fork may borrow. Of the code
each project wrote itself, only SeedSigner's and Trezor's `crypto/` can be copied into an MIT
tree. Coldcard's Commons Clause forbids selling the software, which MIT does not, so its code
cannot come here however useful. Passport's own modules are GPL-3.0, though the tree also carries
permissively licensed files it vendors from elsewhere, and those travel under their own terms
rather than Passport's. Ideas travel freely, and the Seed XOR row below is one of them: the
scheme is Coldcard's, the code here is not.

## How data reaches the device

| | piJade | Jade | SeedSigner | Coldcard | Passport | Trezor |
|---|---|---|---|---|---|---|
| Camera, for QR | yes | yes | yes | yes, on the Q | yes | no |
| USB data | no, cut | yes | no | yes | no | yes |
| Bluetooth | no, cut | yes | no | no | no | yes |
| NFC | no | no | no | yes, on the Mk4 | no | yes |
| microSD | the system disk | no | yes | yes | yes | yes |
| Secure element | no | no | no | yes | yes | yes |

piJade's row reads the way it does for a physical reason rather than a setting: the Pi Zero W's
WiFi and Bluetooth circuits are cut on the board, and the fork removes the code that would have
driven them. What is left is the camera in and the screen out, which is the whole interface.

Two cells need their qualifier. Coldcard's camera belongs to the Q (`shared/q1.py`); the Mk4 has
none, and its NFC tag is the Mk4's (`shared/nfc.py:3`). Jade's camera is code in the tree too
(`main/camera.c`), and which Jade models carry one was not measured either. Trezor's NFC and
Bluetooth are drivers in the tree (`core/embed/io/nfc/st25/nfc.c`, `core/embed/io/ble`); which
models carry the parts was not measured.

The last row is the one this fork loses on its own hardware. A secure element stores the key where
firmware cannot read it back. Coldcard has two, SE1 and SE2, and draws randomness from both
(`shared/mk4.py:43-44`); Passport has an ATECC608A (`se-atecc608a.c`), Trezor its own
`core/embed/sec/`. A Pi Zero has nothing of the kind,
so piJade encrypts the wallet with the user's PIN and writes it to the same card it boots from.
SeedSigner does not have the part either, and answers differently: it stores no seed at all.

## Getting a seed onto the device

| | piJade | Jade | SeedSigner | Coldcard | Passport | Trezor |
|---|---|---|---|---|---|---|
| Type the words | yes | yes | yes | yes | yes | yes |
| Enter words by their number | yes | yes | no | no | no | no |
| Scan a standard SeedQR | yes | yes | yes | yes | yes | no |
| Scan a CompactSeedQR | yes | yes | yes | no | yes | no |
| SLIP-0039 shares | recover only | no | no | no | no | create and recover |
| Seed XOR | yes | no | no | yes | no | no |

Three rows carry a history. **Entering words by number** comes from upstream Jade
(`main/ui/mnemonic.c`, the `WORD_NUMBERS` buttons) and, measured against the other four, exists
nowhere else: SeedSigner spells words on a keyboard (`gui/screens/seed_screens.py`) and Coldcard
narrows them letter by letter (`shared/seed.py:112`). **SeedQR** was SeedSigner's invention and
has travelled furthest of anything in this table; Coldcard reads the standard form and writes it
(`shared/decoders.py:23`, `shared/actions.py:701`), Passport carries an encoder and a decoder for
both forms and reaches them from restoring and from viewing a seed
(`modules/data_codecs/seedqr_codec.py`, `compact_seedqr_codec.py`), and Jade exports the compact
form and scans it back to check the drawing, calling it by SeedSigner's name
(`main/process/mnemonic.c:68`). **Seed XOR** went the
other way: the scheme is Coldcard's (`shared/xor_seed.py`), and the implementation here is this
fork's own C (`main/seedxor.c`).

SLIP-0039 is the row where this fork is deliberately half a device. Trezor both splits a seed into
shares and puts it back together; piJade only puts it back together, so a backup made on a Trezor
can be restored here, and a wallet created here cannot be split into SLIP-0039 shares. Splitting
is offered in the other scheme instead, Seed XOR, where every part is needed.

## Making a seed on the device

| | piJade | Jade | SeedSigner | Coldcard | Passport | Trezor |
|---|---|---|---|---|---|---|
| Camera frames as entropy | yes | yes, at boot | yes | no | no | no |
| Dice rolls as entropy | yes | no | yes | yes | no | no |
| Coin flips as entropy | no | no | yes, for the final word | yes | no | no |
| Another hardware source, besides the camera | no | yes | no | yes | yes | yes |

The last row is where leaving the ESP32 cost something measurable. Jade's `get_random()` hashes
power and temperature sensor readings, a cycle counter and the chip's own generator into every
draw (`main/random.c`). On a Pi none of that exists, and this fork's `get_random()` is the
`getrandom` system call (`libjade/libjade.c`), which is the operating system's generator rather
than a source the firmware reads itself. The other three read one: Passport combines
an avalanche source, the MCU generator and the secure element's in a single call, and makes a new
seed from all three at once (`noise.h:12-16`, `modules/tasks/new_seed_task.py`), Coldcard hashes
its own generator with both secure elements
(`shared/seed.py:647-659`), and Trezor exclusive-ors the MCU generator with the Optiga or Tropic
chip and halts the device if either source fails (`core/embed/sec/rng/rng_strong.c`). SeedSigner
stands where this fork stands and answers it the same way.

What the user supplies is the other half of the question, and only the two Pi devices and Coldcard
ask for any. Coldcard asks for the most and claims the least for it: dice rolls and coin flips are
supplemental on top of a seed that is already the generator plus both secure elements, each entry
is checked for a lopsided distribution, and the separate dice-only mode says in as many words that
no hardware randomness is mixed in (`shared/seed.py`). This fork's `Combined` does the same thing
with the sources it has, taking dice first and then camera frames and ending the chain in
`get_random()`, while the plain `Dice Rolls` path stays unmixed for anyone who wants to check the
arithmetic by hand. SeedSigner mixes in nothing of the device's own: the camera chain is the CPU
serial number, the clock and the frames (`views/tools_views.py`), and the dice path is the rolls
alone. Its coin-flip cell is narrower than Coldcard's and needs its qualifier: the menu offers
coin flips only for the last word of a mnemonic the user already holds, as one of three ways to
fill those final bits (`views/tools_views.py:321`, the view that runs the entry screen at
`:366`), while the function that would build a whole mnemonic out of flips exists and is called
from nowhere in `src/` (`helpers/mnemonic_generation.py:85`).

Jade's camera cell is the one that needs its qualifier. There is no ceremony behind it: while
the splash screen is up, ten frames go straight into the entropy pool and the user is never asked
(`main/main.c`, `rnd_camera_feed`). Coldcard's Q has a camera and uses it only to read QR codes
(`shared/scanner.py`).

## Signing, and everyday use

| | piJade | Jade | SeedSigner | Coldcard | Passport | Trezor |
|---|---|---|---|---|---|---|
| Animated QR (UR) | yes | yes | yes | no | yes | no |
| BBQr | read | no | read | read and write | no | no |
| Multisig | yes | yes | yes | yes | yes | yes |
| Address explorer | yes | no | yes | yes | yes | no |
| Sign a message | yes | yes | yes | yes | yes | yes |
| BIP-85 child seeds | yes | yes | yes | yes | yes | no |

The two QR transports split the field rather than ranking it. UR is what Jade, SeedSigner,
Passport and this fork speak; BBQr is Coinkite's, and Coldcard is the only device here that both
reads and writes it. This fork reads BBQr and does not produce it, which is enough to accept a
transaction prepared by a Coldcard but not to hand one back the same way. Of the trees read here
Coldcard is the only one that both reads and writes BBQr; nothing in Passport's tree mentions the
format or its `B$` frame header, which is worth saying plainly because this fork's own source
called BBQr the format Coldcard and Passport emit until this table was measured.

Two cells are easy to misread. Trezor's device firmware carries no address explorer; whether its
desktop software offers one was not measured, and this table is about the device. Jade's `no` is
narrower than it looks: upstream will check an address you scan at it and say whether the wallet
owns it (`main/qrmode.c`, `Address verified:`), which this fork inherits; what the fork added is
the listing, so the row separates browsing your own addresses from confirming one.

## Protecting the device

| | piJade | Jade | SeedSigner | Coldcard | Passport | Trezor |
|---|---|---|---|---|---|---|
| Duress or wipe PIN | yes | yes | no | yes | yes | yes |
| Wallet kept on the device | yes, encrypted | yes | no, memory only | yes | yes | yes |
| Firmware signature checked at boot | no | not measured | no | not measured | yes | yes |

The duress row is the one where the four makers agree and SeedSigner opts out: Coldcard keeps
fourteen trick-PIN slots (`shared/trick_pins.py:18`), Passport a duress secret
(`modules/pincodes.py:33`), Trezor a wipe code (`core/src/apps/management/change_wipe_code.py`),
and this fork inherits Jade's wallet-erase PIN. What differs is where the setting can be read back
from, and on this hardware that question has a sharp edge: with no secure element, the duress PIN
lives on the same card as everything else. The guide's `The duress PIN` section states that limit
rather than dressing it up.

The last row is where this fork is behind on purpose and says so. Verifying a firmware signature
at boot needs a root of trust the Pi Zero does not have, and the code doing the verifying would
sit on the same removable card as the code it verifies. SeedSigner answers the same way for the
same reason. Passport's bootloader refuses an image whose signature does not check
(`bootloader/flash.c:380`, the error reaching the screen at `bootloader/main.c:594`), and Trezor's
has a check of its own (`core/embed/projects/bootloader/fw_check.c`). Two cells say `not measured`
and mean different things: Coldcard's bootloader is not in the repository that was read, while
what was read on the Jade side is an update-time comparison against a hash the client sends
(`main/process/ota_util.c`), which is not a signature checked at boot; whether ESP32 secure boot
is turned on in a shipped Jade was not measured.

## How these cells were measured

Each axis was a targeted search in the repository named above, and then the file was opened and
the symbol read. The count of matching files was never the answer. Every trap below was caught
that way, and each of them would have put a wrong cell in a table built from counts alone.

- **The tree can be the wrong tree.** The Passport column was first measured against
  `passport-firmware`, which is the project's old repository and stops in 2022. Five cells were
  wrong: SeedQR, CompactSeedQR, BIP-85, the address explorer and NFC. That is why the table now
  carries a repository and a date for every device rather than a hash alone.
- **A word can match something else.** `NFC` matched SeedSigner's `models/seed.py`, where it is
  Unicode's `normalize("NFC")`; the cell is `no`.
- **A zero can be a vocabulary gap.** `duress` matched nothing in Trezor, because Trezor calls
  it a wipe code. That zero was a measurement artefact, not an absence; the cell is `yes`.
- **A repository can contain another one, and it can move.** Passport's tree carries the whole
  of `trezor-firmware`, in a different place in each repository. In the 2022 tree it sat inside
  the directory read here, where an unfiltered search for `slip39` returned seventy-five files
  and seventy-four of them were Trezor's; in `passport2` it moved to `extmod/`, outside that
  directory, and the same search inside the read scope now returns nothing. A scope that was
  safe once is not safe in the next repository, so every Passport search here excludes that
  directory by name as well.
- **A wordlist is not a feature.** Searched without word boundaries, `dice` matches in Trezor,
  Passport and Jade, and not one match is a die: they are the word `indices` and the BIP-39 word
  `dice` sitting in a wordlist. Bounded to the word, Trezor's declared scope has a single match
  and it is that wordlist, Jade's tree has none, and Passport's two are both wordlists. All
  three cells are `no`.
- **A file can be compiled and never reached.** Passport builds the vendored `slip39.c` in both
  repositories, from a different list in each: the object list at `py/py.mk:350` in `passport2`,
  next to the `bip39.o` that the firmware does use, and the source list at `mpconfigboard.mk:47`
  in the 2022 tree. Nothing in Passport's own modules calls it in either tree, and the cell is
  `no` because of the second measurement rather than the first.

The piJade column was not searched for at all; it was read from this tree, and the section below
carries its evidence line by line. One claim ran the other way: this fork's `main/bbqr.h` called
BBQr the format Coldcard and Passport emit, and measuring Passport's tree for this table is what
showed the second half had no evidence behind it.

---

## SeedSigner, row by row: the seed menu and the settings

> First written 2026-08-31; **remeasured in full on 2026-09-13** against `d5a4c095`, because the
> fork had moved on and the document was still listing as missing several features that now exist.
> Every cell and every `file:line` below was read again at that commit. Versions compared:
> **SeedSigner `d70b322`** and **piJade `bbb-airgap`** at `d5a4c095`. Method: reading the code. On
> the SeedSigner side `views/seed_views.py` (2270 lines), `views/tools_views.py` (775),
> `models/settings_definition.py` (814), `views/settings_views.py` (387), `views/psbt_views.py`,
> `views/scan_views.py` and `views/view.py` were read in full or against specific questions, and
> for individual claims `models/decode_qr.py`, `models/seed.py`, `gui/renderer.py`,
> `gui/screens/screen.py` and `gui/screens/tools_screens.py`; between the first version and that
> reading the SeedSigner side had not changed, and every `file:line` cited for it was matched
> against the symbol it is cited for rather than carried over on trust. The piJade side was
> verified through `main/process/dashboard.c`, `main/ui/dashboard.c`, `main/process/mnemonic.c`,
> `main/ui/mnemonic.c`, `main/process/sign_psbt.c`, `main/process/auth_user.c`,
> `main/utils/psbt.c`, `main/qrmode.c`, `main/ui/qrmode.c`, `main/ui/sign_tx.c`,
> `main/entropy_sources.{c,h}`, `main/keychain.{c,h}`, `main/storage.h`, `main/gui.c`,
> `main/qrcode.c`, `main/idletimer.c`, `main/seedxor.c`, `main/bbqr.c`,
> `libjade/pijade_settings.c` and `pijade/host/panel_st7789.c`. The list runs **one way**: what
> SeedSigner has and this fork does not. The other direction is not a full inventory; section 5
> names the part of it that falls inside this ground. **Line numbers reread 2026-09-15 at
> `01f266aa`, then moved to `bb77e43f`.** Every `file:line` on the fork's side, the bare `:NNNN`
> ones included, was matched against the tree again and the stale ones corrected. Later the same
> day `bb77e43f` brought the tree to the `format.sh` contract, which shifted lines in 13 files;
> the references here were moved with it and verified against the lines they name, so they stand
> at `bb77e43f`. What the cells themselves claim, which side has a feature and which does not, was
> not remeasured; that still stands from the 2026-09-13 reading above.

**Scope statement:** the question was seed management and use. SeedSigner keeps its seed
**generation** and **verification** tools in a separate `Tools` menu rather than under the
fingerprint (`views/tools_views.py:21`). Reading only `SeedOptionsView` would have missed that
layer, so Tools was deliberately included. On the settings side the single source of truth is
`models/settings_definition.py`; `settings_views.py` is only a generic renderer
(`settings_views.py:33-36`).

---

### 1. The seed menu, row by row

**SeedSigner:** `Home > Seeds > <fingerprint>` is `SeedOptionsView` (`views/seed_views.py:528`),
with menu rows built at `:572-587` and the title being the seed's fingerprint (`:592`).

**piJade:** `Session > <fingerprint>` is `handle_session()` (`main/process/dashboard.c:3337`),
with wallet rows built at `:3442-3481` and the title being the slot's fingerprint (`:3423`).

| # | SeedSigner row | SS evidence | piJade state | Evidence / note |
|---|---|---|---|---|
| 1 | **Scan transaction** (PSBT) | `seed_views.py:574` -> `:601` (`controller.psbt_seed = self.seed`). A psbt scanned from the home screen instead routes to `PSBTSelectSeedView` (`scan_views.py:92-97` -> `psbt_views.py:11`), which lists the loaded seeds and marks with `(?)` the ones whose fingerprint the psbt does not name (`psbt_views.py:36-39`) | **Present, reached the other way round** | Both sides can start from either end; what differs is the shape. SeedSigner lists every loaded seed and lets the user pick. Here the psbt is read first; if it does not name the active wallet, the device offers matching loaded wallets one at a time (`sign_psbt.c:734-768`): `offer_wallet_named_by_psbt()` (`sign_psbt.c:728`), called at `:848` before the output and fee screens. The offer is made only when `process` is NULL, ie. the user is at the device; over the rpc the interface assertion (`ASSERT_KEYCHAIN_UNLOCKED_BY_MESSAGE_SOURCE`) decides instead, since switching underneath it would let a client sign with a wallet its own connection never unlocked. Ownership is still derived from the wallet in use (`utils/psbt.c:162`); the offer is what bridges the two. If the active wallet has no signable inputs, the flow pauses early: "No inputs here can be signed by this wallet. Continue anyway?" (`sign_psbt.c:1043-1049`), asked before the output and fee screens; continuing is allowed. SeedSigner marks `(?)` at seed selection using fingerprints, before deriving input keys. `Scan QR` under the fingerprint opens the same scan (`dashboard.c:3442`) |
| 2 | **Export xpub** | `seed_views.py:576` -> `SeedExportXpubSigTypeView:664` | **Present** | `dashboard.c:3448` -> `display_xpub_qr()`. This fork also has `Xpub Settings` (Script / Wallet / Account Index / QR Settings) at `ui/qrmode.c:109-126` |
| 3 | **Address explorer** | `seed_views.py:578` -> `ToolsAddressExplorerAddressTypeView:568`; receive and change addresses, ten per page, a QR for each (`tools_views.py:665, 753`) | **Present** | `Session > <fingerprint> > Address Explorer` (`dashboard.c:3447`) -> `handle_address_explorer()` (`qrmode.c:1829`) -> `address_explorer()` (`:1554`). Receive and change addresses, derived and listed, with a QR for each. `Verify Address` (`qrmode.c:1170`) stays a separate job: it searches for an address it is given |
| 4 | **Backup seed** (submenu) | `seed_views.py:579` -> `SeedBackupView:628` | **Present and grouped**; four rows against SeedSigner's two | `handle_wallet_backup()` (`mnemonic.c:853`), rows at `:864-870`. The way in appears only where the slot still carries the entropy it was built from (`dashboard.c:3470-3472`). Of the two extra rows, `Verify Backup` (`mnemonic.c:866`) runs a lighter quiz than SeedSigner, with a dedicated menu entry as well as the setup flow; `Split (SeedXOR)` (`mnemonic.c:870`) is the one that runs the other way |
| 4a | **View seed words** | `SeedBackupView:629` -> `SeedWordsWarningView:1002` -> `SeedWordsView:1037` (four words per page) | **Present** | `Backup > View Words` (`mnemonic.c:864`) -> `show_wallet_words()` (`:680`). The words are also shown during setup (`display_confirm_mnemonic()`, `:484`) and for BIP85 child words |
| 4b | **Export as SeedQR** | `SeedBackupView:630` (guarded by `seed.seedqr_supported`), format choice `:1412`, warning `:1472`, full QR `:1510`, zoom `:1548`, scan-back verification `:1595-1721` | **Present** | `Backup > Export SeedQR` (`mnemonic.c:865`) -> `export_wallet_seedqr()` (`:385`). Standard and Compact, a guide grid, zoom, and scan-back verification through the camera (`:331-346`). The export opens with drawing instructions (`:108-109`) and, when `Features > Warnings` is on, the private-key warning SeedSigner shows at the same point (`:115-126`) |
| 5 | **Sign message** | `seed_views.py:582`, guarded by `SETTING__MESSAGE_SIGNING == ENABLED` (**off** by default, `settings_definition.py:686-690`) | **Present, behind the same kind of switch** | `Session > <fingerprint> > Sign Message` (`dashboard.c:3455`), offered when `FEATURE_FLAGS_SIGN_MESSAGE` is set (`:3453`; `Options > Features`). The general QR scan is also gated (`qrmode.c:2056-2057`) and calls `sign_message_file()` (`:2000`, declaration at `:99`); the rpc path remains in the shared code |
| 6 | **BIP-85 child seed** | `seed_views.py:585`, guarded by `SETTING__BIP85_CHILD_SEEDS == ENABLED` (**off** by default, `settings_definition.py:663-668`) and `seed.bip85_supported` | **Present, and guarded the same way** | `dashboard.c:3461` -> `handle_bip85_mnemonic()`, offered when `FEATURE_FLAGS_BIP85` is set (`:3460`; `Options > Features`); 12 or 24 words (`ui/mnemonic.c:187-188`) |
| 7 | **Discard seed** | `seed_views.py:587` -> `SeedDiscardView:459`; the screen reads "Wipe seed {fingerprint} from the device?" (`:478`) | **Partly: temporary wallets only** | `Forget` at `dashboard.c:3479-3481`; no row appears for a persistent wallet, and the way to drop one is `Log Out` (`:3379`). The reason is written in the code (`:3475-3478`): `keychain_load()` refuses to read the blob back while a wallet is in memory |

#### 1.1 One more difference: the fingerprint list itself

SeedSigner's `SeedsMenuView` (`seed_views.py:26`) puts one uniform icon next to each fingerprint
(`:45`) and ends the list with a **"Load a seed"** row (`:46`), so a new seed can be loaded
straight from the list. Here the `Session` list marks the persistent/temporary distinction with a
symbol (`dashboard.c:3374`, filled circle = persistent, hollow = temporary), a difference in this
fork's favour, but there is no row for loading a new wallet from the list itself; that happens
through `Scan QR` or `Options > Add Wallet` (`dashboard.c:2806`).

---

### 2. Loading and generating seeds (SeedSigner `Tools` and `LoadSeedView`)

| Feature | SS evidence | piJade state | Evidence / note |
|---|---|---|---|
| Load by scanning a SeedQR | `LoadSeedView:163` | **Present** | `Restore Wallet > Scan QR` (`ui/mnemonic.c:89`); `Scan SeedQR` sits on the home screen itself while the device is uninitialised (`dashboard.c:96`, `:3699`), and moves one level down into the `QR Mode` menu once a PIN wallet exists (`dashboard.c:103`, `ui/dashboard.c:155-157`) |
| **SLIP-0039 share recovery** | Not in SeedSigner: `slip39` and `shamir` appear in none of the 365 files of its tree at `d70b322` | **Present, recovery only** | `Restore Wallet > Split Backup > SLIP39` (`ui/mnemonic.c:95`, `:117`).  Shares reach the device typed or scanned (`ui/mnemonic.c:136-137`) and in both lengths the standard defines (`:153-154`); shares are collected until the set is complete, across groups as well as within one (`slip39.h:87-88`, `process/mnemonic.c:2411`, `:2342`).  Producing shares is deliberately out of scope, so a backup made elsewhere can be restored here but this fork does not split a wallet into SLIP-0039 shares |
| 12 / 24 words entered by hand | `LoadSeedView:164-165` | **Present** | `ui/mnemonic.c:85-86`, word entry through `make_enter_wordlist_word_activity` (`:353`) |
| Electrum seed entry | `LoadSeedView:166`, guarded by `SETTING__ELECTRUM_SEEDS` (**off** by default, `settings_definition.py:670-676`) | **Absent** | Electrum's own seed format is not supported.  The one non-BIP39 format this fork does read is SLIP-0039, two rows above |
| New seed: **camera entropy** | `tools_views.py:22` -> `:62-225`; device id plus time plus frames, chained through sha256 | **Present, with different collection and health checks** | `Entropy Source > Camera` (`entropy_sources.c:226`); at least 50 accepted frames (`entropy_sources.h:45`, `entropy_sources.c:492`), streamed into one SHA256 context with device id, time and a final device-CSPRNG input (`entropy_sources.c:475-476`, `:499-505`). SeedSigner chains hashes of a rolling pool of 50 frames and a final still (`gui/screens/tools_screens.py:23`, `:115-119`; `tools_views.py:193-199`). It already rejects flat and duplicate preview frames (`gui/screens/tools_screens.py:100-119`); this fork uses a pixel-range floor of 8 and a block-average change threshold of 128 (`entropy_sources.h:52`, `:88`). These are different checks, not proof of greater entropy |
| New seed: **dice entropy** | `tools_views.py:23` -> `:232-288` | **Present, same scheme** | `entropy_sources.h:28-29` (50 or 99 rolls; the code records at `:24` that this follows SeedSigner's `dice_verification.md` scheme) |
| New seed: device RNG | Not in SeedSigner (camera and dice only) | **Present** | `Entropy Source > Device` (`entropy_sources.c:223`) |
| New seed: **all sources at once** | Not in SeedSigner | **Present** | `Entropy Source > Combined` (`entropy_sources.c:227`): dice first, then the camera chain that ends in the device CSPRNG, the two digests hashed together. On a build without a camera the row is named `Dice + Device` (`:229`) rather than silently dropping a source |
| **Calculating the 12th / 24th word** | `tools_views.py:24` -> `ToolsCalcFinalWordNumWordsView:295`; coin flips, a chosen word, or zero for the final bits (`:319-362`) | **Present** | `make_calculate_final_word_activity` (`ui/mnemonic.c:446`); "Existing" / "Calculate" |
| **Address verification** | `tools_views.py:26` -> `ScanAddressView`; for single-sig, derives addresses from the `xpub` in order and compares (`seed_views.py:1956-2005`), with "Skip 10"; multisig descriptors are also checked (`:1984-1986`) | **Present, with different search controls** | `verify_address()` (`qrmode.c:1170`): single-sig, multisig and descriptor, batched search, a progress bar, skipping, and "Check next N?" (`:1347`). The search covers the receive and the change branch together, and for an unregistered wallet accounts 0, 1 and 2 plus whichever account the last xpub export used (`:867-974`), so reaching a change address costs no menu turn |
| **Backup verification test** | `SeedWordsBackupTestView:1244`; four choices for **every word** (one right, three random), a warning screen on a wrong answer (`:1335`), "Backup Verified" when all pass (`:1386`) | **Present, and repeatable; still a lighter quiz** | `Backup > Verify Backup` (`mnemonic.c:866`) -> `verify_wallet_words()` (`:730`), which runs the setup quiz again and says "Backup Verified" when it passes (`:735`). The quiz itself is unchanged and remains lighter than SeedSigner's: groups of three, **one random word** from each group, among 6 (12 words) or 8 (24 words) choices (`:528-529`), so 4 checks for 12 words and 8 for 24, against SeedSigner asking every word |
| Passphrase flow | `SeedFinalizeView:303` -> `SeedAddPassphraseView:348`; shows the fingerprints **side by side**, with and without the passphrase (`SeedReviewPassphraseView:416-448`) | **Partly, elsewhere** | Here the passphrase policy is a setting (`Options > Security > BIP39 Passphrase`: Frequency and Method, `ui/dashboard.c:200`) and entry happens in the login flow (`get_passphrase()`, `mnemonic.c:2161`). There is no screen showing the two fingerprints side by side |

---

### 3. Settings comparison

SeedSigner's settings live in `models/settings_definition.py:551-757`; each entry carries a
**visibility layer** (`GENERAL` / `ADVANCED` / `HARDWARE` / `HIDDEN`, `:384-388`) and the menu is
generated from those layers (`settings_views.py:33-56`).

The piJade settings tree (read from the code at this round; the whole tree was rebuilt since the
first version of this document, so nothing below carries over):

```
Options                                   main/process/dashboard.c:2771 (run_options_list)
|-- Add Wallet                            :2806  (only with a wallet already open)
|-- Temporary Signer                      :2810  (only with no wallet open)
|-- OTP                                   :2813  (only when an OTP record is usable)
|-- Mining                                :2820  (camera builds)
|-- USB Storage                           :2828  (ESP32-S3 with a battery only)
|-- Preferences                           :2831 -> :2857
|   |-- Idle Timeout                      :2871
|   |-- Screen Timeout                    :2873
|   |-- Network: Mainnet / Testnet        :2877  (conditional)
|   `-- QR Settings                       :2882  (QR Density, Frame Rate; ui/qrmode.c:259-269)
|-- Features                              :2832 -> :2518
|   `-- BIP85, Sign Msg, Warnings, Xpub Info, Singlesig, Multisig, Units   (FEATURE_ROWS, :2475)
|-- Display                               :2833 -> ui/dashboard.c:289
|   `-- Display Brightness, Flip Orientation, Theme, Camera Rotation       (:275-284)
|-- Security                              :2834 -> :2891
|   |-- Change PIN                        :2901  (locked, PIN configured)
|   |-- Change PIN (QR)                   :2905  (PIN wallet unlocked, camera builds)
|   |-- Duress PIN                        :2917  (PIN wallet unlocked)
|   `-- BIP39 Passphrase                  :2921
|-- Info                                  :2835 -> ui/dashboard.c:495
|   `-- firmware version (:477), Device Info (:482), I/O Test (:487), Legal (:491, official Jade hardware only)
`-- Factory Reset                         :2849
```

> Note: the `USB Storage` row is compiled only for ESP32-S3 boards with a battery
> (`dashboard.c:2822`), so it never appears in this port's menu.

The `Features` screen is what several rows of the table below now map onto: it is a flat list of
optional features, each a bit in one stored byte (`storage.h:53-59`), read where the feature is
offered rather than once per session. That is the same shape as SeedSigner's per-setting switches.

#### 3.1 What each SeedSigner setting maps to here

Together, sections 3.1 and 3.2 cover all 24 `SettingsEntry` values exactly once. This table has
twenty-one rows: eighteen settings and three menu or import extras. Counted at this round: four
are **absent** here, ten are **partly** covered, and seven have a full counterpart. Rows that
match are kept rather than dropped; equivalence is evidence too. `Show privacy warnings` moved
from absent to partly when the xpub warning screen was added; what keeps it out of the last
column is that SeedSigner splits its three warnings across two settings and this port has one.

| SS setting | Layer / default | SS evidence | piJade state |
|---|---|---|---|
| **Language** (interface language) | GENERAL / English | `settings_definition.py:554-560`, selection screen `settings_views.py:116` | **Absent.** The interface is English only |
| **Denomination display** (BTC / sats / threshold / mixed) | GENERAL / threshold 0.01 | `:579-585`, `:218-227` | **Partly.** `Options > Features > Units` switches between BTC and sats (`dashboard.c:2482`, read at `ui/sign_tx.c:45`); SeedSigner's threshold and mixed modes have no counterpart |
| **Bitcoin network** | ADVANCED / Mainnet | `settings_definition.py:589-595`; Mainnet / Testnet / Regtest (`:260-263`) | **Partly.** `Preferences > Network` (`dashboard.c:2874-2878`) offers Mainnet / Testnet for an open QR-mode wallet (`:2426-2460`). There is no separate Regtest choice; the address explorer resolves this restriction to Bitcoin mainnet or testnet (`qrmode.c:1560-1562`) |
| **QR code density** | ADVANCED / Medium | `settings_definition.py:597-603` | **Present.** `Preferences > QR Settings > QR Density` (`dashboard.c:2882`, `ui/qrmode.c:259-269`) offers Low / Medium / High (`qrmode.c:250-255`), persisted at `:1923-1924` and applied to PSBT and xpub QR sizes (`:226-247`). Default is Low here (`:200-208`), Medium there |
| **Sig types** (single-sig / multisig on or off) | ADVANCED / both on | `:605-612` | **Present.** `Features > Singlesig` and `> Multisig` (`dashboard.c:2480-2481`), read where the wallet type is offered (`qrmode.c:409-413`, `:452`). Turning both off is refused (`dashboard.c:2537-2542`) |
| **Script types** (which script types to offer) | ADVANCED / segwit + nested + taproot | `:614-621` | **Partly.** The xpub screen has a `Script` choice (`ui/qrmode.c:120`); there is no global "never offer these types" setting |
| **Xpub QR format** (animated / static / legacy Specter) | ADVANCED / both on | `:623-632` | **Partly.** Every export is `crypto-account`: the `crypto-hdkey` branch is disabled in the source itself (`qrmode.c:366-367`, `use_format_hdkey` fixed to false), and no format setting is offered; the file records the missing choice in a comment (`ui/qrmode.c:118`) |
| **Show xpub details** | ADVANCED / on | `:634-638` | **Partly.** `Features > Xpub Info` (`dashboard.c:2479`, `qrmode.c:682`) skips the description screen, but that screen shows only wallet type and derivation path (`ui/qrmode.c:62-70`). SeedSigner also shows the fingerprint and xpub text (`seed_views.py:942-947`) |
| **BIP-39 passphrase** | ADVANCED / Enabled | `settings_definition.py:640-646`; Enabled / Disabled / Required (`:31-33`) | **Partly.** `Security > BIP39 Passphrase` (`dashboard.c:2921`, `ui/dashboard.c:200`) has Disabled / Next Login Only / Always Ask and Manual / WordList (`dashboard.c:1466-1474`). There is no separate Required choice: entry offers Enter / Skip (`mnemonic.c:2166-2182`). SeedSigner's Required routes a scanned seed to entry (`scan_views.py:86-90`), but still permits skipping through a confirmation (`seed_views.py:370-411`) |
| **Compact SeedQR** | ADVANCED / on | `settings_definition.py:657-661`; disabling it bypasses the format picker for Standard (`seed_views.py:1428-1438`) | **Partly.** Both formats exist, but `mnemonic_export_qr()` asks Compact / Standard on each export (`mnemonic.c:138-140`); there is no stored switch to suppress Compact |
| **BIP-85 child seeds** on/off | ADVANCED / **off** | `:663-668` | **Present.** `Features > BIP85` (`dashboard.c:2476`), read where the row is laid out (`:3460`). Default is on here, off there |
| **Electrum seeds** on/off | ADVANCED / off | `:670-676` | **Absent** (so is the feature itself) |
| **Message signing** on/off | ADVANCED / **off** | `:686-690` | **Present.** `Features > Sign Msg` (`dashboard.c:2477`), read where the row is laid out (`:3453`). Default is on here, off there |
| **Show privacy warnings** | ADVANCED / on | `:692-697`; skips the xpub leak warning (`seed_views.py:863`) | **Partly: the screen exists, the separate switch does not.** `Export Xpub` opens a privacy warning before either branch (`qrmode.c:663-673`), worded for a 240px panel: "Whoever scans this / sees every address / and payment of this / wallet, forever." The same account keys leave by a second door, and that one warns too: exporting a registered multisig record (`dashboard.c:1258-1269`) says "This code holds every / signer's key. Whoever / scans it sees every / payment, forever." before it draws the code, and before the unsorted-multisig note, because that note cannot be answered with 'no'. SeedSigner has no counterpart to that screen at all: it scans a wallet descriptor (`tools_views.py:506`) and never exports one, so the only two exports it warns in front of are the xpub and the SeedQR. SeedSigner keeps this warning on a setting of its own; here it answers to `Features > Warnings` along with the dire ones, so a device cannot silence one and keep the other |
| **Show dire warnings** | ADVANCED / on | `:699-704`; skips the word-display warning (`seed_views.py:1019` -> `:1025`) and the SeedQR one (`:1491` -> `:1497-1498`) | **Partly: both warnings exist, one switch covers three.** `Features > Warnings` (`dashboard.c:2478`) guards the word banner in two places, at setup (`mnemonic.c:493-495`) and before the words screen (`:686-688`), and the SeedQR export as well (`:115-126`): "This code is your / wallet. Never scan or / photograph it into a / connected device." It is shown after the user accepts the export rather than before the offer, so a user who skips drawing never reads it. The same switch also carries the two privacy warnings in the row above, which SeedSigner keeps separate |
| **Show QR brightness tips** | ADVANCED / on | `:706-710` | **Absent.** SeedSigner conditionally overlays Brighter / Darker tips (`gui/screens/screen.py:791-844`). This fork has a brightness button (`ui/qrmode.c:31`, "P"), but no equivalent tip overlay or switch |
| **Camera rotation** | ADVANCED / 180 degrees | `:648-655` | **Present** (`Display > Camera Rotation`, `ui/dashboard.c:284`) |
| **QR background color** | HIDDEN / 62 | `settings_definition.py:750-756`; adjusted on QR screens and saved (`gui/screens/screen.py:893-910`) | **Partly.** The brightness button cycles five QR colours (`ui/qrmode.c:31`, `main/gui.c:128`, `:355-362`), but its index is only in memory (`main/gui.c:133`); SeedSigner saves its background brightness in Settings |
| **I/O test** | menu extra | `settings_views.py:18`, `:351` | **Present.** `Info > I/O Test` (`ui/dashboard.c:487`), the screen itself at `:505-525`: Screen, Buttons and Camera |
| **Version** | menu extra | `settings_views.py:20`, `:367` | **Present** (`Info`, the firmware version as the first row, `ui/dashboard.c:477`) |
| **SettingsQR** (importing settings by QR) | separate flow | `settings_views.py:310` | **Absent.** There is no general settings import. QR configuration handles specific messages, including Blind Oracle configuration (`ui/dashboard.c:336`) and clock synchronisation (`qrmode.c:2740`, `:2773`) |

#### 3.2 SeedSigner settings that do not apply to this port

These are not treated as gaps here: persistence is already provided, the hardware and project
options are specific to the port, and neither side offers another mnemonic language:

| SS setting | Why it does not apply |
|---|---|
| Persistent settings (`settings_definition.py:572`) | SeedSigner stores nothing by default; writing settings to the SD card is an option. Here settings are always persisted: the NVS partition on ESP32, the card's own settings file on this port (`libjade/pijade_settings.c:83-90`) |
| MicroSD notification duration (`settings_definition.py:678`) | A notification when the card is inserted or removed; here the card is the system itself |
| Show partner logos (`settings_definition.py:712`) | SeedSigner's splash-screen sponsor logos |
| Display type (`settings_definition.py:721`) | Choosing between panel drivers; this port is tied to one panel |
| Donate (`settings_views.py:19`) | The project's donation screen |
| Mnemonic language (`settings_definition.py:563`, HIDDEN) | BIP39 wordlist language; SeedSigner also ships with one language enabled (`:318-327`) |
| Invert colors (`settings_definition.py:731-738`, HARDWARE / off) | SeedSigner calls the display driver's `invert()` (`gui/renderer.py:53-54`). This port fixes inversion for its panel at initialisation (`pijade/host/panel_st7789.c:144`). `Display > Theme` changes the highlight colour (`main/gui.c:332-350`), not panel inversion |

---

### 4. Architectural constraints: which gaps can be closed and which need a decision

Two measured facts decide whether a row above is a gap or a decision:

1. **A wallet reloaded through PIN has no backup entropy in its slot.** The blob can hold
   serialised keys, mnemonic entropy, or a SLIP-0039 master secret behind a tag byte
   (`main/keychain.c:851-873`); current non-temporary setup caches entropy for storage regardless
   of passphrase (`mnemonic.c:2998-3001`). PIN unlock loads any of the three
   (`keychain.c:918-949`), derives keys if needed (`auth_user.c:239-265`), and clears the two
   caches (`keychain.c:105-113`, `:653`). It does not populate the slot's backup
   entropy (`keychain.c:96-101`). So `View Words`, `Export SeedQR`, `Verify Backup` and
   `Split (SeedXOR)` are unavailable after PIN reload. A newly created persistent wallet can
   still have backup entropy in its initial session when the passphrase is empty
   (`mnemonic.c:2668-2670`). The whole `Backup` row follows the slot's entropy condition
   (`dashboard.c:3470-3472`).
2. **For a wallet with a passphrase, entropy is deliberately not kept in its backup slot**
   (decision, 2026-08-31; `mnemonic.c:2668-2670`, stored only while `passphrase_len == 0`, the
   reasoning written at `:2662-2667`). The reason: a SeedQR carries the words alone, and scanning
   it back opens a **different** wallet, the one without the passphrase, unless it is entered
   again. This does not prevent the separate encrypted entropy storage described in item 1.
   SeedSigner's per-seed passphrase model is a different architecture; adopting it is not an
   automatic improvement but a separate decision.

Also, `Log Out`, `Sleep`, the idle timeout's power-off/reboot paths and a successful `Factory
Reset` clear the slot table with `wally_bzero` (`keychain.c:323-330`; `dashboard.c:3565`, `:3571`,
`:725`; `idletimer.c:275-283`). Backup helpers separately wipe their mnemonic buffers on return
(`mnemonic.c:396-402`, `:723-727`) and QR image data during cleanup (`:362-374`,
`qrcode.c:1148-1154`); the slot wipe alone does not establish that every display copy is gone.

---

### 5. The other direction (short note)

What this fork has and SeedSigner does not was not inventoried, because the question ran one way.
Still, worth knowing when planning: a persistent encrypted wallet with a PIN, the duress PIN, OTP,
the Blind Oracle, the idle timeout, persistent multisig and descriptor registrations, factory
reset, and an eight-slot wallet list (`keychain.h:40`). Bluetooth and USB/serial are shared-code
capabilities, not interfaces this port exposes (`dashboard.c:797-801`). SeedSigner already uses
wallet descriptors (`models/decode_qr.py:213-227`) and can persist settings; its seeds remain in
memory (`models/seed.py:30-35`). So "SeedSigner does not have it" is not by itself a sign of a
gap.

Six differences sit inside the ground this comparison covers, so they are named below; where
both sides have the feature, the row identifies the narrower difference:

| Feature | Where | What it is |
|---|---|---|
| **SLIP-0039 recovery** | `main/slip39.c`, `main/shamir.c`; `Restore Wallet > Split Backup > SLIP39` (`ui/mnemonic.c:117`) | Restoring a wallet from Shamir shares: typed or scanned, 20 or 33 words, one group or several.  Recovery only, by decision; this fork does not produce shares.  Kept behind a PIN, such a wallet is written to the card as its master secret behind a tag byte and re-derived on unlock (`main/keychain.c:859-868`, `:930-944`), so it comes back carrying a seed and the seed-gated screens (OTP, identity) keep working after a restart.  SeedSigner has no SLIP-0039 path at all, and no persistence to put behind one, so this row is an addition rather than a narrower difference |
| **Seed XOR** | `main/seedxor.c`; `Backup > Split (SeedXOR)` (`mnemonic.c:870`), `Restore Wallet > Split Backup > SeedXOR` (`ui/mnemonic.c:116`) | Splitting a wallet into parts, each a valid BIP39 mnemonic of its own, and combining them back. Not a threshold scheme: every part is needed |
| **BBQr text import** | `main/bbqr.c`; the scanner branches on the file type at `qrmode.c:2910-2931` | Both sides read BBQr PSBTs; SeedSigner recognises type P (`models/decode_qr.py:366-367`, decoder at `:724`). This fork also routes type U text to its text import handlers (`qrmode.c:2923-2927`), including multisig setup files (`:2085-2090`). Reading only; this fork does not produce BBQr |
| **Combined entropy** | `Entropy Source > Combined` (`entropy_sources.c:227`) | Dice and the camera chain that ends in the device CSPRNG, gathered apart and hashed together, so no single source has to be trusted |
| **Camera health thresholds** | `entropy_sources.h:52`, `:88`; applied at `entropy_sources.c:392-404` | A pixel-range floor of 8 and a block-average change threshold of 128. SeedSigner already rejects flat and duplicate frames (`gui/screens/tools_screens.py:100-119`); these stricter variation thresholds are the difference, not the existence of health checks |
| **Backup verification on demand** | `Backup > Verify Backup` (`mnemonic.c:866`) | A dedicated wallet-menu entry to this fork's lighter quiz (`mnemonic.c:528-529`). SeedSigner can also rerun its quiz after setup, through Backup seed > View seed words (`seed_views.py:653-654`, `:1100-1104`); the difference is the dedicated entry |

---

### 6. Classifying the gaps

Measured again at this round against the code, seven of the eight items this section used to list
are closed: matching a psbt to the right slot, the address explorer, the message-signing menu
entry, running the backup verification test outside setup, the `I/O test` screen, turning BIP-85
off, and (section B) viewing a loaded wallet's words. The eighth, "settings to skip warning
screens", turned out to be the wrong shape: SeedSigner guards three warnings across two settings
(`seed_views.py:863`, `:1019`, `:1491`), so the question was never a missing switch but the two
screens behind it. Both have since been written - the xpub privacy warning (`qrmode.c:663-673`)
and the SeedQR transcription warning (`mnemonic.c:115-126`) - and a third was written where
SeedSigner has no screen to compare against, in front of the registered-multisig export
(`dashboard.c:1258-1269`), because that file carries the account key of every signer and this
port, unlike SeedSigner, can export one. All three answer to the one `Features > Warnings` flag,
alongside the word banner it already carried (`mnemonic.c:493`, `:686`). What remains is the
shape of the switch, recorded in section 3.1 rather than here: one flag covers them all, so
these warnings cannot be silenced separately.
The rest of the list is what the settings comparison in section 3.1 turned up.

**A. Real gaps that can be closed without an architectural decision**

| Item | Why it matters | Likely place |
|---|---|---|
| An xpub QR format choice | Every export is `crypto-account`; the `crypto-hdkey` branch is disabled in the source (`qrmode.c:366-367`) and the missing choice is recorded in a comment (`ui/qrmode.c:118`) | `Xpub Settings` (`ui/qrmode.c:109-126`) |
| A standing restriction on script types | The xpub screen offers a `Script` choice per export; SeedSigner lets the user take types off the list once | `Options > Features` |
| Denomination beyond BTC / sats | `Features > Units` has two states; SeedSigner also has a threshold and a mixed mode (`settings_definition.py:579-585`) | `Options > Features`, where `Units` already is |
| A QR brightness tip overlay and switch | SeedSigner displays Brighter / Darker tips under a setting (`gui/screens/screen.py:791-844`); this fork has only the brightness button (`ui/qrmode.c:31`) | QR display UI, with a switch in `Options > Features` |
| Xpub details beyond type and path | SeedSigner shows fingerprint and xpub text (`seed_views.py:942-947`); this fork's description has only type and path (`ui/qrmode.c:62-70`) | The description screen controlled by `Features > Xpub Info` |
| A separate Regtest network choice | SeedSigner offers Mainnet / Testnet / Regtest (`settings_definition.py:260-263`); this fork's explorer resolves only mainnet or testnet (`qrmode.c:1560-1562`) | `Preferences > Network` and the consumers of its network choice |
| A stored switch to suppress Compact SeedQR | SeedSigner bypasses the format picker when Compact is disabled (`seed_views.py:1428-1438`); this fork always asks the format (`mnemonic.c:138-140`) | Export preferences and `mnemonic_export_qr()` |
| Persisting QR background brightness | SeedSigner saves the adjusted value (`gui/screens/screen.py:910`); this fork only changes an in-memory index (`main/gui.c:355-362`) | QR display preferences |

**B. Needs the words, so it can only work on slots that carry entropy**

No open items. The constraint itself still holds and is now expressed in one place: the whole
`Backup` group is offered only where `keychain_slot_has_entropy()` is true
(`dashboard.c:3470-3472`), so `View Words`, `Export SeedQR`, `Verify Backup` and
`Split (SeedXOR)` share a single condition instead of each failing when pressed.

**C. Items that need a decision and are not automatically "missing"**

| Item | Why it is a decision |
|---|---|
| A per-seed passphrase model (SeedSigner style) | Today's architecture keeps the passphrase as device policy; changing it reopens the entropy decision too |
| `Discard` for a persistent wallet | Today `Log Out` drops them all at once; dropping one runs into the `keychain_load()` constraint (`dashboard.c:3475-3478`) |
| Electrum seeds | A new seed format is a new validation surface |
| Importing settings by QR (SettingsQR) | A new parser on the one input this device has, and a way to change the device's behaviour without the menus; the security question comes before the convenience |
| Interface language (Turkish included) | Needs font and localisation infrastructure; in SeedSigner Turkish is still marked incomplete (`settings_definition.py:149`) |

**D. Does not apply:** the seven items in section 3.2.

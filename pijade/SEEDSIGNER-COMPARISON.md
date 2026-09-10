# SeedSigner comparison ; seed menu and settings

> Date: 2026-08-31
> Versions compared: **SeedSigner `d70b322`** and **piJade `bbb-airgap`** (after
> phases 1 / L / 3 / 2)
> The piJade side was read at a development commit whose hash no longer resolves: the
> published history was squashed into a single starting commit afterwards. The date and the
> phase list above are the anchors.
> Method: reading the code. `views/seed_views.py` (2270 lines), `views/tools_views.py` (775),
> `models/settings_definition.py` (814), `views/settings_views.py` (387) and `views/view.py` were
> read in full or against specific questions; the piJade side was verified through
> `main/process/dashboard.c`, `main/ui/dashboard.c`, `main/process/mnemonic.c`,
> `main/ui/mnemonic.c`, `main/qrmode.c`, `main/ui/qrmode.c`, `main/entropy_sources.{c,h}` and
> `main/keychain.c`.
> The list runs **one way**: what SeedSigner has and this fork does not. What this fork has and
> SeedSigner does not is not an inventory here; it is mentioned in one paragraph in section 5.

**Scope statement:** the question was seed management and use. SeedSigner keeps its seed
**generation** and **verification** tools in a separate `Tools` menu rather than under the
fingerprint (`views/tools_views.py:21`). Reading only `SeedOptionsView` would have missed that
layer, so Tools was deliberately included. On the settings side the single source of truth is
`models/settings_definition.py`; `settings_views.py` is only a generic renderer
(`settings_views.py:33-36`).

---

## 1. The seed menu ; row by row

**SeedSigner:** `Home > Seeds > <fingerprint>` ; `SeedOptionsView` (`views/seed_views.py:528`),
menu rows built at `:572-587`, the title being the seed's fingerprint (`:592`).

**piJade:** `Session > <fingerprint>` ; `handle_session()` (`main/process/dashboard.c:2517`),
wallet rows built at `:2596-2616`, the title being the slot's fingerprint (`:2594`).

| # | SeedSigner row | SS evidence | piJade state | Evidence / note |
|---|---|---|---|---|
| 1 | **Scan transaction** (PSBT) | `seed_views.py:574` -> `:601` (`controller.psbt_seed = self.seed`) | **A real gap.** The first version called this a difference in information architecture; measured again over the multiple-slot question, that verdict did not hold | In SeedSigner the seed that will sign is chosen first. Here the ownership test uses **only the active slot's** key: `psbt.c:134` derives from `keychain_get()->xpriv` and a non-matching input is skipped (`sign_psbt.c:814-817`). The other slots are never tried. If no input matches, the flow **does not stop**: the user passes the output and fee screens and only then sees "There are no relevant inputs to be signed" (`sign_psbt.c:1053-1055`), and an unsigned PSBT comes back. Nothing suggests the right slot, switches to it, or stops early |
| 2 | **Export xpub** | `seed_views.py:576` -> `SeedExportXpubSigTypeView:664` | **Present** | `dashboard.c:2596` -> `display_xpub_qr()`. This fork also has `Xpub Settings` (Script / Wallet / Account Index / QR Settings) at `ui/qrmode.c:121-128` |
| 3 | **Address explorer** | `seed_views.py:578` -> `ToolsAddressExplorerAddressTypeView:568`; receive and change addresses, ten per page, a QR for each (`tools_views.py:665, 753`) | **Absent** | There is no screen here that **derives and lists** addresses. `Verify Address` is a different job (it searches for a given address) |
| 4 | **Backup seed** (submenu) | `seed_views.py:579` -> `SeedBackupView:628` | **No grouping**; one of the two items exists | See 4a / 4b below |
| 4a | ; **View seed words** | `SeedBackupView:629` -> `SeedWordsWarningView:1002` -> `SeedWordsView:1037` (four words per page) | **Absent** | Words are shown only during setup (`mnemonic.c:356` `display_confirm_mnemonic`) and for BIP85 child words (`ui/mnemonic.c:98`). There is no way to see the words of a loaded wallet afterwards |
| 4b | ; **Export as SeedQR** | `SeedBackupView:630` (guarded by `seed.seedqr_supported`), format choice `:1412`, warning `:1472`, full QR `:1510`, zoom `:1548`, scan-back verification `:1595-1721` | **Present and equivalent** | `dashboard.c:2606` -> `export_wallet_seedqr()` (`mnemonic.c:311`). Standard and Compact, a guide grid, and scan-back verification through the camera (`mnemonic.c:261-285`) |
| 5 | **Sign message** | `seed_views.py:582`, guarded by `SETTING__MESSAGE_SIGNING == ENABLED` (**off** by default, `settings_definition.py:686-690`) | **Capability present, menu entry absent** | `sign_message_file()` runs only when a "signmessage" QR is scanned (`qrmode.c:1101-1121`) or over USB/serial RPC. It cannot be started from the wallet menu |
| 6 | **BIP-85 child seed** | `seed_views.py:585`, guarded by `SETTING__BIP85_CHILD_SEEDS == ENABLED` (**off** by default, `:663-668`) and `seed.bip85_supported` | **Present, and unguarded** | `dashboard.c:2599` -> `handle_bip85_mnemonic()`; 12 or 24 words (`ui/mnemonic.c:98`) |
| 7 | **Discard seed** | `seed_views.py:587` -> `SeedDiscardView:459`; the screen reads "Wipe seed {fingerprint} from the device?" (`:478`) | **Partly: temporary wallets only** | `dashboard.c:2613-2615`; no row appears for a persistent wallet, and the way to drop one is `Log Out` (`:2661`). The reason is written in the code (`:2609-2612`): `keychain_load()` refuses to read the blob back while a wallet is in memory |

### 1.1 One more difference: the fingerprint list itself

SeedSigner's `SeedsMenuView` (`seed_views.py:26`) puts one uniform icon next to each fingerprint
(`:45`) and ends the list with a **"Load a seed"** row (`:46`), so a new seed can be loaded straight
from the list. Here the `Session` list marks the persistent/temporary distinction with a symbol
(`dashboard.c:2554`, filled circle = persistent, hollow = temporary) ; a difference in this fork's
favour ; but there is no row for loading a new wallet; that happens through `Scan QR` or `Options`.

---

## 2. Loading and generating seeds (SeedSigner `Tools` and `LoadSeedView`)

| Feature | SS evidence | piJade state | Evidence / note |
|---|---|---|---|
| Load by scanning a SeedQR | `LoadSeedView:163` | **Present** | `Restore Wallet > Scan QR` (`ui/mnemonic.c:80`); in QR mode, `Scan SeedQR` (`ui/dashboard.c:165`) |
| 12 / 24 words entered by hand | `LoadSeedView:164-165` | **Present** | `ui/mnemonic.c:78-79`, word entry through `make_enter_wordlist_word_activity` (`ui/mnemonic.c:269`) |
| Electrum seed entry | `LoadSeedView:166`, guarded by `SETTING__ELECTRUM_SEEDS` (**off** by default, `settings_definition.py:670-676`) | **Absent** | No non-BIP39 seed format is supported |
| New seed ; **camera entropy** | `tools_views.py:22` -> `:62-225`; device id plus time plus frames, chained through sha256 | **Present, same scheme** | `main/entropy_sources.h:31-34`, `entropy_sources.c`; `Advanced Setup > Entropy Source > Camera`. This fork adds a frame-to-frame change threshold (`entropy_sources.h:36-41`) |
| New seed ; **dice entropy** | `tools_views.py:23` -> `:232-288` | **Present, same scheme** | `entropy_sources.h:21-29` (50 or 99 rolls; the code records that this follows SeedSigner's `dice_verification.md` scheme) |
| New seed ; device RNG | Not in SeedSigner (camera and dice only) | **Present** | `Entropy Source > Device` (menu at `entropy_sources.c:196`) |
| **Calculating the 12th / 24th word** | `tools_views.py:24` -> `ToolsCalcFinalWordNumWordsView:295`; coin flips, a chosen word, or zero for the final bits (`:319-362`) | **Present** | `make_calculate_final_word_activity` (`ui/mnemonic.c:362`); "Existing" / "Calculate" |
| **Address verification** | `tools_views.py:26` -> `ScanAddressView`; for single-sig, derives addresses from the `xpub` in order and compares (`seed_views.py:1956-2005`), with "Skip 10" | **Present and deeper** | `verify_address()` (`qrmode.c:745`): single-sig, multisig and descriptor, batched search, a progress bar, skipping, and "look at the next N addresses?" (`:891-902`) |
| **Backup verification test** | `SeedWordsBackupTestView:1244`; four choices for **every word** (one right, three random), a warning screen on a wrong answer (`:1335`), "Backup Verified" when all pass (`:1386`) | **Partly, and only during setup** | `mnemonic.c:408-477`: groups of three, **one random word** from each group, among 6 (12 words) or 8 (24 words) choices. So 4 checks for 12 words, 8 for 24. There is no way to run the test again once setup is done |
| Passphrase flow | `SeedFinalizeView:303` -> `SeedAddPassphraseView:348`; shows the fingerprints **side by side**, with and without the passphrase (`SeedReviewPassphraseView:416-448`) | **Present, elsewhere** | Here the passphrase policy is a setting (`Options > BIP39 Passphrase`: Frequency and Method, `ui/dashboard.c:243`) and entry happens in the login flow (`mnemonic.c:1299`). There is no screen showing the two fingerprints side by side |

---

## 3. Settings comparison

SeedSigner's settings live in `models/settings_definition.py:551-757`; each entry carries a
**visibility layer** (`GENERAL` / `ADVANCED` / `HARDWARE` / `HIDDEN`, `:384-388`) and the menu is
generated from those layers (`settings_views.py:33-56`).

The piJade settings tree (verified against the code):

```
Options (unlocked)                        main/ui/dashboard.c:324
├── Device                                :360
│   ├── Settings                          :372
│   │   ├── Display                       :404  (Brightness, Flip Orientation, Theme, Camera: 90)
│   │   ├── Idle Timeout / Network Type    :378, :391 (conditional)
│   │   ├── Bluetooth / Change PIN         :379, :398 (conditional)
│   │   └── QR Settings                    ui/qrmode.c:232 (QR Density, Frame Rate)
│   ├── Factory Reset                      :366
│   └── Info                               :618 (firmware version, Device Info: MAC / battery / storage, Legal)
└── Authentication                         :455 (Duress PIN, OTP, Change PIN (QR))
```

> Note: the `USB Storage` row is compiled only for ESP32-S3 boards with a battery
> (`ui/dashboard.c:333-335`), so it never appears in this port's menu.

### 3.1 What each SeedSigner setting maps to here

The table walks every setting SeedSigner offers the user, in order. Eleven rows are **absent**
here, three are **partly** covered, and three (camera rotation, colour theme, version) already
have a full counterpart; equivalence is evidence too, so those rows were kept.

| SS setting | Layer / default | SS evidence | piJade state |
|---|---|---|---|
| **Language** (interface language) | GENERAL / English | `settings_definition.py:554-560`, selection screen `settings_views.py:116` | **Absent.** The interface is English only |
| **Denomination display** (BTC / sats / threshold / mixed) | GENERAL / threshold 0.01 | `:579-585`, `:218-227` | **Absent.** Amounts are always BTC with eight decimal places (`ui/sign_tx.c:455`) |
| **Sig types** (single-sig / multisig on or off) | ADVANCED / both on | `:605-612` | **Absent** as a setting. The wallet type can be chosen in the xpub flow (`ui/qrmode.c:123`), but that is not a standing restriction |
| **Script types** (which script types to offer) | ADVANCED / segwit + nested + taproot | `:614-621` | **Partly.** The xpub screen has a `Script` choice (`ui/qrmode.c:122`); there is no global "never offer these types" setting |
| **Xpub QR format** (animated / static / legacy Specter) | ADVANCED / both on | `:623-632` | **Partly.** The code chooses between `crypto-hdkey` and `crypto-account` (`qrmode.c:343`), but no format setting is offered to the user; the file records this as a TODO (`ui/qrmode.c:120`) |
| **Show xpub details** | ADVANCED / on | `:634-638` | **Absent.** The detail screen is always shown |
| **BIP-85 child seeds** on/off | ADVANCED / **off** | `:663-668` | **Absent.** BIP85 is always in the menu here (`dashboard.c:2599`) |
| **Electrum seeds** on/off | ADVANCED / off | `:670-676` | **Absent** (so is the feature itself) |
| **Message signing** on/off | ADVANCED / **off** | `:686-690` | **Absent.** Message signing over the QR path is always available here (`qrmode.c:1101`) |
| **Show privacy warnings** | ADVANCED / on | `:692-697`; skips the xpub leak warning (`seed_views.py:863`) | **Absent.** The warnings are fixed |
| **Show dire warnings** | ADVANCED / on | `:699-704`; skips the word-display and SeedQR warnings (`seed_views.py:1019`, `:1491`) | **Absent** |
| **Show QR brightness tips** | ADVANCED / on | `:706-710` | **Partly.** QR screens here have a brightness button (`ui/qrmode.c:31`, "P"), but no setting for the tip text |
| **Camera rotation** | ADVANCED / 180 degrees | `:648-655` | **Present** (`ui/dashboard.c:439`, `Display > Camera`) |
| **Invert colors** | HARDWARE / off | `:731-738` | **Counterpart present:** `Display > Theme` (`ui/dashboard.c:419`) |
| **I/O test** | menu extra | `settings_views.py:18`, `:351` | **Absent.** There is no button/screen/camera hardware test screen |
| **Version** | menu extra | `settings_views.py:20`, `:367` | **Present** (`Info > firmware version`, `ui/dashboard.c:640`) |
| **SettingsQR** (importing settings by QR) | separate flow | `settings_views.py:310` | **Absent.** QR configuration here exists only for the Blind Oracle (`ui/dashboard.c:510`) |

### 3.2 SeedSigner settings that do not apply to this port

These are not gaps; they follow from SeedSigner's architecture and can have no counterpart here:

| SS setting | Why it does not apply |
|---|---|
| Persistent settings (`:572`) | SeedSigner stores nothing by default; writing settings to the SD card is an option. Here settings are already persistent in NVS |
| MicroSD notification duration (`:678`) | A notification when the card is inserted or removed; here the card is the system itself |
| Show partner logos (`:712`) | SeedSigner's splash-screen sponsor logos |
| Display type (`:721`) | Choosing between panel drivers; this port is tied to one panel |
| Donate (`settings_views.py:19`) | The project's donation screen |
| Mnemonic language (`:563`, HIDDEN) | BIP39 wordlist language; SeedSigner also ships with one language enabled (`:318-327`) |
| QR background color (`:750`, HIDDEN) | An internal value never offered to the user |

---

## 4. Architectural constraints ; which gaps can be closed and which need a decision

Two measured facts decide whether a row above is a gap or a decision:

1. **A persistent wallet unlocked by PIN, with no passphrase, holds no words.** The blob stores a
   serialised key (xpriv), not entropy (`main/keychain.c:777-779`). Entropy reaches the blob only
   for a wallet with a passphrase (`:767-774`). So every feature that needs the words
   (`View seed words`, `Export SeedQR`) cannot work on a persistent wallet. Today's menu already
   knows this: the `Export SeedQR` row appears only where the slot carries entropy
   (`dashboard.c:2604`).
2. **For a wallet with a passphrase, entropy is deliberately not kept** (decision, 2026-08-31;
   `mnemonic.c:1356` with `:1403-1404`, stored only while `passphrase_len == 0`). The reason: a
   SeedQR carries the words alone, and scanning it back opens a **different** wallet, the one
   without the passphrase. SeedSigner's per-seed passphrase model is a different architecture;
   adopting it is not an automatic improvement but a separate decision.

Also, `Log Out`, `Sleep`, the idle timeout and `Factory Reset` all clear the slot table with
`wally_bzero` (`keychain.c:281`); a new screen that displays words has to stay inside that
contract and must not leave a copy outside it.

---

## 5. The other direction (short note)

What this fork has and SeedSigner does not was not inventoried, because the question ran one way.
Still, worth knowing when planning: a persistent encrypted wallet with a PIN, the duress PIN, OTP,
the Blind Oracle, the idle timeout, Bluetooth management, registered multisig wallets, descriptor
support, the USB/serial protocol, factory reset, and an eight-slot wallet list. All of SeedSigner
is per-session, while this device has a persistent layer; so "SeedSigner does not have it" is not
by itself a sign of a gap.

---

## 6. Classifying the gaps

**A. Real gaps that can be closed without an architectural decision**

| Item | Why it matters | Likely place |
|---|---|---|
| Matching a PSBT to the right slot (and a signing entry under the fingerprint) | With several slots the device does not find the right wallet itself; from the wrong slot the user passes every confirmation screen and ends up with unsigned output | A guard after the input loop in `sign_psbt.c`, plus a `Session > <fingerprint>` row |
| An address explorer (deriving and listing receive and change addresses, a QR for each) | The verification tool most often wanted on an airgapped device; the xpub derivation is already there (`wallet_search_for_singlesig_script` and `get_singlesig_search_root`) | A new screen plus a `Session > <fingerprint>` row |
| Wiring message signing into the menu | The capability is written and works; only the door is missing (`qrmode.c:1101`) | `Session > <fingerprint> > Sign Message` -> QR scan |
| Running the backup verification test outside setup | Today the test runs only during setup and is partial (4 checks for 12 words) | Under `Session > <fingerprint>`, for slots that carry entropy |
| An `I/O test`-style hardware screen | The camera and the button layout were changed by hand in this port; this makes field diagnosis easier | Under `Options > Device > Info` |
| Settings to skip warning screens (privacy / dire) | Warning fatigue in frequent use; SeedSigner made both of them settings | `Options > Device > Settings` |
| Being able to turn BIP-85 off | It shows on every wallet today; SeedSigner keeps it off by default | Same place |

**B. Needs the words, so it can only work on slots that carry entropy**

| Item | Constraint |
|---|---|
| `View seed words` (showing a loaded wallet's words) | Impossible on a persistent PIN wallet (section 4.1). In the menu it has to carry the same condition as `Export SeedQR` (`keychain_slot_has_entropy`) |

**C. Items that need a decision and are not automatically "missing"**

| Item | Why it is a decision |
|---|---|
| A per-seed passphrase model (SeedSigner style) | Today's architecture keeps the passphrase as device policy; changing it reopens the entropy decision too |
| `Discard` for a persistent wallet | Today `Log Out` drops them all at once; dropping one runs into the `keychain_load()` constraint (`dashboard.c:2609-2612`) |
| Electrum seeds | A new seed format is a new validation surface |
| A denomination setting (BTC / sats) | A display preference that touches every signing screen |
| Interface language (Turkish included) | Needs font and localisation infrastructure; in SeedSigner Turkish is still marked incomplete (`settings_definition.py:149`) |

**D. Does not apply:** the seven items in section 3.2.

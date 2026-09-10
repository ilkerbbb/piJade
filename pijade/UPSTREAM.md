# Upstream compatibility discipline

> piJade is a fork of Blockstream Jade. The aim: to be able to take an upstream Jade release
> without the update turning brittle. This file keeps the size of the divergence, and the
> procedure for updating, in one place.

## 1. Remote layout

| Remote | Address | Use |
|---|---|---|
| `upstream` | https://github.com/Blockstream/Jade.git | Read only; NEVER pushed to |
| `origin` | https://github.com/ilkerbbb/piJade.git | Where the fork is published; made public on 2026-09-08, default branch `bbb-airgap` |

The `master` branch is kept as a mirror of upstream; no change is ever written on top of `master`.
All work happens on `bbb-airgap`.

## 2. Divergence inventory (2026-09-07, after the OTP clock round)

> The numbers were measured with `git diff --numstat fdb67a3f..HEAD -- . ':(exclude)pijade'`.
> `fdb67a3f` is the branch point. `pijade/` is ours and has no upstream counterpart, so it does
> not enter the table.
>
> The reason column has two sources: hand-written rows explain why the divergence exists; rows
> added in the 2026-09-06 refresh instead carry the subject line of the commit that changed that
> file MOST. The second kind is a record, not a reason; it does not stand in for a sentence
> explaining the divergence, and it must be written by hand the next time the file is touched.

| File | Added / removed | Kind | Reason |
|---|---|---|---|
| `main/qrmode.c` | +1439 / -123 | Upstream file | Wallet QR codes go full screen; the information screen and the code screen were separated. Also the xpub density/rate ladder: xpub transfer follows the user's QR setting, and a device with no setting is treated as Low |
| `components/miner/miner.c` | +1109 / -0 | **New file** | feat(miner): take in the mining component, write two sims, close three defects; fix(miner): close three P2 and three P3 findings from review round 1 |
| `main/process/dashboard.c` | +1169 / -179 | Upstream file | The main menu submenu; the camera rotation setting and its label; the `QR Settings` event branch; the same macro added to both board gates of the brightness handler; the Select Connection back event clears a sourceless wallet and returns to the home screen; `Session > Sleep` draws an information screen before shutting down (`#ifdef CONFIG_LIBJADE`; the Pi cannot cut its own supply, so the user learns of the shutdown from the screen, and the message deliberately does not say when the power may be pulled, with the reason written in a comment in the code); the `Set Clock` event first shows the page address on a back/continue screen, and on `Continue` opens the `handle_scan_qr()` flow and leaves the menu loop (a scan can also load a wallet, so screens the menu was holding may be released), while the back arrow keeps it in the menu; `handle_scan_qr()` now takes the help address as a parameter; the `Buttons` check (`handle_io_test_buttons()`): each input turns its own mark green, centre click and KEY2 produce the same event so both light up together, and KEY3 leaves the screen |
| `libjade/selfcheck/descriptor.c` | +988 / -0 | **New file** | test: descriptor tests moved into the libjade verification branch; bcur: correct skipping over tagged map values, and the descriptor selfcheck now covers the QR paths |
| `main/bcur.c` | +824 / -21 | Upstream file | Full-screen scale; upstream's table as the floor, the panel as the ceiling, quiet zone in between. QR version 3 support (`BCUR_FRAGMENT_SIZE_V3`): upstream's lower bound was 4, and xpubs fit into 3. Also the limits of 'I'-filtered generation, and treating fragment content as wallet data |
| `main/process/mnemonic.c` | +596 / -96 | Upstream file | Entropy source selection; the two SeedQR export formats (Compact and Standard), a bounds check against the silent overflow in `qrcode_initText()`, and not logging the word count |
| `main/entropy_sources.c` | +549 / -0 | **New file** | Dice and camera entropy; it does not spread into upstream files. The dice screen is drawn with Jade's own vocabulary: the `ui/digit_entry.c` cell (35 px, a 25/50/25 vertical split, `K`/`L` arrows, DEJAVU24 for the value), the `make_progress_bar()` bar (solid fill; not transparent, because undo lowers the value) and a counter strip. Band margins and cell gaps step with panel height (`DICE_TALL_PANEL`), for the same reason as `ui/digit_entry.c` |
| `libjade/libjade.c` | +547 / -40 | Emulator layer | A real implementation of `sensitive_push/pop`, `esp_efuse_mac_get_default`, host callbacks, removal of the keyboard stubs, handing power requests to the host, the clock handler, and pulling the version string from a placeholder to the real value; the backlight level is handed to the host; `libjade_start()` resets the tick base and the serial state; `libjade_stop()` calls `idletimer_request_stop()` first and stops the idle task before the GUI; settings persistence covers all five namespaces and passes a factory reset on as a separate erase request; two-axis input routes FIRST and ALT calls to the GUI. `libjade_activity_generation()` exposes the screen-change counter, and `libjade_jobs_posted()` and `libjade_jobs_drained()` expose the GUI work queue's produce and consume counters, to the host. |
| `main/ui/dialogs.c` | +493 / -1 | Upstream file | `await_choice_activity()`: a two-option question whose labels the caller supplies; for screens that ask for a choice rather than a confirmation |
| `main/descriptor_text.c` | +537 / -0 | **New file** | descriptor: a parser reducing text and Specter JSON descriptors to wally's canonical policy-template form |
| `libjade/pijade_settings.c` | +377 / -0 | **New file** | The format of the records written to the card (`PIJADES4`), namespace-aware validation, and the serialiser checking its own output; it does not spread into upstream files |
| `libjade/selfcheck/mining.c` | +359 / -0 | **New file** | feat(miner): take in the mining component, write two sims, close three defects; fix(miner): close three P2 and three P3 findings from review round 1 |
| `main/keychain.c` | +289 / -35 | Upstream file | feat(keychain): a slot table instead of a single static wallet (phase 3, step A); feat(keychain): wire the SeedQR export into the Session menu |
| `main/gui.c` | +457 / -16 | Upstream file | Camera rotation state; read from the stored setting, defaulting to 90 degrees. Also a white QR background by default; vertical neighbour selection by coordinate, a horizontal fallback, FIRST selection and the ALT event. On a board with no battery, the icon strip of the home screen status bar: the invisible battery column is moved from the right to the left, and the Bluetooth icon at the end of the row is pushed right, so the two visible icons sit against the right edge; on boards with a battery the layout is exactly upstream's. Input echo (`gui_set_input_echo()`): while it is on, `gui_up()`, `gui_down()` and `gui_select_first()` emit an event naming themselves instead of navigating, because on a screen with no selectable item the first two fall through to a horizontal event and the third emits nothing at all; only the Buttons check turns it on. The vertical echo follows the screen's rotation (the same event navigation would emit), because the marks rotate with the picture; `gui_stop()` clears the flag when the session ends. The activity generation counter (`activity_generation`): incremented whenever the gui task actually changes the current activity, read through `gui_get_activity_generation()`; it lets the host's input gate tell a redraw of the same screen from a change of screen. The job counters (`gui_jobs_posted`, `gui_jobs_drained`): the number of jobs queued and dequeued, read through `gui_get_jobs_posted()` and `gui_get_jobs_drained()`; because the queue is FIFO, the host knows from these that a frame which has reached the produce count it read after a press carries that press's work. |
| `libjade/selfcheck/urldecode.c` | +188 / -0 | **New file** | urldecode: add self-tests for validation and decode |
| `libjade/libjade.h` | +221 / -0 | Emulator layer | The host integration API; declarations only, touching no existing line; `libjade_set_backlight_handler`; the clock handler; the settings callback contract covering the five namespaces and the zero-length erase request; the UP, DOWN, FIRST and ALT input contract. The `libjade_activity_generation()` declaration and its contract (callable from any thread). `libjade_jobs_posted()` and `libjade_jobs_drained()`: the number of jobs handed to the GUI task and taken off its queue; the host measures which frame carries a press's work with these, compared as a wrap-safe signed difference. |
| `main/idletimer.c` | +177 / -17 | Upstream file | The libjade branch: an exit path for the task (`idletimer_stop()`), on the same pattern as `gui_stop()`. The running flag is lowered by a POSIX cleanup handler; the lifecycle flags are `_Atomic bool`; `idletimer_register_activity()` returns `false` while the timer is not ready |
| `main/ui/qrmode.c` | +126 / -84 | Upstream file | Two information-screen constructors and the full-screen code activity; a `QR Settings` shortcut in `Xpub Settings` |
| `main/ui/dashboard.c` | +232 / -158 | Upstream file | "Camera" in the Display menu; `QR Settings` in the `Settings` menu; the home screen's selected tile split `HOME_SCREEN_SELECTED_TILE_PCT` (the label font follows upstream's height rule); the `Display Brightness` entry; the Select Connection title back button and an explicit first menu selection; a `Factory Reset` entry in the uninitialised menu (`#ifdef CONFIG_LIBJADE`; upstream offers the same entry in the Boot Menu reached by clicking at startup, and that menu is structurally unreachable in this port, with the reason written in a comment in the code); a `Set Clock` row in the OTP menu (camera-conditional); a `Buttons` row in the `I/O Test` menu and the drawing of that screen (`make_io_test_buttons_activity()`); the `Buttons` row sits behind `CONFIG_LIBJADE`, because the only way out of that screen is `GUI_ALT_EVENT` and only `libjade_input()` calls `gui_alt_click()`; official Jade hardware has no third button |
| `main/storage.c` | +117 / -7 | Upstream file | security: the duress PIN is not written to the card in the clear; fix(storage): on the erase PIN, delete the blob first and stay fail-closed if marking fails |
| `main/process/register_multisig.c` | +116 / -45 | Upstream file | fix(multisig): network equality on registration, and hygiene for hidden data in the parser; multisig: record v4, body sealed with AES; the legacy v0-v2 read paths were removed; the same-record check happens in the clear |
| `main/entropy_sources.h` | +113 / -0 | **New file** | Same |
| `main/ui/mnemonic.c` | +99 / -82 | Upstream file | Opens the advanced-branch entropy source menu; icon ownership and label updates on the SeedQR fragment screen (icons carrying seed material are not handed to the plain `free()` path). The SeedQR overview screen uses `add_title_bar()` (the top strip is no longer empty; the screen name is set in `GUI_TITLE_FONT`); the code area of the fragment screen is `TFT_WHITE` (the same contrast as the full-screen SeedQR) |
| `main/button_events.h` | +98 / -4 | Upstream file | Enum additions only, including the Select Connection back event; the kind of change least likely to conflict; `BTN_SETTINGS_OTP_SET_CLOCK` included; `BTN_IO_TEST_BUTTONS` included |
| `main/gui.h` | +137 / -2 | Upstream file | Declarations and macros; `gui_flags` encoding helpers; `HOME_SCREEN_SELECTED_TILE_PCT` (a 78/22 tile at 240 px width, with the font on upstream's rule; the longest home screen label, "Scan SeedQR", is 169 px); `HAVE_DISPLAY_BRIGHTNESS_SETTING`; vertical navigation, the ALT events and the input functions, plus the `gui_set_input_echo()` declaration. The `gui_get_activity_generation()`, `gui_get_jobs_posted()` and `gui_get_jobs_drained()` declarations. |
| `main/registration_seal.c` | +92 / -0 | **New file** | seal: AES-256-CBC plus HMAC seal primitives for registered-wallet records; the AES_PADDED_LEN argument was parenthesised |
| `main/multisig.c` | +91 / -121 | Upstream file | multisig: record v4, body sealed with AES; the legacy v0-v2 read paths were removed; the same-record check happens in the clear |
| `libjade/task.c` | +84 / -13 | Emulator layer | A resettable counter base (`libjade_tick_epoch_reset()`); tasks with `output == NULL` are created detached; an initialised `pthread_attr_t` is destroyed on every exit path |
| `main/process/sign_psbt.c` | +82 / -5 | Upstream file | feat(psbt): suggest the right slot with several wallets, and ask early when there is no input to sign |
| `main/utils/urldecode.c` | +82 / -27 | Upstream file | urldecode: add validation for URL encoding |
| `main/process/register_descriptor.c` | +79 / -9 | Upstream file | qr: the descriptor QR path; plain text, Specter JSON and UR crypto-output wired into the registration flow; descriptor: record v1, body sealed with AES; the same-record check happens in the clear |
| `main/seedqr.c` | +77 / -0 | **New file** | Standard SeedQR digit-sequence generation; derived from the BIP39 definition (entropy \|\| SHA256 slices), not from the word list. The output is seed-equivalent, and therefore secret |
| `main/camera.c` | +71 / -9 | Upstream file | The camera image at full width; all four rotations are compiled and chosen at runtime |
| `main/qrcode.c` | +70 / -26 | Upstream file | QR version 3 (29x29, a 6x6 grid) and a context module frame in the fragment icons; 29 does not divide evenly, so the last row and column carry a strip of empty modules inside the mask (the same behaviour as SeedSigner) |
| `main/descriptor.c` | +69 / -40 | Upstream file | descriptor: record v1, body sealed with AES; the same-record check happens in the clear |
| `libjade/daemon.c` | +62 / -1 | Emulator layer | feat(emulator): add --settings to the daemon and measure the failed-erase branch; T8: the device clock handler (libjade_set_clock_handler) and the ARMv6 time64 build |
| `libjade/esp_camera.c` | +54 / -3 | Emulator layer | A separate frame copy for the consumer; `libjade_camera_active()`; on a camera-less build with no frames, `await_error("No camera detected")` (only while `show_ui`; in a camera-less libjade `main/camera.c` drops out of the amalgamation, so this stub is the function's only real implementation, `main/amalgamated.c:34`) |
| `main/ui.h` | +69 / -0 | Upstream file | The `await_choice_activity()` declaration; `io_test_mark_t` (the mark array of the Buttons check). |
| `libjade/pijade_settings.h` | +51 / -0 | **New file** | Same; the internal prototypes for a normal change and for erasing every copy live here |
| `main/keychain.h` | +50 / -1 | Upstream file | fix(keychain): correct the connection lifecycle against the slot table; feat(keychain): list slots and take one into use with a click |
| `main/storage.h` | +48 / -1 | Upstream file | Two free bits for camera rotation, away from the theme mask |
| `main/ui/select_registered_wallet.c` | +47 / -10 | Upstream file | Registered Wallets: make record ownership visible and close the name collision; fix(ui): exclude liquid records in the explorer, and drain a stale click |
| `libjade/CMakeLists.txt` | +46 / -4 | Emulator layer | `DEBUG_MODE` and panel size options |
| `libjade/cxx_terminate.cpp` | +45 / -0 | **New file** | security: a C++ throw was killing the process without clearing keys |
| `main/selfcheck.h` | +38 / -0 | **New file** | test: descriptor tests moved into the libjade verification branch |
| `main/process/register_otp.c` | +35 / -1 | Upstream file | The OTP name is user data; only its length is logged |
| `main/registration_seal.h` | +32 / -0 | **New file** | seal: AES-256-CBC plus HMAC seal primitives for registered-wallet records; the AES_PADDED_LEN argument was parenthesised |
| `components/miner/miner.h` | +31 / -0 | **New file** | feat(miner): take in the mining component, write two sims, close three defects; fix(miner): close the indefinite hang in production, and fit the esp_log shim to the API |
| `main/process.c` | +29 / -2 | Upstream file | libjade: fix deadlock between standard CBOR and libjade CBOR messages |
| `main/ui/otpauth.c` | +29 / -7 | Upstream file | fix(ui): remove the buttons that lie on an unreadable OTP record; otp: validation added for URL-encoded strings in an OTP context |
| `libjade/nvs_flash.c` | +28 / -2 | Emulator layer | `nvs_commit()` and `nvs_flash_erase()` notify the host; the single hook for settings persistence. It covers all five namespaces, asks for every copy to be removed on a factory reset, and provides access to `pijade_settings_storage()` |
| `main/utils/psbt.c` | +28 / -0 | Upstream file | feat(psbt): suggest the right slot with several wallets, and ask early when there is no input to sign |
| `main/ui/sign_tx.c` | +27 / -6 | Upstream file | feat(ui): phase 4E, setting flags, the xpub type restriction and the I/O test; feat(mining): the mining menu, reward address selection and a two-line screen |
| `libjade/include/esp_timer.h` | +25 / -0 | **New file** | feat(miner): take in the mining component, write two sims, close three defects |
| `libjade/include/esp_system.h` | +24 / -0 | Emulator layer | `esp_reset_reason_t` and `esp_reset_reason()`; the names and their order come from the esp-idf 5.5 source |
| `libjade/make_libjade.sh` | +24 / -3 | Emulator layer | The `--no-debug` and `--display=WxH` flags |
| `main/seedqr.h` | +24 / -0 | **New file** | Same; the header states that the output is secret and that the caller must `SENSITIVE_PUSH` its buffer and clear it |
| `main/descriptor_text.h` | +22 / -0 | **New file** | descriptor: a parser reducing text and Specter JSON descriptors to wally's canonical policy-template form |
| `README.md` | +21 / -0 | Upstream file | With the repository made public, a fork introduction was added at the top of the root README: what it is, the warning that it is experimental, and pointers to the documents under `pijade/` and to the two helper pages. Upstream's own build document stays below exactly as it was; only a prefix was added, and not one line was removed |
| `libjade/include/esp_attr.h` | +20 / -0 | **New file** | feat(miner): take in the mining component, write two sims, close three defects |
| `main/multisig.h` | +20 / -7 | Upstream file | multisig: record v4, body sealed with AES; the legacy v0-v2 read paths were removed; the same-record check happens in the clear |
| `main/qrmode.h` | +20 / -0 | Upstream file | Declarations only; `handle_qr_settings()` |
| `main/ui/keyboard.c` | +19 / -0 | Upstream file | Turns the ALT event into the existing Shift button event, opening the next keyboard page |
| `libjade/include/sdkconfig.h` | +18 / -0 | Emulator layer | A pinned `CONFIG_DEBUG_MODE` and a conditional panel size; `__NOINIT_ATTR` as an empty macro |
| `main/descriptor.h` | +18 / -9 | Upstream file | descriptor: record v1, body sealed with AES; the same-record check happens in the clear |
| `main/power/minimal.inc` | +18 / -0 | Upstream file | The libjade branch: the backlight request goes to the host |
| `main/utils/urldecode.h` | +18 / -0 | Upstream file | urldecode: add validation for URL encoding |
| `main/otpauth.c` | +24 / -3 | Upstream file | The OTP account name and issuer are no longer logged; only their lengths are written (they identify the user's services); the TOTP clock check now starts from `clock_has_been_set()`; on a port with no RTC the 2020 threshold was not enough on its own |
| `main/qrscan.c` | +17 / -5 | Upstream file | The scan box dimensions are logged at DEBUG rather than ERROR; a line that says ERROR in a production log has to be a real error |
| `main/bcur.h` | +15 / -0 | Upstream file | Declarations only |
| `main/selfcheck.c` | +15 / -639 | Upstream file | test: descriptor tests moved into the libjade verification branch; multisig: record v4, body sealed with AES; the legacy v0-v2 read paths were removed; the same-record check happens in the clear |
| `main/process/process_utils.c` | +20 / -0 | Upstream file | The RAM flag holding whether the clock was set during this boot sits right next to the success of `settimeofday()`; all three paths that change the clock (the epoch QR, the `set_epoch` RPC, unlocking) pass through here, so the coverage is structural |
| `main/process/process_utils.h` | +19 / -0 | Upstream file | interface: the scanned wallet loading path was reorganised; the `clock_has_been_set()` interface |
| `main/utils/event.c` | +14 / -4 | Upstream file | The libjade branch: the single-slot wait handle (`_last_wait_handle`) is written only by the firmware thread |
| `main/display_hw.c` | +13 / -0 | Upstream file | A libjade branch inside `display_hw_flush()` and `display_hw_flip_orientation()` |
| `main/process/debug_set_mnemonic.c` | +13 / -0 | Upstream file | feat(keychain): wire the SeedQR export into the Session menu |
| `main/process.h` | +11 / -1 | Upstream file | libjade: fix deadlock between standard CBOR and libjade CBOR messages |
| `main/process/pinclient.c` | +11 / -1 | Upstream file | security: the pinserver AES key and the decrypted padding are cleared |
| `libjade/include/esp_log.h` | +9 / -4 | Emulator layer | fix(miner): close the indefinite hang in production, and fit the esp_log shim to the API |
| `main/amalgamated.c` | +9 / -4 | Upstream file | The `#include` of the entropy sources and `seedqr.c`; taking the keyboard screen and the idle timer out of the libjade build was reverted |
| `main/process/sign_message.c` | +9 / -0 | Upstream file | feat(ui): add a Sign Message entry to the wallet menu |
| `.gitignore` | +13 / -0 | Upstream file | feat(emulator): add --settings to the daemon and measure the failed-erase branch; feat(ui): registered multisig and descriptor wallets in the address explorer |
| `main/process/auth_user.c` | +8 / -4 | Upstream file | fix(storage): on the erase PIN, delete the blob first and stay fail-closed if marking fails; security: the duress PIN is not written to the card in the clear |
| `main/utils/psbt.h` | +7 / -0 | Upstream file | feat(psbt): suggest the right slot with several wallets, and ask early when there is no input to sign |
| `main/wire.c` | +7 / -2 | Upstream file | fix(libjade): move classification onto the parsed method, and make the selfchecks runnable; libjade: fix deadlock between standard CBOR and libjade CBOR messages |
| `components/miner/README.md` | +6 / -0 | **New file** | feat(miner): take in the mining component, write two sims, close three defects |
| `main/qrcode.h` | +6 / -2 | Upstream file | The `qrcode_toFragmentsIcons()` signature (a context module and a 16-bit target size) and `qrcode_fragmentsContextFits()` |
| `components/miner/CMakeLists.txt` | +5 / -0 | **New file** | feat(miner): take in the mining component, write two sims, close three defects |
| `libjade/include/freertos/FreeRTOS.h` | +5 / -0 | Emulator layer | feat(miner): take in the mining component, write two sims, close three defects |
| `main/idletimer.h` | +5 / -0 | Upstream file | The `idletimer_stop()` and `idletimer_request_stop()` declarations |
| `jadepy/jade_sw.py` | +4 / -0 | Upstream file | libjade: fix deadlock between standard CBOR and libjade CBOR messages |
| `libjade/include/freertos/task.h` | +4 / -0 | Emulator layer | The `libjade_tick_epoch_reset()` declaration |
| `main/camera.h` | +4 / -1 | Upstream file | security: camera entropy advances with movement, and the user ends the collection |
| `main/CMakeLists.txt` | +2 / -2 | Upstream file | feat(qr): take in the jade-mine template, bound it, reject it; no mining yet; feat(mining): the mining menu, reward address selection and a two-line screen |
| `main/aes.h` | +2 / -1 | Upstream file | seal: AES-256-CBC plus HMAC seal primitives for registered-wallet records; the AES_PADDED_LEN argument was parenthesised |
| `main/process/debug_clean.c` | +2 / -1 | Upstream file | fix(storage): on the erase PIN, delete the blob first and stay fail-closed if marking fails |
| `main/process/sign_tx.c` | +2 / -2 | Upstream file | A multisig record name is user data; only the threshold and the number of xpubs are logged |
| `main/smoketest.c` | +2 / -1 | Upstream file | security: camera entropy advances with movement, and the user ends the collection |
| `main/utils/address.c` | +2 / -1 | Upstream file | The address itself is user data; only its type is logged |
| `format.sh` | +1 / -1 | Upstream file | libjade: add support for custom debug_selfcheck functions |
| `main/main.c` | +1 / -1 | Upstream file | security: camera entropy advances with movement, and the user ends the collection |
| `main/otpauth.h` | +1 / -0 | Upstream file | otp: validation added for URL-encoded strings in an OTP context |
| `main/process/debug_scan_qr.c` | +1 / -1 | Upstream file | security: camera entropy advances with movement, and the user ends the collection |

Total: **+12460 / -1884** (101 files). Breakdown: the `libjade/` emulator layer +2988 / -70
(21 files), Jade's own `main/` files +8308 / -1813 (73 files), the `components/miner/` mining
component +1151 / -0 (4 files), and helper files at the repository root +13 / -1 (3 files).
Most of the removed lines come from three places: the old split-screen layout of the wallet QR
codes (`main/qrmode.c`, `main/ui/qrmode.c`), the reorganisation of the mnemonic flows
(`main/ui/mnemonic.c`, `main/process/mnemonic.c`), and moving the verification tests out of
`main/selfcheck.c` into `libjade/selfcheck/`. A rebase conflict is likelier in those files than
anywhere else.

The five files that touch log lines (`main/otpauth.c`, `main/process/register_otp.c`,
`main/process/sign_tx.c`, `main/utils/address.c`, `main/qrscan.c`) are one-line changes, all on the
same reason: user data is not written to the log. If upstream changes those lines, the conflict
stays on one line and its resolution is obvious.

Every divergence in `main/camera.c` and `main/display_hw.c` sits inside `#ifdef CONFIG_LIBJADE`:
an ESP32 build is not affected by any of it.

### Registered-wallet serialisation has diverged from upstream

The on-card format of registered-wallet records is no longer compatible with upstream, and is not
meant to be. It diverged in two places:

- **The multisig record**, `main/multisig.c`: the version byte went from 3 to 4. A v4 body is
  encrypted with seed-derived AES-256-CBC and carries an HMAC on top; the record name stays in the
  clear. The v0-v2 paths upstream reads were REMOVED from this fork (engineering principle 1: an
  ageing path is not carried forward).
- **The descriptor record**, `main/descriptor.c`: the version byte went from 0 to 1, with the same
  seal layout.

The seal primitives live in one place, `main/registration_seal.c`; both record types call it. The
layout is `version(1) | IV(16) + AES(body) | HMAC(32)`, and the HMAC is computed over
`version || encrypted body`. The key derives from the wallet's master key, so only the seed that
wrote a record can read it; with another seed loaded the record shows as "Record Unreadable" in the
list and can be deleted.

Rebase impact: if upstream touches the serialisation in `multisig.c` or `descriptor.c`, a conflict
is CERTAIN and cannot be resolved automatically. The rule for resolving it: take upstream's field
order and field meanings, keep the seal layer, and leave the version byte at our value. Bringing
back upstream's old version readers counts as a regression; it would leave records unencrypted on
the card.

## 3. Three rules that keep the divergence small

1. **New logic goes in a new file.** Only the call site sits in an upstream file.
2. **SeedSigner's solution is not copied; Jade's own counterpart is used.** For example, the device
   identity comes through Jade's `macid[6]` / `esp_efuse_mac_get_default` path rather than
   `/proc/cpuinfo`.
3. **Every changed upstream line is marked with a `BBB-AIRGAP` comment**, so that during a rebase it
   is visible which line is ours.

## 4. Taking an upstream update

```bash
cd ~/Projects/piJade/Jade
git fetch upstream
git switch master && git merge --ff-only upstream/master   # advance the mirror branch
git switch bbb-airgap
git rebase upstream/master
```

If there is a conflict: it can only be in the upstream files listed above. In each of them our side
is marked with a `BBB-AIRGAP` comment; upstream's new state is kept, and our lines are placed back
on top of it.

## 5. Mandatory verification after a rebase

The updated fork is not used until all of the following pass:

| # | Test | Expected |
|---|---|---|
| 1 | `./libjade/make_libjade.sh Debug --log --camera --no-ci` | exit 0, no warning from our code |
| 2 | The same command without `--camera` | exit 0 |
| 3 | 50 dice rolls, SeedSigner's public vector | 12 words beginning `hole luggage safe present` |
| 4 | The camera branch, 30 differing frames | The seed screen appears |
| 5 | The camera branch, 40 flat frames | None of them counts, no seed |
| 6 | The menu on a camera-less build | Entropy Source: Device + Dice Rolls; Scan QR hidden |
| 7 | The daemon log | No `sensitive:` leak warning |
| 8 | `python3 pijade/tools/rpcprobe.py` on a `--no-debug` build | exit code 0; `ping` ACCEPTED, nine methods REJECTED |
| 9 | `pijade/host/build.sh` and `pijade-host` on a `--no-debug --display=240x240` build | `panel 240x240`, frames flowing, a key changing the menu |
| 10 | The settings unit test (command below) | `all passed`; in particular the wallet and PIN key rows |
| 11 | Open and close with `pijade-host --settings /tmp/s.bin` | The file appears; it starts with `PIJADES2` and carries **only the fields written in that session**: on an uninitialised device just the preferences that changed, plus the wallet and pinserver fields once a PIN wallet exists |
| 12 | The QR scale test (command below) | `all passed`; every version fits the panel and is larger than it was in the split layout |
| 13 | The keyboard symbol check (command below) | Both symbols larger than 0x100; not an empty body |
| 14 | The power callback (command below) | The line `probe: handler called action=0` in the output AND exit code 134; both together |
| 15 | The bc-ur fragment size (command below) | On the `frag=9` row the `v3?` column reads `YES`; version 3 codes still fit the 77-character capacity |

Item 10 checks the allowlist and the length ranges in `libjade/pijade_settings.c`; every persistent
PIN wallet field has to come back unchanged, and fields off the list have to be rejected. The test
itself does not use the library's exported API but links the static archive directly (these
functions are deliberately not `LIBJADE_API`).

**If you touched `pijade_settings.c`, run `make` in `build_linux` first.** The test compiles only its
own `.c` file and links the rest from the archive; if the archive is stale, the old allowlist runs.
On 2026-08-27 this trap produced one false RED (a field already fixed in the source was still being
skipped); the reverse, a false GREEN, is equally possible.

`libjade/include` is passed with `-isystem`, not `-I`: the test should take `nvs_flash.h` from its
real source (hand-written `extern` declarations drift silently when a signature changes), while that
header's own `-Wextra` warning (the unused parameter of `nvs_close`) must not break the test's
zero-warning rule.

```bash
gcc -Wall -Wextra -O1 -o /tmp/settings_test \
    pijade/tools/settings_test.c \
    -I libjade -isystem libjade/include -I components/libwally-core/upstream/include \
    -Wl,--start-group \
      build_linux/libjade/libjade_static.a \
      build_linux/libjade/libcbor_target.a \
      build_linux/libjade/libotpauth_migrate_target.a \
      build_linux/libjade/bcur/libbcur.a \
      build_linux/libjade/mbedtls/library/libmbedcrypto.a \
    -Wl,--end-group \
    -lpthread -lz -lm -lstdc++
/tmp/settings_test
```

Item 12 checks the full-screen scale of the wallet QR codes; if `qr_fullscreen_scale_factor()` in
`main/bcur.c` reverts to upstream's table during a rebase, the codes drop back to the split layout's
size and external scanners struggle with them. The test checks three claims, in this order: the code
**fits** the panel (`display_icon()` asserts that the icon is not larger than the screen, so an
overflow is a crash, not a drawing glitch), it does not **fall** below upstream's table, and it
leaves two modules of **quiet zone** where the panel has room.

The test is panel-independent; it takes the dimensions of whichever library build it links against
through `-DPANEL_W/-DPANEL_H` and picks the table accordingly. All five variants are run:

```bash
LIBS="build_linux/libjade/libjade_static.a build_linux/libjade/libcbor_target.a \
      build_linux/libjade/libotpauth_migrate_target.a build_linux/libjade/bcur/libbcur.a \
      build_linux/libjade/mbedtls/library/libmbedcrypto.a"
for panel in 128x128 240x135 240x240 320x170 480x220; do
    W=${panel%x*}; H=${panel#*x}
    ./libjade/make_libjade.sh Debug --log --camera --no-ci --display=$panel || exit 1
    gcc -Wall -Wextra -Werror -O1 -DPANEL_W=$W -DPANEL_H=$H -o /tmp/qr_scale_test \
        pijade/tools/qr_scale_test.c \
        -Wl,--start-group $LIBS -Wl,--end-group -lpthread -lz -lm -lstdc++ || exit 1
    /tmp/qr_scale_test || exit 1
done
```

Item 13 checks that the keyboard screen stays in the libjade build. Upstream drops
`main/ui/keyboard.c` from this build and satisfies the two entry points with empty bodies; then
`kb_entry.activity` stays empty and the `JADE_ASSERT(kb_entry.activity)` in
`main/process/mnemonic.c` crashes the device on every passphrase entry. If a rebase brings that
wrapper back, the command below shows the empty bodies; a real implementation is a few hundred
bytes, an empty body a few:

```bash
nm -S --defined-only build_linux/libjade/libjade_static.a \
  | grep -E "make_keyboard_entry_activity|run_keyboard_entry_loop"
# expected: both symbols larger than 0x100
```

Item 14 checks that sleep and restart requests reach the host. Upstream satisfies
`esp_deep_sleep_start()` and `esp_restart()` with `abort()` in libjade; in that state the Sleep menu
froze the device with `Internal error WRAPPED:0`. If a rebase brings those two bodies back, the probe
below shows that the handler was never called:

```bash
LIBS="build_linux/libjade/libjade_static.a build_linux/libjade/libcbor_target.a \
  build_linux/libjade/libotpauth_migrate_target.a build_linux/libjade/bcur/libbcur.a \
  build_linux/libjade/mbedtls/library/libmbedcrypto.a"
gcc -o /tmp/power_probe -I libjade pijade/tools/power_probe.c \
  -Wl,--start-group $LIBS -Wl,--end-group -lpthread -lz -lm -lstdc++ || exit 1
OUT=$(/tmp/power_probe 2>&1); RC=$?
echo "$OUT"
# Exit code alone is not enough: if the stub regresses, esp_deep_sleep_start() aborts before the
# handler ever runs, and that abort also exits 134. The output line is what proves the handler
# actually fired.
echo "$OUT" | grep -qx "probe: handler called action=0" && [ "$RC" -eq 134 ] \
  || { echo "FAIL: expected line missing or exit code is not 134 (exit=$RC)"; exit 1; }
# expected: "probe: handler called action=0", probe exit 134, and the check above exits 0
```

The host half (`systemctl --no-block --job-mode=replace-irreversibly start poweroff.target`) is not
exercised here: it would replace the process, and whether the service is allowed to do that is a
question about the image; it is measured on the device.

Item 8 catches not a leak but a flag being pinned back: in upstream `libjade/include/sdkconfig.h`
set `CONFIG_DEBUG_MODE` to `1` unconditionally, which left the debug message handlers and the
libjade RPC surface open even in a Release build. If that line reverts to upstream's form during a
rebase, the build still passes, most tests still pass, and the lock quietly opens.

This item has a trap the tool cannot close on its own: on a production build a **made-up** method
name gets the same rejection (measured: `zzz_not_a_real_method` -> `-32002 hardware locked`). So if
upstream renames a method, the probe list goes stale and the tool still says "passed". During a
rebase, therefore, the probe names are compared against the `IS_METHOD(...)` lines in
`main/process/dashboard.c`; and a `--control` run on a default build shows that the names really are
routed (in that mode the expected result is that no method gets the lock rejection).

Item 15 checks the `BCUR_FRAGMENT_SIZE_V3` constant in `main/bcur.c`. That constant was found by
measurement: the `BCUR_MAX_FRAGMENT_SIZE()` macro leaves only 4 bytes at version 3, which is below
bc-ur's own `min_fragment_len=8`, so the encoder hits an assert. 9 is the only value that both
clears that lower bound and keeps **every** produced fragment inside version 3's 77-character
capacity. If upstream bc-ur's fountain metadata grows, the fragments exceed that capacity and the
length assert inside `bcur_create_qr_icons()` becomes a crash on the device; so it is measured again
at every rebase.

```bash
LIBS="build_linux/libjade/libjade_static.a build_linux/libjade/libcbor_target.a \
      build_linux/libjade/libotpauth_migrate_target.a build_linux/libjade/bcur/libbcur.a \
      build_linux/libjade/mbedtls/library/libmbedcrypto.a"
gcc -Wall -Wextra -O1 -o /tmp/qr_fragsize_test pijade/tools/qr_fragsize_test.c \
    -Wl,--start-group $LIBS -Wl,--end-group -lpthread -lz -lm -lstdc++
/tmp/qr_fragsize_test 128 crypto-account   # 128 = the ceiling of the xpub's cbor buffer
```

Expected: on the row where the `frag` column is 9, the `v3?` column reads `YES`. The measurement
covers the whole payload, not just the first fragment; fragments grow as the sequence numbers get
longer.

## 6. The production build command

A build that goes on the device is **always** compiled with these flags:

```bash
./libjade/make_libjade.sh Release --camera --no-ci --no-debug --display=240x240 --log
./pijade/host/build.sh
```

| Flag | Why it is required |
|---|---|
| `--no-debug` | Leaves Jade's debug message handlers and the libjade RPC surface out of the build |
| `--no-ci` | CI mode confirms automatically instead of waiting for the user (`main/gui.c:2767`). On a device holding keys that is unacceptable |
| `--display=240x240` | The panel is this size. Forget the flag and a 320x200 frame buffer is compiled; the program does not crash, it quietly runs with the wrong geometry |
| `--camera` | Compiles the camera code. Without it `libjade_push_camera_frame` returns `false` on every call |
| `Release` | Carries no debug symbols and no assertions |
| `--log` | Leaves the logging infrastructure in the binary. The default level is still NONE, so nothing is printed; diagnostics are turned on with `pijade-host --log-level ...`. Without the flag the `JADE_LOGx` calls are compiled out, and the only way to get evidence on the device is to build a new package |

No build made with any other command goes on the device.

Test drivers: `pijade/host/pijade_host.c` for the production build (it needs no RPC), and
`pijade/tools/jadectl.py` for the default build.

---

## 16. `qrcode_initText()` silently truncates a payload that does not fit

`bb_appendBits()` (`main/qrcode.c:246-251`) does no bounds checking. On overflow, the
`padding = (dataCapacity * 8) - codewords.bitOffsetOrWidth` inside `qrcode_initBytes()` wraps under
`uint32_t` and the flow continues without an error; the result is a silently truncated QR. The
return value is NOT proof that the payload fit.

```bash
docker cp pijade/tools/v3_capacity_probe.c jade-dev:/jade/pijade/tools/
docker exec jade-dev sh -lc 'cd /jade && \
  LIBS="build_linux/libjade/libjade_static.a build_linux/libjade/libcbor_target.a \
        build_linux/libjade/libotpauth_migrate_target.a build_linux/libjade/bcur/libbcur.a \
        build_linux/libjade/mbedtls/library/libmbedcrypto.a" && \
  gcc -Wall -Wextra -O1 -I libjade/include -I components/libwally-core/upstream/include \
    -o /tmp/v3probe pijade/tools/v3_capacity_probe.c \
    -Wl,--start-group $LIBS -Wl,--end-group -lpthread -lz -lm -lstdc++ && /tmp/v3probe'
```

The expected output today is `2 failure(s)`: v2/96 and v1/48 do not fit, yet `initText=0` is printed.
If a rebase produces `0 failure(s)`, upstream has added bounds checking, and the version/length assert
in `main/process/mnemonic.c` can be revisited. This tool is the twin of check 17 in
`pijade/tools/seedqr_fragments_test.c`.

### The shared build recipe for host tests

The `main/qrcode.h` chain pulls in `main/display.h` -> `arch/sys_arch.h` and `esp_log.h`, both under
`libjade/include`. Also, `<stdbool.h>` has to come BEFORE the QR headers. Hence:

```
-I libjade/include -I components/libwally-core/upstream/include
```

## 17. Scanning back on the host with `quirc` ; `ds->data` belongs to the caller

The caller allocates the `data` pointer inside `struct datastream`
(`components/esp32-quirc/lib/quirc.h:169`, device side `main/qrscan.c:148`). Without that
allocation, `quirc_decode` does a `memset` on NULL at
`components/esp32-quirc/lib/decode.c:960` and crashes.
`pijade/tools/seedqr_roundtrip_probe.c` is the example that uses this pattern correctly, and it
proves end to end that a generated SeedQR really carries the digit string that was drawn.

```bash
docker cp pijade/tools/seedqr_roundtrip_probe.c jade-dev:/jade/pijade/tools/
docker exec jade-dev sh -lc 'cd /jade && \
  LIBS="build_linux/libjade/libjade_static.a build_linux/libjade/libcbor_target.a \
        build_linux/libjade/libotpauth_migrate_target.a build_linux/libjade/bcur/libbcur.a \
        build_linux/libjade/mbedtls/library/libmbedcrypto.a" && \
  gcc -Wall -Wextra -O1 -I libjade/include -I components/libwally-core/upstream/include \
    -o /tmp/rtprobe pijade/tools/seedqr_roundtrip_probe.c \
    -Wl,--start-group $LIBS -Wl,--end-group -lpthread -lz -lm -lstdc++ && /tmp/rtprobe'
```

Expected: `0 failure(s)`.

## 18. The build line for the fragmentation test (so it is not looked up again each time)

`pijade/tools/seedqr_fragments_test.c` is compiled in the container as below. Two traps were
measured: `components/libwally-core/upstream/include` is required for `wally_core.h`, and
`-lcrypto` is NOT in the container (passing it breaks the link).

```bash
docker exec jade-dev sh -lc 'cd /jade && LIBS=$(find /jade/build_linux -name "*.a" | tr "\n" " ") && \
  gcc -o /tmp/seedqr_frag pijade/tools/seedqr_fragments_test.c \
    -I/jade/main -I/jade -I/jade/build_linux/config \
    -I/jade/libjade/include -I/jade/components/libwally-core/upstream/include \
    $LIBS $LIBS -lstdc++ -lm -lz && /tmp/seedqr_frag'
```

Expected: `0 failure(s)`. The test's QR buffers were raised to 112 bytes (for v3,
`qrcode_getBufferSize(3) = 106` and the assert is a strict `>`); growing the buffer does NOT change
the golden fixture, which was measured.

## 19. Decoding the on-screen QR independently

Since `qrcode_initText()` returns 0 even for a payload that does not fit (item 16), "the library did
not complain" is NOT an acceptance criterion. `pijade/tools/screen_qr_decode.c` takes a raw RGB565
dump of the emulator screen, decodes it with `quirc` and compares it against the expected text; it
also prints the dark-pixel bounding box (for measuring icon size).

```bash
docker exec jade-dev sh -lc 'cd /jade && LIBS=$(find /jade/build_linux -name "*.a" | tr "\n" " ") && \
  gcc -o /tmp/screenqr pijade/tools/screen_qr_decode.c \
    -I/jade/main -I/jade -I/jade/build_linux/config \
    -I/jade/libjade/include -I/jade/components/libwally-core/upstream/include \
    $LIBS $LIBS -lstdc++ -lm -lz'
docker exec jade-dev /tmp/screenqr /probe/<image>.rgb565 240 240 "<expected digit string>"
```

Measured on 2026-08-27 in the 24-word Standard flow: a bounding box of 203x203 px, code version 29,
96 digits, IDENTICAL to the expected string.

## 20. Running the device's own verification branch in the emulator

The last step of the export flow reads the QR the user drew back through the camera and compares it
(`main/process/mnemonic.c:265-268`). To run that in the emulator, the QR on the device's OWN screen
is fed back to its camera:

```bash
# 1) take the full-screen QR dump, learn the bounding box
docker exec jade-dev /tmp/screenqr /probe/<dump>.rgb565 240 240 "<expected>"
# 2) redraw as a 320x240 grey camera frame with a quiet zone (20 20 25 8 for v2, 18 18 29 7 for v3)
docker exec jade-dev python3 /jade/pijade/tools/screen_qr_to_camera.py \
  /probe/<dump>.rgb565 20 20 25 8 /probe/cam.gray
# 3) load the frames BEFORE clicking 'Done', then click, then keep feeding
```

Two traps were measured: (a) in a raw 240x240 dump the quiet zone is only 2.5 modules and quirc
cannot decode it, so redrawing is required; (b) the camera window is short, and if the frames are
not loaded in advance the scan catches nothing and the screen comes up with "No QR code captured".

## 21. Auditing log content, and freeing icons

The production binary is built with `LOG=1` (the runtime default being `ESP_LOG_NONE`), and the
service unit appends `stderr` to `/boot/firmware/pijade.log`; that partition is FAT32 and
unencrypted. So every line produced with `--log-level info` lands on the card permanently. Ten lines
that wrote sensitive content were therefore stripped of it; the diagnostic scalars (length, count,
quota, network) were kept. All of them are marked with a `BBB-AIRGAP:` comment, so that a line
reappearing in an upstream merge can be caught with grep:

| File | What used to reach the log |
|---|---|
| `main/bcur.c` | the full content of every BC-UR fragment (psbt/xpub/bip85 payload) |
| `main/process/mnemonic.c` | the number of words in the passphrase (it narrows the search space) |
| `main/process/dashboard.c` (701) | the reset confirmation code the user typed; the same value that is hidden at 681 when entered correctly. Upstream formatted the value only in order to log it, and that formatting was removed too |
| `main/process/dashboard.c` | the factory reset confirmation code |
| `main/utils/address.c` | the address being verified |
| `main/qrmode.c` | the raw scanned data when verification failed |
| `main/otpauth.c` (2 places) | the OTP account name and issuer |
| `main/process/register_otp.c` | the pre-filled OTP name |
| `main/process/sign_tx.c` | the multisig record name |

The acceptance measurement (2026-08-27, emulator, `--log-level info`): the
Options > Wallet > Export Xpub > Show QR flow produced 22 fragment lines, every one of them only
`length: 71`; zero `xpub`/bytewords matches in the log.

**Freeing icons.** `free_view_node_icon_data()` (`main/gui.c`) was releasing animation frames with a
plain `free()`; icon pixels are a reversible encoding of the exported data. It now calls
`qrcode_freeIconData()` (upstream's own clearing helper).

**The condition for this being safe was measured**: there are two separate icon allocators and their
size calculations DIFFER. `main/qrcode.c:962` allocates `((w*h/32)+1)*4` bytes;
`main/display.c:343,437` allocates `written` bytes for deflate-sourced icons and sets
`height = written*8/width`. When `written` is a multiple of 4, the QR formula gives 4 bytes MORE, so
clearing a deflate icon with the QR formula would write outside the allocated block. It is safe
because the ONLY caller of `gui_set_icon_animation()` is `make_qrcode()` (`main/ui/qrmode.c:16`),
and every icon reaching it comes from `qrcode_toIcon` / `qrcode_toFragmentsIcons` /
`bcur_create_qr_icons`. Deflate icons never enter an animation. If an icon from another allocator is
ever attached to this destructor, that invariant has to be measured again.

## 22. Taking the persistent PIN wallet into the card file

`libjade/pijade_settings.c` raised the file format from `PIJADES1` to `PIJADES2`. Old files are not
migrated and open with defaults. Every entry is now `key_len(1), key, value_len(2 LE), value`; the
two-byte length carries both the 256-byte wallet blob and the 2048-byte certificate ceiling.

| Field | Min | Max | Content |
|---|---:|---:|---|
| `privatekey` | 32 | 32 | The PIN session's EC private key |
| `blob` | 80 | 256 | The AES-encrypted wallet |
| `counter` | 1 | 1 | Remaining PIN attempts |
| `antireplay` | 4 | 4 | The pinserver v2 replay counter |
| `keyflags` | 1 | 1 | Key flags |
| `walleterasepin` | 48 | 48 | The wallet-erase PIN verifier: 16 bytes of salt plus 32 bytes of PBKDF2 |
| `networktype` | 4 | 4 | The network restriction; only 0/1/2 |
| `pinsvrurlA` | 1 | 120 | Custom pinserver URL A, including the NUL |
| `pinsvrurlB` | 1 | 120 | Custom pinserver URL B, including the NUL |
| `pinsvrpubkey` | 33 | 33 | The pinserver's EC public key |
| `pinsvrcert` | 1 | 2048 | The custom pinserver certificate, including the NUL |

The three string fields start at 1, because `nvs_set_str()` writes `strlen + 1`; an empty string is
a one-byte value, and for `pinsvrurlB` that is a state Jade sets deliberately ("explicitly no second
url", `main/process/pinclient.c:104`). Say the minimum is 2 and an empty urlB never reaches the
file; after a restart urlA exists and urlB does not, and the `JADE_ASSERT(urlASet == urlBSet)` at
`main/process/pinclient.c:113` drops the PIN unlock. A review caught this as a P1 (2026-08-27); the
unit test now runs with an empty urlB.

Length alone is not enough; six fields have their content checked too. The reasons are downstream
assumptions, all of them measured:

- The floor for `blob` is 80, because `main/keychain.c:556-574` encrypts only three things: 16 or 32
  bytes of mnemonic entropy, and a 206-byte serialised key. The `ENCRYPTED_DATA_LEN` chain
  (`main/aes.h:13,16`) turns those into 80, 96 and 256 bytes. With a floor of 80 the
  `JADE_ASSERT(bytes_len > HMAC_SHA256_LEN)` at `main/keychain.c:441` becomes unreachable.
- The last byte of each of the three string fields has to be NUL. The body of `nvs_get_str()` is
  `nvs_get_blob()` (`libjade/nvs_flash.c:177-180`), so the storage layer promises no termination;
  yet `main/process/pinclient.c:96,108` prints those buffers with `snprintf("%s")`. A one-byte empty
  string is valid, and interior NULs are not searched for.
- The `antireplay` value cannot be `0xFFFFFFFF`; `main/storage.c:539` has
  `JADE_ASSERT(j < UINT32_MAX)`.
- `pinsvrurlA` and `pinsvrurlB` either both exist or neither does
  (`main/process/pinclient.c:113`). This cross-check uses ONLY the flags collected during the
  validation pass (`prefs == NULL`); moved to the storing pass, a rejected file would still store
  one half of the pair.
- There is NO range rule for `walleterasepin`, and there must not be. Under `PIJADES4` the field is
  not raw digits but a salt and a PBKDF2 verifier (`main/storage.h`), so every byte value is
  legitimate. In the `PIJADES3` era there was a digit rule, because `format_pin()` printed the
  stored digits on screen; that screen no longer reads the PIN, so the rule went with it. Keeping it
  would reject ordinary hash bytes, and since rejection is per file it would take the whole settings
  file, wallet blob included.
- `networktype` can only be 0, 1 or 2 (`main/utils/network.h:23`). At startup `main/main.c:203` ->
  `keychain_init_cache()` caches the value without filtering it (`main/keychain.c:709`), and neither
  `keychain_load()` nor `keychain_set()` resets it. On the first successful PIN unlock
  `main/process/auth_user.c:435-440` calls `keychain_set_network_type_restriction()`
  unconditionally, and the `JADE_ASSERT(keychain_is_network_type_consistent(...))` at
  `main/keychain.c:215` fails on an out-of-list cached value. This block IS present in a production
  build: `-DDEBUG_MODE=0` (`pijade/images/build-armv6.sh:38`) defines
  `CONFIG_LIBJADE_NO_DEBUG_MODE` (`libjade/CMakeLists.txt:79-82`), which leaves `CONFIG_DEBUG_MODE`
  undefined (`libjade/include/sdkconfig.h:10-12`).

A write error is no longer swallowed. Before this work the callback was `void` and `nvs_commit()`
returned `ESP_OK` in every case; while the file carried only preferences that was defensible. Once
the wallet moved in, the same behaviour meant this: if the card is full or read-only, Jade believes
it saved the wallet, the user believes PIN setup finished, and at the next boot there is no wallet.
So `libjade_settings_fn` returns `bool`, `libjade_settings_changed()` carries the result, and
`nvs_commit()` and `nvs_flash_erase()` return `ESP_FAIL` on failure. The rest of the chain is Jade's
own: the `STORAGE_COMMIT` macro in `main/storage.c` already handles non-OK, `main/keychain.c:487`
surfaces the wallet write and `main/process/dashboard.c:708` the factory reset to the user as an
error. So this addition invents no new error path; it reconnects a chain the fork had broken.

If no handler is registered the result stays true: running without `--settings` is deliberately a
temporary mode (the emulator), there is nowhere for a write to go, and returning false would break
PIN setup in exactly the mode built to exercise it. The in-memory store is not rolled back; even
when a commit fails the values in RAM stay current, and ESP32 semantics promise no more than that.
The honest contract is the false return itself.

The threat model is deliberately narrow. An actor who can edit the settings file can also write the
`libjade.so` and the rootfs sitting on the same card; so these checks establish no privilege
boundary. They provide robustness, and they close the assert and buffer assumptions above at their
source.

There are NO extra checks on the remaining fields, because there Jade defends itself. That is not an
assumption: the consumption path of all 15 fields was traced, and this is what was measured.
`brightness` is clamped to `BACKLIGHT_MIN..MAX` on the settings screen
(`main/process/dashboard.c:1719-1726`) and clamped again on the host by `panel_set_backlight()`
(`pijade/host/panel_st7789.c:384`). The `guiflags` theme value falls through to `default` inside
`gui_set_highlight_color()` (`main/gui.c`), the camera rotation is taken modulo
(`main/gui.c:277-281`), and the theme index is bounded on the settings screen
(`main/process/dashboard.c:1869`). `qrflags` is a bit mask; the `account_index` derived from it comes
from shifting a 32-bit value by 16, so it is already below `ACCOUNT_INDEX_MAX`
(`main/qrmode.c:35-36,822`). `keyflags` is a pure bit mask. `idletimeout` is only compared
(`main/idletimer.c:207-220`). If `counter` is greater than 3, `storage_decrement_counter()` deletes
the blob (`main/storage.c:490-494`), so an inflated counter grants no extra attempts. `privatekey`
is rejected by `wally_ec_private_key_verify` (`main/storage.c:446`) and `pinsvrpubkey` by
`wally_ec_public_key_verify` (`main/process/pinclient.c:162`); neither is an assert, both return
false. The URL protocol and the certificate content are deliberately not validated either: a wrong
value produces a connection error, and validating it would tie the file format to a second set of
rules independent of Jade's own.

**A correction to this record.** An earlier version of this paragraph also listed the
`walleterasepin` digit range as "deliberately not validated", on the grounds that it "neither trips
an assert nor reads out of bounds". That claim had NOT been measured and was wrong: the assert at
`main/process/dashboard.c:378` is reachable. The same sweep turned up a second reachable assert, for
`networktype`. The record stays, because the real mistake was not that two fields were missed but
that a scoping decision rested on an assumption rather than a measurement. (The `walleterasepin`
digit rule was later dropped, but not because this record proved wrong: the field no longer carries
digits. The `networktype` rule is still in place.)

The four preference fields stay on the same allowlist: `guiflags` 1, `idletimeout` 2, `brightness`
1, `qrflags` 4 bytes. Serialisation skips an out-of-range value; on read-back, a single field that
is out of range or off the list rejects the whole file.

The security difference is plain: Jade's ESP32 flash is encrypted, and a card file can be copied.
The encrypted wallet blob is useless without the key half held by the pinserver; but an attacker
holding the card can copy the PIN private key and the counter, and roll the local attempt counter
back with an old copy. The remaining defence in that case is the pinserver's own rate limiting.

Two more differences, both particular to this fork:

**`walleterasepin` is NOT in the clear on the card (2026-09-03).** The field is now 16 bytes of salt
and a 32-byte PBKDF2-HMAC-SHA256 verifier; the storage API lost its getter, replaced by
`storage_verify_wallet_erase_pin()` and `storage_wallet_erase_pin_exists()` (`main/storage.c`).
Upstream's screen printed the stored PIN with `format_pin()`, which is why the field had to be
readable; the screen now only says "set", so nothing is lost in parity. **It is important not to
claim more for this than it gives:** six digits is about 20 bits, and an attacker holding the card
can still try all 10^6 candidates on their own machine. The gain is only that someone opening the
file in a hex editor cannot read the PIN off the screen. On the ESP32 the same field sits in
encrypted flash, so Jade did not need this layer. `main/storage.c` also compiles for upstream's
ESP32 target; there there is no card file and no `PIJADES4` rejection, and an old six-byte NVS
record reads as "not set" through a `read_blob_fixed()` length mismatch. This fork produces no ESP32
image, so that target was not measured.

**Two-slot writing on FAT.** Replacing a single file through a temporary file and `rename` is not
power-cut safe. VFAT may write the target's directory entry and the FAT chain to the card at
different times; at the next boot `fsck` can truncate to 0 bytes the file that points at a free
cluster. The host therefore alternates between the `<base>.a` and `<base>.b` slots, never renames,
and preserves the other valid generation while writing one. The old single file `<base>` is
deliberately neither read nor migrated.

Each slot carries the `PJSLOT01` magic, a little-endian 32-bit sequence number, the payload length,
the libjade blob and a zlib CRC32. A read validates both slots, picks the highest sequence number,
and prefers A on a tie. If the size, magic, length or CRC is wrong, the slot is logged in one line
and ignored. A new write goes to the other slot from the winner; if no valid slot is known, the
target is the sole remaining slot after an erase, and otherwise A.

**State machine guarantees.** The handle keeps the sequence counter monotonic at the highest value
read or attempted, probes the other slot's 16-byte header before writing, returns `false` on a
sequence ambiguity it cannot clear, and picks the sole remaining slot after an erase, otherwise A,
as the next target. **Accepted residual risks:** **R1:** the read path is deliberately not
fail-closed; if the newest generation is physically unreadable, the previous one is loaded, because
refusing to read while a sound copy sits on the card would make the device unusable.
**R2:** the write target is always the slot that did NOT win; if the winner was chosen wrongly
because it could not be read, a half-finished write takes away that unreadable and possibly newer
frame; the generation that survives is the one the device is already running, and the alternative,
overwriting the winner, would leave no valid copy at all on a torn write.
**R3:** if power is cut between a successful write and the erasure of a slot whose sequence is
unknown, that slot may become readable later and, if its sequence is higher, win at the next boot.

A factory reset overwrites the full length of both slots with zeros, calls `fsync`, removes the
files and syncs the directory. Even so, FAT and the flash or SD layer may leave physical old copies;
carving could recover `privatekey`, an old `blob` and the `walleterasepin` verifier. The previous
generation in the other slot also offers a rollback, though an attacker who can copy the card
already has that. The ESP32's encrypted flash does not have this risk profile.

**Three paths where a write error goes to the log, not to the screen.** Upstream's
`main/process/dashboard.c` checks no `storage_set_*` return (measured: zero calls check it). So
`storage_set_wallet_erase_pin()` (`:1272`), `keychain_persist_key_flags()` and the network
restriction helpers ignore the error our new `bool` propagation carries: if the card fills up or
turns read-only after the wallet was saved, the user believes the duress PIN was set while the old
value stays in the file. The error reaches the host log (`pijade: cannot write ...`), not the
screen. This is an upstream characteristic; this work did not change the code, it changed the
probability profile (a removable card instead of soldered flash). The setup moment is protected: the
wallet's own write surfaces the error (`main/keychain.c:487`), so PIN setup cannot silently look
"finished". Fixing it would mean diverging on parity screens under `main/` and a conflict at every
rebase; by decision the upstream behaviour was kept (2026-08-27), so this is a deliberate
acceptance.

**Rebase trap:** the 256 ceiling on `blob` was measured from the `SERIALIZED_KEY_LEN` ->
`ENCRYPTED_DATA_LEN` chain at `main/keychain.c:19-22`. If upstream grows the key's serialised form
and the encrypted blob passes 256, serialisation SILENTLY skips that field; the user's wallet
appears to work but does not survive a restart. This ceiling has to be measured again at a rebase.

## 23. Taking the record namespaces into the card

**Divergence:** `libjade/nvs_flash.c`, `libjade/pijade_settings.c`, `libjade/pijade_settings.h`,
`libjade/libjade.c`, `libjade/libjade.h`, `pijade/host/settings_store.c`.

**The measured defect.** The earlier work wrote only the default namespace (`nvs_storage[0]`) of the
five to the card. `nvs_commit()` returned `ESP_OK` for the other four handles without writing
anything; so Jade told the user "multisig saved" and the record vanished at the next boot. The same
silence also reset the HOTP counter at every boot, which means HOTP codes repeat: a violation of
that feature's own security assumption.

**The `PIJADES4` format.** One namespace byte was added to the earlier entry layout:
`ns(1), key_len(1), key, value_len(2 LE), value`. The last character of the magic is the version;
a payload headed `PIJADES2` or `PIJADES3` is now rejected by libjade even in an otherwise valid
slot. The only reason for the `PIJADES3` -> `PIJADES4` step is that the `walleterasepin` field grew
from six digits to a 48-byte verifier; since rejection is per file, an old card loses not only its
duress PIN but all its settings and its wallet blob, so existing cards have to be prepared again by
hand. A backward-compatibility layer was deliberately not written (engineering principle 1); and the
old single file is unreadable under the two-slot design anyway. With no device in the field, that
loss was accepted.

**Upstream's own whole-store format was not used.** `libjade_save_nvs()` / `libjade_load_nvs()`
(`libjade/nvs_flash.c`) already serialise the five namespaces with the `JADE_NVS` magic. The card
file was not tied to it, because that format is the daemon RPC's internal contract: an upstream
change made without our knowing would invalidate cards in the field. Keeping it separate also
guarantees that a daemon NVS dump cannot be swallowed as a card settings file. Those two functions
were not touched.

**A sweep for reachable aborts.** Which assert a record coming from the file can reach was measured:

| Bound | Source | Why |
|---|---|---|
| Name 1-15 bytes, all ASCII 33-126 | `libjade/nvs_flash.c:243`, `storage_key_name_valid()` in `main/storage.c` | A name of 16 bytes or more calls `abort()` when the menu lists records. The range is the device's own rule; it also rules out a name with an embedded NUL (such a record could not be matched by C string searches, and so could not be deleted) |
| Multisig 114-3250 | `main/multisig.c:19,284` | The assert runs before the HMAC gate |
| Descriptor **41**-3249 | `main/descriptor.c:24,638,642` | `MIN_DESCRIPTOR_BYTES_LEN` is 9, and although its comment says it includes the HMAC, the arithmetic does not. 9-31 bytes passes the assert, and then `bytes_len - HMAC_SHA256_LEN` underflows. The bound is therefore 41, not 9 |
| OTP 32-288, a multiple of 16 | `main/aes.c:45,51`, `main/otpauth.c:809` | The stored length goes straight into the `aes_decrypt_bytes()` asserts |
| HOTP counter exactly 8 | `main/storage.c:773-783` | A `uint64_t`, through `read_blob_fixed` |
| At most 16 records per namespace | `main/multisig.h:13`, `main/descriptor.h:14`, `main/otpauth.h:14` | The device's own ceiling |

**A file that is written has to be readable; this was made structural.**
`pijade_settings_serialize()` runs the buffer it produced through the loader's own validation pass
(`walk_entries(NULL, ...)`) before returning it. So no rule is written twice. That also closed a
hidden hole in the earlier design: the ns0 content checks (`antireplay`, `walleterasepin`,
`networktype`) and the `pinsvrurlA`/`pinsvrurlB` matching check lived only in the loader, so a
mismatched pair was written without complaint and rejected by libjade at the next boot. Now the same
situation is a rejected write the user sees immediately.

**Validation cuts both ways.** On read, a single bad entry rejects the whole file (the two passes
were kept). On write, ns0 keeps the earlier behaviour (a field whose length is out of range is
skipped and the device stays on its default for that setting); in ns1-4 the write fails. The reason:
every record path calls `storage_key_name_valid()` and produces records within the bounds above, so
a violation is an impossible state. Skipping it quietly would recreate the very class of silent loss
this work exists to remove, and in user data rather than preference data.

**The ceiling went from 8192 to 131072.** The measured worst case is `16*3250 + 16*3249 + 16*288 +
16*8` plus ns0, about 108 KB. Verified against the real binary: a maximal valid store of 109,180
bytes is accepted and 131,073 bytes is rejected. Reads and writes are on the heap and at the real
size; there is no fixed 128 KB buffer anywhere.

**A deliberate behaviour change:** `nvs_commit()` now returns `ESP_FAIL` rather than `ESP_OK` for an
unrecognised handle. Since `nvs_open()` cannot produce a handle outside the five maps, this path is
unreachable in practice; it is defensive. A silent `ESP_OK` was precisely the defect this work
fixed.

**Rebase trap:** all six bounds above were MEASURED from constants under `main/` but written into the
libjade side as numbers (to preserve the upstream separation). If upstream grows one of those
constants (a new multisig record version, say), serialisation SILENTLY rejects that record and the
user finds out only after a restart. At a rebase, every row of the `PERSISTED_NAMESPACES` table has
to be measured again.

**The HOTP write cost (accepted, unmeasured):** generating each HOTP code increments the counter and
commits (`main/otpauth.c:603`, `main/storage.c:773`), so in the worst case the entire ~108 KB file
is rewritten and `fsync`ed. That is the known price of the single-file decision; a realistic file is
1-10 KB. Measuring the card's write load was left for later.

## 24. The device clock handler

**Divergence:** `libjade/libjade.h`, `libjade/libjade.c`, `pijade/host/pijade_host.c`.

**The measured defect.** `libjade/libjade.c` replaced the `settimeofday()` call the amalgamation
sees with an unconditionally successful no-op. The `set_epoch` RPC, the QR epoch and the pairing
handshake therefore reported success while `time(NULL)` in the device process did not move. TOTP
also uses `time(NULL)` in `main/otpauth.c`, so it computed codes from the image build time rather
than the epoch it had been given.

**The contract.** `libjade_set_clock_handler()` calls the host when Jade wants to set the clock. A
handler returning 0 means the request succeeded; a non-zero return goes to Jade's existing
`Failed to set time` path. A NULL handler removes the handler and waits for a call in flight; a
handler may not call its own setter. With no handler the call returns 0 and the clock does not
change. That preserves the emulator's in-process no-op behaviour.

Only the `pijade-host` device path registers a real `settimeofday()` handler. The service runs as
root, and the card has neither an RTC nor a network. The headless path and the `libjade` daemon have
no handler, so as not to change the clock of a development machine.

**A 64-bit `time_t`.** In the target toolchain (armhf Bookworm, glibc 2.36) `time_t` is 4 bytes by
default; the epoch is narrowed into `tv_sec` inside upstream's `params_set_epoch_time()` before it
ever reaches the handler, a post-2038 value wraps silently, and Jade still reports success. The fix
is a build flag: `-D_TIME_BITS=64 -D_FILE_OFFSET_BITS=64` was added to `ARCHFLAGS` in
`pijade/images/build-armv6.sh` (in the same container `sizeof(time_t)` was measured going from 4 to
8); the `_Static_assert(sizeof(time_t) >= 8)` in `pijade_host.c` breaks the build if the flag is
dropped. No upstream file was touched. Evidence: `__settimeofday64`, `__time64` and
`__clock_gettime64` appear in the dynamic symbol tables of the ARMv6 `pijade-host` and `libjade.so`.

**The persistence decision.** The epoch is not written to the card. When power is cut the clock
returns to the image build time; this is the same as Jade's behaviour without an RTC. There is no
fake-hwclock, no time zone, no microseconds and no new configuration.

The probe measures only the libjade half and does not change the real clock. In the `jade-dev`
container:

```bash
LIBS="build_linux/libjade/libjade_static.a build_linux/libjade/libcbor_target.a \
  build_linux/libjade/libotpauth_migrate_target.a build_linux/libjade/bcur/libbcur.a \
  build_linux/libjade/mbedtls/library/libmbedcrypto.a"
gcc -Wall -Wextra -Werror -O0 -g -o /tmp/clock_probe -I libjade \
  pijade/tools/clock_probe.c -Wl,--start-group $LIBS -Wl,--end-group \
  -lpthread -lz -lm -lstdc++ || exit 1
/tmp/clock_probe
```

The expected output shows two CBOR responses in hex; on the first request the handler is called once
with `1700000000`, and on the second the handler has been removed, so the call count does not change
and the real clock drifts by less than 5 seconds. The last line is `PROBE OK` and the exit code
is 0. An error response, a missing response, a wrong epoch, an extra call or clock drift produces
`PROBE FAILED` and exit code 1.

## 25. Generating a `jade-epoch` QR and scanning it in the emulator

Jade takes the epoch by QR as a `ur:jade-epoch` type (`main/qrmode.c:1401`); the body is directly the
CBOR map `{"id":"1","method":"set_epoch","params":{"epoch":N}}` (`handle_epoch_qr`,
`bcur_parse_jade_message`, `params_set_epoch_time`). The generator is `pijade/tools/epoch_qr.py`
(cbor2==6.1.2 and qrcode, in a virtualenv of your own). For a TOTP comparison it is generated immediately
before the scan:

```bash
python3 pijade/tools/epoch_qr.py /tmp/epoch.png                       # epoch = now
python3 pijade/tools/epoch_qr.py --epoch 1700000000 /probe/e.png /probe/e.gray
```

Independent proof through Jade's own decoder (in the `jade-dev` container; `libbcur.a` depends on
tinycbor and mbedcrypto, and `esp_crc.h` wants zlib's `crc32`):

```bash
LIBS="build_linux/libjade/bcur/libbcur.a build_linux/libjade/libcbor_target.a \
  build_linux/libjade/mbedtls/library/libmbedcrypto.a"
gcc -Wall -Wextra -Werror -O0 -g -o /tmp/ur_decode_probe -I components/esp32_bc-ur/src \
  pijade/tools/ur_decode_probe.c -Wl,--start-group $LIBS -Wl,--end-group -lstdc++ -lm -lz
/tmp/ur_decode_probe "$(grep '^ur=' /probe/e.out | cut -d= -f2-)"   # type=jade-epoch + cbor_hex
```

End to end in the emulator: a fresh daemon, `menu_audit.py ... "seed:<public test vector>"`, the grey
frame fed with `set_camera_bytes` BEFORE the Scan QR click and kept flowing afterwards (trap (b) of
item 20), then `right` and `click` on the home screen. Expected: the log line
`qrscan.c:30 Detected 1 QR codes`, and the screen showing "Time set successfully" with a date; since
the daemon registers no clock handler the date will be TODAY, which is not an error. Trap: if the
screen is asleep the first press only wakes it (`idletimer.c:154`), so if the log says
"powering screen" the press is repeated.

## 26. Two-axis input: joystick up/down and the HAT's three buttons

**Divergence:** `main/gui.h`, `main/gui.c`, `main/ui/keyboard.c`, `main/ui/dashboard.c`,
`main/process/dashboard.c`, `main/button_events.h`, `libjade/libjade.h`, `libjade/libjade.c`,
`pijade/host/buttons_gpio.h`, `pijade/host/buttons_gpio.c`, `pijade/host/pijade_host.c`.

**The measured limit.** Jade's own hardware carries one navigation axis and the events left, right,
wheel click and front click. The HAT's joystick has two axes. Since the GUI already keeps its
selectables in a circular list ordered by the screen's `x` and `y` coordinates, no second data
structure was added. A press that wakes the screen is consumed by the existing
`idletimer_register_activity(true)` rule, and when the screen is flipped, up and down are inverted
just as left and right already were.

**The vertical neighbour rule.** On up or down, the nearest row in that direction is found first.
Within that row the enabled item at the smallest horizontal distance is selected; on a tie the
earlier item in the list wins. If there is no row in that direction, the existing previous-or-next
path is used. That fallback preserves today's behaviour on screens that use left and right to change
a value. One press emits exactly one event. Because `sync_wait_event_handler()` keeps no queue and
only updates a single event field, emitting a vertical and a horizontal event on the same press
could lose the first.

FIRST starts from the screen's `is_first` mark and selects the first enabled item; it clicks nothing
and emits no event. On a screen with no selectable item it quietly does nothing. ALT is a
screen-specific second action; today only the next keyboard page. The keyboard handler does not route
ALT into a new page counter of its own, but turns it into the existing `BTN_KEYBOARD_SHIFT` event.
That keeps the bound activity transition and the text box's keyboard counter advancing in the same
order as with the real Shift button.

| HAT input | GPIO | Event or action |
|---|---:|---|
| Joystick left | BCM 5 | Previous |
| Joystick right | BCM 26 | Next |
| Joystick up | BCM 6 | Vertical up; previous if there is no neighbour |
| Joystick down | BCM 19 | Vertical down; next if there is no neighbour |
| Joystick press | BCM 13 | Select |
| KEY1 | BCM 21 | Select the first enabled item, no click |
| KEY2 | BCM 20 | Select |
| KEY3 | BCM 16 | ALT |

All eight lines are requested on both the falling and the rising edge. Directions repeat 500 ms after
the first press and then every 150 ms. Missed intervals are not accumulated; the next repeat time is
computed from real monotonic time. KEY1, KEY2, KEY3 and the joystick press do not repeat. These
durations were not measured; they are starting values close to Jade Plus's press-and-hold repeat, to
be revisited on the device.

**The Select Connection divergence.** Upstream offers no way back from this screen. Since a back
button in the title would become the first item by coordinate order, the activity's initial
selection was tied explicitly to the first menu button; the screen opens on the same first selection
as today. All three call contexts were handled: `handle_mnemonic_qr()` and `initialise_wallet()`
arrive with a new sourceless wallet; `BTN_CONNECT_TO_BACK` may arrive with a sourced wallet, or with
a new but not yet verified `SOURCE_NONE` wallet. Back calls `keychain_clear()` only while
`keychain_get_userdata() == SOURCE_NONE`. That keeps a sourceless wallet from reaching the home
screen's assertion while preserving a sourced one. The clearing is the same operation as on Jade's
existing `BTN_SESSION_LOGOUT` path.

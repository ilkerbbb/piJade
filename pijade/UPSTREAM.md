# Upstream compatibility discipline

> piJade is a fork of Blockstream Jade. The aim: to be able to take an upstream Jade release
> without the update turning brittle. This file keeps the size of the divergence, and the
> procedure for updating, in one place. The `file:line` references in this file were reread at
> `01f266aa` on 2026-09-15 and the stale ones corrected; the prose around them was not otherwise
> remeasured. Later the same day `bb77e43f` brought the tree to the `format.sh` contract, which
> shifted lines in 13 files; the references here were moved with it and verified against the lines
> they name, so they stand at `bb77e43f`.

## 1. Remote layout

| Remote | Address | Use |
|---|---|---|
| `upstream` | https://github.com/Blockstream/Jade.git | Read only; NEVER pushed to |
| `origin` | https://github.com/ilkerbbb/piJade.git | Where the fork is published; made public on 2026-09-08, default branch `bbb-airgap` |

The `master` branch is kept as a mirror of upstream; no change is ever written on top of `master`.
All work happens on `bbb-airgap`.

## 2. Divergence inventory (2026-09-17, after the marker debt was closed)

> The numbers were measured with `git diff --numstat fdb67a3f..HEAD -- . ':(exclude)pijade'`.
> `fdb67a3f` is the branch point. `pijade/` is ours and has no upstream counterpart, so it does
> not enter the table.
>
> One trap when refreshing this table with a script: `--numstat` reports a renamed file as
> `dir/{old => new}/name`, and the table carries the NEW path. Compare the two without normalising
> that form and the thirteen `test_data/qr_vga_*.json` rows all look missing while thirteen
> `qr_qvga_*` rows look unaccounted for. Rewrite the brace form to the new path first; the count
> then matches exactly, with no row left over on either side.
>
> A second trap, measured on 2026-09-15: `--numstat` prints `-` instead of a count for a binary
> file, so a refresh that parses the two numbers drops every binary silently and the table looks
> complete while it is not. Twenty-seven diverging files are binary and are therefore absent from
> the rows below: the twenty-six `test_data/qr_{q,}vga_*.dat` camera recordings and
> `test_data/sign_message_golden_qr.png`. Counting them, the divergence covers 216 files against
> the table's 189 rows. They are fixtures, and what they are for is written where they are used,
> not here; the point of the note is that the difference between 189 and 216 is accounted for
> rather than unexplained. Use `--name-only` when checking the table against reality, which
> reports binaries like any other file and also resolves renames to the new path on its own.
>
> The reason column has two sources: hand-written rows explain why the divergence exists; rows
> added in the 2026-09-06 and 2026-09-12 refreshes instead carry the subject line of the commit
> that changed that file MOST. The second kind is a record, not a reason; it does not stand in for
> a sentence explaining the divergence, and it must be written by hand the next time the file is
> touched.

| File | Added / removed | Kind | Reason |
|---|---|---|---|
| `docs/sign/index.html` | +15071 / -0 | **New file** | docs: read the signature back on the sign page |
| `main/qrmode.c` | +1933 / -217 | Upstream file | An error reply to the pinserver exchange is reported rather than falling into the payload check and being logged as a malformed message (the user abandoning PIN entry arrives as `CBOR_RPC_USER_CANCELLED`, and upstream has no arm for it). Wallet QR codes go full screen; the information screen and the code screen were separated. Also the xpub density/rate ladder: xpub transfer follows the user's QR setting, and a device with no setting is treated as Low. A completed transfer is now routed by what it is rather than by the single format the scanner used to return: the signing tail was split out so a PSBT can arrive either wrapped in BC-UR/CBOR or as the plain serialised transaction BBQr carries, a `U` file goes to the existing multisig registration parser, and every other BBQr file type is refused by name instead of being guessed at. A privacy warning now stands in front of the xpub code when `Features > Warnings` is on; the activity to return to is read BEFORE that warning, because `await_yesno_activity_loop()` returns from inside its own loop and leaves the warning as the current activity. The address the user scans to verify is now searched for on the receive and the change branch together, and across accounts 0, 1 and 2 plus whichever account the last xpub export used, rather than on the one branch of the one account the menu happened to hold; the search is breadth-first over those roots so a low change index is not queued behind five hundred receive addresses.  The three errors that carry a message the device did not choose - the entropy request, the epoch value and the Oracle update - put the caller's fixed words in the title bar instead of a second message row, because a two-row dialog gives each row a fixed height and cuts what does not fit; those messages run to 515 px against a 236 px row, and each is also the reply text sent back to the host, so shortening them would blind it |
| `main/process/mnemonic.c` | +2012 / -461 | Upstream file | Entropy source selection; the two SeedQR export formats (Compact and Standard), a bounds check against the silent overflow in `qrcode_initText()`, and not logging the word count. Seed XOR: the combine flow that joins parts into the wallet they were split from, the split flow that shows and quizzes each part, and the entropy-to-words helper the two share with the SeedQR import. On the same `Warnings` flag, a screen saying the drawn code is the wallet itself, placed after the export is offered rather than before it, so a user who was going to skip the step is not made to read it. A persisted SLIP-0039 recovery hands its master secret to `keychain_cache_slip39_master_secret()` after `keychain_set()`, so the card is written with that secret behind a tag byte rather than with the serialised keychain, and the wallet comes back from a restart carrying its seed the way a BIP39 wallet re-derived from its entropy does. The call sits after `keychain_set()` because that call is what clears the cache, and a temporary restore skips it, having nothing to persist |
| `main/process/dashboard.c` | +1413 / -220 | Upstream file | The main menu submenu; the camera rotation setting and its label; the `QR Settings` event branch; the same macro added to both board gates of the brightness handler; `select_initial_connection()` no longer builds a connection menu, because this board has neither channel it offered: the QR flow is entered directly, the `QR Mode` double-check follows its own flag rather than the menu's existence (upstream gated it on both, so deleting the menu alone would have dropped the question silently), and the cleanup the menu's back button did, forgetting a derived but sourceless wallet, is `forget_unsourced_wallet()`, shared by the QR back button and the KEY3 escape; `make_connect_to_activity()` is called with no arguments; `Session > Sleep` draws an information screen before shutting down (`#ifdef CONFIG_LIBJADE`; the Pi cannot cut its own supply, so the user learns of the shutdown from the screen, and the message deliberately does not say when the power may be pulled, with the reason written in a comment in the code); the `Set Clock` event first shows the page address on a back/continue screen, and on `Continue` opens the `handle_scan_qr()` flow and leaves the menu loop (a scan can also load a wallet, so screens the menu was holding may be released), while the back arrow keeps it in the menu; `handle_scan_qr()` now takes the help address as a parameter; the `Buttons` check (`handle_io_test_buttons()`): each input turns its own mark green, centre click and KEY2 produce the same event so both light up together, and KEY3 leaves the screen; the `debug_set_network` method branch and its forward declaration, both inside the existing `CONFIG_DEBUG_MODE` blocks; exporting a registered multisig record asks a privacy question first when `Features > Warnings` is on, because that file carries the account key of every signer, and it is asked before the unsorted-multisig note that follows it, since that note has no 'no' to give; the comment above the `Authenticator` row now records which wallet shapes reach it without a seed: neither of the two a wallet is persisted in today does, because BIP39 stores its entropy and SLIP-0039 its master secret and both are re-derived on load, and the condition stays because the serialised branch still exists and still reads back seedless; also the upstream `uint32_t` RPC series (`c95ed4ee` to `58c11066`, ten commits), taken on 2026-09-15 |
| `components/miner/miner.c` | +1109 / -0 | **New file** | feat(miner): take in the mining component, write two sims, close three defects; fix(miner): close three P2 and three P3 findings from review round 1 |
| `docs/clock/index.html` | +1032 / -0 | **New file** | piJade: airgapped Jade fork for Raspberry Pi Zero hardware |
| `libjade/selfcheck/descriptor.c` | +996 / -0 | **New file** | test: descriptor tests moved into the libjade verification branch; bcur: correct skipping over tagged map values, and the descriptor selfcheck now covers the QR paths |
| `main/bcur.c` | +926 / -38 | Upstream file | Full-screen scale; upstream's table as the floor, the panel as the ceiling, quiet zone in between. QR version 3 support (`BCUR_FRAGMENT_SIZE_V3`): upstream's lower bound was 4, and xpubs fit into 3. Also the limits of 'I'-filtered generation, and treating fragment content as wallet data. A camera session now carries two collectors rather than one bare decoder, so its context became a struct: BBQr frames are taken only when the caller asks for them by passing a `bbqr_file_type` out-parameter, and a half-collected transfer is released on the path where the user walks away |
| `libjade/selfcheck/slip39.c` | +660 / -0 | **New file** | The SLIP-0039 vector runner: shares from the reference test suite are fed through the recovery path and the master secret they yield is compared against the vector's own. It sits with the other verification tests in `libjade/selfcheck/` rather than in `main/selfcheck.c`, following the split this fork already made |
| `main/ui/dialogs.c` | +711 / -11 | Upstream file | `await_choice_activity()`: a two-option question whose labels the caller supplies; for screens that ask for a choice rather than a confirmation.  `make_show_message_activity()` also splits a single message too wide for the message area at its word boundaries and hands the lines to its own multi-line layout: the renderer wraps at whatever character crosses the right edge and knows nothing about words, so 21 of the 22 single-string messages that overflow used to break mid-word (measured 2026-09-15 against the 236 px message area).  A message that already fits, or that carries its own line breaks, is untouched |
| `main/entropy_sources.c` | +563 / -0 | **New file** | Dice and camera entropy; it does not spread into upstream files. The dice screen is drawn with Jade's own vocabulary: the `ui/digit_entry.c` cell (35 px, a 25/50/25 vertical split, `K`/`L` arrows, DEJAVU24 for the value), the `make_progress_bar()` bar (solid fill; not transparent, because undo lowers the value) and a counter strip. Band margins and cell gaps step with panel height (`DICE_TALL_PANEL`), for the same reason as `ui/digit_entry.c` |
| `libjade/libjade.c` | +560 / -43 | Emulator layer | A real implementation of `sensitive_push/pop`, `esp_efuse_mac_get_default`, host callbacks, removal of the keyboard stubs, handing power requests to the host, the clock handler, and pulling the version string from a placeholder to the real value; the backlight level is handed to the host; `libjade_start()` resets the tick base and the serial state; `libjade_stop()` calls `idletimer_request_stop()` first and stops the idle task before the GUI; settings persistence covers all five namespaces and passes a factory reset on as a separate erase request; two-axis input routes FIRST and ALT calls to the GUI. `libjade_activity_generation()` exposes the screen-change counter, and `libjade_jobs_posted()` and `libjade_jobs_drained()` expose the GUI work queue's produce and consume counters, to the host. |
| `main/descriptor_text.c` | +537 / -0 | **New file** | descriptor: a parser reducing text and Specter JSON descriptors to wally's canonical policy-template form |
| `libjade/selfcheck/bbqr.c` | +516 / -0 | **New file** | Fifteen vectors for the BBQr collector, run without a camera: header detection, the final part arriving first, four complete transfers (base32, hex, single frame, and the 16-plus-part transfer where base36 parts company with hex), repeated and conflicting parts, a foreign transfer, a short middle part, bad deflate, both ceilings, an unfinished collection, and `bbqr_free()` called twice and on NULL |
| `main/gui.c` | +483 / -32 | Upstream file | Camera rotation state; read from the stored setting, defaulting to 90 degrees. Also a white QR background by default; vertical neighbour selection by coordinate, a horizontal fallback, FIRST selection and the ALT event. On a board with no battery, the icon strip of the home screen status bar: the invisible battery column is moved from the right to the left, and the Bluetooth icon at the end of the row is pushed right, so the two visible icons sit against the right edge; on boards with a battery the layout is exactly upstream's. Input echo (`gui_set_input_echo()`): while it is on, `gui_up()`, `gui_down()` and `gui_select_first()` emit an event naming themselves instead of navigating, because on a screen with no selectable item the first two fall through to a horizontal event and the third emits nothing at all; only the Buttons check turns it on. The vertical echo follows the screen's rotation (the same event navigation would emit), because the marks rotate with the picture; `gui_stop()` clears the flag when the session ends. The activity generation counter (`activity_generation`): incremented whenever the gui task actually changes the current activity, read through `gui_get_activity_generation()`; it lets the host's input gate tell a redraw of the same screen from a change of screen. The job counters (`gui_jobs_posted`, `gui_jobs_drained`): the number of jobs queued and dequeued, read through `gui_get_jobs_posted()` and `gui_get_jobs_drained()`; because the queue is FIFO, the host knows from these that a frame which has reached the produce count it read after a press carries that press's work. |
| `main/slip39.c` | +468 / -0 | **New file** | This fork's own SLIP-0039 layer over the vendored Shamir core: share word decoding and the 10-bit unpacking, identifier and threshold agreement across shares, group accounting, the digest check, and `slip39_error_message()`, which turns each failure into one line short enough for the 240 px screen. Recovery only; nothing here can produce a share |
| `libjade/pijade_settings.c` | +381 / -0 | **New file** | The format of the records written to the card (`PIJADES4`), namespace-aware validation, and the serialiser checking its own output; it does not spread into upstream files |
| `main/bbqr.c` | +371 / -0 | **New file** | The BBQr collector. Reading only; this fork never emits BBQr, and there is no encoder side. Parts are written into one buffer at index times block length rather than concatenated, which buys a fixed allocation and costs two rules the reference `join_qrs()` does not need, both enforced here |
| `main/keychain.c` | +362 / -37 | Upstream file | feat(keychain): a slot table instead of a single static wallet (phase 3, step A); feat(keychain): wire the SeedQR export into the Session menu; feat(keychain): persist a SLIP-0039 wallet as its master secret behind a tag byte rather than as the serialised keychain, so the wallet re-derives from the card with a seed. The tag is what keeps the shapes apart, since a bare 16 or 32 byte secret would be indistinguishable from the BIP39 entropy the same field already carries; tagged, the three blob lengths (16 or 32, 17 or 33, `SERIALIZED_KEY_LEN`) are distinct. The card format is untouched: 17 and 33 bytes encrypt to 80 and 96, inside the 80 to 256 the settings file already allows for that field, so there is no version bump and no new field |
| `libjade/selfcheck/mining.c` | +359 / -0 | **New file** | feat(miner): take in the mining component, write two sims, close three defects; fix(miner): close three P2 and three P3 findings from review round 1 |
| `test_jade.py` | +350 / -207 | Upstream file | Adapted to the fork's network rule: the device is put on each fixture's own network and restored to `none` afterwards, and a fixture on a network this fork does not support is checked to be refused rather than dropped from the run (section 28). Also carries the earlier camera change: capture at VGA, and refuse a message the screen cannot show |
| `main/shamir.c` | +346 / -0 | **New file** | Vendored verbatim from Trezor firmware; the header block in the file names the source. Listed in `.clang-format-ignore` so a `format.sh` run cannot reformat it, because being diffable against its upstream is the property that makes it auditable |
| `main/ui/dashboard.c` | +277 / -243 | Upstream file | "Camera" in the Display menu; `QR Settings` in the `Settings` menu; the home screen's selected tile split `HOME_SCREEN_SELECTED_TILE_PCT` (the label font follows upstream's height rule); the `Display Brightness` entry; `make_select_connection_activity_if_required()` is deleted along with the menu it drew; `make_connect_activity()` is now the locked-home `Wallet Locked` screen and `make_connect_to_activity()` the `Wallet Not Saved` one, both pointing at QR mode rather than a wallet app, and the latter takes no arguments; the `QR Mode` question is laid out over the four lines the screen has room for, because its first upstream line was 23 characters and lost its last one off the right edge (the ceiling is 22, measured); a `Factory Reset` entry in the uninitialised menu (`#ifdef CONFIG_LIBJADE`; upstream offers the same entry in the Boot Menu reached by clicking at startup, and that menu is structurally unreachable in this port, with the reason written in a comment in the code); a `Set Clock` row in the OTP menu (camera-conditional); a `Buttons` row in the `I/O Test` menu and the drawing of that screen (`make_io_test_buttons_activity()`); the `Buttons` row sits behind `CONFIG_LIBJADE`, because the only way out of that screen is `GUI_ALT_EVENT` and only `libjade_input()` calls `gui_alt_click()`; official Jade hardware has no third button |
| `libjade/libjade.h` | +231 / -0 | Emulator layer | The host integration API; declarations only, touching no existing line; `libjade_set_backlight_handler`; the clock handler; the settings callback contract covering the five namespaces and the zero-length erase request; the UP, DOWN, FIRST and ALT input contract. The `libjade_activity_generation()` declaration and its contract (callable from any thread). `libjade_jobs_posted()` and `libjade_jobs_drained()`: the number of jobs handed to the GUI task and taken off its queue; the host measures which frame carries a press's work with these, compared as a wrap-safe signed difference. |
| `main/slip39_english.c` | +212 / -0 | **New file** | Vendored verbatim from Trezor firmware: the 1024-word SLIP-0039 wordlist. It is not BIP-39's list and cannot be derived from it, so it is carried rather than computed. Same reformatting protection as `main/shamir.c` |
| `main/idletimer.c` | +200 / -18 | Upstream file | The libjade branch: an exit path for the task (`idletimer_stop()`), on the same pattern as `gui_stop()`. The running flag is lowered by a POSIX cleanup handler; the lifecycle flags are `_Atomic bool`; `idletimer_register_activity()` returns `false` while the timer is not ready |
| `libjade/selfcheck/urldecode.c` | +188 / -0 | **New file** | urldecode: add self-tests for validation and decode |
| `libjade/include/freertos/semphr_darwin.h` | +164 / -0 | **New file** | Darwin has no unnamed POSIX semaphores. This header replaces `semphr.h` there, keeping the FreeRTOS semaphore API libjade uses while backing binary semaphores with libdispatch and mutexes with pthreads. It reuses the same include guard, so `semphr.h` itself needs no conditional body |
| `libjade/selfcheck/seedxor.c` | +163 / -0 | **New file** | Five vectors for the Seed XOR core: the xor identity, the all-zero total that says the parts cancelled, the round trip through every split shape (two to four parts, 12 and 24 words), and the refusal paths |
| `main/gui.h` | +141 / -6 | Upstream file | Declarations and macros; `gui_flags` encoding helpers; `HOME_SCREEN_SELECTED_TILE_PCT` (a 78/22 tile at 240 px width, with the font on upstream's rule; the longest home screen label, "Scan SeedQR", is 169 px); `HAVE_DISPLAY_BRIGHTNESS_SETTING`; vertical navigation, the ALT events and the input functions, plus the `gui_set_input_echo()` declaration. The `gui_get_activity_generation()`, `gui_get_jobs_posted()` and `gui_get_jobs_drained()` declarations. |
| `main/ui/digit_entry.c` | +140 / -33 | Upstream file | piJade: airgapped Jade fork for Raspberry Pi Zero hardware |
| `main/ui/mnemonic.c` | +188 / -89 | Upstream file | Opens the advanced-branch entropy source menu; icon ownership and label updates on the SeedQR fragment screen (icons carrying seed material are not handed to the plain `free()` path). The SeedQR overview screen uses `add_title_bar()` (the top strip is no longer empty; the screen name is set in `GUI_TITLE_FONT`); the code area of the fragment screen is `TFT_WHITE` (the same contrast as the full-screen SeedQR). The `Restore Wallet` menu is counted rather than listed, so the SeedXOR row can join it without the cameraless build losing anything but the camera row |
| `main/ui/qrmode.c` | +137 / -91 | Upstream file | Two information-screen constructors and the full-screen code activity; a `QR Settings` shortcut in `Xpub Settings`; the address-search screen draws its `Edit Root` button only when there is a root to edit, so a registered wallet gets a single full-width `Skip` instead of a button that opens an empty menu |
| `main/qrscan.c` | +132 / -54 | Upstream file | Two-pass recognition: the VGA window is tried as it is, and a scan that fails is retried on a half-scale copy, because quirc loses a code whose modules grow past roughly eight pixels; a second `quirc` instance holds the reduced image. Also: the scan box dimensions are logged at DEBUG rather than ERROR, since a line that says ERROR in a production log has to be a real error |
| `main/process/register_multisig.c` | +128 / -50 | Upstream file | fix(multisig): network equality on registration, and hygiene for hidden data in the parser; multisig: record v4, body sealed with AES; the legacy v0-v2 read paths were removed; the same-record check happens in the clear; also the upstream `uint32_t` RPC series (`c95ed4ee` to `58c11066`, ten commits), taken on 2026-09-15 |
| `main/button_events.h` | +131 / -8 | Upstream file | Enum additions, and removals in two rounds: the fork had already dropped the two `Settings` submenu pairs, and on 2026-09-13 `BTN_CONNECT_VIA_USB`, `_VIA_BLE`, `_VIA_QR` and `BTN_CONNECT_HELP` went with the `Select Connection` menu and the wallet-app help screen, with a comment in their place recording what upstream raises them for (`BTN_CONNECT_SELECT_BACK` went too, but it was the fork's own addition, so it nets out of the count against the branch point). Additions are still the kind of change least likely to conflict; `BTN_SETTINGS_OTP_SET_CLOCK` included; `BTN_IO_TEST_BUTTONS` included; the five SeedXOR ids (one restore method, one backup row, the part-count rows and their exit) included |
| `main/storage.c` | +117 / -7 | Upstream file | security: the duress PIN is not written to the card in the clear; fix(storage): on the erase PIN, delete the blob first and stay fail-closed if marking fails |
| `main/entropy_sources.h` | +113 / -0 | **New file** | Same |
| `main/qrcode.c` | +104 / -33 | Upstream file | QR version 3 (29x29, a 6x6 grid) and a context module frame in the fragment icons; 29 does not divide evenly, so the last row and column carry a strip of empty modules inside the mask (the same behaviour as SeedSigner) |
| `main/bbqr.h` | +103 / -0 | **New file** | The collector's contract, with the frame format and the reasoning behind the two extra rules written at the head of the file |
| `main/ui.h` | +105 / -0 | Upstream file | The `await_choice_activity()` declaration; `io_test_mark_t` (the mark array of the Buttons check). |
| `main/camera.c` | +101 / -11 | Upstream file | Capture at VGA rather than QVGA, because the scan window 320x240 implied was too small for a version 14 code; the display path rescales on its own, so the field of view is unchanged. Also: the camera image at full width, and all four rotations compiled and chosen at runtime |
| `main/slip39.h` | +103 / -0 | **New file** | The interface `main/slip39.c` exposes to the word-entry screens and the selfcheck: the share and word-count constants, the accumulator the entry screens fill share by share, and the error enum `slip39_error_message()` renders. `SLIP39_MASTER_SECRET_MIN` sits beside the existing maximum because `main/keychain.c` asserts a persisted master secret is one of the two lengths the word counts allow, rather than only that it is not too long |
| `main/process/register_otp.c` | +101 / -2 | Upstream file | The OTP name is user data; only its length is logged. The keyboard entry seed gate leaves the function instead of falling through into the two keyboard screens, which is what its three siblings already do; upstream carries the same fall-through |
| `main/process/sign_psbt.c` | +93 / -5 | Upstream file | feat(psbt): suggest the right slot with several wallets, and ask early when there is no input to sign |
| `main/registration_seal.c` | +92 / -0 | **New file** | seal: AES-256-CBC plus HMAC seal primitives for registered-wallet records; the AES_PADDED_LEN argument was parenthesised |
| `main/multisig.c` | +95 / -129 | Upstream file | multisig: record v4, body sealed with AES; the legacy v0-v2 read paths were removed; the same-record check happens in the clear; also the upstream `uint32_t` RPC series (`c95ed4ee` to `58c11066`, ten commits), taken on 2026-09-15 |
| `libjade/task.c` | +91 / -14 | Emulator layer | A resettable counter base (`libjade_tick_epoch_reset()`); tasks with `output == NULL` are created detached; an initialised `pthread_attr_t` is destroyed on every exit path |
| `main/shamir.h` | +91 / -0 | **New file** | The header of the vendored Trezor Shamir core; same provenance and same reformatting protection as `main/shamir.c` |
| `main/process/register_descriptor.c` | +87 / -9 | Upstream file | qr: the descriptor QR path; plain text, Specter JSON and UR crypto-output wired into the registration flow; descriptor: record v1, body sealed with AES; the same-record check happens in the clear |
| `main/qr_downscale.h` | +82 / -0 | **New file** | qrscan: retry a failed scan on a half-scale copy of the window |
| `main/utils/urldecode.c` | +82 / -27 | Upstream file | urldecode: add validation for URL encoding |
| `main/seedxor.c` | +79 / -0 | **New file** | The Seed XOR core: accumulate, the zero test, and the split, which draws every part but the last from the device rng and closes the xor with the last one |
| `main/seedqr.c` | +77 / -0 | **New file** | Standard SeedQR digit-sequence generation; derived from the BIP39 definition (entropy \|\| SHA256 slices), not from the word list. The output is seed-equivalent, and therefore secret |
| `main/process/debug_set_network.c` | +76 / -0 | **New file** | The debug counterpart of the `Settings > Network` screen, making the same two `keychain` calls it makes, so the test suite can register records on both bitcoin networks in one run and the emulator still behaves the way the device does. The production image is built with `-DDEBUG_MODE=0`, so none of it is compiled in; section 28 |
| `main/process/sign_message.c` | +73 / -8 | Upstream file | feat(ui): add a Sign Message entry to the wallet menu |
| `main/descriptor.c` | +70 / -40 | Upstream file | descriptor: record v1, body sealed with AES; the same-record check happens in the clear |
| `libjade/esp_camera.c` | +67 / -4 | Emulator layer | A separate frame copy for the consumer; `libjade_camera_active()`; on a camera-less build with no frames, `await_error("No camera detected")` (only while `show_ui`; in a camera-less libjade `main/camera.c` drops out of the amalgamation, so this stub is the function's only real implementation, `main/amalgamated.c:35-37`) |
| `libjade/pijade_settings.h` | +64 / -0 | **New file** | Same; the internal prototypes for a normal change and for erasing every copy live here |
| `libjade/daemon.c` | +62 / -1 | Emulator layer | feat(emulator): add --settings to the daemon and measure the failed-erase branch; T8: the device clock handler (libjade_set_clock_handler) and the ARMv6 time64 build |
| `libjade/CMakeLists.txt` | +60 / -5 | Emulator layer | `DEBUG_MODE` and panel size options |
| `main/ui/select_registered_wallet.c` | +55 / -10 | Upstream file | Registered Wallets: make record ownership visible and close the name collision; fix(ui): exclude liquid records in the explorer, and drain a stale click |
| `libjade/include/libjade_port.h` | +53 / -0 | **New file** | The portability helpers the macOS port needed in more than one place: the byte order macros, `libjade_thread_setname()` (Darwin can only name the calling thread) and a `getrandom()` stand-in |
| `main/ui/camera.c` | +52 / -3 | Upstream file | piJade: airgapped Jade fork for Raspberry Pi Zero hardware |
| `main/qrmode.h` | +51 / -1 | Upstream file | Declarations only; `handle_qr_settings()` |
| `main/keychain.h` | +54 / -1 | Upstream file | fix(keychain): correct the connection lifecycle against the slot table; feat(keychain): list slots and take one into use with a click; feat(keychain): declare `keychain_cache_slip39_master_secret()`, the hand-off the SLIP-0039 recovery path uses to choose that blob shape |
| `main/seedxor.h` | +49 / -0 | **New file** | The contract, the part-count bounds, and the statement of what the scheme gives and what it does not |
| `main/storage.h` | +50 / -1 | Upstream file | Two free bits for camera rotation, away from the theme mask. 0x04 is left unused on purpose: the xpub privacy warning answers to `FEATURE_FLAGS_HARSH_WARNINGS` rather than taking a bit of its own, so the hole stays open and the remaining bits keep their numbers |
| `libjade/cxx_terminate.cpp` | +45 / -0 | **New file** | security: a C++ throw was killing the process without clearing keys |
| `main/process/auth_user.c` | +44 / -6 | Upstream file | fix(storage): on the erase PIN, delete the blob first and stay fail-closed if marking fails; security: the duress PIN is not written to the card in the clear |
| `main/otpauth.c` | +44 / -6 | Upstream file | The OTP account name and issuer are no longer logged; only their lengths are written (they identify the user's services); the TOTP clock check now starts from `clock_has_been_set()`; on a port with no RTC the 2020 threshold was not enough on its own |
| `docs/imza/index.html` | +43 / -0 | **New file** | piJade: airgapped Jade fork for Raspberry Pi Zero hardware |
| `docs/saat/index.html` | +43 / -0 | **New file** | piJade: airgapped Jade fork for Raspberry Pi Zero hardware |
| `main/ui/otpauth.c` | +41 / -7 | Upstream file | fix(ui): remove the buttons that lie on an unreadable OTP record; otp: validation added for URL-encoded strings in an OTP context |
| `main/selfcheck.h` | +38 / -0 | **New file** | test: descriptor tests moved into the libjade verification branch |
| `docs/index.html` | +37 / -0 | **New file** | piJade: airgapped Jade fork for Raspberry Pi Zero hardware |
| `libjade/make_libjade.sh` | +33 / -3 | Emulator layer | The `--no-debug` and `--display=WxH` flags |
| `main/registration_seal.h` | +32 / -0 | **New file** | seal: AES-256-CBC plus HMAC seal primitives for registered-wallet records; the AES_PADDED_LEN argument was parenthesised |
| `components/miner/miner.h` | +31 / -0 | **New file** | feat(miner): take in the mining component, write two sims, close three defects; fix(miner): close the indefinite hang in production, and fit the esp_log shim to the API |
| `main/ui/sign_tx.c` | +36 / -7 | Upstream file | feat(ui): phase 4E, setting flags, the xpub type restriction and the I/O test; feat(mining): the mining menu, reward address selection and a two-line screen; also the upstream `uint32_t` RPC series (`c95ed4ee` to `58c11066`, ten commits), taken on 2026-09-15 |
| `main/process.c` | +29 / -2 | Upstream file | libjade: fix deadlock between standard CBOR and libjade CBOR messages |
| `libjade/nvs_flash.c` | +29 / -3 | Emulator layer | `nvs_commit()` and `nvs_flash_erase()` notify the host; the single hook for settings persistence. It covers all five namespaces, asks for every copy to be removed on a factory reset, and provides access to `pijade_settings_storage()` |
| `main/utils/psbt.c` | +30 / -0 | Upstream file | feat(psbt): suggest the right slot with several wallets, and ask early when there is no input to sign |
| `main/ui/keyboard.c` | +27 / -0 | Upstream file | Turns the ALT event into the existing Shift button event, opening the next keyboard page |
| `components/esp32-quirc/lib/quirc.c` | +26 / -0 | Vendored library | quirc: the row scratch `threshold()` works on is allocated by `quirc_resize()` alongside the image buffers, from the same width, and zeroed there; `quirc_destroy()` frees it |
| `test_data/sign_message_golden.json` | +26 / -0 | **New file** | docs: read the signature back on the sign page |
| `libjade/include/esp_timer.h` | +25 / -0 | **New file** | feat(miner): take in the mining component, write two sims, close three defects |
| `libjade/include/esp_system.h` | +24 / -0 | Emulator layer | `esp_reset_reason_t` and `esp_reset_reason()`; the names and their order come from the esp-idf 5.5 source |
| `main/process/debug_set_mnemonic.c` | +24 / -1 | Upstream file | feat(keychain): wire the SeedQR export into the Session menu |
| `main/seedqr.h` | +24 / -0 | **New file** | Same; the header states that the output is secret and that the caller must `SENSITIVE_PUSH` its buffer and clear it |
| `main/usbhmsc/usbmode.c` | +24 / -1 | Upstream file | piJade: airgapped Jade fork for Raspberry Pi Zero hardware |
| `SECURITY.md` | +14 / -0 | Upstream file | A fork preface above Blockstream's own reporting document. Left alone until 2026-09-15, it sent a vulnerability in code that exists only in this fork to Blockstream's maintainer; the preface scopes the document below to what the fork shares with upstream and routes fork-only findings to this repository's private advisory channel, with `pijade/UPSTREAM.md` as the line between the two. Not one line of Blockstream's text was changed |
| `.gitignore` | +23 / -0 | Upstream file | feat(emulator): add --settings to the daemon and measure the failed-erase branch; feat(ui): registered multisig and descriptor wallets in the address explorer |
| `main/ui/sign_message.c` | +23 / -7 | Upstream file | camera: capture at VGA, and refuse a message the screen cannot show |
| `README.md` | +146 / -316 | Upstream file | The root README is now the fork's own landing page: what piJade is, three emulator screenshots of the home screen, a table of contents, what it does, the parts it needs, and where each document sits. Upstream's build document, which covers Jade's ESP32 boards and the toolchain they need, occupied lines 27 to 342 of this file and was moved byte for byte to `JADE-BUILD.md`; nothing in it was edited. **Merge discipline:** an upstream hunk that lands in the build document will conflict here against a file that no longer holds it. Resolve the README side with ours and apply that hunk to `JADE-BUILD.md` by hand; the two files are one document split in two, and a hunk dropped at the conflict is a silently missed upstream fix |
| `JADE-BUILD.md` | +316 / -0 | **New file** | Blockstream Jade's own build document, moved out of the root README on 2026-09-16 and byte for byte identical to the lines it came from. It keeps the relative links it always had (`./diy/`, `./FWUPDATE.md`, `./REPRODUCIBLE.md`, `./libjade/README.md`, the two `jade-client-requirements.txt` files), which is why it sits at the root beside them rather than under `docs/`: moving it a directory down would have broken every one of those links and widened the divergence for nothing |
| `FWUPDATE.md` | +1 / -1 | Upstream file | One sentence points readers at the build instructions for other ESP32 boards; it said `the main README.md`, which stopped carrying them on 2026-09-16, so it now names `JADE-BUILD.md` |
| `diy/README.md` | +1 / -1 | Upstream file | The hardware-selection page sends the reader to the build guide "in the main readme"; that guide moved to `JADE-BUILD.md` on 2026-09-16, so the sentence now names it |
| `libjade/gui.py` | +22 / -2 | Upstream file | camera: capture at VGA, and refuse a message the screen cannot show |
| `main/descriptor_text.h` | +22 / -0 | **New file** | descriptor: a parser reducing text and Specter JSON descriptors to wally's canonical policy-template form |
| `main/selfcheck.c` | +94 / -641 | Upstream file | test: descriptor tests moved into the libjade verification branch; multisig: record v4, body sealed with AES; the legacy v0-v2 read paths were removed; the same-record check happens in the clear; storage: a third case stores and reloads a SLIP-0039 master secret, so the tagged blob branch is exercised at both supported lengths |
| `jadepy/jade.py` | +24 / -0 | Upstream file | `set_network_restriction()`, the client side of the debug handler above; section 28 |
| `libjade/include/esp_attr.h` | +19 / -0 | **New file** | feat(miner): take in the mining component, write two sims, close three defects |
| `main/multisig.h` | +20 / -7 | Upstream file | multisig: record v4, body sealed with AES; the legacy v0-v2 read paths were removed; the same-record check happens in the clear |
| `main/process/pinclient.c` | +20 / -1 | Upstream file | security: the pinserver AES key and the decrypted padding are cleared |
| `main/process/process_utils.c` | +36 / -12 | Upstream file | The RAM flag holding whether the clock was set during this boot sits right next to the success of `settimeofday()`; all three paths that change the clock (the epoch QR, the `set_epoch` RPC, unlocking) pass through here, so the coverage is structural; also the upstream `uint32_t` RPC series (`c95ed4ee` to `58c11066`, ten commits), taken on 2026-09-15 |
| `main/process/process_utils.h` | +23 / -2 | Upstream file | interface: the scanned wallet loading path was reorganised; the `clock_has_been_set()` interface; also the upstream `uint32_t` RPC series (`c95ed4ee` to `58c11066`, ten commits), taken on 2026-09-15 |
| `libjade/include/sdkconfig.h` | +18 / -0 | Emulator layer | A pinned `CONFIG_DEBUG_MODE` and a conditional panel size; `__NOINIT_ATTR` as an empty macro |
| `main/descriptor.h` | +18 / -9 | Upstream file | descriptor: record v1, body sealed with AES; the same-record check happens in the clear |
| `main/power/minimal.inc` | +18 / -0 | Upstream file | The libjade branch: the backlight request goes to the host |
| `main/utils/urldecode.h` | +18 / -0 | Upstream file | urldecode: add validation for URL encoding |
| `main/bcur.h` | +22 / -1 | Upstream file | Declarations only; `bcur_scan_qr()` gained the optional `bbqr_file_type` out-parameter that both enables BBQr collection and reports what arrived |
| `main/process/get_bip85_entropy.c` | +20 / -15 | Upstream file | The upstream `uint32_t` RPC series (`c95ed4ee` to `58c11066`, ten commits), taken on 2026-09-15; before that this file carried no divergence.  Its error screen now puts the fixed words in the title bar so the message area can wrap the reply text, all eight variants of which are wider than one 236 px row |
| `main/ui/descriptor.c` | +15 / -0 | Upstream file | piJade: airgapped Jade fork for Raspberry Pi Zero hardware |
| `main/camera.h` | +14 / -3 | Upstream file | The VGA frame size and the constants derived from it; the entropy branch reads the same frame, so its thresholds live here too |
| `main/utils/event.c` | +14 / -4 | Upstream file | The libjade branch: the single-slot wait handle (`_last_wait_handle`) is written only by the firmware thread |
| `main/display_hw.c` | +13 / -0 | Upstream file | A libjade branch inside `display_hw_flush()` and `display_hw_flip_orientation()` |
| `main/process/debug_clean.c` | +13 / -2 | Upstream file | fix(storage): on the erase PIN, delete the blob first and stay fail-closed if marking fails |
| `test_data/msg_bbb_newline.json` | +13 / -0 | **New file** | camera: capture at VGA, and refuse a message the screen cannot show |
| `test_data/msg_bbb_nonascii.json` | +13 / -0 | **New file** | camera: capture at VGA, and refuse a message the screen cannot show |
| `test_data/msg_bbb_nul.json` | +13 / -0 | **New file** | camera: capture at VGA, and refuse a message the screen cannot show |
| `components/esp32-quirc/lib/identify.c` | +12 / -3 | Vendored library | The same change, consumer side: `threshold()` takes the scratch from `struct quirc` instead of declaring a variable length array sized from `q->w` at run time, and zeroes it before returning, because the running sums it leaves behind come from the scanned frame |
| `main/qrcode.h` | +12 / -3 | Upstream file | The `qrcode_toFragmentsIcons()` signature (a context module and a 16-bit target size) and `qrcode_fragmentsContextFits()` |
| `main/amalgamated.c` | +16 / -4 | Upstream file | The `#include` of the entropy sources and `seedqr.c`; taking the keyboard screen and the idle timer out of the libjade build was reverted; the `#include` of `debug_set_network.c`; the `#include` of `bbqr.c`; the `#include` of `seedxor.c` |
| `main/process.h` | +11 / -1 | Upstream file | libjade: fix deadlock between standard CBOR and libjade CBOR messages |
| `main/process/get_receive_address.c` | +20 / -9 | Upstream file | piJade: airgapped Jade fork for Raspberry Pi Zero hardware; also the upstream `uint32_t` RPC series (`c95ed4ee` to `58c11066`, ten commits), taken on 2026-09-15 |
| `components/libwally-core/config.h` | +10 / -0 | Vendored library | macOS has no `explicit_bzero`, and this header's inline asm barrier is off, so falling through to a plain `memset` would leave the wipe elidable. The Apple branch selects `memset_s`, which cannot be optimised away; every other target, the shipping ARM Linux one included, keeps `explicit_bzero` exactly as it was |
| `libjade/include/esp_log.h` | +9 / -4 | Emulator layer | fix(miner): close the indefinite hang in production, and fit the esp_log shim to the API |
| `main/ui/multisig.c` | +13 / -4 | Upstream file | piJade: airgapped Jade fork for Raspberry Pi Zero hardware; also the upstream `uint32_t` RPC series (`c95ed4ee` to `58c11066`, ten commits), taken on 2026-09-15 |
| `main/jade_assert.h` | +8 / -1 | Upstream file | `JADE_STATIC_ASSERT` is built on C11 `_Static_assert` instead of the `sizeof(char[1 - 2 * !(cond)])` idiom. That idiom is silent about the one input it cannot handle: a condition that is not a constant expression turns the array into a variable length array, the code compiles, and nothing is checked (see `main/wallet.c`) |
| `main/utils/cbor_rpc.c` | +8 / -9 | Upstream file | The upstream `uint32_t` RPC series (`c95ed4ee` to `58c11066`, ten commits), taken on 2026-09-15; this file carried no divergence before it |
| `main/rsa.c` | +8 / -6 | Upstream file | The upstream `uint32_t` RPC series (`c95ed4ee` to `58c11066`, ten commits), taken on 2026-09-15; this file carried no divergence before it |
| `components/esp32-quirc/lib/quirc_internal.h` | +7 / -0 | Vendored library | The same change: the `row_average` member, with the comment stating that `quirc_resize()` sizes it from the same width as the image buffers |
| `main/utils/psbt.h` | +7 / -0 | Upstream file | feat(psbt): suggest the right slot with several wallets, and ask early when there is no input to sign |
| `main/wire.c` | +7 / -2 | Upstream file | fix(libjade): move classification onto the parsed method, and make the selfchecks runnable; libjade: fix deadlock between standard CBOR and libjade CBOR messages |
| `libjade/include/freertos/semphr.h` | +7 / -0 | Emulator layer | An Apple branch at the top that includes `semphr_darwin.h` instead |
| `main/wallet.c` | +7 / -1 | Upstream file | The `JADE_STATIC_ASSERT` in `wallet_get_gaservice_path_root()` is bounded by `GASERVICE_ROOT_PATH_LEN` rather than by a function parameter, which is not a constant expression; with the parameter the check had never run, and only `-Wvla` made that visible |
| `.clang-format-ignore` | +7 / -0 | **New file** | Keeps any clang-format run off the three files vendored verbatim from Trezor (`main/shamir.c`, `main/shamir.h`, `main/slip39_english.c`). `format.sh` globs `main/*.c` and `main/*.h`, so without this the next format pass would silently rewrite them into Jade's style and destroy the diff against their source. This is the same call `libjade` makes for miniz, expressed where every clang-format run will see it |
| `main/assets.c` | +7 / -5 | Upstream file | The upstream `uint32_t` RPC series (`c95ed4ee` to `58c11066`, ten commits), taken on 2026-09-15; this file carried no divergence before it |
| `components/miner/README.md` | +6 / -0 | **New file** | feat(miner): take in the mining component, write two sims, close three defects |
| `main/ui/ota.c` | +6 / -0 | Upstream file | piJade: airgapped Jade fork for Raspberry Pi Zero hardware |
| `test_data/msgfile_bbb_nonascii.json` | +6 / -0 | **New file** | camera: capture at VGA, and refuse a message the screen cannot show |
| `test_data/msgfile_bbb_nul.json` | +6 / -0 | **New file** | camera: capture at VGA, and refuse a message the screen cannot show |
| `test_data/msgfile_bbb_tab.json` | +6 / -0 | **New file** | camera: capture at VGA, and refuse a message the screen cannot show |
| `jadepy/jade_sw.py` | +6 / -1 | Upstream file | libjade: fix deadlock between standard CBOR and libjade CBOR messages |
| `main/process/sign_bip85_digest.c` | +6 / -4 | Upstream file | The upstream `uint32_t` RPC series (`c95ed4ee` to `58c11066`, ten commits), taken on 2026-09-15; this file carried no divergence before it |
| `components/miner/CMakeLists.txt` | +5 / -0 | **New file** | feat(miner): take in the mining component, write two sims, close three defects |
| `libjade/include/freertos/FreeRTOS.h` | +5 / -0 | Emulator layer | feat(miner): take in the mining component, write two sims, close three defects |
| `main/idletimer.h` | +5 / -0 | Upstream file | The `idletimer_stop()` and `idletimer_request_stop()` declarations |
| `main/process/update_pinserver.c` | +5 / -0 | Upstream file | piJade: airgapped Jade fork for Raspberry Pi Zero hardware |
| `main/qrscan.h` | +5 / -0 | Upstream file | The second `quirc` instance and its buffer, declared next to the first so both are destroyed in one place |
| `main/process/ota_util.c` | +14 / -9 | Upstream file | Upstream commits only, nothing of ours: the `CONFIG_LIBJADE` guard around the custom app descriptor's section attribute is upstream's own hunk in `fe3e3e94`, taken here as `6ee15ad0`; the rest is the upstream `uint32_t` RPC series (`c95ed4ee` to `58c11066`, ten commits), taken on 2026-09-15 |
| `libjade/include/freertos/task.h` | +4 / -0 | Emulator layer | The `libjade_tick_epoch_reset()` declaration |
| `main/ui/update_pinserver.c` | +4 / -0 | Upstream file | piJade: airgapped Jade fork for Raspberry Pi Zero hardware |
| `main/ui/signer.c` | +3 / -0 | Upstream file | piJade: airgapped Jade fork for Raspberry Pi Zero hardware |
| `libjade/esp_event.c` | +3 / -5 | Emulator layer | The event loop names itself from inside the thread through `libjade_thread_setname()`, because Darwin can only name the calling thread |
| `libjade/README.md` | +3 / -1 | Emulator layer | The macOS build outputs (`libjade.dylib`) and `DYLD_LIBRARY_PATH` |
| `main/rsa.h` | +3 / -3 | Upstream file | The upstream `uint32_t` RPC series (`c95ed4ee` to `58c11066`, ten commits), taken on 2026-09-15; this file carried no divergence before it |
| `main/CMakeLists.txt` | +4 / -2 | Upstream file | feat(qr): take in the jade-mine template, bound it, reject it; no mining yet; feat(mining): the mining menu, reward address selection and a two-line screen |
| `main/aes.h` | +2 / -1 | Upstream file | seal: AES-256-CBC plus HMAC seal primitives for registered-wallet records; the AES_PADDED_LEN argument was parenthesised |
| `main/process/debug_scan_qr.c` | +5 / -2 | Upstream file | security: camera entropy advances with movement, and the user ends the collection |
| `main/process/sign_tx.c` | +13 / -14 | Upstream file | A multisig record name is user data; only the threshold and the number of xpubs are logged; also the upstream `uint32_t` RPC series (`c95ed4ee` to `58c11066`, ten commits), taken on 2026-09-15 |
| `main/ui/confirm_address.c` | +2 / -0 | Upstream file | piJade: airgapped Jade fork for Raspberry Pi Zero hardware |
| `main/ui/sign_identity.c` | +2 / -0 | Upstream file | piJade: airgapped Jade fork for Raspberry Pi Zero hardware |
| `main/utils/address.c` | +2 / -1 | Upstream file | The address itself is user data; only its type is logged |
| `main/utils/cbor_rpc.h` | +2 / -2 | Upstream file | The upstream `uint32_t` RPC series (`c95ed4ee` to `58c11066`, ten commits), taken on 2026-09-15; this file carried no divergence before it |
| `main/process/get_bip85_pubkey.c` | +2 / -2 | Upstream file | The upstream `uint32_t` RPC series (`c95ed4ee` to `58c11066`, ten commits), taken on 2026-09-15; this file carried no divergence before it |
| `format.sh` | +1 / -1 | Upstream file | libjade: add support for custom debug_selfcheck functions |
| `main/main.c` | +4 / -1 | Upstream file | security: camera entropy advances with movement, and the user ends the collection |
| `main/otpauth.h` | +1 / -0 | Upstream file | otp: validation added for URL-encoded strings in an OTP context |
| `main/smoketest.c` | +3 / -1 | Upstream file | security: camera entropy advances with movement, and the user ends the collection |
| `test_data/msg_large.json` | +1 / -1 | Upstream file | camera: capture at VGA, and refuse a message the screen cannot show |
| `test_data/msgfile_large.json` | +1 / -1 | Upstream file | camera: capture at VGA, and refuse a message the screen cannot show |
| `test_data/qr_vga_bcur_psbt.json` | +1 / -1 | **New file** | Recorded at VGA; the QVGA recording it replaced was removed in `14afea47`, when capture moved to VGA. Git reports the pair as a rename because the payload is the same scene |
| `test_data/qr_vga_compactseedqr_vec1.json` | +1 / -1 | **New file** | Recorded at VGA; the QVGA recording it replaced was removed in `14afea47`, when capture moved to VGA. Git reports the pair as a rename because the payload is the same scene |
| `test_data/qr_vga_compactseedqr_vec7.json` | +1 / -1 | **New file** | Recorded at VGA; the QVGA recording it replaced was removed in `14afea47`, when capture moved to VGA. Git reports the pair as a rename because the payload is the same scene |
| `test_data/qr_vga_compactseedqr_vec8.json` | +1 / -1 | **New file** | Recorded at VGA; the QVGA recording it replaced was removed in `14afea47`, when capture moved to VGA. Git reports the pair as a rename because the payload is the same scene |
| `test_data/qr_vga_compactseedqr_vec9.json` | +1 / -1 | **New file** | Recorded at VGA; the QVGA recording it replaced was removed in `14afea47`, when capture moved to VGA. Git reports the pair as a rename because the payload is the same scene |
| `test_data/qr_vga_high_res.json` | +1 / -1 | **New file** | Recorded at VGA; the QVGA recording it replaced was removed in `14afea47`, when capture moved to VGA. Git reports the pair as a rename because the payload is the same scene |
| `test_data/qr_vga_hotp.json` | +1 / -1 | **New file** | Recorded at VGA; the QVGA recording it replaced was removed in `14afea47`, when capture moved to VGA. Git reports the pair as a rename because the payload is the same scene |
| `test_data/qr_vga_low_res.json` | +1 / -1 | **New file** | Recorded at VGA; the QVGA recording it replaced was removed in `14afea47`, when capture moved to VGA. Git reports the pair as a rename because the payload is the same scene |
| `test_data/qr_vga_mnemonic_prefixes.json` | +1 / -1 | **New file** | Recorded at VGA; the QVGA recording it replaced was removed in `14afea47`, when capture moved to VGA. Git reports the pair as a rename because the payload is the same scene |
| `test_data/qr_vga_seedqr_vec1.json` | +1 / -1 | **New file** | Recorded at VGA; the QVGA recording it replaced was removed in `14afea47`, when capture moved to VGA. Git reports the pair as a rename because the payload is the same scene |
| `test_data/qr_vga_set_epoch.json` | +1 / -1 | **New file** | Recorded at VGA; the QVGA recording it replaced was removed in `14afea47`, when capture moved to VGA. Git reports the pair as a rename because the payload is the same scene |
| `test_data/qr_vga_test_mnemonic.json` | +1 / -1 | **New file** | Recorded at VGA; the QVGA recording it replaced was removed in `14afea47`, when capture moved to VGA. Git reports the pair as a rename because the payload is the same scene |
| `test_data/qr_vga_totp.json` | +1 / -1 | **New file** | Recorded at VGA; the QVGA recording it replaced was removed in `14afea47`, when capture moved to VGA. Git reports the pair as a rename because the payload is the same scene |
| `main/process/get_commitments.c` | +1 / -1 | Upstream file | The upstream `uint32_t` RPC series (`c95ed4ee` to `58c11066`, ten commits), taken on 2026-09-15; this file carried no divergence before it |
| `main/process/get_blinding_factor.c` | +1 / -1 | Upstream file | The upstream `uint32_t` RPC series (`c95ed4ee` to `58c11066`, ten commits), taken on 2026-09-15; this file carried no divergence before it |
| `docs/.nojekyll` | +0 / -0 | **New file** | piJade: airgapped Jade fork for Raspberry Pi Zero hardware |

**Totals (measured 2026-09-17, with this commit staged):** 216 files, of which 189 are text
(+38261 / -3348) and 27 are binary fixtures, listed below rather than in the table because
`--numstat` reports no line counts for them. A refresh on 2026-09-12 listed 175 files and 148 text
files, and a later one the same day listed 187 and 160; the macOS port of libjade, the
`_Static_assert` round, the quirc round and the test-suite network adaptation are the difference.
The second of those refreshes went stale within hours, which is the argument for refreshing this
table from a measurement rather than by hand: the count that matters is the one taken at the
commit named in the heading.

That trap caught this paragraph itself, which is worth recording rather than quietly fixing. It
stood at 189 files, measured at `c68c114d`, while the table above had already been refreshed twice
past that commit: the BBQr round added `main/bbqr.c`, `main/bbqr.h` and `libjade/selfcheck/bbqr.c`
as rows without the totals being taken again. Measured at `838404c5` the count was already 192,
and the Seed XOR round took it to 195, the SLIP-0039 round took it to 202, and the `uint32_t` RPC
series takes it to 212: ten `main/` files that had no divergence at all before it now carry one,
because the fork is ahead of its branch point by that series. The lesson is the same one the table
carries: the rows and the totals are one measurement, and refreshing half of it leaves a number
that reads as current and is not. This refresh was taken after the code commit for that reason, so
the table, the totals and the area breakdown all name the same commit.

**Binary fixtures (27).** These are the recorded camera frames the QR scan suite replays, and
`--numstat` reports no line counts for them, so they are named here instead. Measured on
2026-09-12: the working tree holds **only the VGA set**, 13 `test_data/qr_vga_*.dat` recordings
(plus their 13 `.json` descriptors, which are in the table above). The 13
`test_data/qr_qvga_*.dat` recordings they replaced were **removed** in `14afea47`, the commit that
moved capture to VGA, and they appear in the inventory only as deletions; the suite does not
replay a QVGA frame any more, because the device does not produce one. The 27th file,
`test_data/sign_message_golden_qr.png`, is the golden image for the message-signing QR
(`1ab3d4b8`). None of the fixtures carries a secret: every one is a public test vector.


**Where the removed lines come from.** Most of them sit in three places: the old split-screen
layout of the wallet QR codes (`main/qrmode.c`, `main/ui/qrmode.c`), the reorganisation of the
mnemonic flows (`main/ui/mnemonic.c`, `main/process/mnemonic.c`), and moving the verification
tests out of `main/selfcheck.c` into `libjade/selfcheck/`. A rebase conflict is likelier in those
files than anywhere else.

**Breakdown by area (measured 2026-09-17, with this commit staged):** the `libjade/` emulator
layer +4861 / -85 (30 files), Jade's own `main/` files +14988 / -2718 (112 files), the
`components/miner/` mining component +1151 / -0 (4 files), the vendored `components/esp32-quirc/`
scanner +45 / -3 (3 files), the vendored `components/libwally-core/` config header +10 / -0
(1 file), the helper pages under `docs/` +16226 / -0 (6 files), the recorded QR fixtures under
`test_data/` +98 / -15 (49 files), `jadepy/` +32 / -1 (2 files), the `diy/` hardware-selection
page +1 / -1 (1 file), and files at the repository root +858 / -525 (8 files). The ten areas add
up to the 216 files above. Measured against the 2026-09-16 refresh, no area gained or lost a file;
three grew in place. `main/` went from +14961 / -2715 to +14988 / -2718 and `jadepy/` from
+27 / -1 to +32 / -1, because the round that closed the marker debt wrote a `BBB-AIRGAP:` comment
into nine upstream files whose fork lines carried none, and removed a dead `CBOR_RPC_USER_CANCELLED`
arm in `main/qrmode.c` that `params_set_epoch_time()` could never reach. The root went from
+853 / -525 to +856 / -525 when the mining round added its section to the README, and to
+858 / -525 when the screenshot round said on the page that those three images come from the
emulator.

**A paragraph that aged, corrected on 2026-09-12.** It used to say that five files carried a
one-line change on the same reason, that user data is not written to the log. Measured today, only
two of them are still that small: `main/process/sign_tx.c` (+2 / -2) and `main/utils/address.c`
(+2 / -1), and for those the claim holds; a conflict stays on one line and its resolution is
obvious. The other three grew for reasons unrelated to logging and are no longer one-line files:
`main/otpauth.c` (+44 / -6), `main/process/register_otp.c` (+100 / -2) and `main/qrscan.c`
(+132 / -54, the two-pass scan). A conflict in those needs the row in the table above, not this
paragraph.

**The first fork edit inside a vendored component (2026-09-12).** `components/esp32-quirc/` is
Espressif's fork of dlbeer's quirc, vendored by upstream Jade; until this round not one line of it
was ours, which is why the table above had no `components/esp32-quirc/` row and the `Vendored
library` kind did not exist. Three files carry `BBB-AIRGAP` marks now, for a single reason:
`threshold()` in `identify.c` declared a variable length array sized from the image width at run
time, the last one left in the tree, and that scratch is now a buffer owned by `struct quirc` and
allocated by `quirc_resize()`. Two things follow for a rebase. dlbeer's own master has no
counterpart for this function (it thresholds with `otsu()` over a histogram, not with a per-row
mean), so no upstream patch will ever arrive that resolves this conflict for us; and the buffer
holds running sums taken from the frame being scanned, which on a SeedQR scan is the mnemonic, so
it is zeroed at both ends of its life (on allocation, and before `threshold()` returns). Keeping
the zeroing while dropping the ownership move would put the variable length array back.

Every divergence in `main/camera.c` and `main/display_hw.c` sits inside `#ifdef CONFIG_LIBJADE`:
an ESP32 build is not affected by any of it. The VGA capture size is the one exception worth
naming, because it is a constant the scan window derives from (`main/camera.h`); an upstream
change to the capture size has to be read together with `main/qrscan.c`.

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
layout is `version(1) | IV(16) + AES(body) | HMAC(32)`, and the HMAC is computed over `version ||
encrypted body`. The key derives from the wallet's master key, so only the seed that wrote a
record can read it; with another seed loaded the record shows as "Record Unreadable" in the list
and can be deleted.

Rebase impact: if upstream touches the serialisation in `multisig.c` or `descriptor.c`, a conflict
is CERTAIN and cannot be resolved automatically. The rule for resolving it: take upstream's field
order and field meanings, keep the seal layer, and leave the version byte at our value. Bringing
back upstream's old version readers counts as a regression; it would leave records unencrypted on
the card.

## 3. Three rules that keep the divergence small

1. **New logic goes in a new file.** Only the call site sits in an upstream file.
2. **SeedSigner's solution is not copied; Jade's own counterpart is used.** For example, the
   device identity comes through Jade's `macid[6]` / `esp_efuse_mac_get_default` path rather than
   `/proc/cpuinfo`.
3. **A line the fork writes into an upstream file carries a `BBB-AIRGAP` comment**, so that during
   a rebase it is visible which line is ours. The rule is about lines written here, not about
   every line that differs from the branch point: the diff against `fdb67a3f` also contains
   upstream commits this fork has taken, and those stay as upstream wrote them.

   Measured on 2026-09-17. The diff touches 150 files that existed at the branch point, 13 of
   which are fixtures the fork deleted. Of the 137 that survive, 98 carry the marker. Of the 39
   that do not: 16 are byte-identical to `upstream/master`, 3 differ from `upstream/master` only
   by upstream commits this fork has not taken, and so neither group holds anything of ours; 15
   are JSON fixtures that cannot carry a comment, 4 are documents, and 1 is `format.sh`. No
   source or build file carries a change of ours that is unmarked.

## 4. Taking an upstream update

```bash
cd ~/Projects/piJade/Jade
git fetch upstream
git switch master && git merge --ff-only upstream/master   # advance the mirror branch
git switch bbb-airgap
git rebase upstream/master
```

If there is a conflict: it can only be in the upstream files listed above. In most of them our
side is marked with a `BBB-AIRGAP` comment; upstream's new state is kept, and our lines are placed
back on top of it. Where rule 3 counts the marker as absent (the JSON fixtures, the four
documents and `format.sh`) the diff against `upstream/master` is what separates our side from
upstream's.

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

Item 10 checks the allowlist and the length ranges in `libjade/pijade_settings.c`; every
persistent PIN wallet field has to come back unchanged, and fields off the list have to be
rejected. The test itself does not use the library's exported API but links the static archive
directly (these functions are deliberately not `LIBJADE_API`).

**If you touched `pijade_settings.c`, run `make` in `build_linux` first.** The test compiles only
its own `.c` file and links the rest from the archive; if the archive is stale, the old allowlist
runs. On 2026-08-27 this trap produced one false RED (a field already fixed in the source was
still being skipped); the reverse, a false GREEN, is equally possible.

`libjade/include` is passed with `-isystem`, not `-I`: the test should take `nvs_flash.h` from its
real source (hand-written `extern` declarations drift silently when a signature changes), while
that header's own `-Wextra` warning (the unused parameter of `nvs_close`) must not break the
test's zero-warning rule.

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
`main/bcur.c` reverts to upstream's table during a rebase, the codes drop back to the split
layout's size and external scanners struggle with them. The test checks three claims, in this
order: the code **fits** the panel (`display_icon()` asserts that the icon is not larger than the
screen, so an overflow is a crash, not a drawing glitch), it does not **fall** below upstream's
table, and it leaves two modules of **quiet zone** where the panel has room.

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
# The loop leaves build_linux configured for the last panel in the list, and every later
# measurement that links against build_linux inherits it (item 18 is panel specific and fails
# three checks on the wrong geometry).  Put the real configuration back before moving on, and
# repeat the whole flag set: make_libjade.sh writes every -D from the command line, so an
# omitted flag returns to its default (CAMERA=0, LOG=0, CI=CI) rather than staying as it was,
# and a rebuild that only names the panel would silently disable the camera that items 20 and
# 25 feed frames to.
./libjade/make_libjade.sh Debug --log --camera --no-ci --display=240x240
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
`esp_deep_sleep_start()` and `esp_restart()` with `abort()` in libjade; in that state the Sleep
menu froze the device with `Internal error WRAPPED:0`. If a rebase brings those two bodies back,
the probe below shows that the handler was never called:

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

The host half (`systemctl --no-block --job-mode=replace-irreversibly start poweroff.target`) is
not exercised here: it would replace the process, and whether the service is allowed to do that is
a question about the image; it is measured on the device.

Item 8 catches not a leak but a flag being pinned back: in upstream `libjade/include/sdkconfig.h`
set `CONFIG_DEBUG_MODE` to `1` unconditionally, which left the debug message handlers and the
libjade RPC surface open even in a Release build. If that line reverts to upstream's form during a
rebase, the build still passes, most tests still pass, and the lock quietly opens.

This item has a trap the tool cannot close on its own: on a production build a **made-up** method
name gets the same rejection (measured: `zzz_not_a_real_method` -> `-32002 hardware locked`). So
if upstream renames a method, the probe list goes stale and the tool still says "passed". During a
rebase, therefore, the probe names are compared against the `IS_METHOD(...)` lines in
`main/process/dashboard.c`; and a `--control` run on a default build shows that the names really
are routed (in that mode the expected result is that no method gets the lock rejection).

Item 15 checks the `BCUR_FRAGMENT_SIZE_V3` constant in `main/bcur.c`. That constant was found by
measurement: the `BCUR_MAX_FRAGMENT_SIZE()` macro leaves only 4 bytes at version 3, which is below
bc-ur's own `min_fragment_len=8`, so the encoder hits an assert. 9 is the only value that both
clears that lower bound and keeps **every** produced fragment inside version 3's 77-character
capacity. If upstream bc-ur's fountain metadata grows, the fragments exceed that capacity and the
length assert inside `bcur_create_qr_icons()` becomes a crash on the device; so it is measured
again at every rebase.

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
| `--no-ci` | CI mode confirms automatically instead of waiting for the user (`main/gui.c:3222-3225`). On a device holding keys that is unacceptable |
| `--display=240x240` | The panel is this size. Forget the flag and a 320x200 frame buffer is compiled; the program does not crash, it quietly runs with the wrong geometry |
| `--camera` | Compiles the camera code. Without it `libjade_push_camera_frame` returns `false` on every call |
| `Release` | Carries no debug symbols and no assertions |
| `--log` | Leaves the logging infrastructure in the binary. The default level is still NONE, so nothing is printed; diagnostics are turned on with `pijade-host --log-level ...`. Without the flag the `JADE_LOGx` calls are compiled out, and the only way to get evidence on the device is to build a new package |

No build made with any other command goes on the device.

Test drivers: `pijade/host/pijade_host.c` for the production build (it needs no RPC), and
`pijade/tools/jadectl.py` for the default build.

---

> **A note on the numbering.** Sections 1 to 6 are this file's own structure: the remote layout,
> the divergence inventory, the rules, the update procedure, the verification and the build line.
> From 16 onwards each section is a numbered work item, and the number belongs to the item rather
> than to this file, which is why the sequence jumps from 6 to 16. Nothing was removed; sections
> 7 to 15 have never existed in this file's history. The numbers are left as they are so that a
> section can still be named by its item number, and renumbering would break that link without
> telling a reader anything new.

## 16. `qrcode_initText()` silently truncates a payload that does not fit

`bb_appendBits()` (`main/qrcode.c:261-268`) does no bounds checking. On overflow, the `padding =
(dataCapacity * 8) - codewords.bitOffsetOrWidth` inside `qrcode_initBytes()` wraps under
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

The expected output today is `2 failure(s)`: v2/96 and v1/48 do not fit, yet `initText=0` is
printed. If a rebase produces `0 failure(s)`, upstream has added bounds checking, and the
version/length assert in `main/process/mnemonic.c` can be revisited. This tool is the twin of
check 17 in `pijade/tools/seedqr_fragments_test.c`.

### The shared build recipe for host tests

The `main/qrcode.h` chain pulls in `main/display.h` -> `arch/sys_arch.h` and `esp_log.h`, both
under `libjade/include`. Also, `<stdbool.h>` has to come BEFORE the QR headers. Hence:

```
-I libjade/include -I components/libwally-core/upstream/include
```

## 17. Scanning back on the host with `quirc`: `ds->data` belongs to the caller

The caller allocates the `data` pointer inside `struct datastream`
(`components/esp32-quirc/lib/quirc.h:169`, device side `main/qrscan.c:55`). Without that
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

A third trap is the library the test links against. Unlike `qr_scale_test.c` (item 5), this test
is panel specific: it hard codes the 240x240 panel and the icon sizes that follow from it, while
`make_libjade.sh` always writes `build_linux`, defaults to libjade's 320x200 placeholder, and
resets every flag it is not given. Link the test against a library built without
`--display=240x240` and exactly three checks fail, `v1/v2/v3 fullscreen icon size is exactly
189/200/203 px`. That is a mismatched build and not a regression: measured 2026-09-12, a 320x200
library returns scale factors 8/6/6 and icons of 168/150/174 px, and the same source against a
240x240 library returns `0 failure(s)`. So build the library first; the line below does that.
(`-I/jade/build_linux/config` used to be passed here and was dropped: that directory does not
exist in this build.)

```bash
docker exec jade-dev sh -lc 'cd /jade && ./libjade/make_libjade.sh Debug --log --camera --no-ci --display=240x240'
docker exec jade-dev sh -lc 'cd /jade && LIBS=$(find /jade/build_linux -name "*.a" | tr "\n" " ") && \
  gcc -o /tmp/seedqr_frag pijade/tools/seedqr_fragments_test.c \
    -I/jade/main -I/jade \
    -I/jade/libjade/include -I/jade/components/libwally-core/upstream/include \
    $LIBS $LIBS -lstdc++ -lm -lz && /tmp/seedqr_frag'
```

Expected: `0 failure(s)`. The test's QR buffers were raised to 112 bytes (for v3,
`qrcode_getBufferSize(3) = 106` and the assert is a strict `>`); growing the buffer does NOT
change the golden fixture, which was measured.

## 19. Decoding the on-screen QR independently

Since `qrcode_initText()` returns 0 even for a payload that does not fit (item 16), "the library
did not complain" is NOT an acceptance criterion. `pijade/tools/screen_qr_decode.c` takes a raw
RGB565 dump of the emulator screen, decodes it with `quirc` and compares it against the expected
text; it also prints the dark-pixel bounding box (for measuring icon size).

```bash
docker exec jade-dev sh -lc 'cd /jade && LIBS=$(find /jade/build_linux -name "*.a" | tr "\n" " ") && \
  gcc -o /tmp/screenqr pijade/tools/screen_qr_decode.c \
    -I/jade/main -I/jade -I/jade/build_linux/config \
    -I/jade/libjade/include -I/jade/components/libwally-core/upstream/include \
    $LIBS $LIBS -lstdc++ -lm -lz'
docker exec jade-dev /tmp/screenqr /probe/<image>.rgb565 240 240 "<expected digit string>"
```

Measured on 2026-08-27 in the 24-word Standard flow: a bounding box of 203x203 px, a 29-module
code, 96 digits, IDENTICAL to the expected string.

## 20. Running the device's own verification branch in the emulator

The last step of the export flow reads the QR the user drew back through the camera and compares
it (`main/process/mnemonic.c:333-337`). To run that in the emulator, the QR on the device's OWN
screen is fed back to its camera:

```bash
# 1) take the full-screen QR dump, learn the bounding box
docker exec jade-dev /tmp/screenqr /probe/<dump>.rgb565 240 240 "<expected>"
# 2) redraw as a 640x480 grey camera frame with a quiet zone (20 20 25 8 for v2, 18 18 29 7 for v3)
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
unencrypted. So every line produced with `--log-level info` lands on the card permanently. Ten
lines that wrote sensitive content were therefore stripped of it; the diagnostic scalars (length,
count, quota, network) were kept. All of them are marked with a `BBB-AIRGAP:` comment, so that a
line reappearing in an upstream merge can be caught with grep:

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

**Freeing icons.** `free_view_node_icon_data()` (`main/gui.c`) was releasing animation frames with
a plain `free()`; icon pixels are a reversible encoding of the exported data. It now calls
`qrcode_freeIconData()` (upstream's own clearing helper).

**The condition for this being safe was measured**: there are two separate icon allocators and
their size calculations DIFFER. `main/qrcode.c:979-980` allocates `((w*h/32)+1)*4` bytes;
`main/display.c:343,437` allocates `written` bytes for deflate-sourced icons and sets `height =
written*8/width`. When `written` is a multiple of 4, the QR formula gives 4 bytes MORE, so
clearing a deflate icon with the QR formula would write outside the allocated block. It is safe
because the ONLY caller of `gui_set_icon_animation()` is `make_qrcode()` (`main/ui/qrmode.c:16`),
and every icon reaching it comes from `qrcode_toIcon` / `qrcode_toFragmentsIcons` /
`bcur_create_qr_icons`. Deflate icons never enter an animation. If an icon from another allocator
is ever attached to this destructor, that invariant has to be measured again.

## 22. Taking the persistent PIN wallet into the card file

`libjade/pijade_settings.c` raised the file format from `PIJADES1` to `PIJADES2`. Old files are
not migrated and open with defaults. Every entry is now `key_len(1), key, value_len(2 LE), value`;
the two-byte length carries both the 256-byte wallet blob and the 2048-byte certificate ceiling.

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

The three string fields start at 1, because `nvs_set_str()` writes `strlen + 1`; an empty string
is a one-byte value, and for `pinsvrurlB` that is a state Jade sets deliberately ("explicitly no
second url", `main/process/pinclient.c:104`). Say the minimum is 2 and an empty urlB never reaches
the file; after a restart urlA exists and urlB does not, and the `JADE_ASSERT(urlASet == urlBSet)`
at `main/process/pinclient.c:113` drops the PIN unlock. A review caught this as a P1 (2026-08-27);
the unit test now runs with an empty urlB.

Length alone is not enough; six fields have their content checked too. The reasons are downstream
assumptions, all of them measured:

- The floor for `blob` is 80, because `main/keychain.c:851-874` encrypts only three kinds of
  payload, five lengths in all: 16 or 32 bytes of mnemonic entropy, a 17 or 33 byte SLIP-0039
  master secret behind its tag byte, and a 206-byte serialised key. The `ENCRYPTED_DATA_LEN`
  chain (`main/aes.h:14,17`) turns those five into 80, 96, 80, 96 and 256 bytes, so the floor is
  unchanged by the tagged lengths. With a floor of 80 the `JADE_ASSERT(bytes_len >
  HMAC_SHA256_LEN)` at `main/keychain.c:719` becomes unreachable.
- The last byte of each of the three string fields has to be NUL. The body of `nvs_get_str()` is
  `nvs_get_blob()` (`libjade/nvs_flash.c:177-180`), so the storage layer promises no termination;
  yet `main/process/pinclient.c:96,108` prints those buffers with `snprintf("%s")`. A one-byte
  empty string is valid, and interior NULs are not searched for.
- The `antireplay` value cannot be `0xFFFFFFFF`; `main/storage.c:564` has
  `JADE_ASSERT(j < UINT32_MAX)`.
- `pinsvrurlA` and `pinsvrurlB` either both exist or neither does
  (`main/process/pinclient.c:113`). This cross-check uses ONLY the flags collected during the
  validation pass (`prefs == NULL`); moved to the storing pass, a rejected file would still store
  one half of the pair.
- There is NO range rule for `walleterasepin`, and there must not be. Under `PIJADES4` the field
  is not raw digits but a salt and a PBKDF2 verifier (`main/storage.h`), so every byte value is
  legitimate. In the `PIJADES3` era there was a digit rule, because `format_pin()` printed the
  stored digits on screen; that screen no longer reads the PIN, so the rule went with it. Keeping
  it would reject ordinary hash bytes, and since rejection is per file it would take the whole
  settings file, wallet blob included.
- `networktype` can only be 0, 1 or 2 (`main/utils/network.h:23`). At startup `main/main.c:203` ->
  `keychain_init_cache()` caches the value without filtering it (`main/keychain.c:1034`), and
  neither `keychain_load()` nor `keychain_set()` resets it. On the first successful PIN unlock
  `main/process/auth_user.c:474-480` calls `keychain_set_network_type_restriction()`
  unconditionally, and the `JADE_ASSERT(keychain_is_network_type_consistent(...))` at
  `main/keychain.c:493` fails on an out-of-list cached value. This block IS present in a
  production build: `-DDEBUG_MODE=0` (`pijade/images/build-armv6.sh:54`) defines
  `CONFIG_LIBJADE_NO_DEBUG_MODE` (`libjade/CMakeLists.txt:101-104`), which leaves
  `CONFIG_DEBUG_MODE` undefined (`libjade/include/sdkconfig.h:10-12`).

A write error is no longer swallowed. Before this work the callback was `void` and `nvs_commit()`
returned `ESP_OK` in every case; while the file carried only preferences that was defensible. Once
the wallet moved in, the same behaviour meant this: if the card is full or read-only, Jade
believes it saved the wallet, the user believes PIN setup finished, and at the next boot there is
no wallet. So `libjade_settings_fn` returns `bool`, `libjade_settings_changed()` carries the
result, and `nvs_commit()` and `nvs_flash_erase()` return `ESP_FAIL` on failure. The rest of the
chain is Jade's own: the `STORAGE_COMMIT` macro in `main/storage.c` already handles non-OK,
`main/keychain.c:765-766` surfaces the wallet write and `main/process/dashboard.c:730-732` the
factory reset to the user as an error. So this addition invents no new error path; it reconnects a
chain the fork had broken.

If no handler is registered the result stays true: running without `--settings` is deliberately a
temporary mode (the emulator), there is nowhere for a write to go, and returning false would break
PIN setup in exactly the mode built to exercise it. The in-memory store is not rolled back; even
when a commit fails the values in RAM stay current, and ESP32 semantics promise no more than that.
The honest contract is the false return itself.

The threat model is deliberately narrow. An actor who can edit the settings file can also write
the `libjade.so` and the rootfs sitting on the same card; so these checks establish no privilege
boundary. They provide robustness, and they close the assert and buffer assumptions above at their
source.

There are NO extra checks on the remaining fields, because there Jade defends itself. That is not
an assumption: the consumption path of all 15 fields was traced, and this is what was measured.
`brightness` is clamped to `BACKLIGHT_MIN..MAX` on the settings screen
(`main/process/dashboard.c:1943-1948`) and clamped again on the host by `panel_set_backlight()`
(`pijade/host/panel_st7789.c:387`). The `guiflags` theme value falls through to `default` inside
`gui_set_highlight_color()` (`main/gui.c`), the camera rotation is taken modulo
(`main/gui.c:388-389`), and the theme index is bounded on the settings screen
(`main/process/dashboard.c:2173`). `qrflags` is a bit mask; the `account_index` derived from it
comes from shifting a 32-bit value by 16, so it is already below `ACCOUNT_INDEX_MAX`
(`main/qrmode.c:41-42,1239-1240`). `keyflags` is a pure bit mask. `idletimeout` is only compared
(`main/idletimer.c:217-237`). If `counter` is greater than 3, `storage_decrement_counter()`
deletes the blob (`main/storage.c:518-519`), so an inflated counter grants no extra attempts.
`privatekey` is rejected by `wally_ec_private_key_verify` (`main/storage.c:452`) and
`pinsvrpubkey` by `wally_ec_public_key_verify` (`main/process/pinclient.c:163`); neither is an
assert, both return false. The URL protocol and the certificate content are deliberately not
validated either: a wrong value produces a connection error, and validating it would tie the file
format to a second set of rules independent of Jade's own.

**A correction to this record.** An earlier version of this paragraph also listed the
`walleterasepin` digit range as "deliberately not validated", on the grounds that it "neither
trips an assert nor reads out of bounds". That claim had NOT been measured and was wrong: the
assert at `main/process/dashboard.c:387` is reachable. The same sweep turned up a second reachable
assert, for `networktype`. The record stays, because the real mistake was not that two fields were
missed but that a scoping decision rested on an assumption rather than a measurement. (The
`walleterasepin` digit rule was later dropped, but not because this record proved wrong: the field
no longer carries digits. The `networktype` rule is still in place.)

The four preference fields stay on the same allowlist: `guiflags` 1, `idletimeout` 2, `brightness`
1, `qrflags` 4 bytes. Serialisation skips an out-of-range value; on read-back, a single field that
is out of range or off the list rejects the whole file.

The security difference is plain: Jade's ESP32 flash is encrypted, and a card file can be copied.
The encrypted wallet blob is useless without the key half held by the pinserver; but an attacker
holding the card can copy the PIN private key and the counter, and roll the local attempt counter
back with an old copy. The remaining defence in that case is the pinserver's own rate limiting.

Two more differences, both particular to this fork:

**`walleterasepin` is NOT in the clear on the card (2026-09-03).** The field is now 16 bytes of
salt and a 32-byte PBKDF2-HMAC-SHA256 verifier; the storage API lost its getter, replaced by
`storage_verify_wallet_erase_pin()` and `storage_wallet_erase_pin_exists()` (`main/storage.c`).
Upstream's screen printed the stored PIN with `format_pin()`, which is why the field had to be
readable; the screen now only says "set", so nothing is lost in parity. **It is important not to
claim more for this than it gives:** six digits is about 20 bits, and an attacker holding the card
can still try all 10^6 candidates on their own machine. The gain is only that someone opening the
file in a hex editor cannot read the PIN off the screen. On the ESP32 the same field sits in
encrypted flash, so Jade did not need this layer. `main/storage.c` also compiles for upstream's
ESP32 target; there there is no card file and no `PIJADES4` rejection, and an old six-byte NVS
record reads as "not set" through a `read_blob_fixed()` length mismatch. This fork produces no
ESP32 image, so that target was not measured.

**Two-slot writing on FAT.** Replacing a single file through a temporary file and `rename` is not
power-cut safe. VFAT may write the target's directory entry and the FAT chain to the card at
different times; at the next boot `fsck` can truncate to 0 bytes the file that points at a free
cluster. The host therefore alternates between the `<base>.a` and `<base>.b` slots, never renames,
and preserves the other valid generation while writing one. The old single file `<base>` is
deliberately neither read nor migrated.

Each slot carries the `PJSLOT01` magic, a little-endian 32-bit sequence number, the payload
length, the libjade blob and a zlib CRC32. A read validates both slots, picks the highest sequence
number, and prefers A on a tie. If the size, magic, length or CRC is wrong, the slot is logged in
one line and ignored. A new write goes to the other slot from the winner; if no valid slot is
known, the target is the sole remaining slot after an erase, and otherwise A.

**State machine guarantees.** The handle keeps the sequence counter monotonic at the highest value
read or attempted, probes the other slot's 16-byte header before writing, returns `false` on a
sequence ambiguity it cannot clear, and picks the sole remaining slot after an erase, otherwise A,
as the next target. **Accepted residual risks:** **R1:** the read path is deliberately not
fail-closed; if the newest generation is physically unreadable, the previous one is loaded,
because refusing to read while a sound copy sits on the card would make the device unusable.
**R2:** the write target is always the slot that did NOT win; if the winner was chosen wrongly
because it could not be read, a half-finished write takes away that unreadable and possibly newer
frame; the generation that survives is the one the device is already running, and the alternative,
overwriting the winner, would leave no valid copy at all on a torn write. **R3:** if power is cut
between a successful write and the erasure of a slot whose sequence is unknown, that slot may
become readable later and, if its sequence is higher, win at the next boot.

A factory reset overwrites the full length of both slots with zeros, calls `fsync`, removes the
files and syncs the directory. Even so, FAT and the flash or SD layer may leave physical old
copies; carving could recover `privatekey`, an old `blob` and the `walleterasepin` verifier. The
previous generation in the other slot also offers a rollback, though an attacker who can copy the
card already has that. The ESP32's encrypted flash does not have this risk profile.

**Three paths where a write error goes to the log, not to the screen.** Upstream's
`main/process/dashboard.c` checks no `storage_set_*` return (measured: zero calls check it). So
`storage_set_wallet_erase_pin()` (`:1413`), `keychain_persist_key_flags()` and the network
restriction helpers ignore the error our new `bool` propagation carries: if the card fills up or
turns read-only after the wallet was saved, the user believes the duress PIN was set while the old
value stays in the file. The error reaches the host log (`pijade: cannot write ...`), not the
screen. This is an upstream characteristic; this work did not change the code, it changed the
probability profile (a removable card instead of soldered flash). The setup moment is protected:
the wallet's own write surfaces the error (`main/keychain.c:765-766`), so PIN setup cannot
silently look "finished". Fixing it would mean diverging on parity screens under `main/` and a
conflict at every rebase; by decision the upstream behaviour was kept (2026-08-27), so this is a
deliberate acceptance.

**Rebase trap:** the 256 ceiling on `blob` was measured from the `SERIALIZED_KEY_LEN` ->
`ENCRYPTED_DATA_LEN` chain at `main/keychain.c:20-24`. If upstream grows the key's serialised form
and the encrypted blob passes 256, serialisation SILENTLY skips that field; the user's wallet
appears to work but does not survive a restart. This ceiling has to be measured again at a rebase.

## 23. Taking the record namespaces into the card

**Divergence:** `libjade/nvs_flash.c`, `libjade/pijade_settings.c`, `libjade/pijade_settings.h`,
`libjade/libjade.c`, `libjade/libjade.h`, `pijade/host/settings_store.c`.

**The measured defect.** The earlier work wrote only the default namespace (`nvs_storage[0]`) of
the five to the card. `nvs_commit()` returned `ESP_OK` for the other four handles without writing
anything; so Jade told the user "multisig saved" and the record vanished at the next boot. The
same silence also reset the HOTP counter at every boot, which means HOTP codes repeat: a violation
of that feature's own security assumption.

**The `PIJADES4` format.** One namespace byte was added to the earlier entry layout: `ns(1),
key_len(1), key, value_len(2 LE), value`. The last character of the magic is the version; a
payload headed `PIJADES2` or `PIJADES3` is now rejected by libjade even in an otherwise valid
slot. The only reason for the `PIJADES3` -> `PIJADES4` step is that the `walleterasepin` field
grew from six digits to a 48-byte verifier; since rejection is per file, an old card loses not
only its duress PIN but all its settings and its wallet blob, so existing cards have to be
prepared again by hand. A backward-compatibility layer was deliberately not written (engineering
principle 1); and the old single file is unreadable under the two-slot design anyway. With no
device in the field, that loss was accepted.

**Upstream's own whole-store format was not used.** `libjade_save_nvs()` / `libjade_load_nvs()`
(`libjade/nvs_flash.c`) already serialise the five namespaces with the `JADE_NVS` magic. The card
file was not tied to it, because that format is the daemon RPC's internal contract: an upstream
change made without our knowing would invalidate cards in the field. Keeping it separate also
guarantees that a daemon NVS dump cannot be swallowed as a card settings file. Those two functions
were not touched.

**A sweep for reachable aborts.** Which assert a record coming from the file can reach was
measured:

| Bound | Source | Why |
|---|---|---|
| Name 1-15 bytes, all ASCII 33-126 | `libjade/nvs_flash.c:243`, `storage_key_name_valid()` in `main/storage.c` | A name of 16 bytes or more calls `abort()` when the menu lists records. The range is the device's own rule; it also rules out a name with an embedded NUL (such a record could not be matched by C string searches, and so could not be deleted) |
| Multisig 114-3281 | `libjade/pijade_settings.c:108`, `main/multisig.c:20`, `main/registration_seal.c:64` | Since the v4 seal the length is a gate, not an assert: a record that clears the file floor but does not fit the sealed layout is reported as not readable and the device stays up |
| Descriptor **41**-3281 | `libjade/pijade_settings.c:111`, `main/descriptor.c:24-25`, `main/registration_seal.c:64` | Same gate. The floor is the pre-sealing record size, kept so that a record written by an older image is refused by `registration_open()` rather than taking the whole settings file down |
| OTP 32-288, a multiple of 16 | `main/aes.c:45,51`, `main/otpauth.c:855` | The stored length goes straight into the `aes_decrypt_bytes()` asserts |
| HOTP counter exactly 8 | `main/storage.c:888-891` | A `uint64_t`, through `read_blob_fixed` |
| At most 16 records per namespace | `main/multisig.h:14`, `main/descriptor.h:15`, `main/otpauth.h:15` | The device's own ceiling |

**A file that is written has to be readable; this was made structural.**
`pijade_settings_serialize()` runs the buffer it produced through the loader's own validation pass
(`walk_entries(NULL, ...)`) before returning it. So no rule is written twice. That also closed a
hidden hole in the earlier design: the ns0 content checks (`antireplay`, `walleterasepin`,
`networktype`) and the `pinsvrurlA`/`pinsvrurlB` matching check lived only in the loader, so a
mismatched pair was written without complaint and rejected by libjade at the next boot. Now the
same situation is a rejected write the user sees immediately.

**Validation cuts both ways.** On read, a single bad entry rejects the whole file (the two passes
were kept). On write, ns0 keeps the earlier behaviour (a field whose length is out of range is
skipped and the device stays on its default for that setting); in ns1-4 the write fails. The
reason: every record path calls `storage_key_name_valid()` and produces records within the bounds
above, so a violation is an impossible state. Skipping it quietly would recreate the very class of
silent loss this work exists to remove, and in user data rather than preference data.

**The ceiling went from 8192 to 131072.** The worst case is `16*3281 + 16*3281 + 16*288 + 16*8` of
record payload plus the per-entry framing and ns0: 113,866 bytes, with every key name at the
15-byte ceiling. The two 3281 figures are `MAX_MULTISIG_BYTES_LEN` and `MAX_DESCRIPTOR_BYTES_LEN`,
which are `REGISTRATION_SEALED_LEN(body)` = `1 + AES_ENCRYPTED_LEN(body) + HMAC_SHA256_LEN`
(`main/registration_seal.h`), not the raw body sizes. Verified against the real binary: the store
the test builds measures 113,418 bytes (its key names are shorter than that ceiling) and is
accepted; 131,073 bytes is rejected. Reads and writes are on the heap and at the real size; there
is no fixed 128 KB buffer anywhere.

**A deliberate behaviour change:** `nvs_commit()` now returns `ESP_FAIL` rather than `ESP_OK` for
an unrecognised handle. Since `nvs_open()` cannot produce a handle outside the five maps, this
path is unreachable in practice; it is defensive. A silent `ESP_OK` was precisely the defect this
work fixed.

**Rebase trap:** all six bounds above were MEASURED from constants under `main/`. The two
registration ceilings are no longer hand-copied numbers: `libjade/pijade_settings.h` defines
`PIJADE_SETTINGS_MAX_MULTISIG_LEN` and `PIJADE_SETTINGS_MAX_DESCRIPTOR_LEN`, and
`libjade/pijade_settings.c` binds each to `MAX_MULTISIG_BYTES_LEN` and `MAX_DESCRIPTOR_BYTES_LEN`
with a `_Static_assert`, so a record size that grows under `main/` now fails the build instead of
being silently rejected at the next restart. They are literals in that header rather than an
include of `main/multisig.h` because `pijade/tools/settings_test.c` includes the same header and
is built on its own, without `main/` on its include path. The rest of the table is still written
into the libjade side as numbers (to preserve the upstream separation): the 114 and 41 floors,
which are deliberately the pre-sealing values, the OTP 32-288 rule, the 8-byte HOTP counter and
the 16-record caps. If upstream grows one of those, serialisation SILENTLY rejects that record and
the user finds out only after a restart, so those rows still have to be measured again at a
rebase.

**The HOTP write cost (accepted, unmeasured):** generating each HOTP code increments the counter
and commits (`main/otpauth.c:624`, `main/storage.c:883-885`), so in the worst case the entire ~111
KB file is rewritten and `fsync`ed. That is the known price of the single-file decision; a
realistic file is 1-10 KB. Measuring the card's write load was left for later.

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
root, and the card has neither an RTC nor a network. The headless path and the `libjade` daemon
have no handler, so as not to change the clock of a development machine.

**A 64-bit `time_t`.** In the target toolchain (armhf Bookworm, glibc 2.36) `time_t` is 4 bytes by
default; the epoch is narrowed into `tv_sec` inside upstream's `params_set_epoch_time()` before it
ever reaches the handler, a post-2038 value wraps silently, and Jade still reports success. The
fix is a build flag: `-D_TIME_BITS=64 -D_FILE_OFFSET_BITS=64` was added to `ARCHFLAGS` in
`pijade/images/build-armv6.sh` (in the same container `sizeof(time_t)` was measured going from 4
to 8); the `_Static_assert(sizeof(time_t) >= 8)` in `pijade_host.c` breaks the build if the flag
is dropped. No upstream file was touched. Evidence: `__settimeofday64`, `__time64` and
`__clock_gettime64` appear in the dynamic symbol tables of the ARMv6 `pijade-host` and
`libjade.so`.

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

The expected output shows two CBOR responses in hex; on the first request the handler is called
once with `1700000000`, and on the second the handler has been removed, so the call count does not
change and the real clock drifts by less than 5 seconds. The last line is `PROBE OK` and the exit
code is 0. An error response, a missing response, a wrong epoch, an extra call or clock drift
produces `PROBE FAILED` and exit code 1.

## 25. Generating a `jade-epoch` QR and scanning it in the emulator

Jade takes the epoch by QR as a `ur:jade-epoch` type (`main/qrmode.c:2875`); the body is directly
the CBOR map `{"id":"1","method":"set_epoch","params":{"epoch":N}}` (`handle_epoch_qr`,
`bcur_parse_jade_message`, `params_set_epoch_time`). The generator is `pijade/tools/epoch_qr.py`
(cbor2==6.1.2 and qrcode, in a virtualenv of your own). For a TOTP comparison it is generated
immediately before the scan:

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

End to end in the emulator: a fresh daemon, `menu_audit.py ... "seed:<public test vector>"`, the
grey frame fed with `set_camera_bytes` BEFORE the Scan QR click and kept flowing afterwards (trap
(b) of item 20), then `right` and `click` on the home screen. Expected: the log line `qrscan.c:30
Detected 1 QR codes`, and the screen showing "Time set successfully" with a date; since the daemon
registers no clock handler the date will be TODAY, which is not an error. Trap: if the screen is
asleep the first press only wakes it (`main/idletimer.c:163-171`), so if the log says "powering
screen" the press is repeated.

## 26. Two-axis input: joystick up/down and the HAT's three buttons

**Divergence:** `main/gui.h`, `main/gui.c`, `main/ui/keyboard.c`, `main/ui/dashboard.c`,
`main/process/dashboard.c`, `main/button_events.h`, `libjade/libjade.h`, `libjade/libjade.c`,
`pijade/host/buttons_gpio.h`, `pijade/host/buttons_gpio.c`, `pijade/host/pijade_host.c`.

**The measured limit.** Jade's own hardware carries one navigation axis and the events left,
right, wheel click and front click. The HAT's joystick has two axes. Since the GUI already keeps
its selectables in a circular list ordered by the screen's `x` and `y` coordinates, no second data
structure was added. A press that wakes the screen is consumed by the existing
`idletimer_register_activity(true)` rule, and when the screen is flipped, up and down are inverted
just as left and right already were.

**The vertical neighbour rule.** On up or down, the nearest row in that direction is found first.
Within that row the enabled item at the smallest horizontal distance is selected; on a tie the
earlier item in the list wins. If there is no row in that direction, the existing previous-or-next
path is used. That fallback preserves today's behaviour on screens that use left and right to
change a value. One press emits exactly one event. Because `sync_wait_event_handler()` keeps no
queue and only updates a single event field, emitting a vertical and a horizontal event on the
same press could lose the first.

FIRST starts from the screen's `is_first` mark and selects the first enabled item; it clicks
nothing and emits no event. On a screen with no selectable item it quietly does nothing. ALT is a
screen-specific second action; today only the next keyboard page. The keyboard handler does not
route ALT into a new page counter of its own, but turns it into the existing `BTN_KEYBOARD_SHIFT`
event. That keeps the bound activity transition and the text box's keyboard counter advancing in
the same order as with the real Shift button.

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

All eight lines are requested on both the falling and the rising edge. Directions repeat 500 ms
after the first press and then every 150 ms. Missed intervals are not accumulated; the next repeat
time is computed from real monotonic time. KEY1, KEY2, KEY3 and the joystick press do not repeat.
These durations were not measured; they are starting values close to Jade Plus's press-and-hold
repeat, to be revisited on the device.

**The Select Connection divergence.** Upstream offers no way back from this screen. Since a back
button in the title would become the first item by coordinate order, the activity's initial
selection was tied explicitly to the first menu button; the screen opens on the same first
selection as today. All three call contexts were handled: `handle_mnemonic_qr()` and
`initialise_wallet()` arrive with a new sourceless wallet; `BTN_CONNECT_TO_BACK` may arrive with a
sourced wallet, or with a new but not yet verified `SOURCE_NONE` wallet. Back calls
`keychain_clear()` only while `keychain_get_userdata() == SOURCE_NONE`. That keeps a sourceless
wallet from reaching the home screen's assertion while preserving a sourced one. The clearing is
the same operation as on Jade's existing `BTN_SESSION_LOGOUT` path.

## 27. Building libjade on macOS (upstream `fe3e3e94`, taken with fork adaptations)

**Divergence:** `libjade/task.c`, `libjade/libjade.c`, `libjade/nvs_flash.c`,
`libjade/CMakeLists.txt`, `libjade/make_libjade.sh`, `components/libwally-core/config.h`,
`pijade/host/settings_store.h`, `.gitignore`. Upstream's own files in the commit
(`libjade/include/libjade_port.h`, `libjade/include/freertos/semphr_darwin.h`,
`libjade/include/freertos/semphr.h`, `libjade/esp_event.c`, `libjade/README.md`,
`jadepy/jade_sw.py`, `main/process/ota_util.c`) were taken unchanged.

**Why the host build is worth having.** The emulator runs in the `jade-dev` container, so every
measurement pays a Docker round trip and every crash is read through a container log. A native
build gives the same firmware under the host debugger. It is a development convenience only: the
shipping target stays 32-bit ARM Linux, and `libjade_daemon` never enters a production image.

**Three conflicts, and how they were resolved.**

*`libjade/task.c`.* Upstream moved the naming call from the parent to the child (the new
`libjade_thread_setname()` in `libjade_port.h`), because macOS can only name the calling thread.
The fork had separately made the parent's `pthread_setname_np()` failure fatal, which upstream's
patch deletes. Both halves are kept: the call happens in the child, and the name is truncated to
15 characters plus the terminator before `strdup()`. Linux caps a thread name at 16 bytes, so it
answers `ERANGE` for anything longer, and `auth_qr_client_task` (19 characters, `main/qrmode.c`)
is the only such name in the firmware. Truncating matches what the API being emulated does:
FreeRTOS copies `configMAX_TASK_NAME_LEN` bytes into the TCB, 16 by default (measured in ESP-IDF's
`components/freertos/Kconfig`), and drops the rest. Task creation no longer fails over a name.

*`libjade/libjade.c`.* `main/process/pinclient.c` and `main/process/update_pinserver.c` ask for
the embedded pinserver key through `asm("_binary_pinserver_public_key_pub_start")`. Mach-O's
linker prepends an underscore of its own, so the definition here must drop it on `__APPLE__` and
keep it everywhere else. Neither file in `main/` is touched.

*`libjade/nvs_flash.c`.* The fork added `pijade_settings.h`; upstream added `libjade_port.h` for
the `le32toh`/`htole32` macros macOS lacks. Both includes are present.

**Two fork-side portability gates.** Each was measured on the failing build, not guessed.

| File | macOS failure | Gate |
|---|---|---|
| `components/libwally-core/config.h` | `call to undeclared function 'explicit_bzero'` | macOS has no `explicit_bzero` (measured: it does not compile even with `<strings.h>`), and this header's `HAVE_INLINE_ASM` barrier is off, so falling through to a plain `memset` would leave the wipe elidable. `HAVE_MEMSET_S` with `__STDC_WANT_LIB_EXT1__` is used there instead, and it is the branch that actually runs: `components/libwally-core/upstream/src/internal.c:335` selects `memset_s()` and `nm -u build_macos/libjade/libjade.dylib` lists `_memset_s` as undefined. Every other target keeps `HAVE_EXPLICIT_BZERO`. |
| `pijade/host/settings_store.h` | the same call, from `pijade/host/settings_store.c` (7 sites) and `libjade/daemon.c` (1) | An `__APPLE__`-only `static inline explicit_bzero()` whose `memset` is followed by an empty asm barrier, the technique libwally uses for the same job. `memset_s` was not used here: it compiles on macOS too (measured, in both include orders), but it is Annex K, which is optional and absent from glibc, and it is declared only where `__STDC_WANT_LIB_EXT1__` is set, so using it would push that feature-test macro onto every host translation unit including this header. |

**The third macOS failure was upstream's, and its fix is upstream's too.** clang rejects
`components/esp32-quirc/openmv/fmath.h` with `invalid output constraint '=f' in asm`:
`fast_sqrtf()` is an Xtensa `fsqrt.s` instruction with an Xtensa register constraint. It compiled
on x86 and on 32-bit ARM only because nothing calls the function and GCC emits no body for an
uncalled static inline; clang validates the constraint while parsing. The first pass at this port
answered by gating that body behind `__XTENSA__`, which edits a vendored file. That gate has been
reverted, because upstream had already solved the same failure one commit earlier, in `1c0025f6`,
which is an ancestor of `fe3e3e94` and was missed when the port was taken: `libjade/libjade.c`
defines `__FMATH_H` before including `identify.c`, so the header never enters the amalgamated
build, and defines the two functions quirc actually calls. The measurement behind that choice:
`identify.c` is the only file that includes `fmath.h`, and the only symbols it uses from it are
`fast_roundf()` at lines 108 and 109 and `fast_fabsf()` at 1134, 1179 and 1180. Both replacements
match the header's own semantics, `(int)(x)` and `fabsf(d)` respectively, so nothing about QR
decoding changes. `cos_table`, `sin_table` and the rest of the `fast_*` family are never
referenced. `components/esp32-quirc/openmv/fmath.h` is now byte identical to upstream again (blob
`76c3b938`), and the ESP32 build still gets the Xtensa instruction, because there the header is
included normally. One limit this leaves standing, stated rather than hidden: `identify.c` has a
second consumer, `pijade/tools/t44_bench.c`, which includes it without the define. That file is
compiled only by `pijade/images/build-armv6.sh` with the target's own GCC, where the uncalled
`fast_sqrtf()` costs nothing, which is how it stood before this port. Building `t44_bench.c` with
clang would hit the same asm error, and the answer then is the same define, not an edit to the
vendored header.

**Hardening flags are now platform gated** (`libjade/CMakeLists.txt`). `-fstack-clash-protection`
and `-Wl,-z,relro,-z,now` are ELF and GCC features that Apple's toolchain does not have, so they
follow the rule upstream already applied to the debug flags and are set for every target except
macOS. `-D_FORTIFY_SOURCE=3`, `-D_GLIBCXX_ASSERTIONS` and `-fstack-protector-strong` stay
unconditional. Verified by configuring a Release tree in the container: all four flags appear in
`flags.make` for `jade`, `jade_static` and `libjade_daemon`, and `-Wl,-z,relro,-z,now` appears in
the link line of `jade` and `libjade_daemon` (a static archive has no link line).

**The build directory is separate on purpose.** `make_libjade.sh` writes `build_linux`, which the
container bind-mounts from this same tree; running it on macOS would overwrite the emulator's
build with host objects. The script now refuses to run on Darwin, and the host build goes to
`build_macos`, which `.gitignore` carries along with its configure line. The source root is the
repository, as it is for every other build in the fork (`build_linux` in the container,
`build_sc_desc` in `.gitignore`, and `cmake -S /src` in `pijade/images/build-armv6.sh`); the root
`CMakeLists.txt` adds `libjade` as a subdirectory when `ESP_PLATFORM` is not set, which is why the
artefacts land under `build_macos/libjade/`:

```sh
IDF_PATH=~/esp/esp-idf cmake -S . -B build_macos -DCMAKE_BUILD_TYPE=Debug -DLOG=LOG \
    -DCI=0 -DDEBUG_MODE=DEBUG_MODE -DDISPLAY_WIDTH=240 -DDISPLAY_HEIGHT=240 -DCAMERA=0
make -C build_macos -j8
```

Only two things are needed from ESP-IDF: `components/mbedtls/mbedtls` and
`components/http_parser`. No cross compiler is installed; the measured tree was ESP-IDF v5.5.4,
whose mbedtls submodule matches the container's.

**What the macOS build does not prove.** The full `test_jade.py` is not run there (it needs
`cbor`, `wallycore`, `pyserial` and `bleak`), the camera path is not built (`-DCAMERA=0`), and
only Debug is exercised. The evidence collected for this port is: `libjade.dylib`,
`libjade_static.a` and `libjade_daemon` all link, and the daemon answers a real `get_version_info`
over a Unix socket with a 484-byte CBOR reply (`JADE_VERSION 1.0.41-pijade`, `JADE_CONFIG
NORADIO`).

**Entropy caveat.** `libjade_getrandom()` calls `arc4random_buf()` on macOS instead of Linux's
`getrandom()`. That is upstream's own porting choice and is fine for a development host, but it
means the entropy work in the security plan (the camera characterisation, and anything measuring
the CSPRNG mix) must not be run on macOS: the source under measurement would be a different one
from the device's.

## 28. Running the upstream test suite against this fork's network rule

**The rule the suite runs into.** This fork registers a multisig or descriptor record for exactly
one bitcoin network, the one the device is set to: `main/process/register_multisig.c:49` and
`main/process/register_descriptor.c:44` both read
`keychain_get_network_type_restriction() == NETWORK_TYPE_TEST ? NETWORK_BITCOIN_TESTNET :
NETWORK_BITCOIN`, and refuse anything else before any other validation runs. Upstream's suite
registers mainnet, testnet and liquid records in one run, so on this fork it stopped at the first
testnet fixture and never reached its tail.

**Why the suite could not simply set the network.** The restriction lives in one variable
(`main/keychain.c:65`) with three writers: `:480` clears it, `:491` sets it, `:1034` reads it back
from the card. Of the two callers that set it, `main/process/auth_user.c:474-480` is behind
`#ifndef CONFIG_DEBUG_MODE`, so on a debug build the only remaining writer is
`main/process/dashboard.c:2460`, the `Settings > Network` screen, which needs a button press.
An unset restriction is not neutral here: the expression above reads `none` as mainnet.

**The handler.** `main/process/debug_set_network.c` is that screen and nothing more; the two calls
it makes are the two `handle_network_type()` makes, so the emulator keeps the shipped behaviour.
It refuses when no wallet is loaded, because the in-memory write is gated on `keychain_data` and
the caller would otherwise get an `ok` reply and no change. `none` restores the unrestricted state
a debug build starts in.

It is a debug handler in the sense `debug_set_mnemonic.c` is, and the same gate keeps it out of
the shipped image: `pijade/images/build-armv6.sh:54` passes `-DDEBUG_MODE=0`,
`libjade/CMakeLists.txt:102-103` turns that into `-DCONFIG_LIBJADE_NO_DEBUG_MODE`, and
`libjade/include/sdkconfig.h:10-12` defines `CONFIG_DEBUG_MODE` only when that macro is absent. In
the production image the whole file compiles to nothing.

**The Python side.** `jadepy/jade.py` `set_network_restriction()` wraps the call; its docstring
carries both conditions (debug build, wallet loaded).

**Running it.**

```
docker exec jade-dev sh -lc 'cd /jade && LD_LIBRARY_PATH=/jade/build_linux_log \
  timeout 2400 python3 test_jade.py --libjade'
```

`build_linux_log` is not just a name: it is a Release build with `CI=ON` and no camera, at
320x200. Point the same command at a Debug `--no-ci --camera` build of the same tree and it ends
at the first `debug_clean_reset` with a premature end of CBOR stream (measured 2026-09-15). Build
the variant this line names before reading a failure as a regression.

`test_jade.py` sets the device to each fixture's own network and always restores `none`
afterwards, because the restriction outlives the test that set it: `keychain_clear()` does not
touch that variable, so a stray `testnet` would silently change the network every later test runs
on. A failed restore is reported rather than swallowed. The fork's own constants sit together at
the top of the file (`FORK_BITCOIN_NETWORKS` and the three refusal messages).

**What the suite now measures as a refusal rather than a success.** Two paths rest on records this
fork will not register, so the refusal is the whole test: the liquid 2of2 comparison against GA
signatures (`test_generic_multisig_matches_ga_signatures_liquid`), and the one multisig file
carrying a master blinding key inside `test_generic_multisig_files`. Signing against a wallet that
cannot be registered would only measure the missing record. This is not a regression on this fork,
but it is a change in what the suite covers, and it is written here so it is read rather than
discovered.

## 29. Taking upstream's `uint32_t` RPC series (`c95ed4ee` to `58c11066`)

Ten upstream commits replace the `size_t` RPC getters with `uint32_t` ones and change every RPC
parameter that used them. They were taken on 2026-09-15, one cherry-pick each, in upstream order:

| Upstream | Subject |
|---|---|
| `c95ed4ee` | rpc: add getters for uint32_t |
| `4594cb77` | assets: refactor vout and precision to uint32_t |
| `5aec9db0` | multisig: refactor threshold to uint32_t |
| `adcafcbb` | sign_tx: refactor RPC parameters to uint32_t |
| `69d50f3f` | ota: change type of RPC parameters to uint32_t |
| `ff869fde` | get_receive_address: change type of RPC parameters to uint32_t |
| `a8f7da37` | bip85: convert RPC parameters to uint32_t |
| `d0745f33` | process: change type of RPC parameters to uint32_t |
| `b32b0929` | rpc: remove size_t getters |
| `58c11066` | rpc: resolve implicit type conversion warning |

**Why it matters on this board.** On the ESP32 `size_t` is already 32 bits, so upstream's change
is a tidy-up there. In `libjade` it is not: the emulator and the host tools are 64-bit, and
`rpc_get_sizet()` accepted values up to `SIZE_MAX` that the device would never have accepted.
After the series both read the same width, so a value the emulator takes is a value the device
takes.

**One conflict, in the one place that could corrupt a card.** Nine of the ten applied unchanged;
`5aec9db0` conflicted in `main/multisig.c` (three blocks) and `main/multisig.h` (one), because
this fork had already rewritten the record writer. The resolution and its reasoning are in that
commit's own message. The short version: widening the parameter alone would have written four
bytes where `MULTISIG_BODY_LEN()` counts one, so upstream's single-byte write had to come with it.
The record format is unchanged, which is what the round trip below measures.

**No collision with the fork's own `PRIu32` work.** Measured at the branch point, `main/` carried
eight `PRIu32` uses in four files; before the series it carried the same eight (three of them the
fork's own, in `main/bcur.c`); after it, twenty-one in eleven files. The series brings thirteen of
its own and leaves the fork's in place, including the two in `main/process/sign_tx.c` that log a
truncated input amount, which moved from lines 811 and 814 to 810 and 813 without being rewritten.
No call site of `rpc_get_sizet()` remained anywhere in the tree afterwards; every one of them came
from upstream in the first place, and this fork had added none.

**What was measured before the series was taken into the branch** (all in the emulator container,
on a scratch worktree, so the branch was untouched until it passed):

1. A full `libjade` build of the merged tree, Debug with logging, camera and the 240x240 panel:
   no new warning; the five it prints are the pre-existing `-fno-rtti` and `noreturn` ones.
2. All six custom selfchecks (`descriptor`, `urldecode`, `mining`, `bbqr`, `seedxor`, `slip39`)
   through `pijade/tools/run_libjade_selfchecks.py`: PASS, each with its own runtime marker.
3. A multisig record round trip **across the two binaries**: a registration written by the daemon
   built from the commit before the series is read back by the daemon built from the end of it,
   and the reverse, on the same card file, with threshold 2 and two signers intact both ways. The
   card slot holding the record came out the same size from both binaries. This is the test that
   would have caught the four-byte threshold, and it is worth repeating for any future upstream
   commit that touches `multisig_body_to_bytes()`.
4. The full `test_jade.py --libjade` suite against the merged tree: exit code 0.


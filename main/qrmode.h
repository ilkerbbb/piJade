#ifndef QRMODE_H_
#define QRMODE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <cbor.h>

#include "jade_assert.h"
#include "otpauth.h"

typedef struct gui_activity_t gui_activity_t;

// NOTE: Jade only supports the bip39 English wordlist,
// with a 12 or 24 word mnemonic phrase.
#define MNEMONIC_MAXWORDS 24

// The longest valid words in the English wordlist are 8 characters.
#define MNEMONIC_MAX_WORD_LEN 8

// Size of a buffer for holding a mnemonic phrase.
// 24 8-character words + 23 spaces + NUL = 216 bytes
#define MNEMONIC_BUFLEN 216

// Display singlesig xpub qr code
void display_xpub_qr(void);

// Handle scanning a QR - supports addresses and PSBTs
// BBB-AIRGAP: 'title' and 'help_url' are what the camera's header row and its '?' key show.  Both
// are parameters rather than constants because this one scanner serves several entry points: the
// home Scan QR tile is a general scan and wants the general page, while Set Clock is waiting for
// one particular kind of code and wants the page that draws it.  The title follows the same split
// - a screen opened to read a clock code should not call itself 'Scan QR' when the caller knows
// exactly what is being scanned.
void handle_scan_qr(const char* title, const char* help_url);

// BBB-AIRGAP: scan a 'signmessage' QR and show the signature, refusing anything else - the way in
// from a wallet menu to signing code the general scan above could only reach by chance
void handle_sign_message(void);

// BBB-AIRGAP: list the addresses of the wallet in use, so one can be checked against what a
// watch-only wallet shows without scanning it first
void handle_address_explorer(void);

// Display a BC-UR bytes message
WARN_UNUSED_RESULT bool display_bcur_bytes_qr(
    const char* message[], size_t message_size, const uint8_t* data, size_t data_len, const char* help_url);

// Display bip85/bip39 encrypted entropy as BC-UR QR.
void show_bip85_bip39_entropy_qr(const uint8_t* cbor, const size_t cbor_len);

// Display screen with qr code
// Handles up to v6. codes - ie text up to 134 bytes
void await_single_qr_activity(const char* message[], size_t message_size, const uint8_t* data, size_t data_len);

// Display screen with help url and qr code
void await_qr_help_activity(const char* url);

// BBB-AIRGAP: the help addresses this fork points at.  Two constraints shape them.  First,
// await_qr_help_activity() asserts the address is shorter than MAX_QR_V4_DATA_LEN (78) - a longer
// one halts the device rather than showing a screen - so these stay well under that; both are 30
// bytes, measured.  Second, they are ours rather than blkstrm.com because Blockstream has no page
// that draws either of the two codes these screens need: it has none for ur:jade-epoch, and its
// jadescan page lists the kinds of code a Jade camera reads and then sends the reader on to
// companion apps this port does not build (measured 2026-09-08).  Both pages are drawn from
// sources kept here - docs/clock/index.html and docs/sign/index.html - and served by this
// repository's own GitHub Pages site, so a fork gets the pages together with the firmware
// (fork decision, 2026-09-08: the pages belong in the piJade repository, not a second one).  The device
// never fetches them: the QR is read by a phone, which is the only side that touches a network.
// The address is kept in three pieces because the back/continue screen draws each of its three
// message rows as its own text node, and a node that does not fit is clipped rather than
// wrapped: measured on the emulator, that column takes about 11 to 13 characters, so the whole
// 30-character address needs all three rows and no room is left for a label.  The pieces break
// at a dot and a slash so each row still reads as part of an address.  The whole address is
// composed from the same pieces, so the rows and the code in the QR cannot drift apart.
#define PIJADE_HELP_HOST_1 "ilkerbbb."
#define PIJADE_HELP_HOST_2 "github.io/"
#define PIJADE_HELP_CLOCK_PATH "piJade/clock"
#define PIJADE_HELP_SIGN_PATH "piJade/sign"
#define PIJADE_HELP_CLOCK_URL PIJADE_HELP_HOST_1 PIJADE_HELP_HOST_2 PIJADE_HELP_CLOCK_PATH
#define PIJADE_HELP_SIGN_URL PIJADE_HELP_HOST_1 PIJADE_HELP_HOST_2 PIJADE_HELP_SIGN_PATH

// BBB-AIRGAP: qr density/speed screen, opened from the device settings menu
void handle_qr_settings(void);

// BBB-AIRGAP: mining menu (start, and whose address the block reward pays), opened from the device
// settings menu.  Upstream drives mining over the serial link this port does not build, so without
// this entry mining could only be reached by a template turning up in the general scan.
void handle_mining_settings(void);

int32_t wait_export_screen_button(gui_activity_t* act, int32_t exit_ev_id, bool* code_shown);

// Display screen with label, url, qr code, and back/continue buttons
bool await_qr_back_continue_activity(
    const char* message[], size_t message_size, const char* url, bool default_selection);

// Display a QR code for the OTP context
bool show_otp_uri_qr_activity(const otpauth_ctx_t* otp_ctx);

// Start pinserver authentication via qr codes
void handle_qr_auth(bool suppress_pin_change_confirmation);

#endif /* QRMODE_H_ */

#ifndef STORAGE_H_
#define STORAGE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <nvs.h>

#include "jade_assert.h"
#include "utils/network.h"

#define BLE_ENABLED 0x1

#define GUI_FLAGS_THEMES_MASK 0x7
// BBB-AIRGAP: camera mounting angle in quarter turns, 0-3. Placed at 0x30 rather than next to the
// theme mask because that mask is the one with room to grow (0x7 -> 0xF on a ninth theme).
#define GUI_FLAGS_CAMERA_ROTATION_MASK 0x30
#define GUI_FLAGS_CAMERA_ROTATION_SHIFT 4
#define GUI_FLAGS_FLIP_ORIENTATION 0x40
#define GUI_FLAGS_USE_WHEEL_CLICK 0x80

#define QR_DENSITY_LOW 0x1
#define QR_DENSITY_HIGH 0x2
#define QR_SPEED_LOW 0x4
#define QR_SPEED_HIGH 0x8

#define QR_XPUB_WITNESS 0x100
#define QR_XPUB_MULTISIG 0x200
#define QR_XPUB_HDKEY 0x400
#define QR_XPUB_LEGACY 0x800
#define QR_XPUB_TAPROOT 0x1000

// BBB-AIRGAP: whose address a mined block pays.  Unset (the default) means the address the scanned
// mining template names; set means one the user picks from this wallet.  It lives in the qr flags
// because mining is a qr flow on this port - the template arrives as a code and the solution leaves
// as one - and because the feature-flags byte has only its reserved hole left (below).  The address
// itself is not stored: only the choice is, and the address is picked when mining starts.
#define QR_MINING_ADDRESS_WALLET 0x2000

#define KEY_FLAGS_AUTO_DEFAULT_PASSPHRASE 0x1
#define KEY_FLAGS_USER_TO_ENTER_PASSPHRASE 0x2
#define KEY_FLAGS_WORDLIST_PASSPHRASE 0x4
#define KEY_FLAGS_CONFIRM_EXPORT_BLINDING_KEY 0x80

// BBB-AIRGAP: which optional wallet features are offered, kept in their own byte because gui_flags
// above has none left (themes, camera rotation, orientation and wheel click take all eight bits).
// 0x04 is deliberately unused: it was reserved for skipping an xpub privacy warning, but this
// port's xpub flow has no such screen to skip (main/qrmode.c, display_xpub_qr()).  The hole is
// left open rather than closed up, so the remaining bits keep the numbers the plan gave them.
#define FEATURE_FLAGS_BIP85 0x01
#define FEATURE_FLAGS_SIGN_MESSAGE 0x02
#define FEATURE_FLAGS_HARSH_WARNINGS 0x08
#define FEATURE_FLAGS_XPUB_DETAILS 0x10
#define FEATURE_FLAGS_SINGLESIG 0x20
#define FEATURE_FLAGS_MULTISIG 0x40
#define FEATURE_FLAGS_DENOMINATION_SATS 0x80

// Every screen as it was before these flags existed: each optional feature offered, amounts in BTC.
#define FEATURE_FLAGS_DEFAULT                                                                                          \
    (FEATURE_FLAGS_BIP85 | FEATURE_FLAGS_SIGN_MESSAGE | FEATURE_FLAGS_HARSH_WARNINGS | FEATURE_FLAGS_XPUB_DETAILS      \
        | FEATURE_FLAGS_SINGLESIG | FEATURE_FLAGS_MULTISIG)

#define MAX_PINSVR_CERTIFICATE_LENGTH 2048
#define MAX_PINSVR_URL_LENGTH 120

bool storage_init(void);
bool storage_erase(void);
bool storage_get_stats(size_t* entries_used, size_t* entries_free);
bool storage_key_name_valid(const char* name);
void storage_key_name_make_valid(char* name);

bool storage_set_pin_privatekey(const uint8_t* privatekey, size_t key_len);
bool storage_get_pin_privatekey(uint8_t* privatekey, size_t key_len);

bool storage_set_encrypted_blob(const uint8_t* encrypted, size_t encrypted_len);
bool storage_get_encrypted_blob(uint8_t* encrypted, size_t encrypted_len, size_t* written);
bool storage_decrement_counter(void);
bool storage_restore_counter(void);
uint8_t storage_get_counter(void);
bool storage_get_replay_counter(uint32_t* replay_counter);
bool storage_erase_encrypted_blob(void);

bool storage_set_key_flags(uint8_t flags);
uint8_t storage_get_key_flags(void);

// BBB-AIRGAP: the duress ('wallet-erase') PIN is not stored in clear.  What is
// written is a random salt followed by a PBKDF2-HMAC-SHA256 verifier derived from
// the PIN; the PIN itself cannot be read back, so the record is checked rather
// than fetched.  This removes plaintext storage and nothing more: six digits are
// ~20 bits, so whoever takes the card can still try all of them offline.  The
// iteration count slows the device down, not that attacker, and claims nothing more.
#define WALLET_ERASE_PIN_SALT_LEN 16
#define WALLET_ERASE_PIN_VERIFIER_LEN 32 // PBKDF2_HMAC_SHA256_LEN; asserted in storage.c
#define WALLET_ERASE_PIN_RECORD_LEN (WALLET_ERASE_PIN_SALT_LEN + WALLET_ERASE_PIN_VERIFIER_LEN)

bool storage_set_wallet_erase_pin(const uint8_t* pin, size_t pin_len);
bool storage_verify_wallet_erase_pin(const uint8_t* pin, size_t pin_len);
bool storage_wallet_erase_pin_exists(void);
bool storage_erase_wallet_erase_pin(void);

bool storage_set_pinserver_details(const char* urlA, const char* urlB, const uint8_t* pubkey, size_t pubkey_len);
bool storage_get_pinserver_urlA(char* url, size_t len, size_t* written);
bool storage_get_pinserver_urlB(char* url, size_t len, size_t* written);
bool storage_get_pinserver_pubkey(uint8_t* pubkey, size_t pubkey_len);
bool storage_erase_pinserver_details(void);

bool storage_set_pinserver_cert(const char* cert);
bool storage_get_pinserver_cert(char* cert, size_t len, size_t* written);
bool storage_erase_pinserver_cert(void);

bool storage_set_network_type_restriction(network_type_t networktype);
network_type_t storage_get_network_type_restriction(void);

bool storage_set_idle_timeout(uint16_t timeout);
uint16_t storage_get_idle_timeout(void);

// BBB-AIRGAP: screen dimming threshold, separate from the power-off idle timeout above
bool storage_set_screen_timeout(uint16_t timeout);
uint16_t storage_get_screen_timeout(void);

bool storage_set_brightness(uint8_t brightness);
uint8_t storage_get_brightness(void);

bool storage_set_gui_flags(uint8_t color);
uint8_t storage_get_gui_flags(void);

bool storage_set_ble_flags(uint8_t flags);
uint8_t storage_get_ble_flags(void);

bool storage_set_qr_flags(uint32_t flags);
uint32_t storage_get_qr_flags(void);

bool storage_set_feature_flags(uint8_t flags);
uint8_t storage_get_feature_flags(void);

// Generic multisig
bool storage_set_multisig_registration(const char* name, const uint8_t* registration, size_t registration_len);
bool storage_get_multisig_registration(
    const char* name, uint8_t* registration, size_t registration_len, size_t* written);

size_t storage_get_multisig_registration_count(void);
bool storage_multisig_name_exists(const char* name);
bool storage_get_all_multisig_registration_names(
    char names[][NVS_KEY_NAME_MAX_SIZE], size_t num_names, size_t* num_written);

bool storage_erase_multisig_registration(const char* name);

// Descriptor wallets
bool storage_set_descriptor_registration(const char* name, const uint8_t* registration, size_t registration_len);
bool storage_get_descriptor_registration(
    const char* name, uint8_t* registration, size_t registration_len, size_t* written);

size_t storage_get_descriptor_registration_count(void);
bool storage_descriptor_name_exists(const char* name);
bool storage_get_all_descriptor_registration_names(
    char names[][NVS_KEY_NAME_MAX_SIZE], size_t num_names, size_t* num_written);

bool storage_erase_descriptor_registration(const char* name);

// HOTP / TOTP
bool storage_set_otp_data(const char* name, const uint8_t* data, size_t data_len);
bool storage_get_otp_data(const char* name, uint8_t* data, size_t data_len, size_t* written);

bool storage_set_otp_hotp_counter(const char* name, const uint64_t counter);
uint64_t storage_get_otp_hotp_counter(const char* name);

size_t storage_get_otp_count(void);
bool storage_otp_exists(const char* name);
bool storage_get_all_otp_names(char names[][NVS_KEY_NAME_MAX_SIZE], size_t num_names, size_t* num_written);

bool storage_erase_otp(const char* name);

#endif /* STORAGE_H_ */

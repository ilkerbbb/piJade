#ifndef AMALGAMATED_BUILD
#include "bcur.h"
#include "jade_assert.h"
#include "keychain.h"
#include "qrcode.h"
#include "qrmode.h"
#include "descriptor.h"
#include "descriptor_text.h"
#include "qrscan.h"
#include "ui.h"
#include "utils/malloc_ext.h"
#include "utils/network.h"
#include "utils/util.h"

#include <cbor.h>
#include <cdecoder.h>
#include <cencoder.h>
#include <inttypes.h>

// PSBT serialisation functions
bool deserialise_psbt(const uint8_t* bytes, size_t bytes_len, struct wally_psbt** psbt_out);
bool serialise_psbt(const struct wally_psbt* psbt, uint8_t** output, size_t* output_len);

const char BCUR_TYPE_CRYPTO_BIP39[] = "crypto-bip39";
const char BCUR_TYPE_CRYPTO_ACCOUNT[] = "crypto-account";
const char BCUR_TYPE_CRYPTO_HDKEY[] = "crypto-hdkey";
const char BCUR_TYPE_CRYPTO_PSBT[] = "crypto-psbt";
const char BCUR_TYPE_JADE_PIN[] = "jade-pin";
const char BCUR_TYPE_JADE_EPOCH[] = "jade-epoch";
const char BCUR_TYPE_JADE_UPDPS[] = "jade-updps";
const char BCUR_TYPE_JADE_BIP8539_REQUEST[] = "jade-bip8539-request";
const char BCUR_TYPE_JADE_BIP8539_REPLY[] = "jade-bip8539-reply";
const char BCUR_TYPE_JADE_MINE[] = "jade-mine";
const char BCUR_TYPE_JADE_MINE_REPLY[] = "jade-mine-reply";
const char BCUR_TYPE_BYTES[] = "bytes";
const char BCUR_TYPE_CRYPTO_OUTPUT[] = "crypto-output";

static const char BCUR_PREFIX[] = "ur:";

// Index is QR 'version' (ie size), value is the capacity of
// 'alphanumeric' mode - which is what we use for bcur display
// as restricted to uppercase (assumes BCUR_QR_ECC).
// See: https://www.qrcode.com/en/about/version.html - 'Alphanumeric'
static const uint32_t QR_ALPHANUMERIC_CAPACITY[] = { 0, 25, 47, 77, 114, 154, 195, 224, 279, 335, 395, 468, 535 };

// Index is QR 'version' (ie size), value is the scale factor
// used to get an image as large as sensibly fits the Jade screen.
// NOTE: we can scale up more on larger screens
#if CONFIG_DISPLAY_WIDTH >= 480 && CONFIG_DISPLAY_HEIGHT >= 220
static const uint32_t QR_SCALE_FACTOR[] = { 0, 10, 8, 7, 6, 5, 5, 4, 4, 4, 3, 3, 3 };
#elif CONFIG_DISPLAY_WIDTH >= 320 && CONFIG_DISPLAY_HEIGHT >= 170
static const uint32_t QR_SCALE_FACTOR[] = { 0, 8, 6, 5, 5, 4, 4, 3, 3, 3, 2, 2, 2 };
#else
static const uint32_t QR_SCALE_FACTOR[] = { 0, 6, 5, 4, 4, 3, 3, 2, 2, 2, 2, 2, 2 };
#endif

// BBB-AIRGAP: wallet QRs are drawn on the whole panel rather than in the right-hand column of a
// split screen (see display_fullscreen_qr() in main/qrmode.c), so where the panel has room for a
// larger code than the split layout used, take it. The table above is the floor - on a short screen
// like the 240x135 Jade v1 those values already fill the height, and quiet zone or not, a code that
// shrinks is a code that stops scanning - but only as far as the panel actually holds the code.
uint32_t qr_fullscreen_scale_factor(const uint8_t qr_version)
{
    JADE_ASSERT(qr_version < sizeof(QR_SCALE_FACTOR) / sizeof(QR_SCALE_FACTOR[0]));

    const uint32_t modules = 17 + (4 * qr_version); // a version-N code is 17+4N modules square
    const uint32_t shorter_side
        = CONFIG_DISPLAY_WIDTH < CONFIG_DISPLAY_HEIGHT ? CONFIG_DISPLAY_WIDTH : CONFIG_DISPLAY_HEIGHT;
    // Two modules of quiet zone per side; the QR fill node paints the rest of the panel the same
    // colour, so what is left over stays quiet zone too.
    const uint32_t with_quiet_zone = shorter_side / (modules + 4);
    // display_icon() asserts an icon is no larger than the screen (main/display.c), and on a 128px
    // panel the table asks for more than that from version 4 up, so cap the floor at what fits.
    const uint32_t fits_panel = shorter_side / modules;
    const uint32_t floor
        = QR_SCALE_FACTOR[qr_version] < fits_panel ? QR_SCALE_FACTOR[qr_version] : fits_panel;
    return with_quiet_zone > floor ? with_quiet_zone : floor;
}

// NOTE: educated-guesswork/reverse-engineered - pass this value into bcur encoder
// to get a series of text fragments which are within 'capacity' - ie. within the
// capacity of the qr code version we are intending to use.
// max_frag_size = target-capacity - length of the type label - 12 for 'UR:' and
// '/123-123/' - "magic number" 42 (for the cbor+metadata overhead) then halved, as
// it's 2 encoding chars per underlying byte.
// See function 'bcur_check_fragment_sizes()' below for the test of this macro.
#define BCUR_MAX_FRAGMENT_SIZE(capacity, type) ((capacity - strlen(type) - 12 - 42) / 2)

// BBB-AIRGAP: the macro above cannot reach version 3 - its 42-byte allowance leaves only 4 payload
// bytes at that capacity, below bc-ur's own min_fragment_len of 8, so urcreate_placement_encoder()
// asserts. 9 is the only size that clears that minimum and still keeps every emitted part inside
// version 3's 77-character capacity (parts grow as sequence numbers lengthen, so the whole set is
// measured, not just the first). Verified with pijade/tools/qr_fragsize_test.c for payloads up to
// 150 bytes - xpub export builds into a 128-byte cbor buffer, so it stays under that with margin.
// Re-run that tool on every upstream sync: a change to bc-ur's fountain metadata would push parts
// past the capacity assert below and turn this into a crash on the device.
#define BCUR_FRAGMENT_SIZE_V3 9

// The ECC mode we use for BCUR QR display
#define BCUR_QR_ECC ECC_LOW

// For every 3 pure data fragments, add one fountain-code fragment.
// This should assist where a frame is missed by the scanner.
// Add a maximum of 100 fountain-code fragments, just for sanity's sake.
#define BCUR_NUM_FRAGMENTS(num_pure_fragments)                                                                         \
    (num_pure_fragments <= 300 ? 4 * num_pure_fragments / 3 : num_pure_fragments + 100)

// BBB-AIRGAP: bounds on the 'I'-filtered generation in bcur_create_qr_icons() - see the comment
// there for what the filter is and why it exists. Both bounds are absolute rather than multiples of
// the fragment count, because the encoder's per-part cost and the icon memory both grow with that
// count: at 100x the sequence length a 22KB payload spent 90,200 pulls (97 seconds on a linux
// build, far longer on the device) before giving up, and 4x the fragment count would be over 2000
// icons for that same payload, which is an allocation failure - ie. an abort, ie. a bricked screen.
// The filter is for the sets a device can afford to inflate; larger sets fall back to the plain
// upstream set, which is what they got before this existed. 2000 pulls is ~40x the worst case
// measured on a real xpub set (47 pulls at qr version 3), and the 100 extra icons mirror the cap
// BCUR_NUM_FRAGMENTS() already puts on fountain extras.
// Densities the 'I'-filter runs at: the xpub flow asks for version 3, and version 4 is the next
// step up. See the gate in bcur_create_qr_icons() for why it is not applied above these.
#define BCUR_FILTER_MAX_QR_VERSION 4

#define BCUR_FILTER_MAX_PULLS 2000
#define BCUR_FILTER_MAX_EXTRA_ICONS 100

// Parse bcur bip39 cbor to extract a mnemonic string (space separated words)
// NOTE: only the English wordlist is supported.
bool bcur_parse_bip39(
    const uint8_t* cbor, const size_t cbor_len, char* mnemonic, const size_t mnemonic_len, size_t* written)
{
    JADE_ASSERT(cbor && cbor_len);
    JADE_ASSERT(mnemonic && mnemonic_len == MNEMONIC_BUFLEN);
    JADE_INIT_OUT_SIZE(written);

    // Parse cbor
    CborValue value;
    CborParser parser;
    if (!rpc_untrusted_parser_init(cbor, cbor_len, &parser, &value) || !cbor_value_is_container(&value)) {
        return false;
    }

    CborValue mapItem;
    CborError cberr = cbor_value_enter_container(&value, &mapItem);
    if (cberr != CborNoError || !cbor_value_is_valid(&mapItem)) {
        return false;
    }

    int key_res = 0;
    cberr = cbor_value_get_int(&mapItem, &key_res);
    if (cberr != CborNoError || key_res != 1) {
        return false;
    }
    cberr = cbor_value_advance(&mapItem);
    if (cberr != CborNoError || !cbor_value_is_valid(&mapItem) || !cbor_value_is_array(&mapItem)) {
        return false;
    }
    size_t num_words = 0;
    cberr = cbor_value_get_array_length(&mapItem, &num_words);
    if (cberr != CborNoError || (num_words != 12 && num_words != 24) || !cbor_value_is_container(&mapItem)) {
        return false;
    }
    CborValue arrayItem;
    cberr = cbor_value_enter_container(&mapItem, &arrayItem);
    if (cberr != CborNoError || !cbor_value_is_valid(&arrayItem) || !cbor_value_is_text_string(&arrayItem)) {
        return false;
    }
    size_t write_pos = 0;
    for (size_t i = 0; i < num_words; ++i) {
        JADE_ASSERT(write_pos < MNEMONIC_BUFLEN - MNEMONIC_MAX_WORD_LEN - 1);
        if (write_pos) {
            // Add space separator
            mnemonic[write_pos++] = ' ';
        }

        if (!cbor_value_is_text_string(&arrayItem)) {
            return false; // Non-string in array
        }

        // Copy the next word
        CborValue next;
        size_t tmp_len = MNEMONIC_MAX_WORD_LEN + 1;
        cberr = cbor_value_copy_text_string(&arrayItem, mnemonic + write_pos, &tmp_len, &next);
        if (cberr != CborNoError || !tmp_len) {
            return false;
        }
        write_pos += tmp_len;
        arrayItem = next;
    }
    if (!cbor_value_at_end(&arrayItem)) {
        return false;
    }
    cberr = cbor_value_leave_container(&mapItem, &arrayItem);
    if (cberr != CborNoError || !cbor_value_is_valid(&mapItem) || !cbor_value_is_integer(&mapItem)) {
        return false;
    }
    cberr = cbor_value_get_int(&mapItem, &key_res);
    if (cberr != CborNoError || key_res != 2) {
        return false;
    }
    cberr = cbor_value_advance(&mapItem);
    if (cberr != CborNoError || !cbor_value_is_valid(&mapItem) || !cbor_value_is_text_string(&mapItem)) {
        return false;
    }

    // NOTE: only the English wordlist is supported.
    bool string_is_en = false;
    cberr = cbor_value_text_string_equals(&mapItem, "en", &string_is_en);

    if (cberr != CborNoError || !string_is_en) {
        return false;
    }
    cberr = cbor_value_advance(&mapItem);
    if (cberr != CborNoError || !cbor_value_at_end(&mapItem)) {
        return false;
    }

    cberr = cbor_value_leave_container(&value, &mapItem);
    if (cberr != CborNoError) {
        return false;
    }

    mnemonic[write_pos++] = '\0';
    *written = write_pos;
    return true;
}

// BBB-AIRGAP: reject a malformed sequence component before the decoder sees it.
//
// URDecoder::parse_sequence_component() (components/esp32_bc-ur/src/ur-decoder.cpp) runs
// std::stoul on the two halves of "<seq_num>-<seq_len>".  That file is compiled with
// -fno-exceptions (libjade/CMakeLists.txt) while the standard library it calls is not, so a
// non-numeric or oversized component throws where nothing can catch it.  Measured on this
// build: "ur:bytes/x-3/..." dies with std::invalid_argument and
// "ur:bytes/99999999999999999999-2/..." with std::out_of_range, both SIGABRT, and the frame
// that gets there came off the camera.
//
// The check lives here rather than in the decoder because that decoder is a submodule owned
// upstream (components/esp32_bc-ur): a fix there could not be committed with this repository,
// only carried as an untracked edit that the next checkout would drop.  The cost is that this
// guards the two paths that take outside input rather than the parser itself, so a future
// caller could bypass it; libjade/cxx_terminate.cpp is the backstop that keeps even that case
// from dying with keys in memory.
//
// SEQ_MAX bounds what the sequence length can make the decoder allocate downstream
// (FountainDecoder::validate_part inserts one index per part).  Jade's own encoder stays far
// below it - the fountain fragment count is capped at pure fragments + 100, see
// BCUR_NUM_FRAGMENTS above.
#define BCUR_SEQ_LEN_MAX 4096

// Both halves of a "<seq_num>-<seq_len>" component must be digits: parse_sequence_component()
// feeds them straight to std::stoul, which throws on anything else.  The 4096 ceiling belongs to
// seq_len alone, because that is the number the decoder turns into an allocation; sequence
// NUMBERS keep counting past seq_len in a fountain animation, so capping them would reject a
// legitimate long scan.  They only need to fit the uint32_t the decoder reads them into.
static bool bcur_seq_halves_ok(const char* const start, const char* const end)
{
    const char* const dash = memchr(start, '-', (size_t)(end - start));
    if (!dash) {
        return true; // not a sequence component; the decoder rejects it on its own
    }

    const char* const halves[] = { start, dash + 1 };
    const char* const limits[] = { dash, end };
    const uint64_t maxima[] = { UINT32_MAX, BCUR_SEQ_LEN_MAX };
    for (size_t i = 0; i < 2; ++i) {
        const char* q = halves[i];
        if (q >= limits[i]) {
            return false; // empty half
        }
        uint64_t value = 0;
        for (; q < limits[i]; ++q) {
            if (*q < '0' || *q > '9') {
                return false;
            }
            value = value * 10 + (uint64_t)(*q - '0');
            if (value > maxima[i]) {
                return false;
            }
        }
    }
    return true;
}

// The decoder splits everything after "ur:" on '/' and DROPS empty components
// (esp32_bc-ur/src/utils.cpp:48), so "ur:bytes//x-3/<body>" reaches the sequence parser as
// exactly "x-3" - identical to "ur:bytes/x-3/<body>".  A gate that walks the raw string with
// strchr() therefore sees a different shape than the decoder does, which is how the first
// version of this check was bypassed (Codex review, 2026-09-03).  Split the way the decoder does.
static bool bcur_sequence_component_ok(const char* bcur)
{
    JADE_ASSERT(bcur);

    const char* p = strchr(bcur, ':');
    if (!p) {
        return true; // no scheme; the decoder rejects it
    }
    ++p;

    const char* starts[4];
    const char* ends[4];
    size_t ncomps = 0;
    while (*p && ncomps < 4) {
        while (*p == '/') {
            ++p; // empty component, exactly as split() skips it
        }
        if (!*p) {
            break;
        }
        starts[ncomps] = p;
        while (*p && *p != '/') {
            ++p;
        }
        ends[ncomps] = p;
        ++ncomps;
    }

    // Components are <type>/[<seq>/]<body>.  A single component after the type is a one-part UR
    // with no sequence at all, and the decoder refuses anything past two on its own.
    if (ncomps != 3) {
        return true;
    }
    return bcur_seq_halves_ok(starts[1], ends[1]);
}

// Parse bcur bip39 data to extract a mnemonic string (space separated words)
// NOTE: only the English wordlist is supported.
// See: https://github.com/BlockchainCommons/Research/blob/master/papers/bcr-2020-006-urtypes.md
bool bcur_parse_bip39_wrapper(
    const char* bcur, const size_t bcur_len, char* mnemonic, const size_t mnemonic_len, size_t* written)
{
    JADE_ASSERT(bcur);
    JADE_ASSERT(mnemonic);
    JADE_ASSERT(mnemonic_len);
    JADE_INIT_OUT_SIZE(written);

    // Decode bcur string
    bool ret = false;
    uint8_t decoder[URDECODER_SIZE];
    urcreate_placement_decoder(decoder, sizeof(decoder));
    if (!bcur_sequence_component_ok(bcur)) {
        JADE_LOGW("Rejecting bcur bip39 string with a malformed sequence component");
        goto cleanup;
    }
    if (!urreceive_part_decoder(decoder, bcur) || !uris_success_decoder(decoder)) {
        JADE_LOGW("Unable to decode bcur bip39 string from single part");
        goto cleanup;
    }

    // Read the result
    char const* type = NULL;
    uint8_t* result = NULL;
    size_t result_len = 0;
    urresult_ur_decoder(decoder, &result, &result_len, &type);
    if (!type || !result || !result_len || strcasecmp(BCUR_TYPE_CRYPTO_BIP39, type)) {
        JADE_LOGW("Unable to decode bcur bip39 string to expected type %s", BCUR_TYPE_CRYPTO_BIP39);
        goto cleanup;
    }

    // Decode the cbor
    if (!bcur_parse_bip39(result, result_len, mnemonic, mnemonic_len, written)) {
        JADE_LOGW("Failed to parse bcur bip39 cbor message");
        goto cleanup;
    }

    // All good
    ret = true;

cleanup:
    urfree_placement_decoder(decoder);
    return ret;
}

// Parse the bcur cbor for raw undifferentiated bytes (bytes) - just bytes.
// NOTE: this returns a pointer to the byte buffer allocated in the existing cbor input
// *AND NOT* a freshly allocated or copied range.
// See: https://github.com/BlockchainCommons/Research/blob/master/papers/bcr-2020-006-urtypes.md
bool bcur_parse_bytes(const uint8_t* cbor, size_t cbor_len, const uint8_t** bytes, size_t* bytes_len)
{
    JADE_ASSERT(cbor);
    JADE_ASSERT(cbor_len);
    JADE_INIT_OUT_PPTR(bytes);
    JADE_INIT_OUT_SIZE(bytes_len);

    // Parse cbor
    CborValue value;
    CborParser parser;
    if (!rpc_untrusted_parser_init(cbor, cbor_len, &parser, &value)) {
        return false;
    }

    rpc_get_raw_bytes_ptr(&value, bytes, bytes_len);
    return *bytes && *bytes_len;
}

// Parse the bcur cbor for a PSBT (crypto-psbt) - just bytes
// See: https://github.com/BlockchainCommons/Research/blob/master/papers/bcr-2020-006-urtypes.md
bool bcur_parse_psbt(const uint8_t* cbor, const size_t cbor_len, struct wally_psbt** psbt_out)
{
    JADE_ASSERT(cbor);
    JADE_ASSERT(cbor_len);
    JADE_INIT_OUT_PPTR(psbt_out);

    // Parse cbor, get pointer to existing bytes
    const uint8_t* data = NULL;
    size_t data_len = 0;
    if (!bcur_parse_bytes(cbor, cbor_len, &data, &data_len)) {
        return false;
    }

    // Convert to wally psbt structure
    if (!deserialise_psbt(data, data_len, psbt_out)) {
        JADE_LOGW("wally_psbt_from_bytes() failed for %u bytes", data_len);
        return false;
    }

    return true;
}

// BBB-AIRGAP: crypto-output / crypto-hdkey tags (bcr-2020-007, bcr-2020-010); the 40xxx values
// are the registered replacements some encoders now emit.  Both forms are accepted.
#define UR_TAG_OUTPUT 308
#define UR_TAG_OUTPUT_NEW 40308
#define UR_TAG_SH 400
#define UR_TAG_WSH 401
#define UR_TAG_PKH 403
#define UR_TAG_WPKH 404
#define UR_TAG_MULTI 406
#define UR_TAG_SORTEDMULTI 407
#define UR_TAG_HDKEY 303
#define UR_TAG_HDKEY_NEW 40303
#define UR_TAG_KEYPATH 304
#define UR_TAG_KEYPATH_NEW 40304
#define UR_TAG_USEINFO 305
#define UR_TAG_USEINFO_NEW 40305

static const char UR_ERR_UNSUPPORTED[] = "Unsupported descriptor";
static const char UR_ERR_INVALID[] = "Invalid descriptor";
static const char UR_ERR_TOO_LARGE[] = "Descriptor too large";
static const char UR_ERR_TOO_MANY[] = "Too many signers";

// If 'value' is tagged with 'a' or 'b', step over the tag
// BBB-AIRGAP: tinycbor's cbor_value_skip_tag() steps over a whole CHAIN of tags in one call
// (managed_components/espressif__cbor/tinycbor/src/cborparser.c:554-562), and these UR types nest
// tags directly - 401(407({...})) is wsh(sortedmulti(...)) - so one tag is stepped over at a time
// with cbor_value_advance_fixed(), which advances exactly one fixed-size item.
static bool skip_one_tag(CborValue* value, CborTag* tag_out)
{
    CborTag tag = 0;
    if (!cbor_value_is_tag(value) || cbor_value_get_tag(value, &tag) != CborNoError) {
        return false;
    }
    if (tag_out) {
        *tag_out = tag;
    }
    return cbor_value_advance_fixed(value) == CborNoError;
}

static bool skip_tag_if(CborValue* value, const CborTag a, const CborTag b)
{
    CborTag tag = 0;
    if (!cbor_value_is_tag(value) || cbor_value_get_tag(value, &tag) != CborNoError) {
        return false;
    }
    if (tag != a && tag != b) {
        return false;
    }
    return cbor_value_advance_fixed(value) == CborNoError;
}

// Step over one complete item, tags included
// BBB-AIRGAP: tinycbor counts a tag as a standalone fixed-size item, so cbor_value_advance() on a
// tagged value lands ON the tagged content rather than after it (advance_recursive() takes the
// is_fixed_type() branch for CborTagType - managed_components/espressif__cbor/tinycbor/src/
// cborparser.c:480-481). Iterating a map whose values carry tags then reads the payload as the
// next key and silently desynchronises, so the tag chain is consumed before the content is.
static bool advance_over_value(CborValue* value)
{
    while (cbor_value_is_tag(value)) {
        if (cbor_value_advance_fixed(value) != CborNoError) {
            return false;
        }
    }
    return cbor_value_advance(value) == CborNoError;
}

// BBB-AIRGAP: BCR-2020-007/010 map keys are unique unsigned integers. A non-integer key,
// duplicate key or unreadable item declares something we cannot read. Validate the whole map
// before searching: map_find_uint_key() stops at its first match and would miss later defects.
// Also returns false when the value is not a map at all, so callers need no separate check.
// Duplicates are tracked with a bitmask over keys below 64: every key this file reads is in
// that range (the registries define 1 to 8), so any duplicate able to hide a declaration is
// caught, while a repeat among keys we never read changes nothing. Comparing each key with all
// of its predecessors instead is quadratic, and a multipart QR can carry thousands of entries.
static bool map_keys_are_unique_uints(const CborValue* map)
{
    CborValue it;
    if (!cbor_value_is_map(map) || cbor_value_enter_container(map, &it) != CborNoError) {
        return false;
    }
    uint64_t seen = 0;
    while (!cbor_value_at_end(&it)) {
        uint64_t key = 0;
        if (!cbor_value_is_unsigned_integer(&it) || cbor_value_get_uint64(&it, &key) != CborNoError
            || cbor_value_advance(&it) != CborNoError || cbor_value_at_end(&it)) {
            return false;
        }
        if (key < 64) {
            const uint64_t bit = (uint64_t)1 << key;
            if (seen & bit) {
                return false;
            }
            seen |= bit;
        }
        if (!advance_over_value(&it)) {
            return false;
        }
    }
    return true;
}

// Locate the value under an unsigned-integer key of a map (rpc_get_* only handles string keys)
static bool map_find_uint_key(const CborValue* map, const uint64_t wanted, CborValue* out)
{
    if (!cbor_value_is_map(map)) {
        return false;
    }
    CborValue it;
    if (cbor_value_enter_container(map, &it) != CborNoError) {
        return false;
    }
    while (!cbor_value_at_end(&it)) {
        uint64_t key = 0;
        if (!cbor_value_is_unsigned_integer(&it) || cbor_value_get_uint64(&it, &key) != CborNoError
            || cbor_value_advance(&it) != CborNoError) {
            return false;
        }
        if (key == wanted) {
            // returned with its tags intact - the caller decides which tag it will accept
            *out = it;
            return true;
        }
        if (!advance_over_value(&it)) {
            return false;
        }
    }
    return false;
}

static bool get_uint32(CborValue* value, uint32_t* out)
{
    uint64_t v = 0;
    if (!cbor_value_is_unsigned_integer(value) || cbor_value_get_uint64(value, &v) != CborNoError || v > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

// Origin components '[n, bool, n, bool, ...]' -> '48h/0h/0h/2h'; count and the last child number
static bool origin_components_to_path(
    const CborValue* array, char* path, const size_t path_len, size_t* count, uint32_t* last_child)
{
    JADE_ASSERT(path_len >= MAX_PATH_STR_LEN(MAX_PATH_LEN));
    *count = 0;
    *last_child = 0;
    path[0] = '\0';
    if (!cbor_value_is_array(array)) {
        return false;
    }
    CborValue it;
    if (cbor_value_enter_container(array, &it) != CborNoError) {
        return false;
    }
    size_t written = 0;
    while (!cbor_value_at_end(&it)) {
        uint32_t index = 0;
        bool hardened = false;
        if (*count >= MAX_PATH_LEN || !get_uint32(&it, &index) || index > BIP32_MAX_CHILD_INDEX
            || cbor_value_advance(&it) != CborNoError || !cbor_value_is_boolean(&it)
            || cbor_value_get_boolean(&it, &hardened) != CborNoError || cbor_value_advance(&it) != CborNoError) {
            return false; // a wildcard, pair or range in an origin is not a key origin
        }
        const int n = snprintf(
            path + written, path_len - written, "%s%" PRIu32 "%s", *count ? "/" : "", index, hardened ? "h" : "");
        if (n <= 0 || (size_t)n >= path_len - written) {
            return false;
        }
        written += n;
        *last_child = hardened ? harden(index) : index;
        ++*count;
    }
    return true;
}

// Children '[[a, false, b, false], [], false]' -> '/<a;b>/*'; anything else is unsupported (the
// record needs a receive and a change branch, main/process/register_descriptor.c)
static bool children_to_child_path(const CborValue* array, char* child, const size_t child_len)
{
    if (!cbor_value_is_array(array)) {
        return false;
    }
    CborValue it;
    if (cbor_value_enter_container(array, &it) != CborNoError || !cbor_value_is_array(&it)) {
        return false;
    }
    size_t pair_len = 0;
    if (cbor_value_get_array_length(&it, &pair_len) != CborNoError || pair_len != 4) {
        return false;
    }
    CborValue pair;
    if (cbor_value_enter_container(&it, &pair) != CborNoError) {
        return false;
    }
    uint32_t a = 0;
    uint32_t b = 0;
    bool ha = true;
    bool hb = true;
    if (!get_uint32(&pair, &a) || cbor_value_advance(&pair) != CborNoError || !cbor_value_is_boolean(&pair)
        || cbor_value_get_boolean(&pair, &ha) != CborNoError || cbor_value_advance(&pair) != CborNoError
        || !get_uint32(&pair, &b) || cbor_value_advance(&pair) != CborNoError || !cbor_value_is_boolean(&pair)
        || cbor_value_get_boolean(&pair, &hb) != CborNoError || ha || hb
        // Unhardened by flag is not enough: an index above the unhardened range would be written
        // out as a plain number and read back as a hardened child.  origin_components_to_path()
        // above already refuses that; this branch had the same gap.
        || a > BIP32_MAX_CHILD_INDEX || b > BIP32_MAX_CHILD_INDEX) {
        return false;
    }
    // Then the wildcard: an empty array followed by 'false'
    size_t wild_len = 1;
    bool wild_hardened = true;
    if (cbor_value_advance(&it) != CborNoError || !cbor_value_is_array(&it)
        || cbor_value_get_array_length(&it, &wild_len) != CborNoError || wild_len
        || cbor_value_advance(&it) != CborNoError || !cbor_value_is_boolean(&it)
        || cbor_value_get_boolean(&it, &wild_hardened) != CborNoError || wild_hardened
        || cbor_value_advance(&it) != CborNoError || !cbor_value_at_end(&it)) {
        return false;
    }
    const int n = snprintf(child, child_len, "/<%" PRIu32 ";%" PRIu32 ">/*", a, b);
    return n > 0 && (size_t)n < child_len;
}

// One crypto-hdkey -> '[fp/path]xpub/<a;b>/*'
static bool hdkey_to_text(CborValue* value, char* out, const size_t out_len, size_t* written, const char** errmsg)
{
    *written = 0;
    skip_tag_if(value, UR_TAG_HDKEY, UR_TAG_HDKEY_NEW); // the tag is optional inside an output
    if (!map_keys_are_unique_uints(value)) {
        *errmsg = UR_ERR_UNSUPPORTED;
        return false;
    }

    CborValue item;
    bool is_private = false;
    if (map_find_uint_key(value, 2, &item)
        && (!cbor_value_is_boolean(&item) || cbor_value_get_boolean(&item, &is_private) != CborNoError || is_private)) {
        *errmsg = UR_ERR_UNSUPPORTED;
        return false;
    }

    uint8_t key[EC_PUBLIC_KEY_LEN];
    uint8_t chain_code[WALLY_BIP32_CHAIN_CODE_LEN];
    size_t len = sizeof(key);
    if (!map_find_uint_key(value, 3, &item) || !cbor_value_is_byte_string(&item)
        || cbor_value_copy_byte_string(&item, key, &len, NULL) != CborNoError || len != sizeof(key)) {
        *errmsg = UR_ERR_INVALID;
        return false;
    }
    len = sizeof(chain_code);
    if (!map_find_uint_key(value, 4, &item) || !cbor_value_is_byte_string(&item)
        || cbor_value_copy_byte_string(&item, chain_code, &len, NULL) != CborNoError || len != sizeof(chain_code)) {
        *errmsg = UR_ERR_INVALID;
        return false;
    }

    // use-info (crypto-coin-info, BCR-2020-007): {1: coin type, 2: network}, both defaulting to 0
    // (Bitcoin, mainnet).  Absent entirely means the same defaults.  Every part of the value is
    // consumed on purpose: this is what decides whether the raw public key below becomes an xpub or
    // a tpub, so anything we cannot read has to be refused rather than silently treated as Bitcoin
    // mainnet.  Both registry tags are accepted (305 is the CBOR-tag draft, 40305 the registered
    // one); an unknown tag leaves a tagged value here, which is not a map, and is refused below.
    // Testnet keeps coin type 0 in this spec and moves to network 1, so requiring type 0 does not
    // shut out testnet.
    uint32_t version = BIP32_VER_MAIN_PUBLIC;
    if (map_find_uint_key(value, 5, &item)) {
        skip_tag_if(&item, UR_TAG_USEINFO, UR_TAG_USEINFO_NEW);
        if (!map_keys_are_unique_uints(&item)) {
            *errmsg = UR_ERR_UNSUPPORTED;
            return false;
        }
        CborValue field;
        uint32_t coin_type = 0;
        if (map_find_uint_key(&item, 1, &field) && (!get_uint32(&field, &coin_type) || coin_type)) {
            *errmsg = UR_ERR_UNSUPPORTED; // a declared non-Bitcoin coin
            return false;
        }
        uint32_t net = 0;
        if (map_find_uint_key(&item, 2, &field)) {
            if (!get_uint32(&field, &net) || net > 1) {
                *errmsg = UR_ERR_UNSUPPORTED;
                return false;
            }
            version = net ? BIP32_VER_TEST_PUBLIC : BIP32_VER_MAIN_PUBLIC;
        }
    }

    // origin (required: register_descriptor() needs a key origin for every signer)
    char path[MAX_PATH_STR_LEN(MAX_PATH_LEN)];
    size_t depth = 0;
    uint32_t child_num = 0;
    uint32_t fingerprint = 0;
    CborValue origin;
    if (!map_find_uint_key(value, 6, &origin)) {
        *errmsg = UR_ERR_UNSUPPORTED;
        return false;
    }
    skip_tag_if(&origin, UR_TAG_KEYPATH, UR_TAG_KEYPATH_NEW);
    if (!map_keys_are_unique_uints(&origin)) {
        *errmsg = UR_ERR_UNSUPPORTED;
        return false;
    }
    if (!map_find_uint_key(&origin, 1, &item) || !origin_components_to_path(&item, path, sizeof(path), &depth, &child_num)
        || !map_find_uint_key(&origin, 2, &item) || !get_uint32(&item, &fingerprint)) {
        *errmsg = UR_ERR_UNSUPPORTED;
        return false;
    }
    if (map_find_uint_key(&origin, 3, &item)) {
        uint32_t declared_depth = 0;
        if (!get_uint32(&item, &declared_depth) || declared_depth > 255) {
            *errmsg = UR_ERR_INVALID;
            return false;
        }
        depth = declared_depth;
    }

    uint32_t parent_fingerprint = 0;
    if (map_find_uint_key(value, 8, &item) && !get_uint32(&item, &parent_fingerprint)) {
        *errmsg = UR_ERR_INVALID;
        return false;
    }

    // children: absent -> '<0;1>/*'
    char child[32] = "/<0;1>/*";
    if (map_find_uint_key(value, 7, &item)) {
        skip_tag_if(&item, UR_TAG_KEYPATH, UR_TAG_KEYPATH_NEW);
        if (!map_keys_are_unique_uints(&item)) {
            *errmsg = UR_ERR_UNSUPPORTED;
            return false;
        }
        CborValue components;
        if (!map_find_uint_key(&item, 1, &components) || !children_to_child_path(&components, child, sizeof(child))) {
            *errmsg = UR_ERR_UNSUPPORTED;
            return false;
        }
    }

    // Rebuild the xpub
    uint8_t parent160[BIP32_KEY_FINGERPRINT_LEN];
    uint32_to_be(parent_fingerprint, parent160);
    struct ext_key hdkey;
    if (bip32_key_init(version, depth, depth ? child_num : 0, chain_code, sizeof(chain_code), key, sizeof(key), NULL,
            0, NULL, 0, parent160, sizeof(parent160), &hdkey)
        != WALLY_OK) {
        *errmsg = UR_ERR_INVALID;
        return false;
    }
    char* xpub = NULL;
    if (bip32_key_to_base58(&hdkey, BIP32_FLAG_KEY_PUBLIC, &xpub) != WALLY_OK || !xpub) {
        *errmsg = UR_ERR_INVALID;
        return false;
    }
    // BBB-AIRGAP: an empty origin path must be '[fp]'; wally rejects the dangling slash in '[fp/]'.
    const int n = path[0] ? snprintf(out, out_len, "[%08" PRIx32 "/%s]%s%s", fingerprint, path, xpub, child)
                          : snprintf(out, out_len, "[%08" PRIx32 "]%s%s", fingerprint, xpub, child);
    JADE_WALLY_VERIFY(wally_free_string(xpub));
    if (n <= 0 || (size_t)n >= out_len) {
        *errmsg = UR_ERR_TOO_LARGE;
        return false;
    }
    *written = n;
    return true;
}

#define APPEND_TEXT(str)                                                                                               \
    do {                                                                                                               \
        const size_t slen = strlen(str);                                                                               \
        if (written + slen >= text_len) {                                                                              \
            *errmsg = UR_ERR_TOO_LARGE;                                                                                \
            return false;                                                                                              \
        }                                                                                                              \
        memcpy(text + written, str, slen + 1);                                                                         \
        written += slen;                                                                                               \
    } while (false)

bool bcur_parse_crypto_output(
    const uint8_t* cbor, const size_t cbor_len, char* text, const size_t text_len, const char** errmsg)
{
    JADE_ASSERT(cbor);
    JADE_ASSERT(cbor_len);
    JADE_ASSERT(text);
    JADE_ASSERT(text_len > MAX_DESCRIPTOR_SCRIPT_LEN);
    JADE_INIT_OUT_PPTR(errmsg);
    text[0] = '\0';
    size_t written = 0;

    CborValue value;
    CborParser parser;
    if (!rpc_untrusted_parser_init(cbor, cbor_len, &parser, &value)) {
        *errmsg = UR_ERR_INVALID;
        return false;
    }

    skip_tag_if(&value, UR_TAG_OUTPUT, UR_TAG_OUTPUT_NEW); // optional outermost tag
    size_t closers = 0;
    if (skip_tag_if(&value, UR_TAG_SH, UR_TAG_SH)) {
        APPEND_TEXT("sh(");
        ++closers;
    }

    CborTag tag = 0;
    if (!skip_one_tag(&value, &tag)) {
        *errmsg = UR_ERR_UNSUPPORTED;
        return false;
    }

    if (tag == UR_TAG_WSH) {
        APPEND_TEXT("wsh(");
        ++closers;
        CborTag multi_tag = 0;
        if (!skip_one_tag(&value, &multi_tag) || (multi_tag != UR_TAG_MULTI && multi_tag != UR_TAG_SORTEDMULTI)) {
            *errmsg = UR_ERR_UNSUPPORTED;
            return false;
        }
        if (!map_keys_are_unique_uints(&value)) {
            *errmsg = UR_ERR_UNSUPPORTED;
            return false;
        }
        CborValue item;
        uint32_t threshold = 0;
        if (!map_find_uint_key(&value, 1, &item) || !get_uint32(&item, &threshold) || !threshold) {
            *errmsg = UR_ERR_INVALID;
            return false;
        }
        CborValue keys;
        size_t num_keys = 0;
        if (!map_find_uint_key(&value, 2, &keys) || !cbor_value_is_array(&keys)
            || cbor_value_get_array_length(&keys, &num_keys) != CborNoError || !num_keys) {
            *errmsg = UR_ERR_INVALID;
            return false;
        }
        if (num_keys > MAX_ALLOWED_SIGNERS) {
            *errmsg = UR_ERR_TOO_MANY;
            return false;
        }
        if (threshold > num_keys) {
            *errmsg = UR_ERR_INVALID;
            return false;
        }
        char head[24];
        snprintf(
            head, sizeof(head), "%s(%" PRIu32, multi_tag == UR_TAG_SORTEDMULTI ? "sortedmulti" : "multi", threshold);
        APPEND_TEXT(head);
        ++closers;

        CborValue key;
        if (cbor_value_enter_container(&keys, &key) != CborNoError) {
            *errmsg = UR_ERR_INVALID;
            return false;
        }
        for (size_t i = 0; i < num_keys; ++i) {
            APPEND_TEXT(",");
            size_t key_written = 0;
            if (!hdkey_to_text(&key, text + written, text_len - written, &key_written, errmsg)) {
                return false;
            }
            written += key_written;
            if (!advance_over_value(&key)) {
                *errmsg = UR_ERR_INVALID;
                return false;
            }
        }
    } else if (tag == UR_TAG_WPKH || tag == UR_TAG_PKH) {
        APPEND_TEXT(tag == UR_TAG_WPKH ? "wpkh(" : "pkh(");
        ++closers;
        size_t key_written = 0;
        if (!hdkey_to_text(&value, text + written, text_len - written, &key_written, errmsg)) {
            return false;
        }
        written += key_written;
    } else {
        *errmsg = UR_ERR_UNSUPPORTED;
        return false;
    }

    while (closers--) {
        APPEND_TEXT(")");
    }
    return true;
}

// Helper to initiate parsing a Jade message
bool bcur_parse_jade_message(const uint8_t* cbor, size_t cbor_len, CborParser* parser, CborValue* root,
    const char* expected_method, CborValue* params)
{
    JADE_ASSERT(cbor);
    JADE_ASSERT(cbor_len);
    JADE_ASSERT(parser);
    JADE_ASSERT(root);
    // expected_method is optional
    // params is optional

    // Parse cbor
    if (!rpc_untrusted_parser_init(cbor, cbor_len, parser, root) || !cbor_value_is_map(root)) {
        JADE_LOGE("Failed to parse bcur cbor message");
        return false;
    }

    // Caller can optionally pass the expected method name - if so this is verified.
    if (expected_method) {
        size_t method_len = 0;
        const char* method = NULL;
        rpc_get_method(root, &method, &method_len);
        if (!method || !method_len || strncmp(expected_method, method, method_len)
            || method_len != strlen(expected_method)) {
            JADE_LOGE("Failed to read expected method name");
            return false;
        }
    }

    // If caller also wants params, the params map must be present
    // If caller hasn't asked for params, they are allowed to not be present.
    if (params) {
        const CborError cberr = cbor_value_map_find_value(root, CBOR_RPC_TAG_PARAMS, params);
        if (cberr != CborNoError || !cbor_value_is_valid(params) || cbor_value_get_type(params) == CborInvalidType
            || !cbor_value_is_map(params)) {
            JADE_LOGE("Failed to fetch parameters map");
            return false;
        }
    }
    return true;
}

// Encode a txn psbt as a bcur cbor 'bytes' - just bytes
// See: https://github.com/BlockchainCommons/Research/blob/master/papers/bcr-2020-006-urtypes.md
bool bcur_build_cbor_bytes(const uint8_t* data, const size_t data_len, uint8_t** output, size_t* output_len)
{
    JADE_ASSERT(data);
    JADE_ASSERT(data_len);
    JADE_INIT_OUT_PPTR(output);
    JADE_INIT_OUT_SIZE(output_len);

    // Format as simple cbor message containing only bytes
    const size_t buflen = data_len + 8; // sufficient for cbor overhead
    uint8_t* buf = JADE_MALLOC_PREFER_SPIRAM(buflen);
    CborEncoder root_encoder;
    cbor_encoder_init(&root_encoder, buf, buflen, 0);
    const CborError cberr = cbor_encode_byte_string(&root_encoder, data, data_len);
    JADE_ASSERT(cberr == CborNoError);

    const size_t cbor_len = cbor_encoder_get_buffer_size(&root_encoder, buf);
    JADE_ASSERT(cbor_len > data_len && cbor_len <= buflen);

    // Copy cbor buffer to output
    *output = buf;
    *output_len = cbor_len;
    return true;
}

// Encode a txn psbt as a bcur cbor 'crypto-psbt' - just bytes
// See: https://github.com/BlockchainCommons/Research/blob/master/papers/bcr-2020-006-urtypes.md
bool bcur_build_cbor_crypto_psbt(const struct wally_psbt* psbt, uint8_t** output, size_t* output_len)
{
    JADE_ASSERT(psbt);
    JADE_INIT_OUT_PPTR(output);
    JADE_INIT_OUT_SIZE(output_len);

    // Serialise updated psbt
    uint8_t* psbt_bytes_out = NULL;
    size_t psbt_len_out = 0;
    if (!serialise_psbt(psbt, &psbt_bytes_out, &psbt_len_out)) {
        return false;
    }

    // Format as simple cbor message
    const bool ret = bcur_build_cbor_bytes(psbt_bytes_out, psbt_len_out, output, output_len);
    free(psbt_bytes_out);
    return ret;
}

static void encode_script_variant_tag(CborEncoder* encoder, const script_variant_t script_variant)
{
    JADE_ASSERT(encoder);

    CborError cberr = CborNoError;
    switch (script_variant) {
    // Singlesig
    case P2PKH:
        cberr = cbor_encode_tag(encoder, 403);
        JADE_ASSERT(cberr == CborNoError);
        break;
    case P2WPKH:
        cberr = cbor_encode_tag(encoder, 404);
        JADE_ASSERT(cberr == CborNoError);
        break;
    case P2WPKH_P2SH:
        cberr = cbor_encode_tag(encoder, 400);
        JADE_ASSERT(cberr == CborNoError);
        cberr = cbor_encode_tag(encoder, 404);
        JADE_ASSERT(cberr == CborNoError);
        break;
    // Generic multisig
    case MULTI_P2SH:
        cberr = cbor_encode_tag(encoder, 400);
        JADE_ASSERT(cberr == CborNoError);
        break;
    case MULTI_P2WSH:
        cberr = cbor_encode_tag(encoder, 401);
        JADE_ASSERT(cberr == CborNoError);
        break;
    case MULTI_P2WSH_P2SH:
        cberr = cbor_encode_tag(encoder, 400);
        JADE_ASSERT(cberr == CborNoError);
        cberr = cbor_encode_tag(encoder, 401);
        JADE_ASSERT(cberr == CborNoError);
        break;
    // Taproot
    case P2TR:
        cberr = cbor_encode_tag(encoder, 409);
        JADE_ASSERT(cberr == CborNoError);
        break;
    default:
        JADE_ASSERT_MSG(false, "Unhandled script variant");
    }
}

static void encode_hdkey(CborEncoder* encoder, const uint32_t fingerprint, const uint32_t* path, const size_t path_len)
{
    JADE_ASSERT(encoder);
    JADE_ASSERT(fingerprint);
    JADE_ASSERT(path);
    JADE_ASSERT(path_len);

    // We will include the 'useinfo' section if testnet
    const bool testnet = keychain_get_network_type_restriction() == NETWORK_TYPE_TEST;

    // The hdkey for the passed path
    struct ext_key hdkey;
    const bool ret = wallet_get_hdkey(path, path_len, BIP32_FLAG_KEY_PUBLIC, &hdkey);
    JADE_ASSERT(ret);
    JADE_ASSERT(hdkey.depth == path_len);

    // hdkey
    CborEncoder key_map_encoder;
    CborError cberr = cbor_encoder_create_map(encoder, &key_map_encoder, testnet ? 5 : 4);
    JADE_ASSERT(cberr == CborNoError);

    // pubkey
    cberr = cbor_encode_uint(&key_map_encoder, 3);
    JADE_ASSERT(cberr == CborNoError);
    cberr = cbor_encode_byte_string(&key_map_encoder, hdkey.pub_key, sizeof(hdkey.pub_key));
    JADE_ASSERT(cberr == CborNoError);

    // chaincode
    cberr = cbor_encode_uint(&key_map_encoder, 4);
    JADE_ASSERT(cberr == CborNoError);
    cberr = cbor_encode_byte_string(&key_map_encoder, hdkey.chain_code, sizeof(hdkey.chain_code));
    JADE_ASSERT(cberr == CborNoError);

    // use-info (to indicate testnet wallet)
    if (testnet) {
        cberr = cbor_encode_uint(&key_map_encoder, 5);
        JADE_ASSERT(cberr == CborNoError);
        {
            cbor_encode_tag(&key_map_encoder, 305);
            CborEncoder use_info_map_encoder;
            cberr = cbor_encoder_create_map(&key_map_encoder, &use_info_map_encoder, 2);
            JADE_ASSERT(cberr == CborNoError);

            // type - btc
            cberr = cbor_encode_uint(&use_info_map_encoder, 1);
            JADE_ASSERT(cberr == CborNoError);
            cberr = cbor_encode_uint(&use_info_map_encoder, 0);
            JADE_ASSERT(cberr == CborNoError);

            // network
            cberr = cbor_encode_uint(&use_info_map_encoder, 2);
            JADE_ASSERT(cberr == CborNoError);
            cberr = cbor_encode_uint(&use_info_map_encoder, testnet ? 1 : 0);
            JADE_ASSERT(cberr == CborNoError);

            // Close the use-info map
            cberr = cbor_encoder_close_container(&key_map_encoder, &use_info_map_encoder);
            JADE_ASSERT(cberr == CborNoError);
        }
    }

    // origin information
    cberr = cbor_encode_uint(&key_map_encoder, 6);
    JADE_ASSERT(cberr == CborNoError);
    {
        // key path
        cbor_encode_tag(&key_map_encoder, 304);
        CborEncoder key_path_map_encoder;
        cberr = cbor_encoder_create_map(&key_map_encoder, &key_path_map_encoder, 3);
        JADE_ASSERT(cberr == CborNoError);

        {
            cberr = cbor_encode_uint(&key_path_map_encoder, 1);
            JADE_ASSERT(cberr == CborNoError);
            CborEncoder key_path_array_encoder;
            cberr = cbor_encoder_create_array(&key_path_map_encoder, &key_path_array_encoder, 2 * path_len);
            JADE_ASSERT(cberr == CborNoError);
            for (int i = 0; i < path_len; ++i) {
                cberr = cbor_encode_uint(&key_path_array_encoder, unharden(path[i]));
                JADE_ASSERT(cberr == CborNoError);
                cberr = cbor_encode_boolean(&key_path_array_encoder, ishardened(path[i]));
                JADE_ASSERT(cberr == CborNoError);
            }

            // Close the path array
            cberr = cbor_encoder_close_container(&key_path_map_encoder, &key_path_array_encoder);
            JADE_ASSERT(cberr == CborNoError);
        }

        // origin fingerprint - ie. master key fingerprint
        cberr = cbor_encode_uint(&key_path_map_encoder, 2);
        JADE_ASSERT(cberr == CborNoError);
        cberr = cbor_encode_uint(&key_path_map_encoder, fingerprint);
        JADE_ASSERT(cberr == CborNoError);

        // path length / depth
        cberr = cbor_encode_uint(&key_path_map_encoder, 3);
        JADE_ASSERT(cberr == CborNoError);
        cberr = cbor_encode_uint(&key_path_map_encoder, hdkey.depth);
        JADE_ASSERT(cberr == CborNoError);

        // Close the path map
        cberr = cbor_encoder_close_container(&key_map_encoder, &key_path_map_encoder);
        JADE_ASSERT(cberr == CborNoError);
    }

    // parent fingerprint - immediate parent
    uint32_t parentfp = 0;
    uint32_to_be(*(uint32_t*)hdkey.parent160, (uint8_t*)(&parentfp));

    cberr = cbor_encode_uint(&key_map_encoder, 8);
    JADE_ASSERT(cberr == CborNoError);
    cberr = cbor_encode_uint(&key_map_encoder, parentfp);
    JADE_ASSERT(cberr == CborNoError);

    // Close the key map
    cberr = cbor_encoder_close_container(encoder, &key_map_encoder);
    JADE_ASSERT(cberr == CborNoError);
}

// Encode an wallet path/key as a bcur cbor 'crypto-hdkey'
// See: https://github.com/BlockchainCommons/Research/blob/master/papers/bcr-2020-007-hdkey.md
void bcur_build_cbor_crypto_hdkey(
    const uint32_t* path, const size_t path_len, uint8_t* output, const size_t output_len, size_t* written)
{
    JADE_ASSERT(path);
    JADE_ASSERT(path_len);
    JADE_ASSERT(output);
    JADE_ASSERT(output_len >= 128);
    JADE_INIT_OUT_SIZE(written);

    // Wallet fingerprint
    uint8_t fingerprint_bytes[BIP32_KEY_FINGERPRINT_LEN];
    wallet_get_fingerprint(fingerprint_bytes, sizeof(fingerprint_bytes));
    uint32_t fingerprint = 0;
    uint32_to_be(*(uint32_t*)fingerprint_bytes, (uint8_t*)(&fingerprint));

    CborEncoder root_encoder;
    cbor_encoder_init(&root_encoder, output, output_len, 0);

    // hdkey
    encode_hdkey(&root_encoder, fingerprint, path, path_len);

    *written = cbor_encoder_get_buffer_size(&root_encoder, output);
    JADE_ASSERT(*written);
}

// Encode an wallet path/key as a bcur cbor 'crypto-account'
// See: https://github.com/BlockchainCommons/Research/blob/master/papers/bcr-2020-015-account.md
void bcur_build_cbor_crypto_account(const script_variant_t script_variant, const uint32_t* path, const size_t path_len,
    uint8_t* output, const size_t output_len, size_t* written)
{
    JADE_ASSERT(path);
    JADE_ASSERT(path_len);
    JADE_ASSERT(output);
    JADE_ASSERT(output_len >= 128);
    JADE_INIT_OUT_SIZE(written);

    // Green multisig-shield not supported
    JADE_ASSERT(!is_greenaddress(script_variant));

    // Wallet fingerprint
    uint8_t fingerprint_bytes[BIP32_KEY_FINGERPRINT_LEN];
    wallet_get_fingerprint(fingerprint_bytes, sizeof(fingerprint_bytes));
    uint32_t fingerprint = 0;
    uint32_to_be(*(uint32_t*)fingerprint_bytes, (uint8_t*)(&fingerprint));

    CborEncoder root_encoder;
    cbor_encoder_init(&root_encoder, output, output_len, 0);
    {
        // Fingerprint and list of output descriptors
        CborEncoder root_map_encoder;
        CborError cberr = cbor_encoder_create_map(&root_encoder, &root_map_encoder, 2);
        JADE_ASSERT(cberr == CborNoError);

        // fingerprint - immediate parent, or root parent of the path given ?
        cberr = cbor_encode_uint(&root_map_encoder, 1);
        JADE_ASSERT(cberr == CborNoError);
        cberr = cbor_encode_uint(&root_map_encoder, fingerprint);
        JADE_ASSERT(cberr == CborNoError);

        cberr = cbor_encode_uint(&root_map_encoder, 2);
        JADE_ASSERT(cberr == CborNoError);
        {
            // Just one output descriptor
            CborEncoder key_array_encoder;
            cberr = cbor_encoder_create_array(&root_map_encoder, &key_array_encoder, 1);
            JADE_ASSERT(cberr == CborNoError);

            // script-type tag(s)
            encode_script_variant_tag(&key_array_encoder, script_variant);

            // Single hdkey
            cbor_encode_tag(&key_array_encoder, 303);
            encode_hdkey(&key_array_encoder, fingerprint, path, path_len);

            // Close the array
            cberr = cbor_encoder_close_container(&root_map_encoder, &key_array_encoder);
            JADE_ASSERT(cberr == CborNoError);
        }

        // Close the root map
        cberr = cbor_encoder_close_container(&root_encoder, &root_map_encoder);
        JADE_ASSERT(cberr == CborNoError);
    }
    *written = cbor_encoder_get_buffer_size(&root_encoder, output);
    JADE_ASSERT(*written);
}

// Support scanning a bc-ur qr-code - single-frame or animated/multi-frame.
// Adds a scanned bc-ur qr the bcur decoder - only returns true when the decoder is complete.
// ie. collates multiple frames until the entire bc-ur data is complete.
// If the qr-code scanned is not a bc-ur part, return success immediately.
// Updates associated progress-bar as parts are scanned.
static bool collect_any_bcur(qr_data_t* qr_data)
{
    JADE_ASSERT(qr_data);
    JADE_ASSERT(qr_data->len);
    JADE_ASSERT(qr_data->ctx);
    JADE_ASSERT(qr_data->progress_bar);
    JADE_ASSERT(qr_data->data[qr_data->len] == '\0');

    if (qr_data->len < sizeof(BCUR_PREFIX)
        || strncasecmp((const char*)qr_data->data, BCUR_PREFIX, sizeof(BCUR_PREFIX) - 1)) {
        // Not bc-ur - return immediately
        update_progress_bar(qr_data->progress_bar, 1, 1);
        return true;
    }

    // The scanned data looks like a bcur code or fragment, add it to the bcur decoder
    // and return true only when the bcur decoder says the message is complete.
    if (!bcur_sequence_component_ok((const char*)qr_data->data)) {
        JADE_LOGW("Rejecting bcur part with a malformed sequence component");
        return false;
    }
    const bool processed_part = urreceive_part_decoder(qr_data->ctx, (const char*)qr_data->data);

    // On hard failure, reset the decoder
    if (uris_failure_decoder(qr_data->ctx)) {
        JADE_LOGE("Failure to scan bcur data - resetting the decoder");
        urfree_placement_decoder(qr_data->ctx);
        urcreate_placement_decoder(qr_data->ctx, URDECODER_SIZE);
        return false;
    }

    // Update associated progress bar - be a bit defensive here
    const bool decoded = uris_success_decoder(qr_data->ctx);
    const size_t nreceived = urreceived_parts_count_decoder(qr_data->ctx);
    if (processed_part && nreceived) {
        // NOTE: can only call 'expected' once we have received at least one part
        const size_t nexpected = urexpected_part_count_decoder(qr_data->ctx);

        // If fully decoded show full bar - but if not fully decoded
        // don't show a full bar - pause at 'almost done' if required.
        if (decoded) {
            update_progress_bar(qr_data->progress_bar, nexpected, nexpected);
        } else if (nreceived < nexpected) {
            update_progress_bar(qr_data->progress_bar, nexpected, nreceived);
        }
        // else, appear to have all pieces but not fully decoded ...
        // just leave progress bar showing whatever 'almost done' level.
    }

    // Return true if complete
    return decoded;
}

bool bcur_scan_qr(const char* prompt_text, char** output_type, uint8_t** output, size_t* output_len, size_t offset,
    const char* help_url)
{
    // prompt_text is optional
    JADE_INIT_OUT_PPTR(output_type);
    JADE_INIT_OUT_PPTR(output);
    JADE_INIT_OUT_SIZE(output_len);

    uint8_t urdecoder[URDECODER_SIZE];
    urcreate_placement_decoder(urdecoder, sizeof(urdecoder));
    progress_bar_t progress_bar = {};
    qr_data_t qr_data = { .len = 0, .is_valid = collect_any_bcur, .ctx = urdecoder, .progress_bar = &progress_bar };

    // Scan qr code using the bcur decoder to collate multiple frames if required
    if (!jade_camera_scan_qr(&qr_data, prompt_text, QR_GUIDE_SHOW, help_url)) {
        // User exited without completing scanning
        urfree_placement_decoder(urdecoder);
        return false;
    }

    // Copy output into output params - caller takes ownership
    if (uris_success_decoder(urdecoder)) {
        // bcur message scanned - extract from decoder and return the payload
        uint8_t* result = NULL;
        size_t result_len = 0;
        const char* result_type = NULL;
        urresult_ur_decoder(urdecoder, &result, &result_len, &result_type);
        JADE_ASSERT(result);
        JADE_ASSERT(result_len);
        JADE_ASSERT(result_type);

        // Copy payload and bc-ur type.
        // BBB-AIRGAP: allocate one extra byte and nul-terminate, as the non-bc-ur branch below
        // already does.  handle_qr_bytes() (main/qrmode.c) reads strbytes[bytes_len] to decide
        // whether the payload is a string, and urresult_ur_decoder() hands back a pointer into
        // the decoded CBOR buffer rather than a fresh copy (see bcur_parse_bytes() above), so
        // without the extra byte that read runs one past the allocation whenever the byte string
        // is the last element.  Both branches now honour the same contract.
        *output = JADE_MALLOC_PREFER_SPIRAM(result_len + offset + 1);
        memcpy(*output + offset, result, result_len);
        (*output)[result_len + offset] = '\0';
        *output_len = result_len + offset;
        *output_type = strdup(result_type);
    } else {
        // Not a bc-ur code - copy straight payload and append a nul-terminator.
        // Leave bc-ur type as NULL to indicate data was not a bc-ur payload.
        *output = JADE_MALLOC(qr_data.len + offset + 1);
        memcpy(*output + offset, qr_data.data, qr_data.len);
        (*output)[qr_data.len + offset] = '\0';
        *output_len = qr_data.len + offset;
        *output_type = NULL;
    }

    // Free the decoder and return true (as we scanned data successfully)
    urfree_placement_decoder(urdecoder);
    return true;
}

// BBB-AIRGAP: a UR type whose own name contains 'I' puts an 'I' in every part it produces, so the
// 'I'-filter in bcur_create_qr_icons() below could never be satisfied for it. Checked on the type
// string rather than a list of names, so a new type cannot silently fall into that trap.
static bool bcur_type_contains_i(const char* bcur_type)
{
    JADE_ASSERT(bcur_type);
    return strchr(bcur_type, 'I') || strchr(bcur_type, 'i');
}

// Encodes the passed payload into a set of one or more BC-UR fragments with the given 'type'.
// These are then rendered as a set of QR codes of the passed version/size.
// NOTE: input is expected to be a valid CBOR message, although this is not validated
// Caller takes ownership of the icons returned.
// NOTE Only supports qr-versions from 4 to 12  (4, 6 and 12 fit nicely on a v1 Jade screen, and
// 4, 6 and 9 on the larger v2 screen (with greater scaling)).
void bcur_create_qr_icons(const uint8_t* payload, const size_t len, const char* bcur_type, const uint8_t qr_version,
    Icon** icons, size_t* num_icons)
{
    JADE_ASSERT(payload);
    JADE_ASSERT(len);
    JADE_ASSERT(bcur_type);
    JADE_ASSERT(qr_version >= 3); // BBB-AIRGAP: 3 for xpubs, see BCUR_FRAGMENT_SIZE_V3
    // BBB-AIRGAP: the ceiling is the encoder's, and the two tables above are indexed by the same
    // version, so their length is tied to it here rather than left to agree by hand.
    JADE_STATIC_ASSERT(
        sizeof(QR_ALPHANUMERIC_CAPACITY) / sizeof(QR_ALPHANUMERIC_CAPACITY[0]) == QRCODE_MAX_VERSION + 1);
    JADE_STATIC_ASSERT(sizeof(QR_SCALE_FACTOR) / sizeof(QR_SCALE_FACTOR[0]) == QRCODE_MAX_VERSION + 1);
    JADE_ASSERT(qr_version <= QRCODE_MAX_VERSION);
    JADE_INIT_OUT_PPTR(icons);
    JADE_INIT_OUT_SIZE(num_icons);

    // The 'bcur_max_fragment_size' passed into the encoder is the number of payload bytes to aim to put into
    // each fragment. The final fragment is much larger as also contains metadata/checksums, is then encoded
    // into ascii using 'codewords' (which is two characters per byte), and then has a text header - but it is
    // this final size that must respect the QR code capacity. The 'BCUR_MAX_FRAGMENT_SIZE()' macro should yield
    // the maximum amount of payload data for each fragment, and there are asserts to check - but if tweaking the
    // qrcode version we need to check the passed 'capacity' produces fragments <= 'qrcode_alphanumeric_capacity'
    // as the qrcode.c library is not very robust if too much data is passed to 'qrcode_initText()'.
    const uint16_t qrcode_alphanumeric_capacity = QR_ALPHANUMERIC_CAPACITY[qr_version];
    const uint16_t bcur_max_fragment_size = qr_version == 3
        ? BCUR_FRAGMENT_SIZE_V3
        : BCUR_MAX_FRAGMENT_SIZE(qrcode_alphanumeric_capacity, bcur_type);
    JADE_ASSERT(bcur_max_fragment_size < qrcode_alphanumeric_capacity); // didn't 'under'flow

    // Encode the message as bc-ur
    JADE_LOGI("BC-UR encoding payload length %u as type %s", len, bcur_type);
    JADE_LOGI("Targetting qr-code version %u, capacity %u (alphanumeric mode), using max fragment size %u", qr_version,
        qrcode_alphanumeric_capacity, bcur_max_fragment_size);
    uint8_t encoder[URENCODER_SIZE];
    urcreate_placement_encoder(encoder, sizeof(encoder), bcur_type, payload, len, bcur_max_fragment_size, 0, 8);
    const size_t min_num_fragments = urseqlen_encoder(encoder); // the number of 'pure' data fragments
    const size_t num_fragments = BCUR_NUM_FRAGMENTS(min_num_fragments); // add some fountain-code fragments
    JADE_ASSERT(num_fragments >= min_num_fragments);
    JADE_LOGI("Encoded payload length %u as %u pure fragments and %u fountain-code fragments", len, min_num_fragments,
        num_fragments - min_num_fragments);

    // Underlying qrcode data/work area - opaque
    uint8_t* qrbuffer = JADE_MALLOC(qrcode_getBufferSize(qr_version));

    // Convert to qr-code icons
    const uint32_t scale_factor = qr_fullscreen_scale_factor(qr_version);
    const bool force_uppercase = true; // fetch bcur fragment as uppercase to conform to 'alphanumeric' qr mode

    // BBB-AIRGAP: the icon set is displayed as a fixed loop, so it has to be sufficient on its own.
    // Upstream's 4n/3 covers a scanner that randomly misses a frame, but not a scanner that rejects
    // the same parts on every pass: Sparrow's hummingbird calls toLowerCase() without a locale, so
    // under a Turkish (or Azeri) locale 'I' maps to dotless 'i', every part containing 'I' fails its
    // checksum, and the loop stalls forever (measured: 57.1%, 1407 InvalidChecksumExceptions in
    // sparrow.log). SeedSigner survives the identical bug only because it streams fresh fountain
    // parts indefinitely rather than looping a fixed set - and only for payloads where an 'I'-free
    // part exists at all; where the invariant header itself carries one, streaming does not save it
    // either (see the payload class described further down).
    //
    // Raising the multiplier does not fix this - rejection is deterministic per payload, so any
    // multiplier is a guess that some payload defeats. Two rules make the set sufficient by
    // construction instead:
    //   1. a part containing 'I' is never displayed, and never fed to the decoder below either;
    //   2. displayed parts are fed to a decoder here, and generation continues until that decoder
    //      reports success, so the set is proven to decode before it reaches the screen.
    // Upstream's 4n/3 is then kept on top as the scanner-miss margin it was written for.
    //
    // The invariant these two rules hold: every part the verification decoder sees is a part the
    // screen paints, every painted part is 'I'-free, and the painted set contains a prefix the
    // decoder proved sufficient. Emitting an unverified set, or letting a rejected part back in,
    // breaks it and recreates the stall.
    //
    // Not every payload can satisfy rule 1, and no generator-side scheme can make it: measured at
    // roughly a fifth of crypto-account payloads at qr version 3, a payload is simply unreachable
    // for a tr_TR Sparrow. It happens two ways. Every multi-part bc-ur part carries the same
    // constant header (seqLen, message length, message checksum), so if that header's bytewords
    // contain 'I' then no part the encoder can ever produce is 'I'-free. Otherwise the header is
    // clean but some fragment combination the decoder must see always encodes an 'I' in its own
    // bytes - at qr version 6 a 115-byte payload is two fragments, ie. the fountain draws from only
    // three distinct fragment mixtures, so if the decoder needs a mixture that is always rejected, no
    // length of this loop recovers it. (Part texts are not limited to three: each carries its own
    // changing sequence number, whose encoding can itself introduce or remove an 'I'.) Neither
    // is a budget the cap could raise its way out of: 2,000,000 pulls keeping 500,000 parts still
    // never verifies.
    // For those payloads a cap trips and this falls back to the plain upstream set: all-or-nothing,
    // never a half-filtered or unverified set. A UR type whose own name contains 'I' (jade-pin,
    // jade-bip8539-reply; not Sparrow-facing flows anyway) never enters the filter at all - no cap
    // trips, the encoder is not restarted, and the emitted set is upstream's from the first pull.
    // The filter is gated to the densities the xpub flow uses. Above them a payload is only two or
    // three fragments, so too few distinct part contents exist for the filter to find a decodable
    // 'I'-free set: measured 96% of payloads fall back at qr version 6, each paying the full pull cap
    // (~2000 pulls, ~168 ms on the linux build, unmeasured on a Pi Zero) before it does. Those flows
    // keep upstream behaviour exactly, and pay nothing for a filter that cannot help them.
    const bool filter_uppercase_i = !bcur_type_contains_i(bcur_type) && qr_version <= BCUR_FILTER_MAX_QR_VERSION;
    const size_t max_num_icons = filter_uppercase_i ? num_fragments + BCUR_FILTER_MAX_EXTRA_ICONS : num_fragments;
    Icon* const qr_icons = JADE_MALLOC(max_num_icons * sizeof(Icon));

    uint8_t vdecoder[URDECODER_SIZE];
    size_t num_icons_made = 0;
    size_t pulls = 0;
    size_t skipped = 0;
    bool filtering = filter_uppercase_i;
    bool degraded = false;

    for (;;) {
        // With filtering off this is upstream's loop exactly: pull num_fragments parts, keep them all.
        const size_t pull_limit = filtering ? BCUR_FILTER_MAX_PULLS : num_fragments;
        const size_t icon_limit = filtering ? max_num_icons : num_fragments;
        bool verified = !filtering;

        // How many icons this attempt must end with. Upstream's num_fragments is the floor; once a
        // filtered set verifies, the target is raised so the miss margin sits on top of the parts the
        // decoder actually needed (see where 'verified' is set below).
        size_t target_num_icons = num_fragments;

        if (filtering) {
            urcreate_placement_decoder(vdecoder, sizeof(vdecoder));
        }

        while (pulls < pull_limit && num_icons_made < icon_limit && !(verified && num_icons_made >= target_num_icons)) {
            char* fragment = NULL;
            urnext_part_encoder(encoder, force_uppercase, &fragment);
            ++pulls;

            const size_t fragment_len = strlen(fragment);

            // Rule 1 - never displayed, and never seen by the verification decoder either. An
            // over-capacity part is dropped the same way: pulling past upstream's fragment count
            // raises the sequence number, and once a part's text outgrows the qr code it cannot be
            // painted at all. Only this path pulls that far, so upstream's assert below still stands
            // as the check on BCUR_MAX_FRAGMENT_SIZE() that it was written to be.
            if (filtering && (strchr(fragment, 'I') || fragment_len > qrcode_alphanumeric_capacity)) {
                urfree_encoded_encoder(fragment);
                ++skipped;
                continue;
            }
            // BBB-AIRGAP: fragment content is wallet data (psbt/xpub/bip85 payload) and the
            // production log can be appended to the unencrypted FAT32 boot partition, so only
            // the diagnostic scalars are logged.
            JADE_LOGI("Fragment %u, making qr-code icon with data (length: %u)", num_icons_made, fragment_len);

            // We assert here that our BCUR_MAX_FRAGMENT_SIZE() macro did not come up
            // with a size that was too large for the qr-code target.
            JADE_ASSERT(fragment_len <= qrcode_alphanumeric_capacity);

            QRCode qrcode;
            const int qret = qrcode_initText(&qrcode, qrbuffer, qr_version, BCUR_QR_ECC, fragment);
            JADE_ASSERT(qret == 0);

            // Rule 2 - the decoder is fed the exact string the screen will show.
            if (!verified) {
                urreceive_part_decoder(vdecoder, fragment);
                verified = uris_success_decoder(vdecoder);
                if (verified) {
                    // The set verified only on this part, so every part so far is one the decoder
                    // needed - stopping here would hand the scanner a set with no slack at all. That
                    // is not the stall the locale rejection causes: the loop repeats indefinitely and
                    // the decoder tolerates duplicates, so a transient miss is healed on a later pass.
                    // It costs a whole extra pass though, which is exactly what upstream's 4n/3 buys
                    // off, so the same third is added here, on top of the parts this set needed.
                    const size_t essential = num_icons_made + 1; // this part is counted just below
                    target_num_icons = essential + ((essential + 2) / 3);
                    if (target_num_icons < num_fragments) {
                        target_num_icons = num_fragments;
                    }
                }
            }
            urfree_encoded_encoder(fragment);

            // Convert fragment to Icon
            qrcode_toIcon(&qrcode, qr_icons + num_icons_made, scale_factor);
            ++num_icons_made;
        }

        if (filtering) {
            urfree_placement_decoder(vdecoder);
        }

        if ((verified && num_icons_made >= num_fragments) || !filtering) {
            // Note the floor here stays num_fragments: hitting the icon ceiling before the raised
            // target is reached still yields a verified set with some margin, which is worth keeping.
            break;
        }

        // Cap hit while filtering: discard the whole attempt - a partly filtered or unverified set is
        // worse than upstream, as it looks fixed while stalling just the same. Aborting here would
        // brick the screen (JADE_ASSERT wraps abort()), so this degrades instead.
        JADE_LOGW("BC-UR: no 'I'-free fragment set within %u pulls (%u kept, %u skipped) - falling back to "
                  "the plain %u-fragment set",
            pulls, num_icons_made, skipped, num_fragments);
        for (size_t i = 0; i < num_icons_made; ++i) {
            free(qr_icons[i].data);
        }
        num_icons_made = 0;
        pulls = 0;
        skipped = 0;
        filtering = false;
        degraded = true;

        // The encoder's fountain state has been consumed, so restart it - the fallback set is then
        // exactly the set an unpatched build would emit.
        urfree_placement_encoder(encoder);
        urcreate_placement_encoder(encoder, sizeof(encoder), bcur_type, payload, len, bcur_max_fragment_size, 0, 8);
    }

    JADE_LOGI("Emitting %u icons (%u pure fragments, %u pulls, %u skipped, filtered: %u, degraded: %u)", num_icons_made,
        min_num_fragments, pulls, skipped, filter_uppercase_i, degraded);
    JADE_ASSERT(num_icons_made >= min_num_fragments);
    JADE_ASSERT(uris_complete_encoder(encoder));

    free(qrbuffer);
    urfree_placement_encoder(encoder);

    // Return the created icons
    *icons = qr_icons;
    *num_icons = num_icons_made;
}

#ifdef CONFIG_DEBUG_MODE
// NOTE: iterative test for the BCUR_MAX_FRAGMENT_SIZE() macro which yields the input
// 'max fragment size' (of payload data) to produce output bcur-encoded fragments close to
// (but not more than!) the desired qr-code capacity.
// NOTE: takes a while to run - qemu recommended! (~5mins on my laptop, on qemu)
bool bcur_check_fragment_sizes(void)
{
    // Test various versions, with various message types, and various payload lengths
    bool overflowed = false;
    const uint8_t versions[] = { 4, 6, 12 }; // the versions & ur-types of interest
    const char* types[] = { BCUR_TYPE_CRYPTO_PSBT, BCUR_TYPE_CRYPTO_ACCOUNT, BCUR_TYPE_CRYPTO_HDKEY, BCUR_TYPE_JADE_PIN,
        BCUR_TYPE_JADE_BIP8539_REPLY };
    uint8_t payload[4096];

    for (uint8_t iver = 0; iver < sizeof(versions); ++iver) {
        const uint8_t ver = versions[iver];
        const size_t capacity = QR_ALPHANUMERIC_CAPACITY[ver];
        JADE_LOGI("Testing version %u, capacity %u", ver, capacity);

        for (uint8_t itype = 0; itype < 4; ++itype) {
            const char* type = types[itype];
            const size_t maxlen = BCUR_MAX_FRAGMENT_SIZE(capacity, type);
            JADE_ASSERT(maxlen < capacity);
            JADE_LOGI("Testing type %s, maxlen %u", type, maxlen);
            size_t max = 0;

            for (size_t len = maxlen - 8; len < sizeof(payload); ++len) {
                uint8_t encoder[URENCODER_SIZE];
                urcreate_placement_encoder(encoder, sizeof(encoder), type, payload, len, maxlen, 0, 8);

                char* fragment = NULL;
                urnext_part_encoder(encoder, true, &fragment);
                JADE_ASSERT(fragment);
                const size_t fraglen = strlen(fragment);
                if (fraglen + 4 > capacity) {
                    // In truth fraglen == capacity is valid, but later parts (and of larger payloads)
                    // are larger as the ascii sequence numbers get longer - eg. '/1-6/' vs. '367-826'
                    // so we as we only test the first part we'll insist on a margin to allow for this.
                    JADE_LOGE("len %u -> %u", len, fraglen);
                    overflowed = true;
                }
                if (fraglen > max) {
                    max = fraglen;
                }
                urfree_encoded_encoder(fragment);
                urfree_placement_encoder(encoder);
            }
            JADE_LOGI("max: %u (of target/limit %u)", max, capacity);
        }
    }
    return !overflowed;
}
#endif
#endif // AMALGAMATED_BUILD

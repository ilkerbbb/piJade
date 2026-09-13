#ifndef AMALGAMATED_BUILD
#include "bbqr.h"
#include "jade_assert.h"
#include "jade_wally_verify.h"
#include "process.h"
#include "qrscan.h"
#include "utils/malloc_ext.h"
#include "utils/util.h"

#include <string.h>

#include <wally_core.h>

#ifndef CONFIG_LIBJADE
// On ESP32 deflate.h is what pulls in the miniz header matching the IDF version in use, and
// tinfl_decompress() with it - both live in ROM.  main/display.c leans on the same include for
// the same reason.  In the libjade build miniz.c is already included by libjade.c ahead of this
// file, and deflate.h is not on that build's include path.
#include <deflate.h>
#endif

// What a single frame can carry.  A frame is capped by QR_MAX_PAYLOAD_LENGTH (main/qrscan.h) and
// base32, at five bits per character, is the densest of the three encodings.
#define BBQR_MAX_FRAME_PAYLOAD (QR_MAX_PAYLOAD_LENGTH - BBQR_HEADER_LEN)
#define BBQR_MAX_FRAME_DECODED ((BBQR_MAX_FRAME_PAYLOAD / 8) * 5)

// Two base36 digits cannot name more parts than this, which is why parse_base36_pair() needs no
// ceiling of its own and mark_part_seen() cannot run off the end of seen[]
_Static_assert(BBQR_MAX_PARTS == (36 * 36) - 1, "BBQr part ceiling no longer matches two base36 digits");
_Static_assert(sizeof(((bbqr_ctx_t*)0)->seen) * 8 >= BBQR_MAX_PARTS, "BBQr seen[] too small for the part ceiling");

static inline bool part_seen(const bbqr_ctx_t* ctx, const size_t index)
{
    return ctx->seen[index / 8] & (1 << (index % 8));
}

static inline void mark_part_seen(bbqr_ctx_t* ctx, const size_t index) { ctx->seen[index / 8] |= (1 << (index % 8)); }

// Drop the collection but keep the scratch buffer, which a restart re-reads immediately
static void collection_reset(bbqr_ctx_t* ctx)
{
    JADE_ASSERT(ctx);

    if (ctx->data) {
        // Wiped for the same reason the scanner wipes its camera and quirc buffers: a scanned
        // frame can carry wallet material, and 'the wallet was closed' has to mean the bytes are
        // gone.  See qr_scanner_destroy() in main/qrscan.c.  wally_bzero() rather than a plain
        // memset for the same reason every other wipe in this fork uses it: a zeroing store whose
        // buffer is freed on the next line is dead, and the optimiser is free to delete it.
        JADE_WALLY_VERIFY(wally_bzero(ctx->data, ctx->data_len));
        free(ctx->data);
    }
    if (ctx->inflated) {
        JADE_WALLY_VERIFY(wally_bzero(ctx->inflated, ctx->inflated_len));
        free(ctx->inflated);
    }

    ctx->data = NULL;
    ctx->data_len = 0;
    ctx->inflated = NULL;
    ctx->inflated_len = 0;
    ctx->block_len = 0;
    ctx->final_len = 0;
    ctx->num_parts = 0;
    ctx->num_seen = 0;
    ctx->encoding = 0;
    ctx->file_type = 0;
    memset(ctx->seen, 0, sizeof(ctx->seen));
}

// Two base36 digits, as int2base36() writes them.  The alphanumeric QR mode BBQr uses carries no
// lowercase, so anything outside 0-9 and A-Z is a malformed header rather than a case variant.
static bool parse_base36_pair(const uint8_t* digits, size_t* value)
{
    JADE_ASSERT(digits);
    JADE_ASSERT(value);

    size_t result = 0;
    for (size_t i = 0; i < 2; ++i) {
        const char c = (char)digits[i];
        size_t digit;
        if (c >= '0' && c <= '9') {
            digit = (size_t)(c - '0');
        } else if (c >= 'A' && c <= 'Z') {
            digit = (size_t)(c - 'A') + 10;
        } else {
            return false;
        }
        result = (result * 36) + digit;
    }

    *value = result;
    return true;
}

// The set the standard defines (consts.py FILETYPE_NAMES).  join_qrs() imports KNOWN_FILETYPES
// but never compares against it; checking here keeps a malformed header out of the routing.
static bool is_known_file_type(const char file_type)
{
    return file_type == 'P' || file_type == 'T' || file_type == 'J' || file_type == 'C' || file_type == 'U'
        || file_type == 'X' || file_type == 'B' || file_type == 'R' || file_type == 'S' || file_type == 'E';
}

// True if the payload is base32 as BBQr puts it on the wire: upper case A-Z and 2-7, no padding,
// and a length that can actually complete the bytes it claims.  base32_to_bin() (main/utils/util.c)
// is permissive where the reference is strict - it stops at '=' or an embedded NUL, accepts lower
// case, and drops the leftover bits of a length that is not a whole number of bytes - so without
// this check "AAA" and "AA======JUNK" both decode to one zero byte and the frame is taken as a
// complete one-byte transfer.  The reference decodes with b32decode() (python/bbqr/utils.py), which
// raises on all three.  Checked here rather than in base32_to_bin() because the other caller
// (main/otpauth.c) takes secrets from a different source and is not bound by BBQr's rules.
static bool is_bbqr_base32(const uint8_t* payload, const size_t payload_len)
{
    // RFC 4648 without padding: a run is eight characters per five bytes plus a tail of 2, 4, 5 or
    // 7 characters.  Tails of 1, 3 and 6 carry bits that cannot complete a byte; the encoder never
    // writes one and the permissive decoder would silently shorten it.
    switch (payload_len % 8) {
    case 0:
    case 2:
    case 4:
    case 5:
    case 7:
        break;
    default:
        return false;
    }

    for (size_t i = 0; i < payload_len; ++i) {
        const char c = (char)payload[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '2' && c <= '7'))) {
            return false;
        }
    }
    return true;
}

// Decode one frame payload into 'out', returning the bytes written or 0 for anything malformed.
// An empty payload counts as malformed rather than as nothing to do: base32_to_bin() asserts on a
// zero length (main/utils/util.c), so a header-only frame would otherwise take the firmware down.
static size_t decode_frame_payload(const char encoding, const uint8_t* payload, const size_t payload_len,
    const bool is_final, uint8_t* out, const size_t out_len)
{
    JADE_ASSERT(payload);
    JADE_ASSERT(out);
    JADE_ASSERT(out_len);

    if (!payload_len) {
        return 0;
    }

    if (encoding == 'H') {
        // Hex puts two characters on the wire per byte, so 2 is the split modulus for every part
        if (payload_len % 2 || payload_len / 2 > out_len) {
            return 0;
        }
        size_t written = 0;
        if (wally_hex_n_to_bytes((const char*)payload, payload_len, out, out_len, &written) != WALLY_OK
            || written != payload_len / 2) {
            return 0;
        }
        return written;
    }

    // '2' and 'Z' both put base32 on the wire and differ only in what the bytes then mean
    if (!is_final && payload_len % 8) {
        return 0;
    }
    if (!is_bbqr_base32(payload, payload_len)) {
        return 0;
    }
    return base32_to_bin((const char*)payload, payload_len, out, out_len);
}

// Raw deflate, as zlib.compressobj(wbits=-10) writes it in the reference encoder: no zlib header
// and no adler32 trailer.  tinfl is called directly rather than through the esp32_deflate
// component because that component is an upstream submodule, its TINFL_FLAG_PARSE_ZLIB_HEADER is
// fixed, and a single in-memory buffer needs none of the streaming machinery it wraps around it.
static bool inflate_raw_deflate(bbqr_ctx_t* ctx, const size_t compressed_len)
{
    JADE_ASSERT(ctx);
    JADE_ASSERT(ctx->data);
    JADE_ASSERT(compressed_len);

    // The output ceiling is the decompression-bomb guard.  Nothing else counts these bytes: a
    // stream wanting more room than this stops at TINFL_STATUS_HAS_MORE_OUTPUT and is rejected
    // below.  MAX_INPUT_MSG_SIZE rather than a new number because this payload is the same kind
    // of thing the message path already caps.
    const size_t capacity = MAX_INPUT_MSG_SIZE;
    uint8_t* out = JADE_MALLOC_PREFER_SPIRAM(capacity);

    // Some 11kb of huffman tables, too large to leave on the scanning task's stack
    tinfl_decompressor* decompressor = JADE_MALLOC(sizeof(tinfl_decompressor));
    tinfl_init(decompressor);

    size_t consumed = compressed_len;
    size_t produced = capacity;
    const tinfl_status status = tinfl_decompress(
        decompressor, ctx->data, &consumed, out, out, &produced, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    free(decompressor);

    // All three have to hold: the stream ended cleanly, every compressed byte was consumed - a
    // short read means trailing junk - and something came out of it.
    if (status != TINFL_STATUS_DONE || consumed != compressed_len || !produced) {
        JADE_LOGW("BBQr decompression rejected, status %d", (int)status);
        JADE_WALLY_VERIFY(wally_bzero(out, capacity));
        free(out);
        return false;
    }

    ctx->inflated = out;
    ctx->inflated_len = produced;
    return true;
}

bool bbqr_is_header(const uint8_t* data, const size_t len)
{
    return data && len >= BBQR_HEADER_LEN && data[0] == 'B' && data[1] == '$';
}

bbqr_collect_result_t bbqr_collect(bbqr_ctx_t* ctx, const uint8_t* data, const size_t len)
{
    JADE_ASSERT(ctx);
    JADE_ASSERT(data);

    if (!bbqr_is_header(data, len)) {
        return BBQR_REJECTED;
    }

    const char encoding = (char)data[2];
    const char file_type = (char)data[3];
    if ((encoding != 'H' && encoding != '2' && encoding != 'Z') || !is_known_file_type(file_type)) {
        return BBQR_REJECTED;
    }

    size_t num_parts = 0;
    size_t index = 0;
    if (!parse_base36_pair(data + 4, &num_parts) || !parse_base36_pair(data + 6, &index)) {
        return BBQR_REJECTED;
    }
    if (!num_parts || index >= num_parts) {
        return BBQR_REJECTED;
    }

    // A frame belonging to a different transfer is ignored rather than allowed to disturb this one
    if (ctx->num_parts && (ctx->encoding != encoding || ctx->file_type != file_type || ctx->num_parts != num_parts)) {
        return BBQR_REJECTED;
    }

    if (!ctx->scratch) {
        ctx->scratch = JADE_MALLOC(BBQR_MAX_FRAME_DECODED);
    }

    const bool is_final = (index == num_parts - 1);
    const size_t decoded_len = decode_frame_payload(
        encoding, data + BBQR_HEADER_LEN, len - BBQR_HEADER_LEN, is_final, ctx->scratch, BBQR_MAX_FRAME_DECODED);
    if (!decoded_len) {
        return BBQR_REJECTED;
    }

    if (ctx->num_parts && part_seen(ctx, index)) {
        const size_t held_len = is_final ? ctx->final_len : ctx->block_len;
        if (held_len == decoded_len && !memcmp(ctx->data + (index * ctx->block_len), ctx->scratch, decoded_len)) {
            // The same part again, which is what an animated code hands over every cycle
            return ctx->num_seen == ctx->num_parts ? BBQR_COMPLETE : BBQR_IN_PROGRESS;
        }
        // Same index, different content: either two transfers share these six header characters or
        // the payload changed underneath us.  Keeping both would interleave them into one corrupt
        // buffer, so the collection restarts from this frame instead.
        JADE_LOGW("BBQr part %u repeated with different content, restarting", (unsigned)index);
        collection_reset(ctx);
    }

    if (!ctx->num_parts) {
        if (is_final && num_parts > 1) {
            // The block length is still unknown and the final part is the one part that cannot
            // set it, being the short one.  Wait a frame; the cycle brings a full part next.
            return BBQR_REJECTED;
        }

        // The ceiling is applied to the buffer, which is one block per part.  The message itself
        // is shorter, by however much the final part falls short of a block, but that is not known
        // yet: assembling in place means allocating the whole grid from the first full part, long
        // before the short one arrives.  So the test is conservative by less than one block (at
        // most BBQR_MAX_FRAME_DECODED - 1 = 634 bytes against a ceiling of 410624), and it is
        // conservative in the safe direction: what is allocated is what is measured, and a message
        // that passes is under the ceiling with room to spare.  The cost is that a transfer landing
        // in that last 634-byte sliver is refused although its payload would have fitted.
        const size_t total = decoded_len * num_parts;
        if (total > MAX_INPUT_MSG_SIZE) {
            JADE_LOGW("BBQr transfer of %u parts is over the input ceiling", (unsigned)num_parts);
            return BBQR_REJECTED;
        }

        ctx->data = JADE_MALLOC_PREFER_SPIRAM(total);
        ctx->data_len = total;
        ctx->block_len = decoded_len;
        ctx->num_parts = num_parts;
        ctx->encoding = encoding;
        ctx->file_type = file_type;
    }

    // Only the final part may be short, and no part may be long
    if (is_final ? (decoded_len > ctx->block_len) : (decoded_len != ctx->block_len)) {
        return BBQR_REJECTED;
    }

    // The write lands inside ctx->data by construction, which is worth stating because the four
    // guarantees sit in four different places: index < num_parts (rejected at the header),
    // num_parts == ctx->num_parts once a collection is running (the foreign-transfer check above),
    // ctx->data_len == block_len * num_parts (where the buffer is allocated), and decoded_len is at
    // most block_len (the check immediately above).  The last byte written is therefore at most
    // (num_parts - 1) * block_len + block_len - 1, which is ctx->data_len - 1.
    memcpy(ctx->data + (index * ctx->block_len), ctx->scratch, decoded_len);
    if (is_final) {
        ctx->final_len = decoded_len;
    }
    mark_part_seen(ctx, index);
    ++ctx->num_seen;

    return ctx->num_seen == ctx->num_parts ? BBQR_COMPLETE : BBQR_IN_PROGRESS;
}

bool bbqr_finalise(bbqr_ctx_t* ctx, const uint8_t** data, size_t* data_len, char* file_type)
{
    JADE_ASSERT(ctx);
    JADE_ASSERT(data);
    JADE_ASSERT(data_len);
    // file_type is optional

    *data = NULL;
    *data_len = 0;

    if (!ctx->num_parts || ctx->num_seen != ctx->num_parts || !ctx->data) {
        return false;
    }

    const size_t total = (ctx->block_len * (ctx->num_parts - 1)) + ctx->final_len;
    JADE_ASSERT(total && total <= ctx->data_len);

    if (ctx->encoding == 'Z') {
        if (!ctx->inflated && !inflate_raw_deflate(ctx, total)) {
            return false;
        }
        *data = ctx->inflated;
        *data_len = ctx->inflated_len;
    } else {
        *data = ctx->data;
        *data_len = total;
    }

    if (file_type) {
        *file_type = ctx->file_type;
    }
    return true;
}

void bbqr_free(bbqr_ctx_t* ctx)
{
    if (!ctx) {
        return;
    }

    collection_reset(ctx);

    if (ctx->scratch) {
        JADE_WALLY_VERIFY(wally_bzero(ctx->scratch, BBQR_MAX_FRAME_DECODED));
        free(ctx->scratch);
        ctx->scratch = NULL;
    }
}
#endif // AMALGAMATED_BUILD

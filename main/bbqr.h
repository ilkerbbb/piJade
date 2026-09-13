#ifndef BBQR_H_
#define BBQR_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// BBB-AIRGAP: reader for BBQr, the animated multi-frame QR format Coldcard and Passport emit.
// Ported from coinkite/BBQr (python/bbqr/join.py and python/bbqr/utils.py), which that project
// places in the public domain: UNLICENSE.md at its root, and the header of each of those two
// files says so again.  Reading only: piJade never produces BBQr frames, its own multi-frame
// output stays bc-ur (main/bcur.c).
//
// A frame is a fixed 8-character header followed by the encoded payload:
//
//     B$ <encoding> <file type> <total parts: 2 digits> <index: 2 digits>
//
// Both digit pairs are base36.  A comment in the reference split.py calls them hex, but the code
// on both sides uses int2base36() and int(s, 36), so base36 is what the wire carries; anything
// that believes the comment mis-reads every transfer of 16 or more parts.
//
// Two things the reference join_qrs() leaves to the encoder, and a collector cannot:
//
//  - It never checks that the parts are equally long, because it concatenates whatever each part
//    decodes to.  Here the parts are written into one buffer at index * block length, so a short
//    part would silently shift every byte after it.  Each non-final part must therefore decode
//    to the same length, and only the final part may be shorter.
//
//  - It decodes each part with b32decode() after re-adding padding, which tolerates a part whose
//    character count is not a multiple of 8.  base32_to_bin() (main/utils/util.c) drops the
//    leftover bits instead, so such a part would move the byte alignment of everything after it
//    with no error anywhere.  Non-final parts must be a multiple of 8 characters, which is what
//    the encoder produces anyway (utils.py encode_data returns 8 as the base32 split modulus,
//    and 2 for hex).
//
// One consequence of assembling in place: the block length can only be learned from a part that
// is not the last one, since the last one is the short one.  An animated code cycles and the
// user points the camera at an arbitrary moment, so the first frame seen is the last part once
// every N transfers.  Such a frame is rejected while the block length is unknown and picked up
// on the next pass; the cost is a single frame, where taking its length as the block length
// would reject every full part after it and never finish.  A single-part transfer is the
// exception: there index 0 is both first and last, and its own length is the block length.

// Fixed header: "B$", encoding, file type, 2 base36 digits of total, 2 base36 digits of index
#define BBQR_HEADER_LEN 8

// Ceiling on parts: the total field is two base36 digits, so "ZZ" is the largest transfer the
// header can name.  The reference splitter caps itself at the same number (python/bbqr/split.py,
// find_best_version max_split=1295).  Nothing rejects a larger count at run time because nothing
// can parse one; the number is here to size seen[] and is tied to the digits by a static assert
// in bbqr.c.
#define BBQR_MAX_PARTS 1295

typedef enum {
    // Frame not taken: not BBQr, malformed, or inconsistent with the collection in progress
    BBQR_REJECTED = 0,
    // Frame taken, parts still missing
    BBQR_IN_PROGRESS,
    // Every part is present; call bbqr_finalise()
    BBQR_COMPLETE,
} bbqr_collect_result_t;

typedef struct {
    // Assembled payload, allocated once the block length is known
    uint8_t* data;
    size_t data_len;

    // Decompressed payload of the 'Z' encoding, allocated by bbqr_finalise()
    uint8_t* inflated;
    size_t inflated_len;

    // Per-frame decode target, so a repeated frame can be compared without overwriting
    uint8_t* scratch;

    // Decoded bytes carried by every non-final part, and by the final one; 0 until known
    size_t block_len;
    size_t final_len;

    // 0 when no collection is in progress
    size_t num_parts;
    size_t num_seen;

    char encoding; // 'H', '2' or 'Z'
    char file_type; // 'P', 'T', 'J', 'C', 'U', 'X', 'B', 'R', 'S' or 'E'

    uint8_t seen[(BBQR_MAX_PARTS + 7) / 8];
} bbqr_ctx_t;

// True if the payload can be a BBQr frame: the two-character magic and a full header
bool bbqr_is_header(const uint8_t* data, size_t len);

// Take one frame.  'ctx' must be zeroed before the first call.
bbqr_collect_result_t bbqr_collect(bbqr_ctx_t* ctx, const uint8_t* data, size_t len);

// Hand back the assembled - and for 'Z' decompressed - payload.  The data remains owned by 'ctx'
// and stays valid until bbqr_free().  False if the collection is incomplete or decompression
// fails.  'file_type' is optional.
bool bbqr_finalise(bbqr_ctx_t* ctx, const uint8_t** data, size_t* data_len, char* file_type);

// Wipe and release everything 'ctx' holds.  Safe on a zeroed or already-freed context.
void bbqr_free(bbqr_ctx_t* ctx);

#endif /* BBQR_H_ */

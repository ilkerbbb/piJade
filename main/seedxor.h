#ifndef JADE_SEEDXOR_H_
#define JADE_SEEDXOR_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// BBB-AIRGAP: Seed XOR, the paper-splitting scheme Coldcard publishes.  The raw BIP39 entropies
// of N phrases xored together give back the original entropy, so every part is itself a valid,
// empty wallet and nothing written on a part says it is one.  Order does not matter and the
// checksum is not carried: each part gets its own when its entropy is turned back into words,
// and so does the combined result.
//
// Written from the published description of the scheme.  Coldcard's own implementation
// (shared/xor_seed.py) is MIT with the Commons Clause attached, so its code cannot be taken and
// none of it was; what is shared is one xor loop, which is the whole of the format.
//
// What the scheme gives, and what it does not, is stated where the user meets it - see the notes
// on the combine and split flows in main/process/mnemonic.c.  It is not a threshold scheme: lose
// one part and the seed is gone.
//
// This fork both reads and writes the format: it combines parts made elsewhere, and splits the
// wallet it holds into parts of its own.

// The fork's wallets are 12 or 24 words, ie. 16 or 32 bytes of entropy.
#define SEEDXOR_MAX_PART_LEN 32

// Coldcard splits into two to four parts and so does this.  Combining has no upper bound here
// either: the device cannot know how many parts a user wrote down, so it is the user who says
// when the set is complete.
#define SEEDXOR_MIN_SPLIT_PARTS 2
#define SEEDXOR_MAX_SPLIT_PARTS 4

// Xor one part into a running total.  This is the whole of combining, called once per part as it
// is entered, so that no part has to be held anywhere while the next one is typed.
void seedxor_accumulate(uint8_t* total, const uint8_t* part, size_t len);

// True if every byte is zero.  For a combined total that means the parts cancelled out, which
// for a user means a doubled part rather than a wallet.  Reads the whole buffer rather than
// stopping at the first byte that settles the answer.
bool seedxor_is_zero(const uint8_t* entropy, size_t len);

// Split 'entropy' into 'num_parts' parts of its own length, written end to end into 'parts_out'
// (num_parts * entropy_len bytes).  Every part but the last comes from the device rng; the last
// one closes the xor.  Returns false if the parts do not combine back to 'entropy', which is a
// check on this function rather than on its caller.
bool seedxor_split(const uint8_t* entropy, size_t entropy_len, size_t num_parts, uint8_t* parts_out);

#endif /* JADE_SEEDXOR_H_ */

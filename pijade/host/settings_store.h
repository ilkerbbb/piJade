/*
 * Stores libjade's settings blob in two alternating files on the FAT boot partition.
 *
 * Replacing one file with rename is not power-cut safe on FAT: the directory entry and FAT chain
 * can reach the card in different generations, after which fsck may truncate the file to zero.
 * Alternating between <base>.a and <base>.b keeps the previous valid generation while the other is
 * written. The legacy single file at <base> is deliberately neither read nor migrated.
 *
 * The files CONTAIN KEY MATERIAL once a PIN wallet is set up: the PIN client private key, the
 * encrypted wallet, and the plaintext duress PIN (see libjade/pijade_settings.c for the field list,
 * and pijade/UPSTREAM.md section 22 for what that costs against Jade's encrypted flash). A previous
 * generation in the other slot gives an attacker who can copy the card no capability they did not
 * already have, but rollback remains possible. Treat both files like private keys, and remember
 * that flash and SD media may retain physical copies even after files are overwritten and removed.
 */
#ifndef PIJADE_SETTINGS_STORE_H
#define PIJADE_SETTINGS_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * BBB-AIRGAP: explicit_bzero() is a glibc/BSD extension that macOS does not provide (measured: it
 * does not compile there even with <strings.h>), so the host build of this file and of
 * libjade/daemon.c - its only two callers - needs a stand-in.  The barrier below is the technique
 * libwally-core uses for the same job: the empty asm consumes the pointer and clobbers memory, so
 * the compiler cannot prove the wipe is dead and elide it.  memset_s() also compiles on macOS
 * (measured, including when __STDC_WANT_LIB_EXT1__ is defined after an earlier <string.h>), but it
 * is Annex K: optional, absent from glibc, and declared only where that feature-test macro is set,
 * so using it would push the macro onto every host translation unit that includes this header.
 * The barrier asks nothing of its includers.  Every other target, the shipping ARM Linux one
 * included, uses the real function.
 * If a future macOS gains explicit_bzero, this definition collides with its declaration and the
 * build fails loudly - delete the block then, do not widen the guard.
 */
#ifdef __APPLE__
#include <string.h>
static inline void explicit_bzero(void* buf, size_t len)
{
    memset(buf, 0, len);
    __asm__ __volatile__("" : : "r"(buf) : "memory");
}
#endif

typedef struct settings_store settings_store_t;

/* Allocates the two slot paths from base; reads nothing. NULL on allocation failure. */
settings_store_t* settings_store_open(const char* base);
void settings_store_close(settings_store_t* store);

/*
 * Reads the newest valid slot into a malloc'd buffer the caller owns and wipes. Returns false when
 * no slot is usable; absent slots are silent and invalid-slot reasons are already on stderr.
 * Remembers the winning slot so the next write targets the other slot, and only raises the
 * handle's monotonic sequence counter. If one slot is unreadable, a valid other slot still wins.
 */
bool settings_store_read(settings_store_t* store, uint8_t** data, size_t* len);

/*
 * Writes a new generation to the slot that does not hold the newest valid one, or next_slot when
 * none is known. Probes the other slot's non-secret header before choosing a monotonic sequence.
 * A physically unreadable other slot is erased only after the new frame is durable; cleanup
 * failure makes this return false. Sequence exhaustion is rejected. len must be
 * 1..SETTINGS_MAX_LEN and newly created files use mode 0600.
 */
bool settings_store_write(settings_store_t* store, const uint8_t* data, size_t len);

/*
 * Overwrites each existing slot with zeros for its full size, fsyncs it, removes it, then fsyncs
 * the parent directory. Returns true only when neither slot remains. The sequence counter remains
 * monotonic; if one slot survives, the next write targets that slot, otherwise it targets A.
 */
bool settings_store_erase(settings_store_t* store);

#endif /* PIJADE_SETTINGS_STORE_H */

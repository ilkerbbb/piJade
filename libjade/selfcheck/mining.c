#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "components/miner/miner.h"
#include "selfcheck.h"

#include <wally_address.h>
#include <wally_core.h>
#include <wally_crypto.h>
#include <wally_transaction.h>

// Public test-vector address, taken from libwally's own descriptor tests
// (components/libwally-core/upstream/src/ctest/test_descriptor.c:1829) rather than written from
// memory. Nothing here is broadcast: the coinbase it pays is only ever hashed.
#define MINING_ADDRESS "tb1q0ht9tyks4vh7p5p904t340cr9nvahy7um9zdem"

// on_new_target() now refuses a template whose target is not the one its bits encode, so every pair
// below is written out by hand from the compact encoding rather than taken from the implementation:
// mantissa = bits & 0x007fffff, and its three bytes sit (bits >> 24) bytes from the low end of a
// 32-byte big-endian target.
//
// Regtest difficulty bits, which is also what selects REGTEST_INTERVAL in on_new_target(). Exponent
// 32 puts the mantissa at the very top, so a hash clears it whenever its most significant byte is
// below 0x7f: about one in two, which reaches the callback within a handful of nonces.
#define MINING_BITS_EASY 0x207fffffUL
// Exponent 30 moves the mantissa two bytes down, so the top two bytes of the hash must be zero and
// the third below 0x7f: around 131072 hashes, enough that the loop has to actually run.
#define MINING_BITS_17BIT 0x1e7fffffUL
// get_block_reward() at any height below the first regtest halving
#define MINING_REWARD 5000000000ULL

#define MINING_HEADER_LEN 80
#define MINING_VERSION 0x20000000UL
#define MINING_CURTIME 1600000000UL
#define MINING_TIMEOUT_MS 60000

// BBB-AIRGAP: libjade.c compiles this file into the same translation unit as main/amalgamated.c,
// so nothing here may share a name with the statics of main/qrmode.c (mining_solution_t and
// on_mining_solution live there since M3; measured as a redefinition error, 2026-09-02).
typedef struct {
    uint8_t solution[600];
    size_t len;
    bool called;
} selfcheck_solution_t;

static void selfcheck_on_solution(void* ctx, const uint8_t* solution, uint32_t len)
{
    selfcheck_solution_t* found = ctx;
    JADE_ASSERT(found);
    JADE_ASSERT(solution);
    JADE_ASSERT(len <= sizeof(found->solution));

    memcpy(found->solution, solution, len);
    found->len = len;
    found->called = true;
}

// BBB-AIRGAP: the miner is the first component to be stopped while it is running rather than while
// it is parked, and on this platform a task can only be deleted by handle if it parked itself in
// vTaskDelay(portMAX_DELAY) (libjade/task.c:197-205, :233). Counting the process's threads before
// and after is what turns "stop_miners() returned" into "the miner tasks are actually gone"; the
// same count is why the free() inside stop_miners() is not handing back memory that is still
// written to. Linux-only, which is all this file is ever compiled for.
static size_t thread_count(void)
{
    FILE* status = fopen("/proc/self/status", "r");
    if (!status) {
        return 0;
    }

    char line[128];
    size_t count = 0;
    while (fgets(line, sizeof(line), status)) {
        if (!strncmp(line, "Threads:", 8)) {
            count = (size_t)strtoul(line + 8, NULL, 10);
            break;
        }
    }
    fclose(status);
    return count;
}

// The count is read again rather than trusted once: a task that has published its retirement is
// still a live thread for the instant it takes to leave, so "gone" is only observable as "gone
// within a bounded wait". A miner that never stops never satisfies it.
static bool wait_for_thread_count(size_t expected)
{
    for (uint32_t waited = 0; waited < 5000; ++waited) {
        if (thread_count() == expected) {
            return true;
        }
        vTaskDelay(1 / portTICK_PERIOD_MS);
    }
    return false;
}

// Recompute the header's hash with libwally and compare it to the target the miner was given, so a
// solution has to satisfy an implementation other than the optimised one that produced it. Bitcoin
// compares the digest as a big-endian number written the other way round, so the target's first
// byte meets the digest's last.
static bool hash_within_target(const uint8_t* header, const uint8_t* target)
{
    uint8_t hash[SHA256_LEN];
    JADE_WALLY_VERIFY(wally_sha256d(header, MINING_HEADER_LEN, hash, sizeof(hash)));

    for (size_t i = 0; i < sizeof(hash); ++i) {
        const uint8_t byte = hash[sizeof(hash) - 1 - i];
        if (byte < target[i]) {
            return true;
        }
        if (byte > target[i]) {
            return false;
        }
    }
    return true;
}

static uint32_t read_le32(const uint8_t* buf)
{
    return buf[0] | (buf[1] << 8) | (buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

// The miner only ever hashes the 80-byte header, so a coinbase paying the wrong address - or a
// merkle root written independently of the coinbase - still yields a header whose hash clears the
// target. Checking the header alone would therefore stay green through exactly the failure that
// costs the user the block reward. Parse the coinbase out of the solution and prove both links:
// the first output pays the template's address and amount, and the header's merkle root is that
// transaction's txid.
static bool coinbase_binds_to_header(const char* name, const selfcheck_solution_t* found)
{
    JADE_ASSERT(name);
    JADE_ASSERT(found);
    JADE_ASSERT(found->len > MINING_HEADER_LEN + 1);

    struct wally_tx* tx = NULL;
    if (wally_tx_from_bytes(
            found->solution + MINING_HEADER_LEN + 1, found->len - MINING_HEADER_LEN - 1, WALLY_TX_FLAG_USE_WITNESS, &tx)
        != WALLY_OK) {
        JADE_LOGE("mining %s: coinbase did not parse", name);
        return false;
    }

    bool passed = false;
    uint8_t expected_script[64];
    size_t script_len = 0;
    uint8_t txid[SHA256_LEN];

    if (tx->num_outputs != 2) {
        JADE_LOGE("mining %s: coinbase has %u outputs, expected 2", name, (unsigned)tx->num_outputs);
    } else if (tx->outputs[0].satoshi != MINING_REWARD) {
        JADE_LOGE("mining %s: coinbase pays %llu, expected %llu", name, (unsigned long long)tx->outputs[0].satoshi,
            (unsigned long long)MINING_REWARD);
    } else if (wally_addr_segwit_to_bytes(
                   MINING_ADDRESS, "tb", 0, expected_script, sizeof(expected_script), &script_len)
        != WALLY_OK) {
        JADE_LOGE("mining %s: could not derive the expected script for the mining address", name);
    } else if (tx->outputs[0].script_len != script_len || memcmp(tx->outputs[0].script, expected_script, script_len)) {
        JADE_LOGE("mining %s: coinbase does not pay the address the template asked for", name);
    } else if (wally_tx_get_txid(tx, txid, sizeof(txid)) != WALLY_OK) {
        JADE_LOGE("mining %s: could not recompute the coinbase txid", name);
    } else if (memcmp(found->solution + 36, txid, sizeof(txid))) {
        JADE_LOGE("mining %s: header's merkle root is not the coinbase txid", name);
    } else {
        passed = true;
    }

    wally_tx_free(tx);
    return passed;
}

static bool mine_one(
    const char* name, void* mctx, const uint8_t* target, uint32_t bits, uint32_t height, selfcheck_solution_t* found)
{
    JADE_ASSERT(name);
    JADE_ASSERT(mctx);
    JADE_ASSERT(target);
    JADE_ASSERT(found);

    // Deliberately not all zeros: on_new_target() reverses this into the header, and an all-zero
    // value would pass whether or not it did.
    uint8_t prevhash[SHA256_LEN];
    for (size_t i = 0; i < sizeof(prevhash); ++i) {
        prevhash[i] = (uint8_t)i;
    }

    found->called = false;
    found->len = 0;

    const uint64_t reward
        = on_new_target(mctx, MINING_VERSION, prevhash, target, MINING_CURTIME, bits, height, MINING_ADDRESS);
    if (reward != MINING_REWARD) {
        JADE_LOGE("mining %s: coinbase value %llu, expected %llu", name, (unsigned long long)reward,
            (unsigned long long)MINING_REWARD);
        return false;
    }

    for (uint32_t waited = 0; waited < MINING_TIMEOUT_MS && !check_solutions(mctx); ++waited) {
        vTaskDelay(1 / portTICK_PERIOD_MS);
    }
    if (!found->called) {
        JADE_LOGE("mining %s: no solution within %u ms", name, (unsigned)MINING_TIMEOUT_MS);
        return false;
    }

    // Header, one-transaction count byte, then the coinbase itself
    if (found->len <= MINING_HEADER_LEN + 1) {
        JADE_LOGE("mining %s: solution is %u bytes, expected more than %u", name, (unsigned)found->len,
            (unsigned)MINING_HEADER_LEN + 1);
        return false;
    }
    if (found->solution[MINING_HEADER_LEN] != 0x01) {
        JADE_LOGE(
            "mining %s: transaction count byte is 0x%02x, expected 0x01", name, found->solution[MINING_HEADER_LEN]);
        return false;
    }

    if (read_le32(found->solution) != MINING_VERSION || read_le32(found->solution + 68) != MINING_CURTIME
        || read_le32(found->solution + 72) != bits) {
        JADE_LOGE("mining %s: header did not carry the template's version, timestamp and bits", name);
        return false;
    }
    for (size_t i = 0; i < sizeof(prevhash); ++i) {
        if (found->solution[4 + i] != prevhash[sizeof(prevhash) - 1 - i]) {
            JADE_LOGE("mining %s: previous block hash was not reversed into the header", name);
            return false;
        }
    }

    if (!hash_within_target(found->solution, target)) {
        JADE_LOGE("mining %s: solution's hash is above the target it was mined against", name);
        return false;
    }

    if (!coinbase_binds_to_header(name, found)) {
        return false;
    }

    JADE_LOGI("mining %s: solved with nonce %u, %u byte solution", name, (unsigned)read_le32(found->solution + 76),
        (unsigned)found->len);
    return true;
}

bool test_mining(void)
{
    JADE_LOGI("Testing bitcoin mining hash loop");

    const size_t threads_before = thread_count();
    if (!threads_before) {
        JADE_LOGE("mining: could not read the process thread count");
        return false;
    }

    selfcheck_solution_t found = { 0 };
    void* mctx = NULL;
    start_miners(&mctx, selfcheck_on_solution, &found);
    if (!mctx) {
        JADE_LOGE("mining: start_miners() produced no context");
        return false;
    }

    if (!wait_for_thread_count(threads_before + 2)) {
        JADE_LOGE("mining: %u threads after start_miners(), expected %u", (unsigned)thread_count(),
            (unsigned)threads_before + 2);
        stop_miners(mctx);
        return false;
    }

    // The expansion of MINING_BITS_EASY, written out by hand
    uint8_t target[SHA256_LEN];
    memset(target, 0, sizeof(target));
    target[0] = 0x7f;
    target[1] = 0xff;
    target[2] = 0xff;
    bool passed = mine_one("easy target", mctx, target, MINING_BITS_EASY, 1, &found);

    // The expansion of MINING_BITS_17BIT. The second call also exercises retargeting a miner that is
    // already parked on a solution.
    if (passed) {
        memset(target, 0, sizeof(target));
        target[2] = 0x7f;
        target[3] = 0xff;
        target[4] = 0xff;
        passed = mine_one("17-bit target", mctx, target, MINING_BITS_17BIT, 2, &found);
    }

    if (passed) {
        uint32_t speed = 0;
        check_speed(mctx, &speed);
        if (!speed) {
            JADE_LOGE("mining: check_speed() reported no hashes after a solved 16-bit target");
            passed = false;
        } else {
            JADE_LOGI("mining: %u hashes per second across both tasks", (unsigned)speed);
        }
    }

    // The reward address arrives inside a scanned template, so it is attacker chosen. Before the
    // fix in make_coinbase_tx() this line ended the process: the address converts under neither
    // wally call and the failure reached a live assert(false). The template must be refused instead,
    // with both miners left running on the work they already had.
    // 'target' still holds the MINING_BITS_17BIT expansion, so the pair is consistent and the only
    // thing left for on_new_target() to object to is the address itself. Passing a mismatched pair
    // here would make this test pass for the wrong reason.
    if (passed) {
        const uint64_t refused = on_new_target(
            mctx, MINING_VERSION, target, target, MINING_CURTIME, MINING_BITS_17BIT, 3, "not-a-bitcoin-address");
        if (refused) {
            JADE_LOGE("mining: an unconvertible reward address was accepted, coinbase value %llu",
                (unsigned long long)refused);
            passed = false;
        } else if (thread_count() != threads_before + 2) {
            JADE_LOGE("mining: %u threads after a refused template, expected %u", (unsigned)thread_count(),
                (unsigned)threads_before + 2);
            passed = false;
        }
    }

    // Bits and target arrive as two independent values in the scanned template. A template claiming
    // a difficulty its target does not encode would be mined against the easy target and produce a
    // header the network rejects, so it has to be refused before any work is published. The address
    // here is the valid one, so only the inconsistency can be the reason.
    if (passed) {
        uint8_t mismatched[SHA256_LEN];
        memset(mismatched, 0xff, sizeof(mismatched));
        const uint64_t refused = on_new_target(
            mctx, MINING_VERSION, mismatched, mismatched, MINING_CURTIME, MINING_BITS_17BIT, 4, MINING_ADDRESS);
        if (refused) {
            JADE_LOGE("mining: a target that its bits do not encode was accepted, coinbase value %llu",
                (unsigned long long)refused);
            passed = false;
        } else if (thread_count() != threads_before + 2) {
            JADE_LOGE("mining: %u threads after a refused difficulty, expected %u", (unsigned)thread_count(),
                (unsigned)threads_before + 2);
            passed = false;
        }
    }

    stop_miners(mctx);

    if (!wait_for_thread_count(threads_before)) {
        JADE_LOGE(
            "mining: %u threads after stop_miners(), expected %u", (unsigned)thread_count(), (unsigned)threads_before);
        passed = false;
    }

    return passed;
}

bool debug_selfcheck(jade_process_t* process)
{
    if (!test_mining()) {
        FAIL();
    }
    return true;
}

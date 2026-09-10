#ifndef SELFCHECK_H_
#define SELFCHECK_H_

#include "jade_assert.h"
#include "keychain.h"
#include "process.h"

#define FAIL()                                                                                                         \
    do {                                                                                                               \
        JADE_LOGE("SELFCHECK FAILURE@%d", __LINE__);                                                                   \
        return false;                                                                                                  \
    } while (false)

#define WALLY_FREE_STR(str)                                                                                            \
    do {                                                                                                               \
        if (wally_free_string(str) != WALLY_OK) {                                                                      \
            FAIL();                                                                                                    \
        }                                                                                                              \
    } while (false)

static const char TEST_MNEMONIC[] = "fish inner face ginger orchard permit useful method fence kidney chuckle party "
                                    "favorite sunset draw limb science crane oval letter slot invite sadness banana";
// Set the standard test wallet
static bool selfcheck_set_test_mnemonic(jade_process_t* process, const bool do_set, const bool is_temporary)
{
    JADE_ASSERT(process);

    keychain_t keydata = { 0 };
    if (!keychain_derive_from_mnemonic(TEST_MNEMONIC, NULL, &keydata)) {
        return false;
    }
    if (do_set) {
        keychain_set(&keydata, process->ctx.source, is_temporary);
    }
    return true;
}

#endif /* SELFCHECK_H_ */

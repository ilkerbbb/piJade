#include "bcur.h"
#include "descriptor.h"
#include "descriptor_text.h"
#include "registration_seal.h"
#include "selfcheck.h"
#include "utils/malloc_ext.h"

#define INIT_DESC(d, s)                                                                                                \
    do {                                                                                                               \
        d.script_len = strlen(s);                                                                                      \
        JADE_ASSERT(d.script_len < sizeof(d.script));                                                                  \
        strcpy(d.script, s);                                                                                           \
        d.type = DESCRIPTOR_TYPE_UNKNOWN;                                                                              \
        d.num_values = 0;                                                                                              \
    } while (false)

#define ADD_MAP_VAL(d, k, v)                                                                                           \
    do {                                                                                                               \
        string_value_t* const sv = &(d.values[d.num_values]);                                                          \
        sv->key_len = strlen(k);                                                                                       \
        JADE_ASSERT(sv->key_len < sizeof(sv->key));                                                                    \
        strcpy(sv->key, k);                                                                                            \
        sv->value_len = strlen(v);                                                                                     \
        JADE_ASSERT(sv->value_len < sizeof(sv->value));                                                                \
        strcpy(sv->value, v);                                                                                          \
        ++d.num_values;                                                                                                \
    } while (false)

#define FP_XPUB_MATCH(n, fp, xp)                                                                                       \
    (wally_hex_to_bytes(fp, buf, sizeof(buf), &written) == WALLY_OK && written == sizeof(signers[n].fingerprint)       \
        && !memcmp(buf, signers[n].fingerprint, written) && !strcmp(xp, signers[n].xpub))

static bool check_descriptor_serialisation(descriptor_data_t* const desc)
{
    JADE_ASSERT(desc);

    // BBB-AIRGAP: the sealed record carries a random IV, so two serialisations differ while both
    // open to the same body.  Compare bodies and the parsed structure, not the sealed bytes.
    uint8_t body[768];
    const size_t body_len = DESCRIPTOR_BODY_LEN(desc);
    JADE_ASSERT(body_len < sizeof(body));
    if (!descriptor_body_to_bytes(desc, body, body_len)) {
        FAIL();
    }

    uint8_t sealed[sizeof(body) + 64];
    const size_t sealed_len = DESCRIPTOR_BYTES_LEN(desc);
    JADE_ASSERT(sealed_len <= sizeof(sealed));
    if (!descriptor_seal_body(body, body_len, sealed, sealed_len)) {
        FAIL();
    }

    uint8_t sealed2[sizeof(sealed)];
    if (!descriptor_seal_body(body, body_len, sealed2, sealed_len)) {
        FAIL();
    }
    if (!memcmp(sealed, sealed2, sealed_len)) {
        FAIL(); // fresh IV every time
    }

    // BBB-AIRGAP: descriptor_data_t is some 3KB, so the copy of the original goes on the heap;
    // this function already holds several kilobytes of record buffers on the stack.
    descriptor_data_t* const original = JADE_MALLOC(sizeof(descriptor_data_t));
    memcpy(original, desc, sizeof(descriptor_data_t));
    wally_bzero(desc, sizeof(descriptor_data_t));

#define SERIALISATION_FAIL()                                                                                           \
    do {                                                                                                               \
        free(original);                                                                                                \
        FAIL();                                                                                                        \
    } while (false)

    if (!descriptor_from_bytes(sealed, sealed_len, desc)) {
        SERIALISATION_FAIL();
    }
    if (desc->script_len != original->script_len || desc->num_values != original->num_values
        || desc->type != original->type || strcmp(desc->script, original->script)) {
        SERIALISATION_FAIL();
    }
    for (size_t i = 0; i < desc->num_values; ++i) {
        if (strcmp(desc->values[i].key, original->values[i].key)
            || strcmp(desc->values[i].value, original->values[i].value)) {
            SERIALISATION_FAIL();
        }
    }
    free(original);
#undef SERIALISATION_FAIL

    // Re-serialised body must equal the first body
    uint8_t body2[sizeof(body)];
    if (DESCRIPTOR_BODY_LEN(desc) != body_len || !descriptor_body_to_bytes(desc, body2, body_len)
        || memcmp(body, body2, body_len)) {
        FAIL();
    }

    // A flipped ciphertext byte, a flipped version byte and a pre-sealing-sized record all fail
    // without asserting
    uint8_t tampered[sizeof(sealed)];
    size_t written = 0;
    memcpy(tampered, sealed, sealed_len);
    tampered[1 + AES_BLOCK_LEN] ^= 0x01;
    if (descriptor_open_registration(tampered, sealed_len, body2, sizeof(body2), &written)) {
        FAIL();
    }
    memcpy(tampered, sealed, sealed_len);
    tampered[0] = 0; // the pre-sealing descriptor version
    if (descriptor_open_registration(tampered, sealed_len, body2, sizeof(body2), &written)) {
        FAIL();
    }
    if (descriptor_open_registration(sealed, 41, body2, sizeof(body2), &written)) {
        FAIL(); // the settings-file floor for a descriptor record
    }

    return true;
}

static bool test_miniscript_descriptors(void)
{
    const char* errmsg = NULL;
    descriptor_data_t desc = {};
    bool ret;

    const uint32_t MAINNET = NETWORK_BITCOIN;
    const uint32_t TESTNET = NETWORK_BITCOIN_TESTNET;
    const uint32_t LTESTNET = NETWORK_LIQUID_TESTNET;
    uint8_t buf[BIP32_KEY_FINGERPRINT_LEN];
    size_t num_signers = 0;
    signer_t signers[3];
    size_t written;

    // Anchor Watch example
    INIT_DESC(desc, "wsh(or_d(pk(@0/<0;1>/*),and_v(v:multi(2,@1/<0;1>/*,@2/<0;1>/*),older(4320))))");
    ADD_MAP_VAL(desc, "@0",
        "[1bf12fe0/48'/1'/0'/2']"
        "tpubDEHXLZfMAAM5duEnX6SSnZjGYbrxqXvRJmMxw8MFwr3gu4LC4DSxR9KVEfVDVcZxre4XL5tGcwVRrHwQ9euTMnSq6P"
        "6BqREemaqrFsC96Fy");
    ADD_MAP_VAL(desc, "@1",
        "[eda3d606/48'/1'/0'/2']"
        "tpubDEAmqvQkhqP6SbfbSPu3AeRR9kfHLFXYvNDiWashLy7V2zicg1YLg654AqfomsC6kFwTs4MpcnqwxN2AnYAqi5JZeu"
        "VDBn3rfZZLTaAuS8Y");
    ADD_MAP_VAL(desc, "@2",
        "[e1640396/48'/1'/0'/2']"
        "tpubDFgDvZifofePphQiVjLfkov8YTDg3UPuHRvt6LzbySYMZQhN19p6zvR7NTEXi1ZJAMNostHMTnz2sfXXYcJFQqtyCn"
        "NuUfgYqsahxTLGJq2");

    // Check parsing and key iteration
    ret = descriptor_get_signers("A0", &desc, TESTNET, NULL, NULL, 0, &num_signers, NULL, &errmsg);
    if (!ret) {
        FAIL();
    }
    if (num_signers != 3) {
        FAIL();
    }
    ret = descriptor_get_signers("A0", &desc, TESTNET, &desc.type, signers, 3, &num_signers, NULL, &errmsg);
    if (!ret) {
        FAIL();
    }
    if (desc.type != DESCRIPTOR_TYPE_MIXED) {
        FAIL();
    }
    if (num_signers != 3) {
        FAIL();
    }
    for (size_t i = 0; i < 3; ++i) {
        if (signers[i].derivation_len != 4 || signers[i].derivation[0] != harden(48)
            || signers[i].derivation[1] != harden(1) || signers[i].derivation[2] != harden(0)
            || signers[i].derivation[3] != harden(2)) {
            FAIL();
        }
        if (!signers[i].path_is_string || strcmp(signers[i].path_str, "<0;1>/*")) {
            FAIL();
        }
    }

    if (!FP_XPUB_MATCH(0, "1bf12fe0",
            "tpubDEHXLZfMAAM5duEnX6SSnZjGYbrxqXvRJmMxw8MFwr3gu4LC4DSxR9KVEfVDVcZxre4XL5tGcwVRrHwQ9euTMnSq6P6BqREemaqrFs"
            "C96Fy")) {
        FAIL();
    }
    if (!FP_XPUB_MATCH(1, "eda3d606",
            "tpubDEAmqvQkhqP6SbfbSPu3AeRR9kfHLFXYvNDiWashLy7V2zicg1YLg654AqfomsC6kFwTs4MpcnqwxN2AnYAqi5JZeuVDBn3rfZZLTa"
            "AuS8Y")) {
        FAIL();
    }
    if (!FP_XPUB_MATCH(2, "e1640396",
            "tpubDFgDvZifofePphQiVjLfkov8YTDg3UPuHRvt6LzbySYMZQhN19p6zvR7NTEXi1ZJAMNostHMTnz2sfXXYcJFQqtyCnNuUfgYqsahxT"
            "LGJq2")) {
        FAIL();
    }

    // Check scripts/addresses
    uint32_t multi_index = 0;
    const char* expectedA[2] = { "tb1qcf6egdkhq96vwkn4ge6fyz446zn09alwhuadcz8ezf6remuw7r7stzu9gj",
        "tb1qep0hehn3gl5nse6w5vyqe0g4q9czvhgr3nlzj76uhmfsxvqcz8pq2zyy4c" };

    for (uint32_t child_num = 0; child_num < 2; ++child_num) {
        char* addr = NULL;
        ret = descriptor_to_address("A1", &desc, TESTNET, multi_index, child_num, NULL, &addr, &errmsg);
        if (!ret || strcmp(addr, expectedA[child_num])) {
            wally_free_string(addr);
            FAIL();
        }
        wally_free_string(addr);

        // Pass unknown type - should give same answer
        desc.type = DESCRIPTOR_TYPE_UNKNOWN;
        ret = descriptor_to_address("A2", &desc, TESTNET, multi_index, child_num, NULL, &addr, &errmsg);
        if (!ret || strcmp(addr, expectedA[child_num])) {
            wally_free_string(addr);
            FAIL();
        }
        wally_free_string(addr);

        // Wrong network should fail
        ret = descriptor_to_address("A3", &desc, MAINNET, multi_index, child_num, NULL, &addr, &errmsg);
        if (ret) {
            FAIL();
        }

        // Wrong descriptor type should fail
        desc.type = DESCRIPTOR_TYPE_MINISCRIPT_ONLY;
        ret = descriptor_to_address("A4", &desc, TESTNET, multi_index, child_num, NULL, &addr, &errmsg);
        if (ret) {
            FAIL();
        }
        desc.type = DESCRIPTOR_TYPE_MIXED;
    }

    // Check serialisation
    if (!check_descriptor_serialisation(&desc)) {
        FAIL();
    }

    // Liana example
    INIT_DESC(desc, "wsh(or_d(multi(2,@0/<0;1>/*,@1/<0;1>/*),and_v(v:pkh(@2/<0;1>/*),older(100))))");
    ADD_MAP_VAL(desc, "@0",
        "[7897b5b3/48'/1'/0'/2']"
        "tpubDE8B47dY4JuGLnXVyDzG76UuhBM5hTjc6sXeJjG6ThbPsryiAnKqQY8CmxWcYjM6eVvkyH7CNTVrmPMxSWP9ZzCfHV"
        "Ho6preHp6Xhgd42JH");
    ADD_MAP_VAL(desc, "@1",
        "[1bf12fe0/48'/1'/0'/2']"
        "tpubDEHXLZfMAAM5duEnX6SSnZjGYbrxqXvRJmMxw8MFwr3gu4LC4DSxR9KVEfVDVcZxre4XL5tGcwVRrHwQ9euTMnSq6P"
        "6BqREemaqrFsC96Fy");
    ADD_MAP_VAL(desc, "@2",
        "[7897b5b3/48'/1'/1'/2']"
        "tpubDFf2ES1oUSZRgiCFT4mvBQ4jC2xTfRzVwfa6KewXZthgtL83UquqirWXzo1EKi4et3bx2wQz9QFKLDeu6vXoKpgQnJ"
        "HyV8DomjCjJRT3d57");

    // Check parsing and key iteration
    ret = descriptor_get_signers("B0", &desc, TESTNET, NULL, NULL, 0, &num_signers, NULL, &errmsg);
    if (!ret) {
        FAIL();
    }
    if (num_signers != 3) {
        FAIL();
    }
    ret = descriptor_get_signers("B0", &desc, TESTNET, &desc.type, signers, 3, &num_signers, NULL, &errmsg);
    if (!ret) {
        FAIL();
    }
    if (desc.type != DESCRIPTOR_TYPE_MIXED) {
        FAIL();
    }
    if (num_signers != 3) {
        FAIL();
    }
    for (size_t i = 0; i < 3; ++i) {
        if (signers[i].derivation_len != 4 || signers[i].derivation[0] != harden(48)
            || signers[i].derivation[1] != harden(1) || signers[i].derivation[2] != harden(i == 2 ? 1 : 0)
            || signers[i].derivation[3] != harden(2)) {
            FAIL();
        }
        if (!signers[i].path_is_string || strcmp(signers[i].path_str, "<0;1>/*")) {
            FAIL();
        }
    }

    if (!FP_XPUB_MATCH(0, "7897b5b3",
            "tpubDE8B47dY4JuGLnXVyDzG76UuhBM5hTjc6sXeJjG6ThbPsryiAnKqQY8CmxWcYjM6eVvkyH7CNTVrmPMxSWP9ZzCfHV"
            "Ho6preHp6Xhgd42JH")) {
        FAIL();
    }
    if (!FP_XPUB_MATCH(1, "1bf12fe0",
            "tpubDEHXLZfMAAM5duEnX6SSnZjGYbrxqXvRJmMxw8MFwr3gu4LC4DSxR9KVEfVDVcZxre4XL5tGcwVRrHwQ9euTMnSq6P"
            "6BqREemaqrFsC96Fy")) {
        FAIL();
    }
    if (!FP_XPUB_MATCH(2, "7897b5b3",
            "tpubDFf2ES1oUSZRgiCFT4mvBQ4jC2xTfRzVwfa6KewXZthgtL83UquqirWXzo1EKi4et3bx2wQz9QFKLDeu6vXoKpgQnJ"
            "HyV8DomjCjJRT3d57")) {
        FAIL();
    }

    // Check scripts/addresses
    const char* expectedB[2][2] = { { "tb1q0ddn2fn5y66gt2r69dv6el32lw44lupa2ry9enlm8zduxhpwk6aqen88zh",
                                        "tb1qfj66kfjk98cfcays67c9rvkzals7tnxz2dkxnwrjslwk5tzcd8ysgx9ahf" },
        { "tb1qu6j64q9kezc0dxgfl67fgnm2z9yycc55c0fa09uresf65w6py04s4qwgul",
            "tb1qkmr7qpxagfn7mafmsrt6e3qzzc599w28cl037cktjjegenfnhyysllxj5p" } };

    for (multi_index = 0; multi_index < 2; ++multi_index) {
        for (uint32_t child_num = 0; child_num < 2; ++child_num) {
            // Use unknown type
            char* addr = NULL;
            ret = descriptor_to_address("B1", &desc, TESTNET, multi_index, child_num, NULL, &addr, &errmsg);
            if (!ret || strcmp(addr, expectedB[multi_index][child_num])) {
                wally_free_string(addr);
                FAIL();
            }
            wally_free_string(addr);
        }
    }

    // Check serialisation
    if (!check_descriptor_serialisation(&desc)) {
        FAIL();
    }

    // Addresses should be same
    for (multi_index = 0; multi_index < 2; ++multi_index) {
        for (uint32_t child_num = 0; child_num < 2; ++child_num) {
            // Use unknown type
            char* addr = NULL;
            ret = descriptor_to_address("B2", &desc, TESTNET, multi_index, child_num, NULL, &addr, &errmsg);
            if (!ret || strcmp(addr, expectedB[multi_index][child_num])) {
                wally_free_string(addr);
                FAIL();
            }
            wally_free_string(addr);
        }
    }

    // Another Liana example - NOTE: reusing placeholder @0
    INIT_DESC(desc, "wsh(or_d(multi(2,@0/<0;1>/*,@1/<0;1>/*),and_v(v:pkh(@0/<2;3>/*),older(65535))))");
    ADD_MAP_VAL(desc, "@0",
        "[fb5d3ada/48'/1'/0'/2']"
        "tpubDFa4d4JXKYKrsyxkaxxk6QQscMo1bmwkczNWGKrwkPZiXSbwHueBEsS8Hq4RNTz2cm37MseAhzDRgyrmuaSDTtT6zi"
        "rPxsi8FTVUBY6FLiQ");
    ADD_MAP_VAL(desc, "@1",
        "[077ace32/48'/1'/0'/2']"
        "tpubDDzogRd3Gt71WEQavJggpR6R38iru9kC2uMkMcftBHLF8RzzPNSeZeTUoqvoa9xfXr2qeihmpysKbzwj6NmLbFQ9v2"
        "VHdMw7p8MnQycgAV8");

    // Check parsing and key iteration
    ret = descriptor_get_signers("b0", &desc, TESTNET, NULL, NULL, 0, &num_signers, NULL, &errmsg);
    if (!ret) {
        FAIL();
    }
    if (num_signers != 3) {
        FAIL();
    }
    ret = descriptor_get_signers("b0", &desc, TESTNET, &desc.type, signers, 3, &num_signers, NULL, &errmsg);
    if (!ret) {
        FAIL();
    }
    if (desc.type != DESCRIPTOR_TYPE_MIXED) {
        FAIL();
    }
    if (num_signers != 3) {
        FAIL();
    }
    for (size_t i = 0; i < 2; ++i) {
        if (signers[i].derivation_len != 4 || signers[i].derivation[0] != harden(48)
            || signers[i].derivation[1] != harden(1) || signers[i].derivation[2] != harden(0)
            || signers[i].derivation[3] != harden(2)) {
            FAIL();
        }
        if (!signers[i].path_is_string || strcmp(signers[i].path_str, i == 2 ? "<2;3>/*" : "<0;1>/*")) {
            FAIL();
        }
    }

    if (!FP_XPUB_MATCH(0, "fb5d3ada",
            "tpubDFa4d4JXKYKrsyxkaxxk6QQscMo1bmwkczNWGKrwkPZiXSbwHueBEsS8Hq4RNTz2cm37MseAhzDRgyrmuaSDTtT6zi"
            "rPxsi8FTVUBY6FLiQ")) {
        FAIL();
    }
    if (!FP_XPUB_MATCH(1, "077ace32",
            "tpubDDzogRd3Gt71WEQavJggpR6R38iru9kC2uMkMcftBHLF8RzzPNSeZeTUoqvoa9xfXr2qeihmpysKbzwj6NmLbFQ9v2"
            "VHdMw7p8MnQycgAV8")) {
        FAIL();
    }

    // Check scripts/addresses
    const char* expectedb[2][2] = { { "tb1q8uu3hj86chgu2zgpn4x32crv7cxl8skjdexue2ku2mjgwjvm7t3q4x2yxa",
                                        "tb1qf84tqnrcp36vtghf530406wyct00ns4jma3hw3ztfv5h98cka52qajm5v8" },
        { "tb1qx8zx3x9wf33uaar3ghvzy8y7l5wzh7e8vs5gthwvxr5z3x7ekuvsn8689q",
            "tb1q3a3k2m7qt3v05gar4kynpuu6xspqucet529mavjru4ca90ppayqqj0ad9f" } };

    for (multi_index = 0; multi_index < 2; ++multi_index) {
        for (uint32_t child_num = 0; child_num < 2; ++child_num) {
            // Use unknown type
            char* addr = NULL;
            ret = descriptor_to_address("b1", &desc, TESTNET, multi_index, child_num, NULL, &addr, &errmsg);
            if (!ret || strcmp(addr, expectedb[multi_index][child_num])) {
                wally_free_string(addr);
                FAIL();
            }
            wally_free_string(addr);
        }
    }

    // Check serialisation
    if (!check_descriptor_serialisation(&desc)) {
        FAIL();
    }

    // Addresses should be same
    for (multi_index = 0; multi_index < 2; ++multi_index) {
        for (uint32_t child_num = 0; child_num < 2; ++child_num) {
            // Use unknown type
            char* addr = NULL;
            ret = descriptor_to_address("b2", &desc, TESTNET, multi_index, child_num, NULL, &addr, &errmsg);
            if (!ret || strcmp(addr, expectedb[multi_index][child_num])) {
                wally_free_string(addr);
                FAIL();
            }
            wally_free_string(addr);
        }
    }

    // Another miniscript example... NOTE: not a 'wallet policy'
    INIT_DESC(desc,
        "sh(wsh(or_d(thresh(1,pk("
        "[7897b5b3/48'/1'/0'/2']"
        "tpubDE8B47dY4JuGLnXVyDzG76UuhBM5hTjc6sXeJjG6ThbPsryiAnKqQY8CmxWcYjM6eVvkyH7CNTVrmPMxSWP9ZzCfHV"
        "Ho6preHp6Xhgd42JH/0/*))"
        ",and_v(v:thresh(1,pk("
        "[1bf12fe0/48'/1'/0'/2']"
        "tpubDEHXLZfMAAM5duEnX6SSnZjGYbrxqXvRJmMxw8MFwr3gu4LC4DSxR9KVEfVDVcZxre4XL5tGcwVRrHwQ9euTMnSq6P"
        "6BqREemaqrFsC96Fy/0/*))"
        ",older(30)))))");

    // Check parsing and key iteration
    ret = descriptor_get_signers("C0", &desc, TESTNET, NULL, NULL, 0, &num_signers, NULL, &errmsg);
    if (!ret) {
        FAIL();
    }
    if (num_signers != 2) {
        FAIL();
    }
    ret = descriptor_get_signers("C0", &desc, TESTNET, &desc.type, signers, 3, &num_signers, NULL, &errmsg);
    if (!ret) {
        FAIL();
    }
    if (desc.type != DESCRIPTOR_TYPE_MIXED) {
        FAIL();
    }
    if (num_signers != 2) {
        FAIL();
    }
    for (size_t i = 0; i < 2; ++i) {
        if (signers[i].derivation_len != 4 || signers[i].derivation[0] != harden(48)
            || signers[i].derivation[1] != harden(1) || signers[i].derivation[2] != harden(0)
            || signers[i].derivation[3] != harden(2)) {
            FAIL();
        }
        if (!signers[i].path_is_string || strcmp(signers[i].path_str, "0/*")) {
            FAIL();
        }
    }

    if (!FP_XPUB_MATCH(0, "7897b5b3",
            "tpubDE8B47dY4JuGLnXVyDzG76UuhBM5hTjc6sXeJjG6ThbPsryiAnKqQY8CmxWcYjM6eVvkyH7CNTVrmPMxSWP9ZzCfHV"
            "Ho6preHp6Xhgd42JH")) {
        FAIL();
    }
    if (!FP_XPUB_MATCH(1, "1bf12fe0",
            "tpubDEHXLZfMAAM5duEnX6SSnZjGYbrxqXvRJmMxw8MFwr3gu4LC4DSxR9KVEfVDVcZxre4XL5tGcwVRrHwQ9euTMnSq6P"
            "6BqREemaqrFsC96Fy")) {
        FAIL();
    }

    // Check scripts/addresses
    const char* expectedC[2] = { "2MzcimvUcAwWKDDufQmTwq2FU4qenKL51fL", "2Mw3VJsaTKNCuZ2Drw3UMBvQg7QyHEvt8Nz" };

    multi_index = 0;
    for (uint32_t child_num = 0; child_num < 2; ++child_num) {
        // Use unknown type
        char* addr = NULL;
        ret = descriptor_to_address("C1", &desc, TESTNET, multi_index, child_num, NULL, &addr, &errmsg);
        if (!ret || strcmp(addr, expectedC[child_num])) {
            wally_free_string(addr);
            FAIL();
        }
        wally_free_string(addr);
    }

    // Check serialisation
    if (!check_descriptor_serialisation(&desc)) {
        FAIL();
    }

    // Addresses should be same
    multi_index = 0;
    for (uint32_t child_num = 0; child_num < 2; ++child_num) {
        // Use unknown type
        char* addr = NULL;
        const bool ret = descriptor_to_address("C2", &desc, TESTNET, multi_index, child_num, NULL, &addr, &errmsg);
        if (!ret || strcmp(addr, expectedC[child_num])) {
            wally_free_string(addr);
            FAIL();
        }
        wally_free_string(addr);
    }

    // Test Ledger issue - NOTE: not a 'wallet policy' and 'miniscript-only'
    INIT_DESC(desc,
        "and_b(pk([7897b5b3/48'/1'/0'/2']"
        "tpubDE8B47dY4JuGLnXVyDzG76UuhBM5hTjc6sXeJjG6ThbPsryiAnKqQY8CmxWcYjM6eVvkyH7CNTVrmPMxSWP9ZzCfHVHo6"
        "preHp6Xhgd42JH/0/*),a:1)");

    // Check parsing and key iteration
    ret = descriptor_get_signers("L0", &desc, TESTNET, NULL, NULL, 0, &num_signers, NULL, &errmsg);
    if (!ret) {
        FAIL();
    }
    if (num_signers != 1) {
        FAIL();
    }
    ret = descriptor_get_signers("L0", &desc, TESTNET, &desc.type, signers, 3, &num_signers, NULL, &errmsg);
    if (!ret) {
        FAIL();
    }
    if (desc.type != DESCRIPTOR_TYPE_MIXED) {
        FAIL();
    }
    if (num_signers != 1) {
        FAIL();
    }
    if (signers[0].derivation_len != 4 || signers[0].derivation[0] != harden(48)
        || signers[0].derivation[1] != harden(1) || signers[0].derivation[2] != harden(0)
        || signers[0].derivation[3] != harden(2)) {
        FAIL();
    }
    if (!signers[0].path_is_string || strcmp(signers[0].path_str, "0/*")) {
        FAIL();
    }

    if (!FP_XPUB_MATCH(0, "7897b5b3",
            "tpubDE8B47dY4JuGLnXVyDzG76UuhBM5hTjc6sXeJjG6ThbPsryiAnKqQY8CmxWcYjM6eVvkyH7CNTVrmPMxSWP9ZzCfHV"
            "Ho6preHp6Xhgd42JH")) {
        FAIL();
    }

    // The expected/correct script: <pubkey> OP_CHECKSIG OP_TOALTSTACK 1 OP_FROMALTSTACK OP_BOOLAND
    const char* expectedL[2] = { "2102afd5b7ce022720e0bbef9a34d2ed81196b700e5639229087c817119dd421483cac6b516c9a",
        "210272269b3301a5565a844c8a4d9940a70f0df0adaa1f2d7fd7cb20d8783d225501ac6b516c9a" };

    multi_index = 0;
    for (uint32_t child_num = 0; child_num < 2; ++child_num) {
        // Use unknown type
        uint8_t* script = NULL;
        size_t script_len = 0;
        ret = descriptor_to_script("L1", &desc, TESTNET, multi_index, child_num, NULL, &script, &script_len, &errmsg);
        if (!ret) {
            FAIL();
        }

        char* hex = NULL;
        JADE_WALLY_VERIFY(wally_hex_from_bytes(script, script_len, &hex));
        if (!hex || strcmp(hex, expectedL[child_num])) {
            free(hex);
            free(script);
            FAIL();
        }

        free(hex);
        free(script);
    }

    // Check serialisation
    if (!check_descriptor_serialisation(&desc)) {
        FAIL();
    }

    // Liquid example
    if (!descriptor_allow_liquid()) {
        return true; // Skip test if Liquid support not enabled
    }
    for (int i = 0; i < 2; i++) {
        INIT_DESC(desc, "ct(slip77(@B),wpkh(@0/<0;1>/*))");
        ADD_MAP_VAL(desc, "@B", "afacc503637e85da661ca1706c4ea147f1407868c48d8f92dd339ac272293cdc");
        ADD_MAP_VAL(desc, "@0",
            "[e3ebcc79/84'/1'/0']"
            "tpubDC2Q4xK4XH72Gae41fkTzHPCugZXvDDZJwEDzVT5osM5ejQULHVtUhbjFqUBqTnMX5RVwtkstpBvjdznWG89y3Kt48"
            "4Ta3qxTFPPWoyTh8s");
        bool get_bk = i;
        char* blinding_key = NULL;
        ret = descriptor_get_signers(
            "LQ0", &desc, LTESTNET, NULL, NULL, 0, &num_signers, get_bk ? &blinding_key : NULL, &errmsg);
        if (!ret) {
            FAIL();
        }
        if (num_signers != 1) {
            FAIL();
        }
        if (get_bk && blinding_key) {
            FAIL(); // Blinding key should not be returned when testing number of signers
        }
        ret = descriptor_get_signers(
            "LQ0", &desc, LTESTNET, &desc.type, signers, 3, &num_signers, get_bk ? &blinding_key : NULL, &errmsg);
        if (!ret) {
            FAIL();
        }
        if (desc.type != DESCRIPTOR_TYPE_MIXED) {
            FAIL();
        }
        if (num_signers != 1) {
            FAIL();
        }
        if (get_bk && !blinding_key) {
            FAIL(); // Blinding key should be returned
        }
        if (get_bk && strcmp(blinding_key, "afacc503637e85da661ca1706c4ea147f1407868c48d8f92dd339ac272293cdc")) {
            FAIL(); // Blinding key should match expected value
        }
        if (blinding_key) {
            wally_free_string(blinding_key);
        }

        if (signers[0].derivation_len != 3 || signers[0].derivation[0] != harden(84)
            || signers[0].derivation[1] != harden(1) || signers[0].derivation[2] != harden(0)) {
            FAIL();
        }
        if (!signers[0].path_is_string || strcmp(signers[0].path_str, "<0;1>/*")) {
            FAIL();
        }

        if (!FP_XPUB_MATCH(0, "e3ebcc79",
                "tpubDC2Q4xK4XH72Gae41fkTzHPCugZXvDDZJwEDzVT5osM5ejQULHVtUhbjFqUBqTnMX5RVwtkstpBvjdznWG89y3Kt48"
                "4Ta3qxTFPPWoyTh8s")) {
            FAIL();
        }

        // The expected/correct scriptPubkey
        const char* expectedLQ[2]
            = { "00142aca142db0227433da1e44ccc914b0cf6ad8cf72", "00144ae0383771d8140d4ce37cdfe08f0728b5c599ed" };

        multi_index = 0;
        for (uint32_t child_num = 0; child_num < 2; ++child_num) {
            // Use unknown type
            uint8_t* script = NULL;
            size_t script_len = 0;
            ret = descriptor_to_script(
                "LQ0", &desc, LTESTNET, multi_index, child_num, NULL, &script, &script_len, &errmsg);
            if (!ret) {
                FAIL();
            }

            char* hex = NULL;
            JADE_WALLY_VERIFY(wally_hex_from_bytes(script, script_len, &hex));
            if (!hex || strcmp(hex, expectedLQ[child_num])) {
                free(hex);
                free(script);
                FAIL();
            }

            free(hex);
            free(script);
        }

        if (!check_descriptor_serialisation(&desc)) {
            FAIL();
        }
    }

    return true;
}

// Task 5 fixture: device A m/48h/0h/0h/2h + BIP32 TV1 m/0H/1/2H/2, 1-of-2 wsh(sortedmulti)
static const char CANONICAL_TEXT[]
    = "wsh(sortedmulti(1,[73c5da0a/48h/0h/0h/2h]xpub6DkFAXWQ2dHxq2vatrt9qyA3bXYU4ToWQwCHbf5XB2mSTexcHZCeKS1VZYcPoBd5X8yV"
      "cbXFHJR9R8UCVpt82VX1VhR28mCyxUFL4r6KFrf/<0;1>/*,[3442193e/0h/1/2h/2]xpub6FHa3pjLCk84BayeJxFW2SP4XRrFd1JYnxeLeU8Eq"
      "N3vDfZmbqBqaGJAyiLjTAwm6ZLRQUMv1ZACTj37sR62cfN7fe5JnJ7dh8zL4fiyLHV/<0;1>/*))";
static const char EXPECTED_SCRIPT[] = "wsh(sortedmulti(1,@0/<0;1>/*,@1/<0;1>/*))";
static const char EXPECTED_V0[] = "[73c5da0a/48h/0h/0h/2h]xpub6DkFAXWQ2dHxq2vatrt9qyA3bXYU4ToWQwCHbf5XB2mSTexcHZCeKS1VZYcPoBd5X8yV"
                                  "cbXFHJR9R8UCVpt82VX1VhR28mCyxUFL4r6KFrf";
static const char EXPECTED_V1[] = "[3442193e/0h/1/2h/2]xpub6FHa3pjLCk84BayeJxFW2SP4XRrFd1JYnxeLeU8EqN3vDfZmbqBqaGJAyiLjTAwm6ZLRQ"
                                  "UMv1ZACTj37sR62cfN7fe5JnJ7dh8zL4fiyLHV";
// Task 5 '--hex' output (single / children / bad)
static const char UR_SINGLE_HEX[]
    = "d90191d90197a201010282d9012fa5035821021a3bf5fbf737d0f36993fd46dc4913093beb532d654fe0dfd98bd27585dc9f"
    "29045820bba0c7ca160a870efeb940ab90d0f4284fea1b5e0d2117677e823fc37e2d576305d90131a20100020006d90130a3"
    "01881830f500f500f502f5021a73c5da0a0304081a1cf29716d9012fa503582102e8445082a72f29b75ca48748a914df6062"
    "2a609cacfce8ed0e35804560741d29045820cfb71883f01676f587d023cc53a35bc7f88f724b1f8c2892ac1275ac822a3edd"
    "05d90131a20100020006d90130a3018800f501f402f502f4021a3442193e0304081aee7ab90c";
// BBB-AIRGAP: B3, the first key has no origin components: depth 0, no parent fingerprint.
static const char UR_EMPTY_ORIGIN_HEX[]
    = "d90191d90197a201010282d9012fa4035821021a3bf5fbf737d0f36993fd46dc4913093beb532d654fe0dfd98bd27585dc9f"
    "29045820bba0c7ca160a870efeb940ab90d0f4284fea1b5e0d2117677e823fc37e2d576305d90131a20100020006d90130a3"
    "0180021a73c5da0a0300d9012fa503582102e8445082a72f29b75ca48748a914df60622a609cacfce8ed0e35804560741d29"
    "045820cfb71883f01676f587d023cc53a35bc7f88f724b1f8c2892ac1275ac822a3edd05d90131a20100020006d90130a301"
    "8800f501f402f502f4021a3442193e0304081aee7ab90c";
static const char UR_CHILDREN_HEX[]
    = "d90191d90197a201010282d9012fa6035821021a3bf5fbf737d0f36993fd46dc4913093beb532d654fe0dfd98bd27585dc9f"
    "29045820bba0c7ca160a870efeb940ab90d0f4284fea1b5e0d2117677e823fc37e2d576305d90131a20100020006d90130a3"
    "01881830f500f500f502f5021a73c5da0a0304081a1cf2971607d90130a101838400f401f480f4d9012fa603582102e84450"
    "82a72f29b75ca48748a914df60622a609cacfce8ed0e35804560741d29045820cfb71883f01676f587d023cc53a35bc7f88f"
    "724b1f8c2892ac1275ac822a3edd05d90131a20100020006d90130a3018800f501f402f502f4021a3442193e0304081aee7a"
    "b90c07d90130a101838400f401f480f4";
static const char UR_BAD_HEX[]
    = "d90191d90197a201010282d9012fa6035821021a3bf5fbf737d0f36993fd46dc4913093beb532d654fe0dfd98bd27585dc9f"
    "29045820bba0c7ca160a870efeb940ab90d0f4284fea1b5e0d2117677e823fc37e2d576305d90131a20100020006d90130a3"
    "01881830f500f500f502f5021a73c5da0a0304081a1cf2971607d90130a10184820005f480f4d9012fa603582102e8445082"
    "a72f29b75ca48748a914df60622a609cacfce8ed0e35804560741d29045820cfb71883f01676f587d023cc53a35bc7f88f72"
    "4b1f8c2892ac1275ac822a3edd05d90131a20100020006d90130a3018800f501f402f502f4021a3442193e0304081aee7ab9"
    "0c07d90130a10184820005f480f4";
// Codex Gate G finding 1: the use-info was not being read to the end.  Three variants off the same
// single-signature fixture: registry tag 40305 + network 1, a non-Bitcoin coin type, an unknown tag.
static const char UR_USEINFO_NEW_TESTNET_HEX[]
    = "d90191d90197a201010282d9012fa5035821021a3bf5fbf737d0f36993fd46dc4913093beb532d654fe0dfd98bd27585"
    "dc9f29045820bba0c7ca160a870efeb940ab90d0f4284fea1b5e0d2117677e823fc37e2d576305d99d71a20100020106"
    "d90130a301881830f500f500f502f5021a73c5da0a0304081a1cf29716d9012fa503582102e8445082a72f29b75ca487"
    "48a914df60622a609cacfce8ed0e35804560741d29045820cfb71883f01676f587d023cc53a35bc7f88f724b1f8c2892"
    "ac1275ac822a3edd05d99d71a20100020106d90130a3018800f501f402f502f4021a3442193e0304081aee7ab90c";
static const char UR_USEINFO_ALTCOIN_HEX[]
    = "d90191d90197a201010282d9012fa5035821021a3bf5fbf737d0f36993fd46dc4913093beb532d654fe0dfd98bd27585"
    "dc9f29045820bba0c7ca160a870efeb940ab90d0f4284fea1b5e0d2117677e823fc37e2d576305d90131a201183c0200"
    "06d90130a301881830f500f500f502f5021a73c5da0a0304081a1cf29716d9012fa503582102e8445082a72f29b75ca4"
    "8748a914df60622a609cacfce8ed0e35804560741d29045820cfb71883f01676f587d023cc53a35bc7f88f724b1f8c28"
    "92ac1275ac822a3edd05d90131a201183c020006d90130a3018800f501f402f502f4021a3442193e0304081aee7ab90c";
static const char UR_USEINFO_BADTAG_HEX[]
    = "d90191d90197a201010282d9012fa5035821021a3bf5fbf737d0f36993fd46dc4913093beb532d654fe0dfd98bd27585"
    "dc9f29045820bba0c7ca160a870efeb940ab90d0f4284fea1b5e0d2117677e823fc37e2d576305d903e7a20100020006"
    "d90130a301881830f500f500f502f5021a73c5da0a0304081a1cf29716d9012fa503582102e8445082a72f29b75ca487"
    "48a914df60622a609cacfce8ed0e35804560741d29045820cfb71883f01676f587d023cc53a35bc7f88f724b1f8c2892"
    "ac1275ac822a3edd05d903e7a20100020006d90130a3018800f501f402f502f4021a3442193e0304081aee7ab90c";

// BBB-AIRGAP: C1, the first key carries the text; the coin type and network it declares cannot be
// ignored.
static const char UR_USEINFO_TEXT_FIRST_HEX[]
    = "d90191d90197a201010282d9012fa5035821021a3bf5fbf737d0f36993fd46dc4913093beb532d654fe0dfd98bd27585"
    "dc9f29045820bba0c7ca160a870efeb940ab90d0f4284fea1b5e0d2117677e823fc37e2d576305d90131a36178000118"
    "3c020106d90130a301881830f500f500f502f5021a73c5da0a0304081a1cf29716d9012fa503582102e8445082a72f29"
    "b75ca48748a914df60622a609cacfce8ed0e35804560741d29045820cfb71883f01676f587d023cc53a35bc7f88f724b"
    "1f8c2892ac1275ac822a3edd05d90131a20100020006d90130a3018800f501f402f502f4021a3442193e0304081aee7a"
    "b90c";
// BBB-AIRGAP: C1, a repeated key; matching the first one must not hide what the second declares.
static const char UR_USEINFO_DUPLICATE_HEX[]
    = "d90191d90197a201010282d9012fa5035821021a3bf5fbf737d0f36993fd46dc4913093beb532d654fe0dfd98bd27585"
    "dc9f29045820bba0c7ca160a870efeb940ab90d0f4284fea1b5e0d2117677e823fc37e2d576305d90131a3010001183c"
    "020006d90130a301881830f500f500f502f5021a73c5da0a0304081a1cf29716d9012fa503582102e8445082a72f29b7"
    "5ca48748a914df60622a609cacfce8ed0e35804560741d29045820cfb71883f01676f587d023cc53a35bc7f88f724b1f"
    "8c2892ac1275ac822a3edd05d90131a20100020006d90130a3018800f501f402f502f4021a3442193e0304081aee7ab9"
    "0c";
// BBB-AIRGAP: C1, the last key carries the text; earlier matches must not hide the bad one.
static const char UR_USEINFO_TEXT_LAST_HEX[]
    = "d90191d90197a201010282d9012fa5035821021a3bf5fbf737d0f36993fd46dc4913093beb532d654fe0dfd98bd27585"
    "dc9f29045820bba0c7ca160a870efeb940ab90d0f4284fea1b5e0d2117677e823fc37e2d576305d90131a30100020061"
    "780006d90130a301881830f500f500f502f5021a73c5da0a0304081a1cf29716d9012fa503582102e8445082a72f29b7"
    "5ca48748a914df60622a609cacfce8ed0e35804560741d29045820cfb71883f01676f587d023cc53a35bc7f88f724b1f"
    "8c2892ac1275ac822a3edd05d90131a20100020006d90130a3018800f501f402f502f4021a3442193e0304081aee7ab9"
    "0c";

static bool check_parsed(const descriptor_data_t* desc, const char* name, const char* expected_name)
{
    if (strcmp(desc->script, EXPECTED_SCRIPT) || desc->num_values != 2 || strcmp(desc->values[0].key, "@0")
        || strcmp(desc->values[0].value, EXPECTED_V0) || strcmp(desc->values[1].key, "@1")
        || strcmp(desc->values[1].value, EXPECTED_V1)) {
        FAIL();
    }
    if (expected_name && strcmp(name, expected_name)) {
        FAIL();
    }
    return true;
}

static bool test_descriptor_text(void)
{
    descriptor_data_t* const desc = JADE_MALLOC(sizeof(descriptor_data_t));
    char name[MAX_DESCRIPTOR_NAME_SIZE];
    char hash_name[MAX_DESCRIPTOR_NAME_SIZE];
    const char* errmsg = NULL;
    char text[1024];

    // BBB-AIRGAP: 1. A real Sparrow checksum is verified; the name is 'desc-' plus a digest.
    snprintf(text, sizeof(text), "%s#xs3zjaxw", CANONICAL_TEXT);
    if (!descriptor_text_is_descriptor(text, strlen(text))
        || !descriptor_text_parse(text, strlen(text), desc, name, sizeof(name), &errmsg) || !check_parsed(desc, name, NULL)
        || strncmp(name, "desc-", 5) || strlen(name) != 13) {
        FAIL();
    }
    strcpy(hash_name, name);

    // BBB-AIRGAP: B1, change the child path but keep the old checksum and the text must be refused.
    snprintf(text, sizeof(text), "wsh(sortedmulti(1,%s/<0;1>/*,%s/<2;3>/*))#xs3zjaxw", EXPECTED_V0, EXPECTED_V1);
    if (descriptor_text_parse(text, strlen(text), desc, name, sizeof(name), &errmsg)
        || !errmsg || strcmp(errmsg, "Invalid descriptor")) {
        FAIL();
    }

    // BBB-AIRGAP: 2. Once the Specter JSON escapes are undone, the checksum is verified against the
    // original {0,1} path and ' hardening; the label and the canonical body survive.
    snprintf(text, sizeof(text),
        "{\"label\": \"Vault One\", \"blockheight\": 0, \"descriptor\": "
        "\"wsh(sortedmulti(1,[73C5DA0A\\/48'\\/0'\\/0'\\/2']%s\\/{0,1}\\/*,[3442193e\\/0'\\/1\\/2'\\/2]%s\\/{0,1}\\/*))#pdrq4248\"}",
        EXPECTED_V0 + 23, EXPECTED_V1 + 20);
    if (!descriptor_text_is_descriptor(text, strlen(text))
        || !descriptor_text_parse(text, strlen(text), desc, name, sizeof(name), &errmsg)
        || !check_parsed(desc, name, "Vault_One")) {
        FAIL();
    }

    // BBB-AIRGAP: C2, a label whose value is "descriptor" must not hide the real descriptor member.
    snprintf(text, sizeof(text),
        "{\"label\": \"descriptor\", \"blockheight\": 0, \"descriptor\": "
        "\"wsh(sortedmulti(1,[73C5DA0A\\/48'\\/0'\\/0'\\/2']%s\\/{0,1}\\/*,[3442193e\\/0'\\/1\\/2'\\/2]%s\\/{0,1}\\/*))#pdrq4248\"}",
        EXPECTED_V0 + 23, EXPECTED_V1 + 20);
    if (!descriptor_text_is_descriptor(text, strlen(text))
        || !descriptor_text_parse(text, strlen(text), desc, name, sizeof(name), &errmsg)
        || !check_parsed(desc, name, "descriptor")) {
        FAIL();
    }

    // BBB-AIRGAP: C2, members in reverse order, and a number ahead of the label, are still accepted.
    snprintf(text, sizeof(text),
        "{\"descriptor\": "
        "\"wsh(sortedmulti(1,[73C5DA0A\\/48'\\/0'\\/0'\\/2']%s\\/{0,1}\\/*,[3442193e\\/0'\\/1\\/2'\\/2]%s\\/{0,1}\\/*))#pdrq4248\", "
        "\"blockheight\": 0, \"label\": \"Vault One\"}",
        EXPECTED_V0 + 23, EXPECTED_V1 + 20);
    if (!descriptor_text_is_descriptor(text, strlen(text))
        || !descriptor_text_parse(text, strlen(text), desc, name, sizeof(name), &errmsg)
        || !check_parsed(desc, name, "Vault_One")) {
        FAIL();
    }

    // BBB-AIRGAP: C2, an escape we do not support in the label does not block the record; the digest
    // name is used instead.
    snprintf(text, sizeof(text),
        "{\"label\": \"Vault\\u0020One\", \"descriptor\": "
        "\"wsh(sortedmulti(1,[73C5DA0A\\/48'\\/0'\\/0'\\/2']%s\\/{0,1}\\/*,[3442193e\\/0'\\/1\\/2'\\/2]%s\\/{0,1}\\/*))#pdrq4248\"}",
        EXPECTED_V0 + 23, EXPECTED_V1 + 20);
    if (!descriptor_text_is_descriptor(text, strlen(text))
        || !descriptor_text_parse(text, strlen(text), desc, name, sizeof(name), &errmsg)
        || !check_parsed(desc, name, NULL)) {
        FAIL();
    }

    // 3. '/**' shorthand and whitespace; hash name equals case 1
    snprintf(text, sizeof(text), "wsh( sortedmulti( 1, %s/**, %s/** ) )", EXPECTED_V0, EXPECTED_V1);
    if (!descriptor_text_parse(text, strlen(text), desc, name, sizeof(name), &errmsg)
        || !check_parsed(desc, name, hash_name)) {
        FAIL();
    }

    // 4. Single-path child ('/0/*') is unsupported; a key without origin is unsupported
    snprintf(text, sizeof(text), "wsh(sortedmulti(1,%s/0/*,%s/<0;1>/*))", EXPECTED_V0, EXPECTED_V1);
    if (descriptor_text_parse(text, strlen(text), desc, name, sizeof(name), &errmsg)
        || strcmp(errmsg, "Unsupported descriptor")) {
        FAIL();
    }
    snprintf(text, sizeof(text), "wsh(sortedmulti(1,%s/<0;1>/*,%s/<0;1>/*))", EXPECTED_V0 + 23, EXPECTED_V1);
    if (descriptor_text_parse(text, strlen(text), desc, name, sizeof(name), &errmsg)
        || strcmp(errmsg, "Unsupported descriptor")) {
        FAIL();
    }

    // 4b. Child indices outside the unhardened range are refused, not wrapped.  '4294967296' is ten
    // digits, so it passed the digit guard and a uint32_t accumulator turned it into 0, quietly
    // registering a different valid descriptor; '2147483648' is the first hardened index.
    const char* const bad_children[] = { "/<4294967296;1>/*", "/<0;4294967296>/*", "/<2147483648;1>/*" };
    for (size_t i = 0; i < 3; ++i) {
        snprintf(text, sizeof(text), "wsh(sortedmulti(1,%s%s,%s/<0;1>/*))", EXPECTED_V0, bad_children[i],
            EXPECTED_V1);
        if (descriptor_text_parse(text, strlen(text), desc, name, sizeof(name), &errmsg)) {
            JADE_LOGE("child index %s was accepted", bad_children[i]);
            FAIL();
        }
    }

    // 5. Not a descriptor: a multisig file and a mnemonic-looking string
    if (descriptor_text_is_descriptor("Name: x\nPolicy: 1 of 2\n", 22) || descriptor_text_is_descriptor("abandon", 7)) {
        FAIL();
    }

    free(desc);
    return true;
}

static bool test_crypto_output(void)
{
    uint8_t cbor[1024];
    size_t cbor_len = 0;
    const size_t text_len = MAX_DESCRIPTOR_SCRIPT_LEN + (MAX_ALLOWED_SIGNERS * 160);
    char* const text = JADE_MALLOC(text_len);
    const char* errmsg = NULL;

    // single (no children) and children '<0;1>/*' both yield the canonical text
    const char* const same[] = { UR_SINGLE_HEX, UR_CHILDREN_HEX };
    for (size_t i = 0; i < 2; ++i) {
        JADE_WALLY_VERIFY(wally_hex_to_bytes(same[i], cbor, sizeof(cbor), &cbor_len));
        if (!bcur_parse_crypto_output(cbor, cbor_len, text, text_len, &errmsg)) {
            JADE_LOGE("crypto-output %u not parsed: %s", i, errmsg ? errmsg : "no detail");
            free(text);
            FAIL();
        }
        if (strcmp(text, CANONICAL_TEXT)) {
            JADE_LOGE("crypto-output %u text mismatch:\n got: %s\nwant: %s", i, text, CANONICAL_TEXT);
            free(text);
            FAIL();
        }
    }

    // BBB-AIRGAP: B3, an empty origin path is written without the slash; '[fp/]' is never produced.
    JADE_WALLY_VERIFY(wally_hex_to_bytes(UR_EMPTY_ORIGIN_HEX, cbor, sizeof(cbor), &cbor_len));
    if (!bcur_parse_crypto_output(cbor, cbor_len, text, text_len, &errmsg)
        || !strstr(text, "[73c5da0a]") || strstr(text, "[73c5da0a/]")) {
        free(text);
        FAIL();
    }

    // range component in children is refused
    JADE_WALLY_VERIFY(wally_hex_to_bytes(UR_BAD_HEX, cbor, sizeof(cbor), &cbor_len));
    if (bcur_parse_crypto_output(cbor, cbor_len, text, text_len, &errmsg) || strcmp(errmsg, "Unsupported descriptor")) {
        free(text);
        FAIL();
    }

    // use-info under the registered tag 40305 is read, not ignored: network 1 has to reach the key
    // version.  Before the fix the tag was skipped over, the value stayed tagged, no network was
    // found and the key was rebuilt as a mainnet xpub - a testnet QR registered as Bitcoin mainnet.
    JADE_WALLY_VERIFY(wally_hex_to_bytes(UR_USEINFO_NEW_TESTNET_HEX, cbor, sizeof(cbor), &cbor_len));
    if (!bcur_parse_crypto_output(cbor, cbor_len, text, text_len, &errmsg) || !strstr(text, "tpub")
        || strstr(text, "xpub")) {
        JADE_LOGE("crypto-output use-info 40305 testnet: %s", errmsg ? errmsg : text);
        free(text);
        FAIL();
    }

    // a declared non-Bitcoin coin type, and a use-info under a tag we do not know, are both refused
    // rather than silently taken as Bitcoin mainnet
    // BBB-AIRGAP: C1, every one of the bad maps is refused with exactly "Unsupported descriptor".
    const char* const bad_useinfo[] = { UR_USEINFO_ALTCOIN_HEX, UR_USEINFO_BADTAG_HEX, UR_USEINFO_TEXT_FIRST_HEX,
        UR_USEINFO_DUPLICATE_HEX, UR_USEINFO_TEXT_LAST_HEX };
    for (size_t i = 0; i < 5; ++i) {
        JADE_WALLY_VERIFY(wally_hex_to_bytes(bad_useinfo[i], cbor, sizeof(cbor), &cbor_len));
        if (bcur_parse_crypto_output(cbor, cbor_len, text, text_len, &errmsg)
            || strcmp(errmsg, "Unsupported descriptor")) {
            JADE_LOGE("crypto-output bad use-info %u was accepted", (unsigned)i);
            free(text);
            FAIL();
        }
    }

    // not an output at all (a bare byte string)
    const uint8_t bytes_cbor[] = { 0x43, 0x01, 0x02, 0x03 };
    if (bcur_parse_crypto_output(bytes_cbor, sizeof(bytes_cbor), text, text_len, &errmsg)) {
        free(text);
        FAIL();
    }

    free(text);
    return true;
}

static bool test_version_gate(void)
{
    const uint8_t body[] = { 0x01, 0x02, 0x03, 0x04, 0x05 };
    uint8_t sealed[REGISTRATION_SEALED_LEN(sizeof(body))];
    uint8_t opened[32];
    size_t written = 0;
    if (!registration_seal(0, body, sizeof(body), sealed, sizeof(sealed))) {
        FAIL();
    }
    if (descriptor_open_registration(sealed, sizeof(sealed), opened, sizeof(opened), &written)) {
        FAIL(); // version 0 must be refused by the v1 reader
    }
    if (!registration_open(0, sealed, sizeof(sealed), opened, sizeof(opened), &written) || written != sizeof(body)
        || memcmp(opened, body, sizeof(body))) {
        FAIL();
    }
    return true;
}

bool debug_selfcheck(jade_process_t* process)
{
    // BBB-AIRGAP: the local runner requires a marker unique to this selected selfcheck because the
    // default debug_selfcheck also succeeds and therefore cannot prove descriptor.c was linked.
    JADE_LOGI("Testing miniscript descriptors");

    if (!selfcheck_set_test_mnemonic(process, true, true)) {
        FAIL();
    }

    if (!test_miniscript_descriptors() || !test_descriptor_text() || !test_crypto_output() || !test_version_gate()) {
        FAIL();
    }

    return true;
}

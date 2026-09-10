// Decode a UR string with the Jade bc-ur decoder; print type= and cbor_hex=.
// Build and run (jade-dev container): pijade/UPSTREAM.md section 25.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "cdecoder.h"

int main(int argc, char* argv[])
{
    if (argc != 2) {
        puts("UR_DECODE_FAILED");
        return 1;
    }

    void* decoder = NULL;
    urcreate_decoder(&decoder);
    if (!decoder || !urreceive_part_decoder(decoder, argv[1]) || !uris_success_decoder(decoder)) {
        urfree_decoder(decoder);
        puts("UR_DECODE_FAILED");
        return 1;
    }

    uint8_t* result = NULL;
    size_t result_len = 0;
    const char* type = NULL;
    urresult_ur_decoder(decoder, &result, &result_len, &type);
    if (!type || !result || !result_len) {
        urfree_decoder(decoder);
        puts("UR_DECODE_FAILED");
        return 1;
    }

    printf("type=%s\n", type);
    fputs("cbor_hex=", stdout);
    for (size_t i = 0; i < result_len; ++i) {
        printf("%02x", result[i]);
    }
    putchar('\n');

    urfree_decoder(decoder);
    return 0;
}

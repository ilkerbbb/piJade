// BBB-AIRGAP: measures what bc-ur actually emits for a given max-fragment-size, so the QR version
// can be chosen from measurement rather than from the conservative BCUR_MAX_FRAGMENT_SIZE() macro.
// Written for T3.8: a reference device (SeedSigner) reads a 29-module code in the same room where
// a 33-module one fails, so the question is which fragment size fits version 3's 77-char capacity.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#define URENCODER_SIZE 512
extern void urcreate_placement_encoder(uint8_t* encoder, size_t encoder_len, const char* type,
    const uint8_t* payload, size_t payload_len, size_t max_fragment_len, uint32_t first_seq, size_t min_fragment_len);
extern size_t urseqlen_encoder(const uint8_t* encoder);
extern void urnext_part_encoder(uint8_t* encoder, bool uppercase, char** out);
extern void urfree_encoded_encoder(char* str);
extern void urfree_placement_encoder(uint8_t* encoder);

#define CAP_V2 47
#define CAP_V3 77
#define CAP_V4 114

int main(int argc, char** argv)
{
    const size_t payload_len = argc > 1 ? (size_t)atoi(argv[1]) : 100;
    const char* type = argc > 2 ? argv[2] : "crypto-account";

    uint8_t* payload = malloc(payload_len);
    for (size_t i = 0; i < payload_len; ++i) {
        payload[i] = (uint8_t)(i * 7 + 13);
    }

    printf("payload %zu bytes, type %s\n", payload_len, type);
    printf("%-6s %-6s %-8s %-8s %-6s %-6s %-6s\n", "frag", "parts", "first", "max", "v2?", "v3?", "v4?");

    for (size_t maxfrag = 9; maxfrag <= 26; ++maxfrag) {
        uint8_t encoder[URENCODER_SIZE];
        urcreate_placement_encoder(encoder, sizeof(encoder), type, payload, payload_len, maxfrag, 0, 8);
        const size_t pure = urseqlen_encoder(encoder);
        const size_t total = pure <= 300 ? 4 * pure / 3 : pure + 100;

        size_t first_len = 0, max_len = 0;
        for (size_t i = 0; i < total; ++i) {
            char* frag = NULL;
            urnext_part_encoder(encoder, true, &frag);
            const size_t l = strlen(frag);
            if (i == 0) {
                first_len = l;
            }
            if (l > max_len) {
                max_len = l;
            }
            urfree_encoded_encoder(frag);
        }
        urfree_placement_encoder(encoder);

        // margin of 4 chars, same rule bcur_check_fragment_sizes() applies upstream
        printf("%-6zu %-6zu %-8zu %-8zu %-6s %-6s %-6s\n", maxfrag, total, first_len, max_len,
            max_len + 4 <= CAP_V2 ? "YES" : "-", max_len + 4 <= CAP_V3 ? "YES" : "-",
            max_len + 4 <= CAP_V4 ? "YES" : "-");
    }
    free(payload);
    return 0;
}

/* T3.19B preliminary measurement: does qrcode_initText encode 48 digits in v2 and 96 in v3?
   Empirically test the claim derived from the capacity table. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../main/qrcode.h"

static int probe(uint8_t version, size_t ndigits)
{
    char text[128];
    memset(text, '0', sizeof(text));
    /* A realistic string: varying digits instead of all zeros */
    for (size_t i = 0; i < ndigits; ++i) { text[i] = (char)('0' + (i * 7 + 3) % 10); }
    text[ndigits] = '\0';

    uint8_t buf[256];
    const uint16_t need = qrcode_getBufferSize(version);
    QRCode qr;
    const int r = qrcode_initText(&qr, buf, version, ECC_LOW, text);
    printf("v%u, %3zu digits -> initText=%d, buffer required=%u bytes, code size=%u\n",
           version, ndigits, r, need, r == 0 ? qr.size : 0);
    return r;
}

int main(void)
{
    int fail = 0;
    if (probe(2, 48) != 0) { printf("  ERROR: v2 should accept 48 digits\n"); ++fail; }
    if (probe(2, 96) == 0) { printf("  ERROR: v2 SHOULD NOT accept 96 digits\n"); ++fail; }
    if (probe(3, 96) != 0) { printf("  ERROR: v3 should accept 96 digits\n"); ++fail; }
    if (probe(1, 48) == 0) { printf("  ERROR: v1 SHOULD NOT accept 48 digits\n"); ++fail; }
    printf("\n%d failure(s)\n", fail);
    return fail != 0;
}

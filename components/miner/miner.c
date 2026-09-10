/* BBB-AIRGAP: upstream's '#define NDEBUG' on this line was removed. It never took effect in the
 * libjade unity build (<assert.h> is already included by the time it is read, measured in M3), so
 * it only changed the standalone device build, where it turned the asserts below into nothing and
 * left internal-invariant failures - a task that could not be created, say - silent. The production
 * armv6 image is built Release and gets -DNDEBUG from the compiler anyway (pijade/images/
 * build-armv6.sh, libjade/CMakeLists.txt), so production behaviour is unchanged; what this removes
 * is the case where the same fault behaved differently in two builds of the same file. */
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <wally_address.h>
#include <wally_transaction.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <esp_idf_version.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "miner.h"
#include <math.h>
#include <string.h>

#define HASH_SIZE 32

#define ROTR(x, n) ((x >> n) | (x << ((sizeof(x) << 3) - n)))

#ifndef PUT_UINT32_BE
#define PUT_UINT32_BE(n, data, offset)                                                                                 \
    {                                                                                                                  \
        u.num = n;                                                                                                     \
        p = (data) + (offset);                                                                                         \
        *p = u.b[3];                                                                                                   \
        *(p + 1) = u.b[2];                                                                                             \
        *(p + 2) = u.b[1];                                                                                             \
        *(p + 3) = u.b[0];                                                                                             \
    }
#endif

#ifndef GET_UINT32_BE
#define GET_UINT32_BE(b, i)                                                                                            \
    (((uint32_t)(b)[(i)] << 24) | ((uint32_t)(b)[(i) + 1] << 16) | ((uint32_t)(b)[(i) + 2] << 8)                       \
        | ((uint32_t)(b)[(i) + 3]))
#endif

DRAM_ATTR static const uint32_t K[] = {
    0x428A2F98,
    0x71374491,
    0xB5C0FBCF,
    0xE9B5DBA5,
    0x3956C25B,
    0x59F111F1,
    0x923F82A4,
    0xAB1C5ED5,
    0xD807AA98,
    0x12835B01,
    0x243185BE,
    0x550C7DC3,
    0x72BE5D74,
    0x80DEB1FE,
    0x9BDC06A7,
    0xC19BF174,
    0xE49B69C1,
    0xEFBE4786,
    0x0FC19DC6,
    0x240CA1CC,
    0x2DE92C6F,
    0x4A7484AA,
    0x5CB0A9DC,
    0x76F988DA,
    0x983E5152,
    0xA831C66D,
    0xB00327C8,
    0xBF597FC7,
    0xC6E00BF3,
    0xD5A79147,
    0x06CA6351,
    0x14292967,
    0x27B70A85,
    0x2E1B2138,
    0x4D2C6DFC,
    0x53380D13,
    0x650A7354,
    0x766A0ABB,
    0x81C2C92E,
    0x92722C85,
    0xA2BFE8A1,
    0xA81A664B,
    0xC24B8B70,
    0xC76C51A3,
    0xD192E819,
    0xD6990624,
    0xF40E3585,
    0x106AA070,
    0x19A4C116,
    0x1E376C08,
    0x2748774C,
    0x34B0BCB5,
    0x391C0CB3,
    0x4ED8AA4A,
    0x5B9CCA4F,
    0x682E6FF3,
    0x748F82EE,
    0x78A5636F,
    0x84C87814,
    0x8CC70208,
    0x90BEFFFA,
    0xA4506CEB,
    0xBEF9A3F7,
    0xC67178F2,
};

#define SHR(x, n) ((x & 0xFFFFFFFF) >> n)

#define S0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ SHR(x, 3))
#define S1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ SHR(x, 10))

#define S2(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define S3(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))

#define F0(x, y, z) ((x & y) | (z & (x | y)))
#define F1(x, y, z) (z ^ (x & (y ^ z)))

#define R(t) (W[t] = S1(W[t - 2]) + W[t - 7] + S0(W[t - 15]) + W[t - 16])

#define P(a, b, c, d, e, f, g, h, x, K)                                                                                \
    {                                                                                                                  \
        temp1 = h + S3(e) + F1(e, f, g) + K + x;                                                                       \
        temp2 = S2(a) + F0(a, b, c);                                                                                   \
        d += temp1;                                                                                                    \
        h = temp1 + temp2;                                                                                             \
    }

#define CHECK_BYTES(u1, u2, offset)                                                                                    \
    {                                                                                                                  \
        temp1 = u1 + u2;                                                                                               \
        for (int i = 0; i < 4; ++i) {                                                                                  \
            temp3 = (uint8_t)((temp1 >> (i * 8)) & 0xff);                                                              \
            temp4 = *(target + offset + i);                                                                            \
            if (__builtin_expect(temp4 < temp3, true)) {                                                               \
                return false;                                                                                          \
            }                                                                                                          \
            if (__builtin_expect(temp4 > temp3, false)) {                                                              \
                return true;                                                                                           \
            }                                                                                                          \
        }                                                                                                              \
    }

#define MAINET_TESTNET_INTERVAL 210000
#define REGTEST_INTERVAL 150

const char* TAG = "MINER";
typedef struct _sha256_context {
    uint8_t buffer[64];
    uint32_t state[8];
} _sha256_context;

typedef struct {
    uint32_t version;
    uint8_t prev_block[32];
    uint8_t merkle_root[32];
    uint32_t timestamp;
    uint32_t bits;
    uint32_t nonce;
} block_header;

typedef struct headerandtarget {
    block_header bh;
    uint8_t target[32];
} headerandtarget;

typedef struct task_ctx {
    /* BBB-AIRGAP: 'ht' is a multi-field template that the caller replaces while the miners read it,
     * and 'nonce_solution' and 'solution_found' point into the shared context that check_solutions()
     * reads. Upstream left all of them unsynchronised and said so twice in check_solutions() and
     * check_speed(): "missing memory barrier but appers to work". Torn reads there hand the user a
     * header built half from one template and half from the next, which is a solution QR that can
     * never be a valid block. Each of these fields is now published under 'lock'. */
    headerandtarget ht;
    /* BBB-AIRGAP: written on every hash and read by check_speed() from another task, so it has to be
     * atomic to be read at all without undefined behaviour. It stays 32 bits on purpose: the value is
     * (nonce - nonce_start) mod 2^32, so it can only wrap once a task has swept the whole 32-bit
     * nonce range of the current template, at which point that template is exhausted and nothing is
     * left to find for it. A 64-bit counter would need a 64-bit store in the hash loop, which is not
     * guaranteed lock-free on the Pi Zero W's ARMv6 core. */
    _Atomic uint32_t hashespersec;
    uint32_t nonce_start;
    uint32_t* nonce_solution;
    uint8_t task_n;
    bool* solution_found;
    SemaphoreHandle_t lock;
    _Atomic bool newwork;
    /* BBB-AIRGAP: upstream stops a miner with vTaskDelete(handle), which on FreeRTOS deletes another
     * task outright whatever it is doing. piJade runs the Linux build, where vTaskDelete() can only
     * signal a thread that parked itself in vTaskDelay(portMAX_DELAY) (libjade/task.c:197-205, :233);
     * a miner task never does - its two park loops use vTaskDelay(1) and its hash loop never delays at
     * all. So the handle was never found, both threads outlived stop_miners(), and the free() that
     * follows handed their still-live writes (nonce_solution and solution_found both point inside the
     * freed block) a use-after-free plus two cores of runaway hashing. Stopping is therefore
     * cooperative. Both flags are C11 atomics rather than volatile: volatile orders an access only
     * against other volatile accesses, so a compiler was free to sink the plain writes that precede
     * the retirement notice (the 'newwork' clear, the solution stores) past it, which lands them in
     * memory stop_miners() has already freed. 'exited' is therefore a release store paired with an
     * acquire load in stop_miners(); 'stop' publishes no data alongside it, so relaxed is enough and
     * keeps the hash loop free of a barrier per iteration. */
    _Atomic bool stop;
    _Atomic bool exited;
} task_ctx;

typedef struct miner_ctx {
    /* BBB-AIRGAP: guards every field below that the miner tasks and the caller both touch - the two
     * templates, the block header the nonce is written into, the raw coinbase and its length, the
     * solution flag and the start timestamp. See the task_ctx comment. */
    SemaphoreHandle_t lock;
    uint8_t rawtx[300];
    block_header bh;
    int64_t start;
    TaskHandle_t xHandle1;
    TaskHandle_t xHandle2;
    solution_cb cb;
    void* cbctx;
    task_ctx ctx1;
    task_ctx ctx2;
    size_t txlen;
    bool solution_found;
} miner_ctx;

/* BBB-AIRGAP: upstream pinned the two hot functions into IRAM. Measured on the jade_v1_1 target
 * (ESP-IDF v5.5.4, 2026-09-02): verify_nonce alone is 0x42e3 = 17123 bytes of .iram1 and the link
 * fails with "region `iram0_0_seg' overflowed by 13440 bytes" as soon as main/qrmode.c references
 * the miner. The target of this fork is the Linux host, where IRAM_ATTR is an empty define
 * (libjade/include/esp_attr.h) and placement changes nothing; the ESP32 targets are build checks
 * only, so both functions run from flash there. */
static void calc_midstate(const uint8_t* buf_ptr, _sha256_context* midstate)
{

    uint32_t A[8] = { 0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A, 0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19 };

    uint32_t temp1, temp2, W[64];

    W[0] = GET_UINT32_BE(buf_ptr, 0);
    W[1] = GET_UINT32_BE(buf_ptr, 4);
    W[2] = GET_UINT32_BE(buf_ptr, 8);
    W[3] = GET_UINT32_BE(buf_ptr, 12);
    W[4] = GET_UINT32_BE(buf_ptr, 16);
    W[5] = GET_UINT32_BE(buf_ptr, 20);
    W[6] = GET_UINT32_BE(buf_ptr, 24);
    W[7] = GET_UINT32_BE(buf_ptr, 28);
    W[8] = GET_UINT32_BE(buf_ptr, 32);
    W[9] = GET_UINT32_BE(buf_ptr, 36);
    W[10] = GET_UINT32_BE(buf_ptr, 40);
    W[11] = GET_UINT32_BE(buf_ptr, 44);
    W[12] = GET_UINT32_BE(buf_ptr, 48);
    W[13] = GET_UINT32_BE(buf_ptr, 52);
    W[14] = GET_UINT32_BE(buf_ptr, 56);
    W[15] = GET_UINT32_BE(buf_ptr, 60);

    P(A[0], A[1], A[2], A[3], A[4], A[5], A[6], A[7], W[0], K[0]);
    P(A[7], A[0], A[1], A[2], A[3], A[4], A[5], A[6], W[1], K[1]);
    P(A[6], A[7], A[0], A[1], A[2], A[3], A[4], A[5], W[2], K[2]);
    P(A[5], A[6], A[7], A[0], A[1], A[2], A[3], A[4], W[3], K[3]);
    P(A[4], A[5], A[6], A[7], A[0], A[1], A[2], A[3], W[4], K[4]);
    P(A[3], A[4], A[5], A[6], A[7], A[0], A[1], A[2], W[5], K[5]);
    P(A[2], A[3], A[4], A[5], A[6], A[7], A[0], A[1], W[6], K[6]);
    P(A[1], A[2], A[3], A[4], A[5], A[6], A[7], A[0], W[7], K[7]);
    P(A[0], A[1], A[2], A[3], A[4], A[5], A[6], A[7], W[8], K[8]);
    P(A[7], A[0], A[1], A[2], A[3], A[4], A[5], A[6], W[9], K[9]);
    P(A[6], A[7], A[0], A[1], A[2], A[3], A[4], A[5], W[10], K[10]);
    P(A[5], A[6], A[7], A[0], A[1], A[2], A[3], A[4], W[11], K[11]);
    P(A[4], A[5], A[6], A[7], A[0], A[1], A[2], A[3], W[12], K[12]);
    P(A[3], A[4], A[5], A[6], A[7], A[0], A[1], A[2], W[13], K[13]);
    P(A[2], A[3], A[4], A[5], A[6], A[7], A[0], A[1], W[14], K[14]);
    P(A[1], A[2], A[3], A[4], A[5], A[6], A[7], A[0], W[15], K[15]);
    P(A[0], A[1], A[2], A[3], A[4], A[5], A[6], A[7], R(16), K[16]);
    P(A[7], A[0], A[1], A[2], A[3], A[4], A[5], A[6], R(17), K[17]);
    P(A[6], A[7], A[0], A[1], A[2], A[3], A[4], A[5], R(18), K[18]);
    P(A[5], A[6], A[7], A[0], A[1], A[2], A[3], A[4], R(19), K[19]);
    P(A[4], A[5], A[6], A[7], A[0], A[1], A[2], A[3], R(20), K[20]);
    P(A[3], A[4], A[5], A[6], A[7], A[0], A[1], A[2], R(21), K[21]);
    P(A[2], A[3], A[4], A[5], A[6], A[7], A[0], A[1], R(22), K[22]);
    P(A[1], A[2], A[3], A[4], A[5], A[6], A[7], A[0], R(23), K[23]);
    P(A[0], A[1], A[2], A[3], A[4], A[5], A[6], A[7], R(24), K[24]);
    P(A[7], A[0], A[1], A[2], A[3], A[4], A[5], A[6], R(25), K[25]);
    P(A[6], A[7], A[0], A[1], A[2], A[3], A[4], A[5], R(26), K[26]);
    P(A[5], A[6], A[7], A[0], A[1], A[2], A[3], A[4], R(27), K[27]);
    P(A[4], A[5], A[6], A[7], A[0], A[1], A[2], A[3], R(28), K[28]);
    P(A[3], A[4], A[5], A[6], A[7], A[0], A[1], A[2], R(29), K[29]);
    P(A[2], A[3], A[4], A[5], A[6], A[7], A[0], A[1], R(30), K[30]);
    P(A[1], A[2], A[3], A[4], A[5], A[6], A[7], A[0], R(31), K[31]);
    P(A[0], A[1], A[2], A[3], A[4], A[5], A[6], A[7], R(32), K[32]);
    P(A[7], A[0], A[1], A[2], A[3], A[4], A[5], A[6], R(33), K[33]);
    P(A[6], A[7], A[0], A[1], A[2], A[3], A[4], A[5], R(34), K[34]);
    P(A[5], A[6], A[7], A[0], A[1], A[2], A[3], A[4], R(35), K[35]);
    P(A[4], A[5], A[6], A[7], A[0], A[1], A[2], A[3], R(36), K[36]);
    P(A[3], A[4], A[5], A[6], A[7], A[0], A[1], A[2], R(37), K[37]);
    P(A[2], A[3], A[4], A[5], A[6], A[7], A[0], A[1], R(38), K[38]);
    P(A[1], A[2], A[3], A[4], A[5], A[6], A[7], A[0], R(39), K[39]);
    P(A[0], A[1], A[2], A[3], A[4], A[5], A[6], A[7], R(40), K[40]);
    P(A[7], A[0], A[1], A[2], A[3], A[4], A[5], A[6], R(41), K[41]);
    P(A[6], A[7], A[0], A[1], A[2], A[3], A[4], A[5], R(42), K[42]);
    P(A[5], A[6], A[7], A[0], A[1], A[2], A[3], A[4], R(43), K[43]);
    P(A[4], A[5], A[6], A[7], A[0], A[1], A[2], A[3], R(44), K[44]);
    P(A[3], A[4], A[5], A[6], A[7], A[0], A[1], A[2], R(45), K[45]);
    P(A[2], A[3], A[4], A[5], A[6], A[7], A[0], A[1], R(46), K[46]);
    P(A[1], A[2], A[3], A[4], A[5], A[6], A[7], A[0], R(47), K[47]);
    P(A[0], A[1], A[2], A[3], A[4], A[5], A[6], A[7], R(48), K[48]);
    P(A[7], A[0], A[1], A[2], A[3], A[4], A[5], A[6], R(49), K[49]);
    P(A[6], A[7], A[0], A[1], A[2], A[3], A[4], A[5], R(50), K[50]);
    P(A[5], A[6], A[7], A[0], A[1], A[2], A[3], A[4], R(51), K[51]);
    P(A[4], A[5], A[6], A[7], A[0], A[1], A[2], A[3], R(52), K[52]);
    P(A[3], A[4], A[5], A[6], A[7], A[0], A[1], A[2], R(53), K[53]);
    P(A[2], A[3], A[4], A[5], A[6], A[7], A[0], A[1], R(54), K[54]);
    P(A[1], A[2], A[3], A[4], A[5], A[6], A[7], A[0], R(55), K[55]);
    P(A[0], A[1], A[2], A[3], A[4], A[5], A[6], A[7], R(56), K[56]);
    P(A[7], A[0], A[1], A[2], A[3], A[4], A[5], A[6], R(57), K[57]);
    P(A[6], A[7], A[0], A[1], A[2], A[3], A[4], A[5], R(58), K[58]);
    P(A[5], A[6], A[7], A[0], A[1], A[2], A[3], A[4], R(59), K[59]);
    P(A[4], A[5], A[6], A[7], A[0], A[1], A[2], A[3], R(60), K[60]);
    P(A[3], A[4], A[5], A[6], A[7], A[0], A[1], A[2], R(61), K[61]);
    P(A[2], A[3], A[4], A[5], A[6], A[7], A[0], A[1], R(62), K[62]);
    P(A[1], A[2], A[3], A[4], A[5], A[6], A[7], A[0], R(63), K[63]);

    midstate->state[0] = 0x6A09E667 + A[0];
    midstate->state[1] = 0xBB67AE85 + A[1];
    midstate->state[2] = 0x3C6EF372 + A[2];
    midstate->state[3] = 0xA54FF53A + A[3];
    midstate->state[4] = 0x510E527F + A[4];
    midstate->state[5] = 0x9B05688C + A[5];
    midstate->state[6] = 0x1F83D9AB + A[6];
    midstate->state[7] = 0x5BE0CD19 + A[7];
    midstate->buffer[16] = 0x80;
    memcpy(midstate->buffer, buf_ptr + 64, 12);
}

static bool verify_nonce(_sha256_context* midstate, uint8_t* target)
{
    uint32_t temp1, temp2;
    uint8_t temp3, temp4;

    uint32_t W[64] = { GET_UINT32_BE(midstate->buffer, 0), GET_UINT32_BE(midstate->buffer, 4),
        GET_UINT32_BE(midstate->buffer, 8), GET_UINT32_BE(midstate->buffer, 12), -2147483648, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 640, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    uint32_t A[8] = { midstate->state[0], midstate->state[1], midstate->state[2], midstate->state[3],
        midstate->state[4], midstate->state[5], midstate->state[6], midstate->state[7] };

    union {
        uint32_t num;
        uint8_t b[4];
    } u;
    uint8_t* p = NULL;

    P(A[0], A[1], A[2], A[3], A[4], A[5], A[6], A[7], W[0], K[0]);
    P(A[7], A[0], A[1], A[2], A[3], A[4], A[5], A[6], W[1], K[1]);
    P(A[6], A[7], A[0], A[1], A[2], A[3], A[4], A[5], W[2], K[2]);
    P(A[5], A[6], A[7], A[0], A[1], A[2], A[3], A[4], W[3], K[3]);
    P(A[4], A[5], A[6], A[7], A[0], A[1], A[2], A[3], W[4], K[4]);
    P(A[3], A[4], A[5], A[6], A[7], A[0], A[1], A[2], W[5], K[5]);
    P(A[2], A[3], A[4], A[5], A[6], A[7], A[0], A[1], W[6], K[6]);
    P(A[1], A[2], A[3], A[4], A[5], A[6], A[7], A[0], W[7], K[7]);
    P(A[0], A[1], A[2], A[3], A[4], A[5], A[6], A[7], W[8], K[8]);
    P(A[7], A[0], A[1], A[2], A[3], A[4], A[5], A[6], W[9], K[9]);
    P(A[6], A[7], A[0], A[1], A[2], A[3], A[4], A[5], W[10], K[10]);
    P(A[5], A[6], A[7], A[0], A[1], A[2], A[3], A[4], W[11], K[11]);
    P(A[4], A[5], A[6], A[7], A[0], A[1], A[2], A[3], W[12], K[12]);
    P(A[3], A[4], A[5], A[6], A[7], A[0], A[1], A[2], W[13], K[13]);
    P(A[2], A[3], A[4], A[5], A[6], A[7], A[0], A[1], W[14], K[14]);
    P(A[1], A[2], A[3], A[4], A[5], A[6], A[7], A[0], W[15], K[15]);
    P(A[0], A[1], A[2], A[3], A[4], A[5], A[6], A[7], R(16), K[16]);
    P(A[7], A[0], A[1], A[2], A[3], A[4], A[5], A[6], R(17), K[17]);
    P(A[6], A[7], A[0], A[1], A[2], A[3], A[4], A[5], R(18), K[18]);
    P(A[5], A[6], A[7], A[0], A[1], A[2], A[3], A[4], R(19), K[19]);
    P(A[4], A[5], A[6], A[7], A[0], A[1], A[2], A[3], R(20), K[20]);
    P(A[3], A[4], A[5], A[6], A[7], A[0], A[1], A[2], R(21), K[21]);
    P(A[2], A[3], A[4], A[5], A[6], A[7], A[0], A[1], R(22), K[22]);
    P(A[1], A[2], A[3], A[4], A[5], A[6], A[7], A[0], R(23), K[23]);
    P(A[0], A[1], A[2], A[3], A[4], A[5], A[6], A[7], R(24), K[24]);
    P(A[7], A[0], A[1], A[2], A[3], A[4], A[5], A[6], R(25), K[25]);
    P(A[6], A[7], A[0], A[1], A[2], A[3], A[4], A[5], R(26), K[26]);
    P(A[5], A[6], A[7], A[0], A[1], A[2], A[3], A[4], R(27), K[27]);
    P(A[4], A[5], A[6], A[7], A[0], A[1], A[2], A[3], R(28), K[28]);
    P(A[3], A[4], A[5], A[6], A[7], A[0], A[1], A[2], R(29), K[29]);
    P(A[2], A[3], A[4], A[5], A[6], A[7], A[0], A[1], R(30), K[30]);
    P(A[1], A[2], A[3], A[4], A[5], A[6], A[7], A[0], R(31), K[31]);
    P(A[0], A[1], A[2], A[3], A[4], A[5], A[6], A[7], R(32), K[32]);
    P(A[7], A[0], A[1], A[2], A[3], A[4], A[5], A[6], R(33), K[33]);
    P(A[6], A[7], A[0], A[1], A[2], A[3], A[4], A[5], R(34), K[34]);
    P(A[5], A[6], A[7], A[0], A[1], A[2], A[3], A[4], R(35), K[35]);
    P(A[4], A[5], A[6], A[7], A[0], A[1], A[2], A[3], R(36), K[36]);
    P(A[3], A[4], A[5], A[6], A[7], A[0], A[1], A[2], R(37), K[37]);
    P(A[2], A[3], A[4], A[5], A[6], A[7], A[0], A[1], R(38), K[38]);
    P(A[1], A[2], A[3], A[4], A[5], A[6], A[7], A[0], R(39), K[39]);
    P(A[0], A[1], A[2], A[3], A[4], A[5], A[6], A[7], R(40), K[40]);
    P(A[7], A[0], A[1], A[2], A[3], A[4], A[5], A[6], R(41), K[41]);
    P(A[6], A[7], A[0], A[1], A[2], A[3], A[4], A[5], R(42), K[42]);
    P(A[5], A[6], A[7], A[0], A[1], A[2], A[3], A[4], R(43), K[43]);
    P(A[4], A[5], A[6], A[7], A[0], A[1], A[2], A[3], R(44), K[44]);
    P(A[3], A[4], A[5], A[6], A[7], A[0], A[1], A[2], R(45), K[45]);
    P(A[2], A[3], A[4], A[5], A[6], A[7], A[0], A[1], R(46), K[46]);
    P(A[1], A[2], A[3], A[4], A[5], A[6], A[7], A[0], R(47), K[47]);
    P(A[0], A[1], A[2], A[3], A[4], A[5], A[6], A[7], R(48), K[48]);
    P(A[7], A[0], A[1], A[2], A[3], A[4], A[5], A[6], R(49), K[49]);
    P(A[6], A[7], A[0], A[1], A[2], A[3], A[4], A[5], R(50), K[50]);
    P(A[5], A[6], A[7], A[0], A[1], A[2], A[3], A[4], R(51), K[51]);
    P(A[4], A[5], A[6], A[7], A[0], A[1], A[2], A[3], R(52), K[52]);
    P(A[3], A[4], A[5], A[6], A[7], A[0], A[1], A[2], R(53), K[53]);
    P(A[2], A[3], A[4], A[5], A[6], A[7], A[0], A[1], R(54), K[54]);
    P(A[1], A[2], A[3], A[4], A[5], A[6], A[7], A[0], R(55), K[55]);
    P(A[0], A[1], A[2], A[3], A[4], A[5], A[6], A[7], R(56), K[56]);
    P(A[7], A[0], A[1], A[2], A[3], A[4], A[5], A[6], R(57), K[57]);
    P(A[6], A[7], A[0], A[1], A[2], A[3], A[4], A[5], R(58), K[58]);
    P(A[5], A[6], A[7], A[0], A[1], A[2], A[3], A[4], R(59), K[59]);
    P(A[4], A[5], A[6], A[7], A[0], A[1], A[2], A[3], R(60), K[60]);
    P(A[3], A[4], A[5], A[6], A[7], A[0], A[1], A[2], R(61), K[61]);
    P(A[2], A[3], A[4], A[5], A[6], A[7], A[0], A[1], R(62), K[62]);
    P(A[1], A[2], A[3], A[4], A[5], A[6], A[7], A[0], R(63), K[63]);

    PUT_UINT32_BE(midstate->state[0] + A[0], midstate->buffer, 0);

    PUT_UINT32_BE(midstate->state[1] + A[1], midstate->buffer, 4);
    PUT_UINT32_BE(midstate->state[2] + A[2], midstate->buffer, 8);
    PUT_UINT32_BE(midstate->state[3] + A[3], midstate->buffer, 12);
    PUT_UINT32_BE(midstate->state[4] + A[4], midstate->buffer, 16);
    PUT_UINT32_BE(midstate->state[5] + A[5], midstate->buffer, 20);
    PUT_UINT32_BE(midstate->state[6] + A[6], midstate->buffer, 24);
    PUT_UINT32_BE(midstate->state[7] + A[7], midstate->buffer, 28);

    /* Calculate the second hash (double SHA-256) */
    A[0] = 0x6A09E667;
    A[1] = 0xBB67AE85;
    A[2] = 0x3C6EF372;
    A[3] = 0xA54FF53A;
    A[4] = 0x510E527F;
    A[5] = 0x9B05688C;
    A[6] = 0x1F83D9AB;
    A[7] = 0x5BE0CD19;

    midstate->buffer[32] = 0x80;
    W[0] = GET_UINT32_BE(midstate->buffer, 0);
    W[1] = GET_UINT32_BE(midstate->buffer, 4);
    W[2] = GET_UINT32_BE(midstate->buffer, 8);
    W[3] = GET_UINT32_BE(midstate->buffer, 12);
    W[4] = GET_UINT32_BE(midstate->buffer, 16);
    W[5] = GET_UINT32_BE(midstate->buffer, 20);
    W[6] = GET_UINT32_BE(midstate->buffer, 24);
    W[7] = GET_UINT32_BE(midstate->buffer, 28);
    W[8] = GET_UINT32_BE(midstate->buffer, 32);
    W[9] = GET_UINT32_BE(midstate->buffer, 36);
    W[10] = GET_UINT32_BE(midstate->buffer, 40);
    W[11] = GET_UINT32_BE(midstate->buffer, 44);
    W[12] = GET_UINT32_BE(midstate->buffer, 48);
    W[13] = GET_UINT32_BE(midstate->buffer, 52);
    W[14] = 0;
    W[15] = 256;

    P(A[0], A[1], A[2], A[3], A[4], A[5], A[6], A[7], W[0], K[0]);
    P(A[7], A[0], A[1], A[2], A[3], A[4], A[5], A[6], W[1], K[1]);
    P(A[6], A[7], A[0], A[1], A[2], A[3], A[4], A[5], W[2], K[2]);
    P(A[5], A[6], A[7], A[0], A[1], A[2], A[3], A[4], W[3], K[3]);
    P(A[4], A[5], A[6], A[7], A[0], A[1], A[2], A[3], W[4], K[4]);
    P(A[3], A[4], A[5], A[6], A[7], A[0], A[1], A[2], W[5], K[5]);
    P(A[2], A[3], A[4], A[5], A[6], A[7], A[0], A[1], W[6], K[6]);
    P(A[1], A[2], A[3], A[4], A[5], A[6], A[7], A[0], W[7], K[7]);
    P(A[0], A[1], A[2], A[3], A[4], A[5], A[6], A[7], W[8], K[8]);
    P(A[7], A[0], A[1], A[2], A[3], A[4], A[5], A[6], W[9], K[9]);
    P(A[6], A[7], A[0], A[1], A[2], A[3], A[4], A[5], W[10], K[10]);
    P(A[5], A[6], A[7], A[0], A[1], A[2], A[3], A[4], W[11], K[11]);
    P(A[4], A[5], A[6], A[7], A[0], A[1], A[2], A[3], W[12], K[12]);
    P(A[3], A[4], A[5], A[6], A[7], A[0], A[1], A[2], W[13], K[13]);
    P(A[2], A[3], A[4], A[5], A[6], A[7], A[0], A[1], W[14], K[14]);
    P(A[1], A[2], A[3], A[4], A[5], A[6], A[7], A[0], W[15], K[15]);
    P(A[0], A[1], A[2], A[3], A[4], A[5], A[6], A[7], R(16), K[16]);
    P(A[7], A[0], A[1], A[2], A[3], A[4], A[5], A[6], R(17), K[17]);
    P(A[6], A[7], A[0], A[1], A[2], A[3], A[4], A[5], R(18), K[18]);
    P(A[5], A[6], A[7], A[0], A[1], A[2], A[3], A[4], R(19), K[19]);
    P(A[4], A[5], A[6], A[7], A[0], A[1], A[2], A[3], R(20), K[20]);
    P(A[3], A[4], A[5], A[6], A[7], A[0], A[1], A[2], R(21), K[21]);
    P(A[2], A[3], A[4], A[5], A[6], A[7], A[0], A[1], R(22), K[22]);
    P(A[1], A[2], A[3], A[4], A[5], A[6], A[7], A[0], R(23), K[23]);
    P(A[0], A[1], A[2], A[3], A[4], A[5], A[6], A[7], R(24), K[24]);
    P(A[7], A[0], A[1], A[2], A[3], A[4], A[5], A[6], R(25), K[25]);
    P(A[6], A[7], A[0], A[1], A[2], A[3], A[4], A[5], R(26), K[26]);
    P(A[5], A[6], A[7], A[0], A[1], A[2], A[3], A[4], R(27), K[27]);
    P(A[4], A[5], A[6], A[7], A[0], A[1], A[2], A[3], R(28), K[28]);
    P(A[3], A[4], A[5], A[6], A[7], A[0], A[1], A[2], R(29), K[29]);
    P(A[2], A[3], A[4], A[5], A[6], A[7], A[0], A[1], R(30), K[30]);
    P(A[1], A[2], A[3], A[4], A[5], A[6], A[7], A[0], R(31), K[31]);
    P(A[0], A[1], A[2], A[3], A[4], A[5], A[6], A[7], R(32), K[32]);
    P(A[7], A[0], A[1], A[2], A[3], A[4], A[5], A[6], R(33), K[33]);
    P(A[6], A[7], A[0], A[1], A[2], A[3], A[4], A[5], R(34), K[34]);
    P(A[5], A[6], A[7], A[0], A[1], A[2], A[3], A[4], R(35), K[35]);
    P(A[4], A[5], A[6], A[7], A[0], A[1], A[2], A[3], R(36), K[36]);
    P(A[3], A[4], A[5], A[6], A[7], A[0], A[1], A[2], R(37), K[37]);
    P(A[2], A[3], A[4], A[5], A[6], A[7], A[0], A[1], R(38), K[38]);
    P(A[1], A[2], A[3], A[4], A[5], A[6], A[7], A[0], R(39), K[39]);
    P(A[0], A[1], A[2], A[3], A[4], A[5], A[6], A[7], R(40), K[40]);
    P(A[7], A[0], A[1], A[2], A[3], A[4], A[5], A[6], R(41), K[41]);
    P(A[6], A[7], A[0], A[1], A[2], A[3], A[4], A[5], R(42), K[42]);
    P(A[5], A[6], A[7], A[0], A[1], A[2], A[3], A[4], R(43), K[43]);
    P(A[4], A[5], A[6], A[7], A[0], A[1], A[2], A[3], R(44), K[44]);
    P(A[3], A[4], A[5], A[6], A[7], A[0], A[1], A[2], R(45), K[45]);
    P(A[2], A[3], A[4], A[5], A[6], A[7], A[0], A[1], R(46), K[46]);
    P(A[1], A[2], A[3], A[4], A[5], A[6], A[7], A[0], R(47), K[47]);
    P(A[0], A[1], A[2], A[3], A[4], A[5], A[6], A[7], R(48), K[48]);
    P(A[7], A[0], A[1], A[2], A[3], A[4], A[5], A[6], R(49), K[49]);
    P(A[6], A[7], A[0], A[1], A[2], A[3], A[4], A[5], R(50), K[50]);
    P(A[5], A[6], A[7], A[0], A[1], A[2], A[3], A[4], R(51), K[51]);
    P(A[4], A[5], A[6], A[7], A[0], A[1], A[2], A[3], R(52), K[52]);
    P(A[3], A[4], A[5], A[6], A[7], A[0], A[1], A[2], R(53), K[53]);
    P(A[2], A[3], A[4], A[5], A[6], A[7], A[0], A[1], R(54), K[54]);
    P(A[1], A[2], A[3], A[4], A[5], A[6], A[7], A[0], R(55), K[55]);
    P(A[0], A[1], A[2], A[3], A[4], A[5], A[6], A[7], R(56), K[56]);
    P(A[7], A[0], A[1], A[2], A[3], A[4], A[5], A[6], R(57), K[57]);
    P(A[6], A[7], A[0], A[1], A[2], A[3], A[4], A[5], R(58), K[58]);
    P(A[5], A[6], A[7], A[0], A[1], A[2], A[3], A[4], R(59), K[59]);
    P(A[4], A[5], A[6], A[7], A[0], A[1], A[2], A[3], R(60), K[60]);

    CHECK_BYTES(0x5BE0CD19, A[7], 0);
    P(A[3], A[4], A[5], A[6], A[7], A[0], A[1], A[2], R(61), K[61]);
    CHECK_BYTES(0x1F83D9AB, A[6], 4);
    P(A[2], A[3], A[4], A[5], A[6], A[7], A[0], A[1], R(62), K[62]);
    CHECK_BYTES(0x9B05688C, A[5], 8);
    P(A[1], A[2], A[3], A[4], A[5], A[6], A[7], A[0], R(63), K[63]);

    CHECK_BYTES(0x510E527F, A[4], 12);
    CHECK_BYTES(0xA54FF53A, A[3], 16);
    CHECK_BYTES(0x3C6EF372, A[2], 20);
    CHECK_BYTES(0xBB67AE85, A[1], 24);
    CHECK_BYTES(0x6A09E667, A[0], 28);

    return true;
}

/* BBB-AIRGAP: the nonce sits inside a uint8_t buffer and upstream read and wrote it through a
 * uint32_t* cast. Accessing an object through an incompatible declared type is undefined behaviour,
 * and with strict aliasing on, the compiler is entitled to keep the increment in a register and
 * never let the hash input see it - the task would then rehash one nonce forever or report a nonce
 * it never tried. memcpy of a constant four bytes says the same thing without the aliasing claim and
 * compiles to the same word access. */
static inline uint32_t nonce_get(const _sha256_context* midstate)
{
    uint32_t nonce;
    memcpy(&nonce, &midstate->buffer[12], sizeof(nonce));
    return nonce;
}

static inline void nonce_set(_sha256_context* midstate, uint32_t nonce)
{
    memcpy(&midstate->buffer[12], &nonce, sizeof(nonce));
}

/* BBB-AIRGAP: take the shared template under the lock; see the task_ctx comment. The 'newwork' flag
 * itself is read and written relaxed everywhere: it only says "there is something to collect", and
 * the happens-before that makes the template safe to read comes from this mutex, not from the flag.
 * That keeps the hash loop's per-iteration check to a plain load, which on the Pi Zero W's ARMv6
 * core is the difference between no barrier and one barrier per hash. */
static void take_new_work(task_ctx* tctx, headerandtarget* header)
{
    xSemaphoreTake(tctx->lock, portMAX_DELAY);
    *header = tctx->ht;
    atomic_store_explicit(&tctx->newwork, false, memory_order_relaxed);
    xSemaphoreGive(tctx->lock);
}

static void minertask(void* pctx)
{
    assert(pctx);
    task_ctx* tctx = pctx;
#if ESP_IDF_VERSION_MAJOR > 4
    ESP_LOGI(TAG, "We are task %d and we hash from %ld", tctx->task_n, tctx->nonce_start);
#else
    ESP_LOGI(TAG, "We are task %d and we hash from %d", tctx->task_n, tctx->nonce_start);
#endif

    headerandtarget header;
    _Atomic bool* newwork = &tctx->newwork;
    /* BBB-AIRGAP: both park loops end on 'stop' as well as on new work, so a miner started but never
     * given a template still retires when asked. See the task_ctx comment for why deletion has to be
     * cooperative here. */
    while (!atomic_load_explicit(&tctx->stop, memory_order_relaxed)) {
        if (atomic_load_explicit(newwork, memory_order_relaxed)) {
            break;
        }
        vTaskDelay(1 / portTICK_PERIOD_MS);
    }

    take_new_work(tctx, &header);

    while (!atomic_load_explicit(&tctx->stop, memory_order_relaxed)) {
        _sha256_context midstate_cached = { 0 };
        calc_midstate((uint8_t*)&header.bh, &midstate_cached);

        nonce_set(&midstate_cached, tctx->nonce_start);
        _sha256_context ctx = midstate_cached;
        while (true) {
            const bool within = verify_nonce(&ctx, header.target);
            if (__builtin_expect(within, false)) {
                /* BBB-AIRGAP: the nonce and the flag reach check_solutions() through the shared
                 * context, and the header they belong to is replaced by on_new_target(); publish them
                 * under the same lock so a solution can never be paired with a later template. The
                 * lock alone is not enough, because on_new_target() can land between finding this
                 * nonce and taking the lock: 'newwork' is set under this same lock, so finding it
                 * clear here proves no template has been published since this task took its work,
                 * and finding it set means the nonce belongs to a header that has already been
                 * replaced. Writing it then would hand check_solutions() the new header and coinbase
                 * carrying the old template's nonce, which is a solution QR that can never be a
                 * block. Drop it and take the new work instead. */
                bool published = false;
                xSemaphoreTake(tctx->lock, portMAX_DELAY);
                if (!atomic_load_explicit(newwork, memory_order_relaxed)) {
                    *tctx->nonce_solution = nonce_get(&midstate_cached);
                    *tctx->solution_found = true;
                    published = true;
                }
                xSemaphoreGive(tctx->lock);

                if (!published) {
                    take_new_work(tctx, &header);
                    break;
                }

                /* wait until we have a new header to work on */
                while (!atomic_load_explicit(&tctx->stop, memory_order_relaxed)) {
                    if (__builtin_expect(atomic_load_explicit(newwork, memory_order_relaxed), false)) {
                        take_new_work(tctx, &header);
                        break;
                    }
                    vTaskDelay(1 / portTICK_PERIOD_MS);
                }
                break;
            }

            const bool has_new_work = atomic_load_explicit(newwork, memory_order_relaxed);
            if (__builtin_expect(has_new_work || atomic_load_explicit(&tctx->stop, memory_order_relaxed), false)) {
                if (has_new_work) {
                    take_new_work(tctx, &header);
                }
                break;
            }
            const uint32_t nonce = nonce_get(&midstate_cached) + 1;
            nonce_set(&midstate_cached, nonce);
            atomic_store_explicit(&tctx->hashespersec, nonce - tctx->nonce_start, memory_order_relaxed);
            ctx = midstate_cached;
        }
    }

    /* BBB-AIRGAP: publish the retirement before leaving, so stop_miners() knows the context is no
     * longer referenced and can free it. vTaskDelete(NULL) rather than a plain return because
     * returning from a task function is fatal on FreeRTOS; on Linux it is pthread_exit(). */
    atomic_store_explicit(&tctx->exited, true, memory_order_release);
    vTaskDelete(NULL);
}

static uint8_t varint_encode(unsigned char* out, uint64_t n)
{
    /* apparently good enough for another 100+ years,
     * you wouldn't want jade to not keep up */
    if (__builtin_expect(n <= 16, false)) {
        out[0] = n + 0x50;
        return 1;
    }

    if (__builtin_expect(n <= 127, false)) {
        out[0] = 0x01;
        out[1] = ((uint8_t*)&n)[0];
        out[2] = ((uint8_t*)&n)[1];
        return 2;
    }

    if (__builtin_expect(n <= 32767, false)) {
        out[0] = 0x02;
        out[1] = ((uint8_t*)&n)[0];
        out[2] = ((uint8_t*)&n)[1];
        out[3] = ((uint8_t*)&n)[2];
        return 3;
    }

    out[0] = 0x03;
    out[1] = ((uint8_t*)&n)[0];
    out[2] = ((uint8_t*)&n)[1];
    out[3] = ((uint8_t*)&n)[2];
    out[4] = ((uint8_t*)&n)[3];
    return 4;
}

static uint64_t get_block_reward(uint64_t block_height, size_t interval)
{
    const uint64_t halving_index = block_height / interval;
    static const uint64_t block_rewards[33] = { 5000000000ULL, 2500000000ULL, 1250000000ULL, 625000000ULL, 312500000ULL,
        156250000ULL, 78125000ULL, 39062500ULL, 19531250ULL, 9765625ULL, 4882812ULL, 2441406ULL, 1220703ULL, 610352ULL,
        305176ULL, 152588ULL, 76294ULL, 38147ULL, 19073ULL, 9536ULL, 4768ULL, 2384ULL, 1192ULL, 596ULL, 298ULL, 149ULL,
        74ULL, 37ULL, 18ULL, 9ULL, 4ULL, 2ULL, 1ULL };
    /* BBB-AIRGAP: the bound was sizeof(block_rewards), which is 264 bytes rather than the 33 entries
     * the array holds, so every halving index from 33 to 263 read past the end. piJade takes the
     * block height from a scanned template, so the index is externally chosen; on regtest, where the
     * interval is 150, height 4950 already reaches it. */
    if (__builtin_expect(halving_index >= sizeof(block_rewards) / sizeof(block_rewards[0]), false)) {
        return 0;
    }
    return block_rewards[halving_index];
}

/* BBB-AIRGAP: folds only ASCII A-Z, deliberately: the address alphabet is ASCII and a locale aware
 * fold would map I to a dotless i under a Turkish locale, which would break bech32 prefixes. */
static bool prefix_matches_ci(const char* s, const char* prefix)
{
    for (size_t i = 0; prefix[i]; ++i) {
        const char c = s[i];
        if (!c) {
            return false;
        }
        const char lc = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        if (lc != prefix[i]) {
            return false;
        }
    }
    return true;
}

/* BBB-AIRGAP: returns false instead of asserting, because the address arrives inside a scanned
 * mining template and is therefore attacker chosen. Measured: this file defines NDEBUG on its first
 * line, but in the libjade unity build <assert.h> has already been included by the time that define
 * is read, so the asserts here are live: preprocessing libjade.c and counting __assert_fail inside
 * the miner.c region gives 20 before this change and 9 after it. An address that would not convert
 * therefore aborted the wallet process outright. In the standalone device build the define does
 * apply, and the same input fell through with written == 0, producing a coinbase output with an
 * empty script - a block that pays nobody. Neither belongs on an untrusted path, so every failure
 * now unwinds to the caller, which publishes no work at all. */
static bool make_coinbase_tx(
    const char* address, size_t height, uint64_t satoshis, uint8_t* txid, uint8_t* rawtx, size_t* txlen)
{
    struct wally_tx* tx = NULL;
    struct wally_tx_witness_stack* witness = NULL;
    bool ret = false;

    if (__builtin_expect(
            wally_tx_init_alloc(/*version*/ 2, /*locktime*/ 0, /*inputs*/ 1, /*outputs*/ 2, &tx) != WALLY_OK, false)) {
        goto cleanup;
    }

    memset(txid, 0, 32);
    uint8_t script_sig[5] = { 0 };
    const uint8_t somet = varint_encode(script_sig, height);

    if (__builtin_expect(wally_tx_witness_stack_init_alloc(/*allocation_len*/ 1, &witness) != WALLY_OK, false)) {
        goto cleanup;
    }
    if (__builtin_expect(wally_tx_witness_stack_add(witness, txid, 32) != WALLY_OK, false)) {
        goto cleanup;
    }

    if (__builtin_expect(wally_tx_add_raw_input(tx, txid, WALLY_TXHASH_LEN, /*utxo_index*/ 0xFFFFFFFF,
                             /*sequence*/ 4294967295, script_sig, somet + 1, witness, /*flags*/ 0)
                != WALLY_OK,
            false)) {
        goto cleanup;
    }

    uint8_t scriptpubkey[200];
    size_t written = 0;

    uint32_t network = WALLY_NETWORK_BITCOIN_MAINNET;
    const char* addr_prefix = "bc";

    /* BBB-AIRGAP: the prefix tests were case sensitive, but a bech32 address is equally valid written
     * entirely in upper case (BIP173), so a template carrying TB1... or BCRT1... was measured against
     * the mainnet prefix, failed both conversions and took the failure path above. */
    if (__builtin_expect(
            prefix_matches_ci(address, "tb") || address[0] == 'm' || address[0] == 'n' || address[0] == '2', false)) {
        network = WALLY_NETWORK_BITCOIN_TESTNET;
        addr_prefix = "tb";
    } else if (__builtin_expect(prefix_matches_ci(address, "bcrt"), false)) {
        network = WALLY_NETWORK_BITCOIN_TESTNET;
        addr_prefix = "bcrt";
    }

    if (wally_address_to_scriptpubkey(address, network, scriptpubkey, sizeof(scriptpubkey), &written) != WALLY_OK) {
        written = 0;
        if (__builtin_expect(
                wally_addr_segwit_to_bytes(address, addr_prefix, 0, scriptpubkey, sizeof(scriptpubkey), &written)
                    != WALLY_OK,
                false)) {
            goto cleanup;
        }
    }

    if (__builtin_expect(wally_tx_add_raw_output(tx, satoshis, scriptpubkey, written, 0) != WALLY_OK, false)) {
        goto cleanup;
    }

    scriptpubkey[0] = 0x6a;
    scriptpubkey[1] = 0x24;
    scriptpubkey[2] = 0xaa;
    scriptpubkey[3] = 0x21;
    scriptpubkey[4] = 0xa9;
    scriptpubkey[5] = 0xed;
    scriptpubkey[6] = 0xe2;
    scriptpubkey[7] = 0xf6;
    scriptpubkey[8] = 0x1c;
    scriptpubkey[9] = 0x3f;
    scriptpubkey[10] = 0x71;
    scriptpubkey[11] = 0xd1;
    scriptpubkey[12] = 0xde;
    scriptpubkey[13] = 0xfd;
    scriptpubkey[14] = 0x3f;
    scriptpubkey[15] = 0xa9;
    scriptpubkey[16] = 0x99;
    scriptpubkey[17] = 0xdf;
    scriptpubkey[18] = 0xa3;
    scriptpubkey[19] = 0x69;
    scriptpubkey[20] = 0x53;
    scriptpubkey[21] = 0x75;
    scriptpubkey[22] = 0x5c;
    scriptpubkey[23] = 0x69;
    scriptpubkey[24] = 0x06;
    scriptpubkey[25] = 0x89;
    scriptpubkey[26] = 0x79;
    scriptpubkey[27] = 0x99;
    scriptpubkey[28] = 0x62;
    scriptpubkey[29] = 0xb4;
    scriptpubkey[30] = 0x8b;
    scriptpubkey[31] = 0xeb;
    scriptpubkey[32] = 0xd8;
    scriptpubkey[33] = 0x36;
    scriptpubkey[34] = 0x97;
    scriptpubkey[35] = 0x4e;
    scriptpubkey[36] = 0x8c;
    scriptpubkey[37] = 0xf9;

    if (__builtin_expect(wally_tx_add_raw_output(tx, 0, scriptpubkey, 38, 0) != WALLY_OK, false)) {
        goto cleanup;
    }

    written = 0;

    if (__builtin_expect(wally_tx_is_coinbase(tx, &written) != WALLY_OK || written != 1, false)) {
        goto cleanup;
    }

    if (__builtin_expect(wally_tx_get_txid(tx, txid, WALLY_TXHASH_LEN) != WALLY_OK, false)) {
        goto cleanup;
    }

    if (wally_tx_to_bytes(tx, WALLY_TX_FLAG_USE_WITNESS, rawtx, 300, txlen) != WALLY_OK) {
        goto cleanup;
    }

    ret = true;

cleanup:
    if (witness) {
        wally_tx_witness_stack_free(witness);
    }
    if (tx) {
        wally_tx_free(tx);
    }
    return ret;
}

/* BBB-AIRGAP: expand a compact nBits into the 32-byte big-endian target it stands for, or report that
 * the encoding is not one a block header may carry. The template is scanned from a QR, so 'bits' and
 * 'target' arrive as two independent attacker-chosen values; a node always sends them consistent, but
 * nothing in the wire format forces it. Mining against an easy target while the header claims a hard
 * one produces a header whose hash does not meet the difficulty it declares, which is a block the
 * network rejects outright - the device would burn its battery to hand the user a worthless QR. */
static bool bits_to_target(uint32_t bits, uint8_t* target)
{
    const uint32_t mantissa = bits & 0x007fffff;
    const uint32_t exponent = bits >> 24;

    /* the sign bit is never set in a header's nBits, and a zero mantissa is a zero target */
    if (__builtin_expect((bits & 0x00800000) || !mantissa, false)) {
        return false;
    }

    /* the mantissa is three bytes wide and sits 'exponent' bytes from the low end, so anything below
     * three or above the target's own width cannot be represented here */
    if (__builtin_expect(exponent < 3 || exponent > HASH_SIZE, false)) {
        return false;
    }

    memset(target, 0, HASH_SIZE);
    const size_t offset = HASH_SIZE - exponent;
    target[offset] = (uint8_t)(mantissa >> 16);
    target[offset + 1] = (uint8_t)(mantissa >> 8);
    target[offset + 2] = (uint8_t)mantissa;
    return true;
}

uint64_t on_new_target(void* ctx, uint32_t version, const uint8_t* previousblockhash, const uint8_t* target,
    uint32_t curtime, uint32_t bits, uint32_t height, const char* address)
{
    uint8_t declared_target[HASH_SIZE];
    if (!bits_to_target(bits, declared_target) || memcmp(declared_target, target, HASH_SIZE)) {
        return 0;
    }

    miner_ctx* mctx = ctx;

    headerandtarget ht
        = { .bh = { .nonce = 0, .version = version, .bits = bits, .timestamp = curtime }, .target = { 0 } };

    for (size_t i = 0; i < HASH_SIZE; ++i) {
        ht.bh.prev_block[HASH_SIZE - 1 - i] = previousblockhash[i];
    }

    memcpy(ht.target, target, HASH_SIZE);
    uint8_t txid[32] = { 0 };

    uint64_t coinbasevalue;

    if (bits == 0x207fffff) {
        /* regtest target difficulty */
        coinbasevalue = get_block_reward(height, REGTEST_INTERVAL);

    } else {
        coinbasevalue = get_block_reward(height, MAINET_TESTNET_INTERVAL);
    }

    /* BBB-AIRGAP: a template that pays nothing - a height past the last halving in the table - is
     * not worth a single hash, and returning 0 without publishing gives the caller one meaning for
     * the value zero: this template was refused. */
    if (!coinbasevalue) {
        return 0;
    }

    /* BBB-AIRGAP: build and publish under the lock, so the miners never read a half-written template
     * and check_solutions() never pairs a nonce with the coinbase of a different one. The coinbase is
     * built inside the lock rather than on the stack because it is 300 bytes and this runs on the
     * caller's task; the miners only wait the microseconds the wally calls take. */
    xSemaphoreTake(mctx->lock, portMAX_DELAY);

    const bool built = make_coinbase_tx(address, height, coinbasevalue, txid, mctx->rawtx, &mctx->txlen);
    if (built) {
        memcpy(ht.bh.merkle_root, txid, HASH_SIZE);

        mctx->bh = ht.bh;

        /* if we got a new template then drain all solution found (for regtest races) */
        /* used purely to drain */
        mctx->solution_found = false;

        mctx->ctx1.ht = ht;
        mctx->ctx2.ht = ht;
        mctx->start = esp_timer_get_time();

        atomic_store_explicit(&mctx->ctx1.newwork, true, memory_order_relaxed);
        atomic_store_explicit(&mctx->ctx2.newwork, true, memory_order_relaxed);
    }

    xSemaphoreGive(mctx->lock);

    return built ? coinbasevalue : 0;
}

void start_miners(void** ctx, solution_cb cb, void* cbctx)
{
    /* BBB-AIRGAP: the two asserts below are the caller's contract and stay asserts. Every resource
     * failure past them is handled rather than asserted, because this file uses plain assert() and
     * the production image is built with NDEBUG (pijade/images/build-armv6.sh selects Release and
     * libjade/CMakeLists.txt gives -O2 -DNDEBUG to that build type). A failed task creation was
     * therefore silent there and left a miner that never runs and so never sets 'exited', which made
     * the wait in stop_miners() spin forever - a signing device hung until its power is pulled. On
     * failure this function now unwinds completely and leaves *ctx NULL, the condition the only
     * caller already checks (libjade/selfcheck/mining.c). */
    assert(ctx && *ctx == NULL);
    assert(cb);

    miner_ctx* mctx = calloc(1, sizeof(miner_ctx));
    if (!mctx) {
        ESP_LOGE(TAG, "could not allocate the miner context");
        return;
    }
    mctx->cb = cb;
    mctx->cbctx = cbctx;

    /* BBB-AIRGAP: calloc clears bytes but does not give dynamically allocated C11 atomic objects a
     * known initial value. In particular, an indeterminate 'exited' value could let stop_miners()
     * free this context before a worker has retired, so initialise every atomic before either task
     * can read it. */
    atomic_init(&mctx->ctx1.hashespersec, 0);
    atomic_init(&mctx->ctx1.newwork, false);
    atomic_init(&mctx->ctx1.stop, false);
    atomic_init(&mctx->ctx1.exited, false);
    atomic_init(&mctx->ctx2.hashespersec, 0);
    atomic_init(&mctx->ctx2.newwork, false);
    atomic_init(&mctx->ctx2.stop, false);
    atomic_init(&mctx->ctx2.exited, false);

    mctx->lock = xSemaphoreCreateMutex();
    if (!mctx->lock) {
        ESP_LOGE(TAG, "could not create the miner lock");
        free(mctx);
        return;
    }
    mctx->ctx1.lock = mctx->lock;
    mctx->ctx2.lock = mctx->lock;

    mctx->ctx1.nonce_solution = &mctx->bh.nonce;
    mctx->ctx1.solution_found = &mctx->solution_found;
    mctx->ctx2.nonce_start = UINT32_MAX / 2;
    mctx->ctx2.task_n = 1;
    mctx->ctx2.nonce_solution = &mctx->bh.nonce;
    mctx->ctx2.solution_found = &mctx->solution_found;

    /* FIXME: tweak the task sizes */
    if (xTaskCreatePinnedToCore(minertask, "m", 2400, &mctx->ctx1, tskIDLE_PRIORITY + 1, &mctx->xHandle1, 0)
        != pdPASS) {
        ESP_LOGE(TAG, "could not start the first miner task");
        vSemaphoreDelete(mctx->lock);
        free(mctx);
        return;
    }

    if (xTaskCreatePinnedToCore(minertask, "m", 2400, &mctx->ctx2, tskIDLE_PRIORITY + 1, &mctx->xHandle2, 1)
        != pdPASS) {
        ESP_LOGE(TAG, "could not start the second miner task");
        /* BBB-AIRGAP: retire the task that did start through the same cooperative path stop_miners()
         * uses, and only delete the lock afterwards: the first park loop hands the task into
         * take_new_work(), which takes that lock. */
        atomic_store_explicit(&mctx->ctx1.stop, true, memory_order_relaxed);
        while (!atomic_load_explicit(&mctx->ctx1.exited, memory_order_acquire)) {
            vTaskDelay(1 / portTICK_PERIOD_MS);
        }
        vSemaphoreDelete(mctx->lock);
        free(mctx);
        return;
    }

    *ctx = mctx;
}

void stop_miners(void* ctx)
{
    assert(ctx);
    miner_ctx* mctx = ctx;
    /* BBB-AIRGAP: ask both tasks to retire and wait until they have, instead of deleting them by
     * handle - see the task_ctx comment for why the handles are useless on this platform. The wait is
     * what makes the free() below safe: until both tasks have left, they are still writing through
     * pointers into this block. */
    atomic_store_explicit(&mctx->ctx1.stop, true, memory_order_relaxed);
    atomic_store_explicit(&mctx->ctx2.stop, true, memory_order_relaxed);
    while (!atomic_load_explicit(&mctx->ctx1.exited, memory_order_acquire)
        || !atomic_load_explicit(&mctx->ctx2.exited, memory_order_acquire)) {
        vTaskDelay(1 / portTICK_PERIOD_MS);
    }
    vSemaphoreDelete(mctx->lock);
    free(ctx);
}

bool check_solutions(void* ctx)
{
    assert(ctx);
    miner_ctx* mctx = ctx;
    /* FIXME: find upper bound for solution len ?*/
    /* BBB-AIRGAP: upstream read the header, the raw coinbase and its length here with no
     * synchronisation at all and admitted it in the line this replaces ("missing memory barrier but
     * appers to work"), while a miner task was writing the nonce into that same header and
     * on_new_target() could be replacing the coinbase underneath. Copy the whole solution out under
     * the lock, then release it before the callback runs - the callback renders a QR and must not
     * hold the miners still while it does. */
    xSemaphoreTake(mctx->lock, portMAX_DELAY);

    if (!mctx->solution_found) {
        xSemaphoreGive(mctx->lock);
        return false;
    }

    uint8_t solution[600];
    memcpy(solution, &mctx->bh, 80);
    solution[80] = 0x01; /* number of transactions, solo mining :( */
    const size_t txlen = mctx->txlen;
    memcpy(solution + 81, mctx->rawtx, txlen);
    mctx->solution_found = false;

    xSemaphoreGive(mctx->lock);

    mctx->cb(mctx->cbctx, solution, 81 + txlen);
    return true;
}

void check_speed(void* ctx, uint32_t* speed)
{
    assert(ctx);
    miner_ctx* mctx = ctx;
    /* BBB-AIRGAP: the elapsed time was divided in unguarded, and it is zero whenever the caller asks
     * before a microsecond of the current template has gone by - a menu that polls the rate right
     * after starting does exactly that. Dividing by zero gives an infinity, and converting that to
     * uint32_t is undefined. Report nothing measured instead. */
    xSemaphoreTake(mctx->lock, portMAX_DELAY);
    const int64_t start = mctx->start;
    xSemaphoreGive(mctx->lock);

    const int64_t elapsed = esp_timer_get_time() - start;
    if (elapsed <= 0) {
        *speed = 0;
        return;
    }

    /* BBB-AIRGAP: the two counters were added as uint32_t before the division, so their sum wrapped
     * one whole task earlier than either counter would on its own. Widen the sum, and clamp the
     * result: converting a double that exceeds UINT32_MAX is undefined, and a caller asking a few
     * microseconds after the template started divides by very nearly nothing. */
    const uint64_t hashes = (uint64_t)atomic_load_explicit(&mctx->ctx1.hashespersec, memory_order_relaxed)
        + atomic_load_explicit(&mctx->ctx2.hashespersec, memory_order_relaxed);
    const double rate = hashes / (elapsed / 1000000.0);
    *speed = rate >= (double)UINT32_MAX ? UINT32_MAX : (uint32_t)rate;
}

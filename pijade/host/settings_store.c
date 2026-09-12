#include "settings_store.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

/* Worst case is 16*3281 + 16*3281 + 16*288 + 16*8 plus namespace 0 and the per-entry framing:
 * 113866 bytes with every key name at the 15-byte ceiling, from the bounds in
 * libjade/pijade_settings.c. The same arithmetic with the 8-byte key names settings_test.c uses
 * gives 113418, which is what that test's largest store actually measures. The two registration
 * ceilings are PIJADE_SETTINGS_MAX_MULTISIG_LEN and PIJADE_SETTINGS_MAX_DESCRIPTOR_LEN, which
 * pijade_settings.c binds to main/. This cap bounds hostile-file allocation while reads and writes
 * remain heap-allocated at their real size. */
#define SETTINGS_MAX_LEN 131072

#define SLOT_HEADER_LEN 16
#define SLOT_CRC_LEN 4
#define SLOT_OVERHEAD (SLOT_HEADER_LEN + SLOT_CRC_LEN)

static const uint8_t SLOT_MAGIC[8] = { 'P', 'J', 'S', 'L', 'O', 'T', '0', '1' };

struct settings_store {
    char* slots[2];
    int newest_slot;
    uint32_t newest_seq;
    int next_slot;
};

struct slot_contents {
    uint8_t* data;
    size_t len;
    uint32_t seq;
    bool valid;
};

enum slot_header_state {
    SLOT_HEADER_ABSENT,
    SLOT_HEADER_INVALID,
    SLOT_HEADER_KNOWN,
    SLOT_HEADER_UNKNOWN,
};

static uint32_t read_le32(const uint8_t* const p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write_le32(uint8_t* const p, const uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static bool write_all(const int fd, const uint8_t* data, size_t len)
{
    while (len) {
        const ssize_t written = write(fd, data, len);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (written == 0) {
            errno = EIO;
            return false;
        }
        data += written;
        len -= (size_t)written;
    }
    return true;
}

static bool read_all(const int fd, uint8_t* data, size_t len)
{
    while (len) {
        const ssize_t got = read(fd, data, len);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (got == 0) {
            errno = EIO;
            return false;
        }
        data += got;
        len -= (size_t)got;
    }
    return true;
}

/* Flushes directory-entry changes after slot creation or removal. Some FAT implementations reject
 * directory fsync with EINVAL, so this remains best effort after the file itself has been flushed. */
static void sync_parent_dir(const char* const path)
{
    char* const copy = strdup(path);
    if (!copy) {
        return;
    }
    const int fd = open(dirname(copy), O_RDONLY);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
    free(copy);
}

static void ignore_slot(const char* const path, const char* const reason)
{
    fprintf(stderr, "pijade: ignoring %s: %s\n", path, reason);
}

static void clear_slot_contents(struct slot_contents* const slot)
{
    if (slot->data) {
        explicit_bzero(slot->data, slot->len);
        free(slot->data);
    }
    memset(slot, 0, sizeof(*slot));
}

static void read_slot(const char* const path, struct slot_contents* const slot)
{
    memset(slot, 0, sizeof(*slot));

    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        if (errno != ENOENT) {
            ignore_slot(path, strerror(errno));
        }
        return;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        const int error = errno;
        close(fd);
        ignore_slot(path, strerror(error));
        return;
    }
    if (!S_ISREG(st.st_mode) || st.st_size < SLOT_OVERHEAD
        || st.st_size > (off_t)(SLOT_OVERHEAD + SETTINGS_MAX_LEN)) {
        close(fd);
        ignore_slot(path, "invalid size");
        return;
    }

    const size_t frame_len = (size_t)st.st_size;
    uint8_t* const frame = malloc(frame_len);
    if (!frame) {
        close(fd);
        ignore_slot(path, "out of memory");
        return;
    }
    if (!read_all(fd, frame, frame_len)) {
        const int error = errno;
        close(fd);
        explicit_bzero(frame, frame_len);
        free(frame);
        ignore_slot(path, strerror(error));
        return;
    }
    close(fd);

    const uint32_t payload_len = read_le32(frame + 12);
    const char* reason = NULL;
    if (memcmp(frame, SLOT_MAGIC, sizeof(SLOT_MAGIC)) != 0) {
        reason = "invalid magic";
    } else if (payload_len == 0 || payload_len > SETTINGS_MAX_LEN) {
        reason = "invalid length";
    } else if (frame_len != SLOT_OVERHEAD + (size_t)payload_len) {
        reason = "invalid size";
    } else {
        const uint32_t stored_crc = read_le32(frame + SLOT_HEADER_LEN + payload_len);
        const uint32_t actual_crc = (uint32_t)crc32(
            crc32(0L, Z_NULL, 0), frame, SLOT_HEADER_LEN + payload_len);
        if (stored_crc != actual_crc) {
            reason = "invalid crc";
        }
    }

    if (reason) {
        explicit_bzero(frame, frame_len);
        free(frame);
        ignore_slot(path, reason);
        return;
    }

    slot->data = malloc(payload_len);
    if (!slot->data) {
        explicit_bzero(frame, frame_len);
        free(frame);
        ignore_slot(path, "out of memory");
        return;
    }
    memcpy(slot->data, frame + SLOT_HEADER_LEN, payload_len);
    slot->len = payload_len;
    slot->seq = read_le32(frame + 8);
    slot->valid = true;
    explicit_bzero(frame, frame_len);
    free(frame);
}

static enum slot_header_state probe_slot_header(const char* const path, uint32_t* const seq)
{
    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            return SLOT_HEADER_ABSENT;
        }
        ignore_slot(path, strerror(errno));
        return SLOT_HEADER_UNKNOWN;
    }

    uint8_t header[SLOT_HEADER_LEN];
    size_t total = 0;
    int error = 0;
    while (total < sizeof(header)) {
        const ssize_t got = read(fd, header + total, sizeof(header) - total);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            error = errno;
            break;
        }
        if (got == 0) {
            break;
        }
        total += (size_t)got;
    }
    close(fd);

    enum slot_header_state state = SLOT_HEADER_INVALID;
    if (error) {
        ignore_slot(path, strerror(error));
        state = SLOT_HEADER_UNKNOWN;
    } else if (total == sizeof(header)
        && memcmp(header, SLOT_MAGIC, sizeof(SLOT_MAGIC)) == 0) {
        *seq = read_le32(header + 8);
        state = SLOT_HEADER_KNOWN;
    }
    explicit_bzero(header, sizeof(header));
    return state;
}

static bool erase_slot(const char* path);

settings_store_t* settings_store_open(const char* const base)
{
    if (!base) {
        return NULL;
    }
    const size_t base_len = strlen(base);
    if (base_len > SIZE_MAX - 3) {
        return NULL;
    }

    settings_store_t* const store = calloc(1, sizeof(*store));
    if (!store) {
        return NULL;
    }
    for (size_t i = 0; i < 2; ++i) {
        store->slots[i] = malloc(base_len + 3);
        if (!store->slots[i]) {
            settings_store_close(store);
            return NULL;
        }
        memcpy(store->slots[i], base, base_len);
        store->slots[i][base_len] = '.';
        store->slots[i][base_len + 1] = i == 0 ? 'a' : 'b';
        store->slots[i][base_len + 2] = '\0';
    }
    store->newest_slot = -1;
    store->next_slot = 0;
    return store;
}

void settings_store_close(settings_store_t* const store)
{
    if (!store) {
        return;
    }
    free(store->slots[0]);
    free(store->slots[1]);
    free(store);
}

bool settings_store_read(settings_store_t* const store, uint8_t** const data, size_t* const len)
{
    if (!store || !data || !len) {
        return false;
    }
    *data = NULL;
    *len = 0;

    struct slot_contents slots[2];
    read_slot(store->slots[0], &slots[0]);
    read_slot(store->slots[1], &slots[1]);

    int winner = -1;
    if (slots[0].valid) {
        winner = 0;
    }
    if (slots[1].valid && (winner < 0 || slots[1].seq > slots[0].seq)) {
        winner = 1;
    }

    if (winner < 0) {
        clear_slot_contents(&slots[0]);
        clear_slot_contents(&slots[1]);
        store->newest_slot = -1;
        store->next_slot = 0;
        return false;
    }

    const int other = 1 - winner;
    const uint32_t winner_seq = slots[winner].seq;
    *data = slots[winner].data;
    *len = slots[winner].len;
    slots[winner].data = NULL;
    clear_slot_contents(&slots[winner]);
    clear_slot_contents(&slots[other]);
    store->newest_slot = winner;
    if (winner_seq > store->newest_seq) {
        store->newest_seq = winner_seq;
    }
    store->next_slot = other;
    return true;
}

bool settings_store_write(settings_store_t* const store, const uint8_t* const data, const size_t len)
{
    if (!store || !data || len == 0 || len > SETTINGS_MAX_LEN) {
        return false;
    }

    const int target = store->next_slot;
    const int other = 1 - target;
    uint32_t probed_seq = 0;
    const enum slot_header_state header_state = probe_slot_header(store->slots[other], &probed_seq);
    if (header_state == SLOT_HEADER_KNOWN && probed_seq > store->newest_seq) {
        store->newest_seq = probed_seq;
    }
    if (store->newest_seq == UINT32_MAX) {
        fprintf(stderr, "pijade: cannot save settings: sequence exhausted\n");
        return false;
    }
    const uint32_t seq = store->newest_seq + 1;
    store->newest_seq = seq;
    const size_t frame_len = SLOT_OVERHEAD + len;
    uint8_t* const frame = malloc(frame_len);
    if (!frame) {
        return false;
    }
    memcpy(frame, SLOT_MAGIC, sizeof(SLOT_MAGIC));
    write_le32(frame + 8, seq);
    write_le32(frame + 12, (uint32_t)len);
    memcpy(frame + SLOT_HEADER_LEN, data, len);
    write_le32(frame + SLOT_HEADER_LEN + len,
        (uint32_t)crc32(crc32(0L, Z_NULL, 0), frame, SLOT_HEADER_LEN + len));

    bool ok = false;
    int error = 0;
    const int fd = open(store->slots[target], O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        error = errno;
    } else {
        if (!write_all(fd, frame, frame_len)) {
            error = errno;
        } else if (fsync(fd) != 0) {
            error = errno;
        } else {
            ok = true;
        }
        if (close(fd) != 0) {
            if (ok) {
                error = errno;
            }
            ok = false;
        }
    }

    explicit_bzero(frame, frame_len);
    free(frame);
    if (!ok) {
        fprintf(stderr, "pijade: cannot save settings to %s: %s\n",
            store->slots[target], strerror(error ? error : EIO));
        if (store->newest_slot < 0) {
            store->next_slot = 1 - target;
        }
        return false;
    }

    sync_parent_dir(store->slots[target]);
    store->newest_slot = target;
    store->next_slot = 1 - target;
    if (header_state == SLOT_HEADER_UNKNOWN) {
        if (!erase_slot(store->slots[other])) {
            return false;
        }
        sync_parent_dir(store->slots[other]);
    }
    return true;
}

static bool erase_slot(const char* const path)
{
    struct stat before;
    if (lstat(path, &before) != 0) {
        if (errno == ENOENT) {
            return true;
        }
        fprintf(stderr, "pijade: cannot erase settings from %s: %s\n", path, strerror(errno));
        return false;
    }
    if (!S_ISREG(before.st_mode)) {
        fprintf(stderr, "pijade: cannot erase settings from %s: not a regular file\n", path);
        return false;
    }

    const int fd = open(path, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "pijade: cannot erase settings from %s: %s\n", path, strerror(errno));
        return false;
    }
    struct stat opened;
    bool ok = fstat(fd, &opened) == 0 && S_ISREG(opened.st_mode);
    int error = ok ? 0 : errno;
    static const uint8_t zeros[4096] = { 0 };
    off_t remaining = ok ? opened.st_size : 0;
    while (ok && remaining > 0) {
        const size_t chunk = remaining > (off_t)sizeof(zeros) ? sizeof(zeros) : (size_t)remaining;
        if (!write_all(fd, zeros, chunk)) {
            error = errno;
            ok = false;
        }
        remaining -= (off_t)chunk;
    }
    if (ok && fsync(fd) != 0) {
        error = errno;
        ok = false;
    }
    if (close(fd) != 0) {
        if (ok) {
            error = errno;
        }
        ok = false;
    }
    if (ok && unlink(path) != 0) {
        error = errno;
        ok = false;
    }
    if (!ok) {
        fprintf(stderr, "pijade: cannot erase settings from %s: %s\n",
            path, strerror(error ? error : EIO));
    }
    return ok;
}

static bool slot_present(const char* const path)
{
    struct stat st;
    return lstat(path, &st) == 0 || errno != ENOENT;
}

bool settings_store_erase(settings_store_t* const store)
{
    if (!store) {
        return false;
    }

    erase_slot(store->slots[0]);
    erase_slot(store->slots[1]);
    sync_parent_dir(store->slots[0]);
    store->newest_slot = -1;
    const bool present_a = slot_present(store->slots[0]);
    const bool present_b = slot_present(store->slots[1]);
    store->next_slot = present_a != present_b ? (present_a ? 0 : 1) : 0;
    return !present_a && !present_b;
}

/*
 * Standalone tests for the host settings store; libjade is not required.
 *
 * Build and run:
 *   cc -Wall -Wextra -Werror -O1 -o /tmp/settings_store_test \
 *       pijade/tools/settings_store_test.c -lz && /tmp/settings_store_test
 */
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_fsync_fail;
static unsigned g_read_fail_mask;
static unsigned g_read_calls;

static int fake_fsync(const int fd)
{
    const int result = fsync(fd);
    if (g_fsync_fail > 0) {
        --g_fsync_fail;
        errno = EIO;
        return -1;
    }
    return result;
}

static ssize_t fake_read(const int fd, void* const data, const size_t len)
{
    const unsigned call = g_read_calls++;
    if (call < sizeof(g_read_fail_mask) * 8U && (g_read_fail_mask & (1U << call)) != 0) {
        errno = EIO;
        return -1;
    }
    return read(fd, data, len);
}

#define fsync(fd) fake_fsync(fd)
#define read(fd, data, len) fake_read(fd, data, len)
#include "../host/settings_store.c"

static int failures = 0;

static void check(const bool ok, const char* const what)
{
    printf("%-72s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) {
        ++failures;
    }
}

static void wipe_free(uint8_t* const data, const size_t len)
{
    if (data) {
        explicit_bzero(data, len);
        free(data);
    }
}

static void make_path(char* const out, const size_t out_len, const char* const dir,
    const char* const name, const char* const suffix)
{
    if (snprintf(out, out_len, "%s/%s%s", dir, name, suffix) >= (int)out_len) {
        fprintf(stderr, "test path is too long\n");
        exit(EXIT_FAILURE);
    }
}

static bool slot_seq(const char* const path, uint32_t* const seq)
{
    uint8_t header[12];
    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return false;
    }
    const bool ok = read_all(fd, header, sizeof(header)) && close(fd) == 0
        && !memcmp(header, SLOT_MAGIC, sizeof(SLOT_MAGIC));
    if (ok) {
        *seq = read_le32(header + 8);
    }
    return ok;
}

static bool slot_matches(const char* const path, const uint32_t seq,
    const uint8_t* const expected, const size_t expected_len)
{
    struct slot_contents slot;
    read_slot(path, &slot);
    const bool matches = slot.valid && slot.seq == seq && slot.len == expected_len
        && !memcmp(slot.data, expected, expected_len);
    clear_slot_contents(&slot);
    return matches;
}

static bool write_manual_slot(const char* const path, const uint32_t seq,
    const uint8_t* const payload, const size_t payload_len)
{
    const size_t frame_len = SLOT_OVERHEAD + payload_len;
    uint8_t* const frame = malloc(frame_len);
    if (!frame) {
        return false;
    }
    memcpy(frame, SLOT_MAGIC, sizeof(SLOT_MAGIC));
    write_le32(frame + 8, seq);
    write_le32(frame + 12, (uint32_t)payload_len);
    memcpy(frame + SLOT_HEADER_LEN, payload, payload_len);
    write_le32(frame + SLOT_HEADER_LEN + payload_len,
        (uint32_t)crc32(crc32(0L, Z_NULL, 0), frame, SLOT_HEADER_LEN + payload_len));
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    bool ok = false;
    if (fd >= 0) {
        ok = write_all(fd, frame, frame_len);
        if (close(fd) != 0) {
            ok = false;
        }
    }
    explicit_bzero(frame, frame_len);
    free(frame);
    return ok;
}

static bool read_matches(settings_store_t* const store, const uint8_t* const expected,
    const size_t expected_len)
{
    uint8_t* data = NULL;
    size_t len = 0;
    const bool read = settings_store_read(store, &data, &len);
    const bool matches = read && len == expected_len && !memcmp(data, expected, expected_len);
    wipe_free(data, len);
    return matches;
}

static bool read_logs_ignoring(settings_store_t* const store, const char* const slot,
    const uint8_t* const expected, const size_t expected_len)
{
    FILE* const log = tmpfile();
    if (!log) {
        return false;
    }
    fflush(stderr);
    const int saved_stderr = dup(STDERR_FILENO);
    if (saved_stderr < 0 || dup2(fileno(log), STDERR_FILENO) < 0) {
        if (saved_stderr >= 0) {
            close(saved_stderr);
        }
        fclose(log);
        return false;
    }
    const bool matches = read_matches(store, expected, expected_len);
    fflush(stderr);
    const bool restored = dup2(saved_stderr, STDERR_FILENO) >= 0;
    close(saved_stderr);
    rewind(log);
    char line[1024] = { 0 };
    const bool have_line = fgets(line, sizeof(line), log) != NULL;
    fclose(log);
    char prefix[768];
    snprintf(prefix, sizeof(prefix), "pijade: ignoring %s:", slot);
    return matches && restored && have_line && strstr(line, prefix) == line;
}

static void remove_slot_files(const char* const dir, const char* const name)
{
    char path[1024];
    make_path(path, sizeof(path), dir, name, ".a");
    unlink(path);
    rmdir(path);
    make_path(path, sizeof(path), dir, name, ".b");
    unlink(path);
    rmdir(path);
}

int main(void)
{
    char temp[] = "/tmp/pijade-settings-store.XXXXXX";
    const char* const dir = mkdtemp(temp);
    if (!dir) {
        perror("mkdtemp");
        return EXIT_FAILURE;
    }

    static const uint8_t first[] = { 0x10, 0x20, 0x30, 0x40 };
    static const uint8_t second[] = { 0xa1, 0xb2, 0xc3 };
    static const uint8_t third[] = { 0x31, 0x32, 0x33, 0x34, 0x35 };
    static const uint8_t fourth[] = { 0xf4, 0x04 };
    char base[1024], slot_a[1024], slot_b[1024];
    uint32_t seq = 0;
    struct stat st;

    make_path(base, sizeof(base), dir, "basic", "");
    make_path(slot_a, sizeof(slot_a), dir, "basic", ".a");
    make_path(slot_b, sizeof(slot_b), dir, "basic", ".b");
    settings_store_t* store = settings_store_open(base);
    uint8_t* data = NULL;
    size_t len = 0;
    check(store && !settings_store_read(store, &data, &len), "empty directory reads as no usable settings");
    check(settings_store_write(store, first, sizeof(first)) && slot_seq(slot_a, &seq) && seq == 1,
        "first write creates slot A with sequence 1");
    check(stat(slot_a, &st) == 0 && (st.st_mode & 0777) == 0600,
        "new slot A has mode 0600 on a real filesystem");
    check(read_matches(store, first, sizeof(first)), "first payload reads back unchanged");
    check(settings_store_write(store, second, sizeof(second)) && slot_seq(slot_b, &seq) && seq == 2,
        "second write creates slot B with sequence 2");
    uint32_t seq_a = 0;
    check(slot_seq(slot_a, &seq_a) && seq_a == 1, "second write leaves slot A sequence 1 untouched");
    check(read_matches(store, second, sizeof(second)), "newest sequence wins after the second write");

    int fd = open(slot_b, O_WRONLY | O_TRUNC);
    check(fd >= 0 && close(fd) == 0
            && read_logs_ignoring(store, slot_b, first, sizeof(first)),
        "zero-length slot B is ignored and logged; slot A wins");
    check(settings_store_write(store, second, sizeof(second)) && slot_seq(slot_b, &seq) && seq == 3,
        "write after truncated slot B preserves the monotonic sequence");

    check(write_manual_slot(slot_b, 2, second, sizeof(second)), "CRC scenario starts with a valid slot B");
    fd = open(slot_b, O_RDWR);
    uint8_t byte = 0;
    bool changed = fd >= 0 && lseek(fd, -1, SEEK_END) >= 0 && read(fd, &byte, 1) == 1
        && lseek(fd, -1, SEEK_END) >= 0;
    byte ^= 0x01;
    changed = changed && write(fd, &byte, 1) == 1 && close(fd) == 0;
    check(changed && read_matches(store, first, sizeof(first)), "slot B with a damaged CRC is ignored");

    check(write_manual_slot(slot_b, 2, second, sizeof(second)), "magic scenario starts with a valid slot B");
    fd = open(slot_b, O_WRONLY);
    byte = 'X';
    changed = fd >= 0 && write(fd, &byte, 1) == 1 && close(fd) == 0;
    check(changed && read_matches(store, first, sizeof(first)), "slot B with damaged magic is ignored");

    check(write_manual_slot(slot_b, 2, second, sizeof(second)), "long-size scenario starts with a valid slot B");
    fd = open(slot_b, O_WRONLY | O_APPEND);
    byte = 0;
    changed = fd >= 0 && write(fd, &byte, 1) == 1 && close(fd) == 0;
    check(changed && read_matches(store, first, sizeof(first)), "slot B one byte too long is ignored");

    check(write_manual_slot(slot_b, 2, second, sizeof(second)), "short-size scenario starts with a valid slot B");
    changed = stat(slot_b, &st) == 0 && truncate(slot_b, st.st_size - 1) == 0;
    check(changed && read_matches(store, first, sizeof(first)), "slot B one byte too short is ignored");
    settings_store_close(store);
    remove_slot_files(dir, "basic");

    make_path(base, sizeof(base), dir, "invalid", "");
    make_path(slot_a, sizeof(slot_a), dir, "invalid", ".a");
    make_path(slot_b, sizeof(slot_b), dir, "invalid", ".b");
    fd = open(slot_a, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    bool made_invalid = fd >= 0 && close(fd) == 0;
    fd = open(slot_b, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    made_invalid = made_invalid && fd >= 0 && close(fd) == 0;
    store = settings_store_open(base);
    check(made_invalid && store && !settings_store_read(store, &data, &len), "two invalid slots read as no usable settings");
    check(settings_store_write(store, first, sizeof(first)) && slot_seq(slot_a, &seq) && seq == 1,
        "write after two invalid slots restarts at slot A sequence 1");
    settings_store_close(store);
    remove_slot_files(dir, "invalid");

    make_path(base, sizeof(base), dir, "ordering", "");
    make_path(slot_a, sizeof(slot_a), dir, "ordering", ".a");
    make_path(slot_b, sizeof(slot_b), dir, "ordering", ".b");
    const bool ordered = write_manual_slot(slot_a, 5, first, sizeof(first))
        && write_manual_slot(slot_b, 3, second, sizeof(second));
    store = settings_store_open(base);
    check(ordered && store && read_matches(store, first, sizeof(first)), "slot A sequence 5 wins over slot B sequence 3");
    check(settings_store_write(store, second, sizeof(second)) && slot_seq(slot_b, &seq) && seq == 6,
        "write after sequence 5 targets slot B with sequence 6");
    settings_store_close(store);
    remove_slot_files(dir, "ordering");

    make_path(base, sizeof(base), dir, "tie", "");
    make_path(slot_a, sizeof(slot_a), dir, "tie", ".a");
    make_path(slot_b, sizeof(slot_b), dir, "tie", ".b");
    const bool tied = write_manual_slot(slot_a, 4, first, sizeof(first))
        && write_manual_slot(slot_b, 4, second, sizeof(second));
    store = settings_store_open(base);
    check(tied && store && read_matches(store, first, sizeof(first)), "equal sequences choose slot A");
    settings_store_close(store);
    remove_slot_files(dir, "tie");

    make_path(base, sizeof(base), dir, "erase", "");
    make_path(slot_a, sizeof(slot_a), dir, "erase", ".a");
    make_path(slot_b, sizeof(slot_b), dir, "erase", ".b");
    store = settings_store_open(base);
    const bool wrote_both = store && settings_store_write(store, first, sizeof(first))
        && settings_store_write(store, second, sizeof(second));
    check(wrote_both && settings_store_erase(store) && access(slot_a, F_OK) != 0 && access(slot_b, F_OK) != 0,
        "erase overwrites and removes both slots");
    check(!settings_store_read(store, &data, &len), "read after erase finds no usable settings");
    check(settings_store_write(store, first, sizeof(first)) && slot_seq(slot_a, &seq) && seq == 3,
        "first write after erase keeps the handle sequence monotonic");
    settings_store_close(store);
    remove_slot_files(dir, "erase");

    make_path(base, sizeof(base), dir, "bounds", "");
    make_path(slot_a, sizeof(slot_a), dir, "bounds", ".a");
    make_path(slot_b, sizeof(slot_b), dir, "bounds", ".b");
    store = settings_store_open(base);
    uint8_t* const over_cap = calloc(SETTINGS_MAX_LEN + 1, 1);
    check(store && !settings_store_write(store, first, 0) && access(slot_a, F_OK) != 0,
        "zero-length write is rejected without creating a slot");
    check(over_cap && !settings_store_write(store, over_cap, SETTINGS_MAX_LEN + 1)
            && access(slot_a, F_OK) != 0 && access(slot_b, F_OK) != 0
            && write_manual_slot(slot_a, UINT32_MAX, first, sizeof(first))
            && read_matches(store, first, sizeof(first))
            && !settings_store_write(store, second, sizeof(second)) && access(slot_b, F_OK) != 0,
        "over-cap and exhausted-sequence writes are rejected without wrapping");
    wipe_free(over_cap, SETTINGS_MAX_LEN + 1);
    settings_store_close(store);
    remove_slot_files(dir, "bounds");

    make_path(base, sizeof(base), dir, "directory", "");
    make_path(slot_a, sizeof(slot_a), dir, "directory", ".a");
    make_path(slot_b, sizeof(slot_b), dir, "directory", ".b");
    const bool made_dir = mkdir(slot_a, 0700) == 0;
    store = settings_store_open(base);
    check(made_dir && store && !settings_store_read(store, &data, &len), "a directory in slot A is ignored on read");
    check(!settings_store_write(store, first, sizeof(first)) && !stat(slot_a, &st) && S_ISDIR(st.st_mode),
        "write to directory slot A fails without removing the directory");
    check(!settings_store_write(store, second, sizeof(second)) && slot_seq(slot_b, &seq) && seq == 2,
        "write to B is non-authoritative when unreadable directory A cannot be erased");
    settings_store_close(store);
    remove_slot_files(dir, "directory");

    char real_slot[1024];
    bool scenario;

    make_path(base, sizeof(base), dir, "partial-b", "");
    make_path(slot_a, sizeof(slot_a), dir, "partial-b", ".a");
    make_path(slot_b, sizeof(slot_b), dir, "partial-b", ".b");
    make_path(real_slot, sizeof(real_slot), dir, "partial-b", ".real.b");
    store = settings_store_open(base);
    scenario = store && settings_store_write(store, first, sizeof(first))
        && settings_store_write(store, second, sizeof(second));
    settings_store_close(store);
    scenario = scenario && rename(slot_b, real_slot) == 0 && symlink(real_slot, slot_b) == 0;
    store = settings_store_open(base);
    scenario = scenario && store && read_matches(store, second, sizeof(second));
    const bool partial_b_erased = store && settings_store_erase(store);
    uint32_t survivor_seq = 0;
    scenario = scenario && !partial_b_erased
        && settings_store_write(store, third, sizeof(third))
        && slot_seq(slot_b, &survivor_seq) && survivor_seq > 2;
    settings_store_close(store);
    store = settings_store_open(base);
    scenario = scenario && store && read_matches(store, third, sizeof(third));
    settings_store_close(store);
    check(scenario, "scenario 1: partial erase with survivor B reloads the new data");
    unlink(slot_b);
    unlink(real_slot);
    remove_slot_files(dir, "partial-b");

    make_path(base, sizeof(base), dir, "partial-a", "");
    make_path(slot_a, sizeof(slot_a), dir, "partial-a", ".a");
    make_path(slot_b, sizeof(slot_b), dir, "partial-a", ".b");
    make_path(real_slot, sizeof(real_slot), dir, "partial-a", ".real.a");
    store = settings_store_open(base);
    scenario = store && settings_store_write(store, first, sizeof(first))
        && settings_store_write(store, second, sizeof(second))
        && settings_store_write(store, third, sizeof(third));
    settings_store_close(store);
    scenario = scenario && rename(slot_a, real_slot) == 0 && symlink(real_slot, slot_a) == 0;
    store = settings_store_open(base);
    scenario = scenario && store && read_matches(store, third, sizeof(third));
    const bool partial_a_erased = store && settings_store_erase(store);
    survivor_seq = 0;
    scenario = scenario && !partial_a_erased
        && settings_store_write(store, fourth, sizeof(fourth))
        && slot_seq(slot_a, &survivor_seq) && survivor_seq > 3;
    settings_store_close(store);
    store = settings_store_open(base);
    scenario = scenario && store && read_matches(store, fourth, sizeof(fourth));
    settings_store_close(store);
    check(scenario, "scenario 2: partial erase with survivor A reloads the new data");
    unlink(slot_a);
    unlink(real_slot);
    remove_slot_files(dir, "partial-a");

    make_path(base, sizeof(base), dir, "fsync-failure", "");
    make_path(slot_a, sizeof(slot_a), dir, "fsync-failure", ".a");
    make_path(slot_b, sizeof(slot_b), dir, "fsync-failure", ".b");
    store = settings_store_open(base);
    g_fsync_fail = 1;
    scenario = store && !settings_store_write(store, first, sizeof(first))
        && slot_seq(slot_a, &seq_a) && seq_a == 1
        && settings_store_write(store, second, sizeof(second))
        && slot_seq(slot_b, &seq) && seq > seq_a;
    g_fsync_fail = 0;
    settings_store_close(store);
    store = settings_store_open(base);
    scenario = scenario && store && read_matches(store, second, sizeof(second));
    settings_store_close(store);
    check(scenario, "scenario 3: fsync failure cannot create an equal sequence");
    remove_slot_files(dir, "fsync-failure");

    make_path(base, sizeof(base), dir, "known-good", "");
    make_path(slot_a, sizeof(slot_a), dir, "known-good", ".a");
    make_path(slot_b, sizeof(slot_b), dir, "known-good", ".b");
    store = settings_store_open(base);
    scenario = store && settings_store_write(store, first, sizeof(first))
        && mkdir(slot_b, 0700) == 0 && !settings_store_write(store, second, sizeof(second))
        && slot_matches(slot_a, 1, first, sizeof(first)) && rmdir(slot_b) == 0
        && settings_store_write(store, third, sizeof(third))
        && slot_seq(slot_b, &seq) && seq > 1;
    settings_store_close(store);
    store = settings_store_open(base);
    scenario = scenario && store && read_matches(store, third, sizeof(third));
    settings_store_close(store);
    check(scenario, "scenario 4: failed write never redirects onto the known good slot");
    remove_slot_files(dir, "known-good");

    make_path(base, sizeof(base), dir, "probe-sequence", "");
    make_path(slot_a, sizeof(slot_a), dir, "probe-sequence", ".a");
    make_path(slot_b, sizeof(slot_b), dir, "probe-sequence", ".b");
    store = settings_store_open(base);
    scenario = store && settings_store_write(store, first, sizeof(first))
        && settings_store_write(store, second, sizeof(second));
    settings_store_close(store);
    scenario = scenario && unlink(slot_a) == 0;
    store = settings_store_open(base);
    g_read_calls = 0;
    g_read_fail_mask = 0x1;
    scenario = scenario && store && !settings_store_read(store, &data, &len);
    g_read_calls = 0;
    g_read_fail_mask = 0;
    scenario = scenario && settings_store_write(store, third, sizeof(third))
        && slot_seq(slot_a, &seq) && seq == 3;
    settings_store_close(store);
    store = settings_store_open(base);
    scenario = scenario && store && read_matches(store, third, sizeof(third));
    settings_store_close(store);
    check(scenario, "scenario 5: write probe recovers sequence missed during full read");
    remove_slot_files(dir, "probe-sequence");

    make_path(base, sizeof(base), dir, "probe-cleanup", "");
    make_path(slot_a, sizeof(slot_a), dir, "probe-cleanup", ".a");
    make_path(slot_b, sizeof(slot_b), dir, "probe-cleanup", ".b");
    store = settings_store_open(base);
    scenario = store && settings_store_write(store, first, sizeof(first))
        && settings_store_write(store, second, sizeof(second));
    settings_store_close(store);
    scenario = scenario && unlink(slot_a) == 0;
    store = settings_store_open(base);
    g_read_calls = 0;
    g_read_fail_mask = 0x1;
    scenario = scenario && store && !settings_store_read(store, &data, &len);
    g_read_calls = 0;
    g_read_fail_mask = 0x1;
    scenario = scenario && settings_store_write(store, third, sizeof(third));
    g_read_calls = 0;
    g_read_fail_mask = 0;
    scenario = scenario && slot_seq(slot_a, &seq) && seq == 1 && access(slot_b, F_OK) != 0;
    settings_store_close(store);
    store = settings_store_open(base);
    scenario = scenario && store && read_matches(store, third, sizeof(third));
    settings_store_close(store);
    check(scenario, "scenario 6: unreadable old slot is erased only after durable write");
    remove_slot_files(dir, "probe-cleanup");

    make_path(base, sizeof(base), dir, "cleanup-failure", "");
    make_path(slot_a, sizeof(slot_a), dir, "cleanup-failure", ".a");
    make_path(slot_b, sizeof(slot_b), dir, "cleanup-failure", ".b");
    make_path(real_slot, sizeof(real_slot), dir, "cleanup-failure", ".real.b");
    store = settings_store_open(base);
    scenario = store && settings_store_write(store, first, sizeof(first))
        && settings_store_write(store, second, sizeof(second));
    settings_store_close(store);
    scenario = scenario && rename(slot_b, real_slot) == 0 && symlink(real_slot, slot_b) == 0;
    store = settings_store_open(base);
    scenario = scenario && store && read_matches(store, second, sizeof(second));
    g_read_calls = 0;
    g_read_fail_mask = 0x1;
    scenario = scenario && !settings_store_write(store, third, sizeof(third));
    g_read_calls = 0;
    g_read_fail_mask = 0;
    scenario = scenario && slot_matches(slot_a, 3, third, sizeof(third))
        && slot_matches(slot_b, 2, second, sizeof(second));
    settings_store_close(store);
    store = settings_store_open(base);
    scenario = scenario && store && read_matches(store, third, sizeof(third));
    settings_store_close(store);
    check(scenario, "scenario 7: failed cleanup preserves B and returns false");
    unlink(slot_b);
    unlink(real_slot);
    remove_slot_files(dir, "cleanup-failure");

    if (rmdir(dir) != 0) {
        perror("rmdir");
        ++failures;
    }
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "ok", failures, failures == 1 ? "" : "s");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}

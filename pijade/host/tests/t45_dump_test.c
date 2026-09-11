/*
 * Contract test for the t45 frame dump in pijade_host.c (ROADMAP item 65).
 *
 * The dump writer is a static function, so this file includes pijade_host.c directly and
 * renames its main(). No hardware function is ever called. The writer's path is fixed at
 * /boot/firmware, so the test creates that directory: run it inside the jade-dev container as
 * root, never on a workstation that has a real /boot.
 *
 * Build (in the container, after build_linux exists):
 *   gcc -Wall -Wextra -O2 -I libjade -I pijade/host -o /tmp/t45_dump_test \
 *       pijade/host/tests/t45_dump_test.c pijade/host/panel_st7789.c pijade/host/buttons_gpio.c \
 *       pijade/host/camera_v4l2.c pijade/host/settings_store.c \
 *       -L build_linux/libjade -ljade -lpthread -lz -lm
 * Run:
 *   LD_LIBRARY_PATH=build_linux/libjade /tmp/t45_dump_test
 *
 * Takes about T45_DUMP_MAX x T45_DUMP_INTERVAL_MS (40 s): the interval is real time and the
 * counters are private to the writer, so there is no clock to fake. Exit status 0 means every
 * check passed; each check prints PASS or FAIL.
 */
#define main pijade_host_main
#include "../pijade_host.c"
#undef main

#include <sys/stat.h>
#include <sys/wait.h>

static int failures = 0;
#define CHECK(cond, msg)                                                                                               \
    do {                                                                                                               \
        if (cond) {                                                                                                    \
            printf("PASS %s\n", msg);                                                                                  \
        } else {                                                                                                       \
            printf("FAIL %s\n", msg);                                                                                  \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

static const char* dump_path(const unsigned int index)
{
    static char path[64];
    snprintf(path, sizeof(path), "/boot/firmware/pijade-t45-%02u.pgm", index);
    return path;
}

static bool exists(const char* const path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static void wait_interval(void) { usleep((T45_DUMP_INTERVAL_MS + 100) * 1000); }

/* Phase 1, in a child process: a fresh writer dumps on the first call, respects the interval,
 * writes a well-formed PGM, and stops at the cap. */
static int phase_fresh_writer(void)
{
    uint8_t* const frame = malloc(CAMERA_FRAME_BYTES);
    for (size_t i = 0; i < CAMERA_FRAME_BYTES; ++i) {
        frame[i] = (uint8_t)(i * 7);
    }

    t45_dump_frame(frame);
    struct stat st;
    CHECK(stat(dump_path(0), &st) == 0, "first call writes file 00");
    CHECK(st.st_size == (off_t)(15 + CAMERA_FRAME_BYTES), "file is a 15 byte header plus the frame");

    FILE* const f = fopen(dump_path(0), "rb");
    char header[16] = { 0 };
    uint8_t* const back = malloc(CAMERA_FRAME_BYTES);
    const bool read_ok
        = f && fread(header, 1, 15, f) == 15 && fread(back, 1, CAMERA_FRAME_BYTES, f) == CAMERA_FRAME_BYTES;
    if (f) {
        fclose(f);
    }
    CHECK(read_ok && strcmp(header, "P5\n640 480\n255\n") == 0, "header is P5 640 480 255");
    CHECK(read_ok && memcmp(back, frame, CAMERA_FRAME_BYTES) == 0, "body is the frame, byte for byte");

    t45_dump_frame(frame);
    CHECK(!exists(dump_path(1)), "a second call inside the interval writes nothing");
    wait_interval();
    t45_dump_frame(frame);
    CHECK(exists(dump_path(1)), "a call after the interval writes file 01");

    for (unsigned int i = 2; i < T45_DUMP_MAX; ++i) {
        wait_interval();
        t45_dump_frame(frame);
    }
    CHECK(exists(dump_path(T45_DUMP_MAX - 1)), "the last index below the cap is written");
    wait_interval();
    t45_dump_frame(frame);
    CHECK(!exists(dump_path(T45_DUMP_MAX)), "the cap is not exceeded");

    free(back);
    free(frame);
    return failures;
}

/* Phase 2, in the parent after the child exits: a new run finds file 00 already there, must not
 * touch it (O_EXCL), and must stop rather than continue at a later index. */
static int phase_existing_files(void)
{
    uint8_t* const frame = calloc(1, CAMERA_FRAME_BYTES);
    struct stat before, after;
    CHECK(stat(dump_path(0), &before) == 0, "file 00 is left over from the previous run");
    t45_dump_frame(frame);
    CHECK(stat(dump_path(0), &after) == 0 && before.st_mtime == after.st_mtime && before.st_size == after.st_size,
        "an existing file 00 is not overwritten");
    wait_interval();
    t45_dump_frame(frame);
    CHECK(!exists(dump_path(T45_DUMP_MAX)), "the writer stops instead of skipping ahead");
    free(frame);
    return failures;
}

int main(void)
{
    mkdir("/boot", 0755);
    mkdir("/boot/firmware", 0755);
    for (unsigned int i = 0; i <= T45_DUMP_MAX; ++i) {
        unlink(dump_path(i));
    }

    const pid_t child = fork();
    if (child == 0) {
        const int rc = phase_fresh_writer();
        fflush(stdout); // _exit() skips the stdio flush, and on a pipe stdout is fully buffered
        _exit(rc);
    }
    int status = 0;
    waitpid(child, &status, 0);
    const int child_failures = WIFEXITED(status) ? WEXITSTATUS(status) : 1;

    const int parent_failures = phase_existing_files();

    for (unsigned int i = 0; i <= T45_DUMP_MAX; ++i) {
        unlink(dump_path(i));
    }
    printf("%s (%d failures)\n", child_failures + parent_failures ? "FAILED" : "OK", child_failures + parent_failures);
    return child_failures + parent_failures ? 1 : 0;
}

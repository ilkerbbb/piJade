#ifndef ENTROPY_SOURCES_H_
#define ENTROPY_SOURCES_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gui.h"
#include "sdkconfig.h"

// BBB-AIRGAP: in a libjade build CONFIG_HAS_CAMERA means "the camera code is compiled", not "there
// is a camera": upstream keeps it defined without camera support so the debug scan_qr message
// still works, and the stub then feeds the single injected debug image (libjade/esp_camera.c).
// That is enough to scan a QR code, but never enough for entropy - the frames would all be the
// same image, and gather_camera_entropy() rejects a frame it has already accepted. So the camera
// entropy source is offered only where live frames actually arrive.
#if defined(CONFIG_HAS_CAMERA) && (!defined(CONFIG_LIBJADE) || defined(CONFIG_LIBJADE_CAMERA))
#define HAVE_CAMERA_ENTROPY 1
#endif

// BBB-AIRGAP: user-supplied entropy for new-wallet creation.
//
// Dice rolls follow SeedSigner's scheme so that a wallet created here can be
// reproduced with an independent tool (see SeedSigner docs/dice_verification.md):
// the rolls are hashed as an ASCII string with sha256, truncated to 16 bytes for
// a 12-word mnemonic. The device RNG is deliberately NOT mixed in - mixing would
// make the result impossible to verify externally.
#define DICE_ROLLS_12WORD 50
#define DICE_ROLLS_24WORD 99

// Camera entropy follows SeedSigner's scheme (see its views/tools_views.py):
// hash the device id, then the time, then every captured frame in turn.
// BBB-AIRGAP: unlike the dice path above, the device RNG IS mixed in here, as the last
// input before the digest is taken. The dice rolls stay pure because their whole purpose
// is reproduction with an independent tool; camera frames cannot be reproduced that way,
// so there is no verifiability to trade away and a proven floor to gain - the result is
// at least as unpredictable as getrandom(2) even if the frames carried nothing.
//
// BBB-AIRGAP: the pool matches SeedSigner's size, but not its shape. SeedSigner keeps the last
// 50 frames and hashes them once at the end, so it has to drop the oldest to make room; we hash
// each accepted frame as it arrives and keep nothing, so passing 50 costs nothing and collection
// can go on for as long as the user keeps moving the camera. 50 is therefore a floor, not a
// target: it is the point from which the user is allowed to finish, not the point where the
// device decides for them.
#define CAMERA_ENTROPY_FRAMES 50

// Smallest difference between the darkest and the lightest pixel for a frame to count.
// A covered or failed sensor can emit a run of frames that differ only in overall
// brightness; those pass a duplicate check while carrying no entropy. The threshold is
// deliberately small, so a genuinely dark scene - which still carries sensor noise -
// is not rejected.
#define CAMERA_FRAME_MIN_RANGE 8

// BBB-AIRGAP: a frame is summarised as an 8x8 grid of block averages before being compared with
// the previous accepted frame - see the change gate in entropy_sources.c. A grid rather than the
// pixels themselves because the point is to notice that the scene moved, not that the sensor
// dithered: averaging ~1200 pixels per block cancels per-pixel noise, so a camera lying still in
// front of an unchanging scene scores near zero however noisy its sensor is.
#define CAMERA_SIGNATURE_SIDE 8
#define CAMERA_SIGNATURE_BLOCKS (CAMERA_SIGNATURE_SIDE * CAMERA_SIGNATURE_SIDE)

// Smallest score, summed over the grid, for a frame to count as different from the previous
// accepted one. The score has the mean block change subtracted out first, so an auto-exposure
// step that lifts the whole scene by the same amount does not read as movement.
//
// Measured on synthetic frames (2026-09-03) rather than guessed, because the first value put
// here - 64 - turned out to sit inside the noise band it was meant to exclude:
//
//   still camera, +-8 LSB sensor noise, consecutive frames   score up to  63
//   still camera, +-12 LSB                                   score up to  78
//   still camera, +-16 LSB                                   score up to 114
//   still scene, auto-exposure step of +5 with highlights clipping        65
//   real movement, one single pixel of pan                   score  89 - 128
//   real movement, two pixels                                score 156 - 229
//
// So 64 would have accepted a frozen sensor that merely dithers, which is exactly the failure
// the gate exists to catch. 128 clears the noise ceiling with room to spare and still lets two
// pixels of pan through, which a hand-held camera exceeds constantly.
//
// Two caveats keep this a floor rather than the answer. The synthetic noise is independent per
// pixel; a real sensor's noise is spatially correlated (row noise, fixed-pattern) and will
// average down less within a block, so the real ceiling is higher than measured here. And the
// mean-subtraction only cancels an exposure step while nothing clips - once highlights saturate,
// the step stops being uniform and leaks through, which is why the +5 row above scores at all.
// The number that belongs here comes from characterising the real sensor on device, which has
// not been done. Until it is, this floor errs towards rejecting, which costs the user time and
// never costs them entropy.
#define CAMERA_FRAME_MIN_CHANGE 128

// Menu offering the entropy source for a new wallet.
gui_activity_t* make_new_mnemonic_source_activity(void);

// Ask the user for 12 or 24 words. Returns 0 if the user backs out.
size_t await_new_mnemonic_nwords(void);

// Collect dice rolls from the user and derive entropy from them.
// 'entropy_len' must be 16 for 12 words, 32 for 24 words.
// Returns false if the user abandons entry.
bool gather_dice_entropy(size_t nwords, uint8_t* entropy_out, size_t entropy_len);

// Collect camera frames and derive entropy from them; same conventions as above.
// Returns false if the user leaves the camera before enough frames are captured.
#ifdef HAVE_CAMERA_ENTROPY
bool gather_camera_entropy(size_t nwords, uint8_t* entropy_out, size_t entropy_len);
#endif

// BBB-AIRGAP: run both of the above in turn and hash their outputs together, so that no single
// source has to be trusted on its own. Same conventions as above; returns false if the user
// abandons either half. Without a camera this falls back to dice plus the device CSPRNG, and the
// menu names it accordingly rather than quietly offering a weaker thing under the same label.
bool gather_combined_entropy(size_t nwords, uint8_t* entropy_out, size_t entropy_len);

#endif /* ENTROPY_SOURCES_H_ */

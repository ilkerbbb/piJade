# piJade security audit ; 2026-09-03

> **Mode: READ-ONLY.** This document changes no code. Every finding is written with its evidence
> (file, line, measurement); a claim without evidence does not enter the document. Fixes are
> separate work, done after approval.
>
> **Line numbers:** every `file:line` reference in this document points at the tree as it stood
> during the audit (2026-09-03 to 2026-09-06). Rounds since then have grown several of these files,
> so a reference may now sit a few lines off; the surrounding function and the quoted text are the
> reliable anchors. Re-measuring them against the current tree is outstanding work.
>
> **Commit references:** the published history of the fork was later squashed into a single
> starting commit, so the short hashes of the development commits this document was written
> against no longer resolve anywhere. Where a citation named one, it now describes what the
> citation pointed at instead. Upstream Blockstream hashes and the SeedSigner hash still
> resolve and were kept. The measurements are unaffected; what was lost is their commit-level
> anchor, so a result recorded here can no longer be traced to the commit that produced it.
>
> **Scope:** (A) the security of the fork as a device in use, measured against SeedSigner as the
> reference and with the aim of going beyond it; (B) the parts of the Jade repository and of the
> 1.0.41 announcement that have not reached the fork; (C) every seed generation path, and whether
> the entropy is genuinely high.

## Sources

| Source | Used for |
|---|---|
| `blog.blockstream.com/reflections-on-the-coldcard-fallout/` | Blockstream's RNG defence and the 1.0.41 change list |
| `git log 1.0.40..1.0.41` (161 commits) | The code behind the announcement |
| On-device measurement, 2026-08-28 | The `getrandom(2)` pool verification on real hardware |

---

## Front B ; upstream and the 1.0.41 announcement

### B0. The announcement itself is in our tree (evidence)

The fork's base is `fdb67a3f` (2026-08-24) and it comes **after the 1.0.41 tag** (`dd45a7dc`,
2026-08-21): `git merge-base --is-ancestor 1.0.41 fdb67a3f` says yes. Every security commit the
blog post lists is in our tree:

| Commit | Subject | Present |
|---|---|---|
| `e850c3be` | random: mix in device/version information, jitter in RNG reads | YES |
| `e19472df` | mnemonic: clear the temporary passphrase buffer | YES |
| `47fb3b06` | qr: wipe buffers and icon data on exit | YES |
| `6a9ae2e2` | build: `explicit_bzero` for wally memory clearing | YES |
| `ce4448ef` | ui: wipe modified string content when it is freed | YES |
| `605d86ac` | psbt: require every byte to be consumed during parsing | YES |
| `9f2b916d` | psbt: check that an output amount is present before reading it | YES |
| `8b8e1cd0` | mnemonic: harden the validity check | YES |

### B1. The announcement's RNG assurance is INERT on our device ; **a record (information), not a hole**

The blog's defence of Jade is two sentences: (a) there is no downgraded RNG fallback path,
(b) randomness comes from several sources and is **mixed with SHA512** (chip noise, cycle counter,
timing jitter, sensor readings, camera noise, host entropy).

Half (b) of that defence does **not run** on our device, because the file that implements it is
not compiled:

```
main/amalgamated.c:128    #ifndef CONFIG_LIBJADE
main/amalgamated.c:129    #include "./random.c"
```

piJade is built with `CONFIG_LIBJADE`, so `main/random.c` is **never compiled**. What runs instead
is `get_random` in `libjade/libjade.c:436`, and that reads `getrandom(2)` only. For the same reason
the announcement's own RNG-hardening commit (`e850c3be`) is inert here: that commit changes
**only `main/random.c`** (`git show --stat`: 1 file, +32/-3).

**This is not a hole**, because the source that replaces it is not weak: it is the Linux kernel
CRNG. The on-device measurement (2026-08-28) found `crng init done` at 0.000000 s and
`pijade.service` at 24.21 s; by the time Jade asks for its first byte the pool is long ready.
There is also no entropy seed baked into the image, and `systemd-random-seed` is masked
(`prepare-image.sh:318-325`).

**The finding is this:** the sentence "we are on 1.0.41, so the protection the blog describes is
ours too" cannot be made. Our defence is a different defence (one strong source, fail-closed), not
a multi-source mix. That has to be on the record; otherwise a future upstream improvement to the
RNG will be assumed to have been inherited and will sit here inert. (Same class:
`random_start_collecting()` and `random_full_initialization()` are empty bodies in libjade,
`libjade.c:490-492`.)

### B2. Fail-closed verification ; there is NO downgrade path (evidence)

Coldcard's hole was a downgraded fallback path. In this port the fallback path ends in `abort()`:

```c
libjade/libjade.c:471   if (getrandom_enosys) {
libjade/libjade.c:472       // FIXME: find another source of entropy of cryptographic strength or abort()
libjade/libjade.c:474   }
libjade/libjade.c:475   abort();
```

`EINTR` retries, a short read completes in the loop, `ENOSYS` and every other error `abort()`s.
There is no branch that produces weak bytes. **A Coldcard-class hole is structurally impossible
in this port.**

### B3. The 42 commits the fork has not taken ; **P2, the sweep is not finished**

`git rev-list --left-right --count upstream/master...fdb67a3f` = `65 0`: no divergence, we are
only behind. **42 of** those 65 commits touch areas we compile (`main`, `libjade`, `components`).
The ones that look security-relevant on a first reading:

| Commit | Subject | Why it matters |
|---|---|---|
| `b0552c8e` | otp: validate URL-encoded strings in an OTP context | Input validation; OTP is an active feature here |
| `dd0d699d` | urldecode: validate the URL encoding | The core of the same class |
| `c9873a11` | urldecode: self-tests for validation and decoding | The test for the above |
| `072dd3b1` | libjade: deadlock between standard CBOR and libjade CBOR messages | **Concerns libjade only, which is the path we run** |
| `255cab8e` | sign_message: remove a variable-length array | A VLA is an unchecked size on the stack |
| `b3c45d12` | rpc: wrong flags argument passed to `cbor_parser_init` | Parser behaviour |
| uint32_t series (10+ commits) | Type fixes on RPC parameters | The implicit type conversion class |

This table is a **first reading**; the diffs of the 42 commits were not read one by one. That is
front B's remaining work.

---

## Front C ; every seed generation path

### C0. Inventory of the paths (taken from the code, not from memory)

| # | Path | Entropy source | Bits | Mixing |
|---|---|---|---|---|
| 1 | `New Wallet > Create New > Device` | `get_random()` = `getrandom(2)` | 128 / 256 | none (single source, direct) |
| 2 | `... > Dice Rolls` | 50 or 99 dice rolls | 129 / 255.9 | `sha256(ascii rolls)` |
| 3 | `... > Camera` | `macid` + tick + 25 distinct frames | variable | `sha256` chain |
| 4 | 12/24 word recovery | the user's own seed | ; | none |
| 5 | SeedQR scan | the user's own seed | ; | none |

Path 1 (`main/keychain.c:478-495`): `get_random(entropy, entropy_len)` →
`bip39_mnemonic_from_bytes`. No conversion, truncation or reuse in between; the requested number
of bits comes straight from the kernel.

### C1. The dice path deliberately does not mix in device randomness (a design decision, recorded)

`main/entropy_sources.c:262-267`, comment verbatim: *"As SeedSigner does: hash the rolls as an ascii
string, take the leading bytes. No device randomness is mixed in, so the result stays verifiable."*

This is a deliberate parity decision and it is verified: SeedSigner's own test vectors reproduce
exactly the same 12-word and 24-word results here. As long as the user can enter the same rolls
into SeedSigner and compare, this layer is not a weakness but a gain in auditability.
**We are at parity with SeedSigner.**

### C2. No mixing on the camera path, and no reason for it either ; **P2 (recommendation)** ; **CLOSED**

**Why this is the most important C finding:** the three paths do not offer equal assurance. The
device path takes its 128/256 bits from `getrandom` **provably**; the dice path gives 129/255.9
bits **by construction** (the roll count is fixed); the camera path gives an **unmeasured** amount
of sensor noise plus a near-constant `macid` and a narrow-range tick. For a wallet seed
"unmeasured" is not an acceptable level of assurance, and closing it costs one line.

`main/entropy_sources.c:349-360`: the hash input is, in order, `macid` (6 bytes),
`xTaskGetTickCount()` and the 25 accepted frames. `get_random()` **never enters** that chain.

The reason that holds on the dice path (reproducibility) does not hold here: the camera path is
not deterministic anyway, the user cannot reproduce the result on another device. So not mixing
has a cost and no benefit. Two of the three inputs are weak: `macid` is card-specific but
**constant** (the same for the life of the card, `prepare-image.sh:485-529`), and the tick is time
since boot (a narrow range within the user's flow). The real entropy comes from the frames alone,
and its amount was never measured; it depends on sensor noise.

There are two protections in the code and both were measured: flat-frame rejection
(`CAMERA_FRAME_MIN_RANGE`) and repeated-frame rejection (full digest comparison), so a frozen
camera cannot silently reduce the entropy to `macid + tick`. The protection is built correctly;
what is missing is the **floor**: if 32 bytes from `get_random()` enter the chain, then even if the
camera contributes nothing the result is as strong as the device RNG. This is the cheapest and
most concrete step towards the goal of going beyond SeedSigner.

### C2b. The camera path side by side with SeedSigner ; **we are BELOW the reference (measured)**  ; **CLOSED**

The observation from use ("the camera finishes in one or two seconds, it does not feel safe") was
confirmed in the code. Put side by side, the two implementations differ as follows:

| | SeedSigner | piJade (at audit time) | piJade (after phase 2) |
|---|---|---|---|
| Pool frames | **50** (`tools_screens.py:23`) | 25 | **50** (`entropy_sources.h:34`) |
| Once the pool is full | **Keeps collecting**, sliding window; the newest 50 frames are kept (`:117-119`) | Stops and the seed is produced | **Keeps collecting**; because the hash is incremental no window is needed, every frame enters the chain permanently |
| Who ends the collection | **The user**, by pressing a button (`:127-140`) | The counter, on reaching 25 | **The user**, with the exit button; leaving before 50 produces no seed |
| Final "header" frame | **Yes**: full resolution, at least 4x the screen's pixels (`tools_views.py:115-119`) | None | None (deliberate: after 2.1 the floor is the CSPRNG, no single frame carries weight) |
| Does the user see and approve the frame | **Yes**, and can go back and retake it (`:137-141`) | No | No (same reason) |
| Pool rules | Flat-frame rejection + repeat rejection (`tools_screens.py:99-119`) | The same two | **Three**: flat frame + repeat + **a consecutive-change gate** (not in SeedSigner) |
| CSPRNG mixing | **None** | None | **Yes**, the last input of the chain (not in SeedSigner) |

At audit time we were level on pool health rules and **below on volume and on ceremony**: half the
frames, a collection ended by a counter rather than by the user, and no header frame at all.
Measured against the goal of going beyond SeedSigner, that day this path was **below even parity**.

After phase 2 (third column) volume and ceremony are level, and on health rules and CSPRNG mixing
we are **above** the reference. The only gap left is the header frame and the user's approval of
it; both are deliberate, because after 2.1 the floor is carried by the device CSPRNG rather than
by the camera and no single frame carries weight any more. The sliding window was not copied for
the same reason it is not needed: SeedSigner holds 50 full `Image` objects in Python and so needs
a window, whereas our chain is an incremental SHA256 where a frame is hashed and dropped, and
going past 50 loses nothing.

**Duration is not the right variable; independence is.** Twenty-five frames taken 50 ms apart come
from a single hand movement and are strongly correlated with one another; inserting a `sleep()`
does not change that, it only waits. Two mechanisms genuinely increase the number of independent
samples: (a) mixing in `get_random()` (C2), (b) making frame acceptance **depend on change**: a new
frame does not count unless it differs from the last accepted one by more than a threshold. (b) is
at once a health test (a frozen or half-frozen sensor becomes fail-closed) and a natural rate
limiter: collection advances with the user's movement rather than with the sensor's frame rate.

**Resolution is a feasibility gate (measured).** Adding a "header frame" like SeedSigner's is not
two lines here: libjade's camera shim is hard-bound to QVGA (`libjade/esp_camera.c:52`,
`JADE_ASSERT(config->frame_size == FRAMESIZE_QVGA)`), frames arrive as 320x240 greyscale. A single
high-resolution frame would require a change to the shim.

> **Superseded on 2026-09-10, and the record is kept as it was written.** The shim is now bound to
> `FRAMESIZE_VGA` and frames arrive as 640x480 greyscale (`main/camera.h`), because the quirc scan
> window that 320x240 implied was too small for a version 14 QR code. Every 320x240 and 76800-byte
> figure below is the measurement of that day, not of the code today.

### C2c. An "entropy bit counter" ; **a live counter CANNOT be built, three real tools can**

The second question ("a counter we can look at on every seed generation to see whether we reached
the required bit level") splits into two parts, and the answer to the first is no.

**Why a live counter is impossible:** the entropy of the produced seed cannot be measured, because
a SHA256 output looks random whatever its input; a hash of a constant input passes every test too.
The only thing that can be measured is the raw input, and NIST SP 800-90B separates the two
deliberately:

- **Health tests (SP 800-90B §4.4):** run at runtime, on every sample, catch gross failure (a stuck
  sensor, a repeated frame), behave **fail-closed** and **produce no bit count at all**.
- **Entropy estimation (§6):** on the order of a million samples, a **fixed** noise source, a
  stochastic model, ten separate estimators and taking the smallest. Done offline, once, and the
  result is documented and fixed.

A hand-held camera has no stochastic model; a counter that looks at the difference between
consecutive frames measures scene movement, not sensor noise. Printing "187/256 bits" on the screen
would be a fabricated assurance. SeedSigner's own comment says exactly this
(`tools_views.py:76-80`): the health tests are there to catch gross failure "without trying to
measure entropy".

**The three real tools that can be built:**

1. **Fail-closed health tests.** We have two (the flat-frame threshold `MIN_RANGE=8` and full
   repeat rejection). The third is the change gate from C2b; adding it lands exactly on the posture
   SP 800-90B prescribes.
2. **An honest progress indicator.** The number of accepted and rejected frames and the reason for
   rejection. It shows the source is alive; it claims no bit count.
3. **A one-off characterisation on the device.** Thousands of frames on real hardware, a fixed dark
   scene, the SP 800-90B estimators, the smallest taken; the result is a documented per-frame lower
   bound, and `CAMERA_ENTROPY_FRAMES` is derived from that bound with margin. **This is the only
   place where a real bit figure may legitimately be written**; it is computed once and never shown
   live.

**The heart of the result:** if the `get_random()` mixing in C2 is done, the camera stops being the
floor of the security and becomes a layer on top of it; at that point "how long should it take" is
a question of ceremony and parity, not a cryptographic question. Without the mixing, **no duration
is provably sufficient**, because we cannot measure what the camera gives.

### C3. `get_uniform_random_byte` modulo bias ; **P3 (information)**

The upstream ESP side uses rejection sampling and is unbiased (`main/random.c:165-177`). The
libjade shim returns `ret % upper_bound` (`libjade/libjade.c:483-488`) and its comment says "not
used for crypto". **The claim was verified:** none of the ten call sites produces key material; the
two most meaningful are the factory reset confirmation code (`dashboard.c:681`, a code already
shown to the user) and the random starting letter of the word keyboard (`mnemonic.c:770`). For
`upper_bound=10` the bias is around 4 in 1000 on the first six values. Fixing it is cheap but not
urgent.

### C4. Attribution note

The code behind these three findings (B1, B2, C3) is **not code we wrote**: the whole RNG block of
`libjade/libjade.c` belongs to upstream (`git blame`: 65 of 65 lines, Jon Griffiths, `30aef5c7`,
2025-08-20). The classification is therefore not "our mistake" but **"an inherited development port
is being used in production"**. libjade is Blockstream's emulator/test port; piJade runs it on a
shipped device. The difference has to be taken on knowingly.

---

## Front A ; security of the device in use

### A1. The key to the persistent wallet is the pinserver, and we added NO alternative path ; **evidence**

Jade has no secure element (`grep -rn "ATECC\|secure_element" main/ libjade/` returns nothing). PIN
protection rests on a remote confidant (pinserver / blind oracle): the AES key is not derived on
the device, it is fetched with `pinclient_get`/`pinclient_set`
(`main/process/pinclient.c:564,575`). Even if the card itself is stolen, the blob cannot be
decrypted without a pinserver round: the decryption key is never on the card, so there is no
password an attacker can try locally.

**Anti-hammering has two sides (measured).** On the device side there is a three-attempt counter in
NVS: `storage_decrement_counter()` deletes the blob when it reaches zero or when it sees a corrupt
value (`main/storage.c:512-527`); the counter returns to three only on a successful store
(`:529-533`). On the server side there is a **monotonic replay counter**:
`storage_get_replay_counter()` increases on every use and is **never reset**
(`main/storage.c:540-544`), and the server key is derived tweaked by that counter
(`main/process/pinclient.c:150-190`).

The erase order is deliberate and commented too: first the blob, then the counter
(`main/storage.c:484-509`). The reverse order could leave a device that opens saying "I have no
wallet" while still carrying the encrypted seed; that is exactly what a duress PIN must never do.

**The real question that follows is card cloning.** The device-side counter lives on the card, so
an attacker who copies the card bit for bit can restore the copy after every three attempts and
refresh the counter. At that point the only remaining obstacle is the replay counter, and that
obstacle is **on the server**: the counter value in the cloned card is behind, so the server must
reject a request carrying the stale counter. In other words the persistent wallet's resistance to
card cloning does **not end** in the device's own code; it rests on the server's monotonicity
check. The server side is outside the scope of this audit, so this sentence is not an assurance
but **a record of the dependency**.

SeedSigner does not have this threat at all, because SeedSigner offers no persistent storage. That
is the price of the persistent wallet's convenience, and it is paid knowingly.

The single production call to persistent storage is `main/process/auth_user.c:322` and it takes its
key from the `set_pin_get_aeskey` → `pinclient_set` chain. The two other calls to `keychain_store`
are in `main/selfcheck.c` (self-test) and `main/process/debug_handshake.c`; the second is closed in
A2. **The fork has not added a persistent-storage path that bypasses the pinserver.**

The airgap cost of this is worth noting: opening a persistent wallet requires a blind-oracle round
over QR. That is not a security weakness but a cost of use; SeedSigner's answer is to offer no
persistent storage at all.

### A2. The debug RPC surface is off in production ; **evidence, the chain is complete**

libjade's `sdkconfig.h` leaves `CONFIG_DEBUG_MODE` **on by default**
(`libjade/include/sdkconfig.h:10-12`). That is meant to keep upstream's test tool working, and it
is the wrong default for a device holding keys; the fork turned it into a build option (with a
BBB-AIRGAP note in the same file). The chain was verified end to end:

```
pijade/images/build-armv6.sh:51   -DDEBUG_MODE=0
libjade/CMakeLists.txt:80-82      if (NOT DEBUG_MODE) add_compile_options(-DCONFIG_LIBJADE_NO_DEBUG_MODE)
libjade/include/sdkconfig.h:10    #ifndef CONFIG_LIBJADE_NO_DEBUG_MODE -> CONFIG_DEBUG_MODE is not defined
main/process/dashboard.c:524      #ifdef CONFIG_DEBUG_MODE  (the debug_* dispatch block)
main/wire.c:60,93                 #if defined(CONFIG_DEBUG_MODE) && defined(CONFIG_LIBJADE)
main/process/debug_handshake.c:24 #ifdef CONFIG_DEBUG_MODE  (the entire body)
```

So in the production image `debug_handshake` (a key without the pinserver), `debug_set_mnemonic`
(writing a seed from outside), `debug_clean_reset` and `debug_scan_qr` are **not compiled**, and
there is no `libjade_request` dispatch either. This was the single highest-impact point of the
audit, and it was found closed.

**One link was not measured:** it has not been verified at symbol level that the binary inside the
shipped `pijade-tur3.img` really came from this script (the armv6 binary is on the card; the three
`.so` files we have are emulator builds). When the card is attached to the Mac, `nm -D` proves the
absence of `debug_handshake` in a minute; because the source chain is strong this is a
confirmation, not a doubt.

### A3. The QR attack surface ; **narrow, measured**

On an airgapped device the only input is the camera. The set of CBOR messages accepted from the
camera path is three (`main/qrmode.c`): `mine` (2152), `set_epoch` (2351), `update_pinserver`
(2385). To those are added the parsers of seed/PSBT/address QRs. All three go through a type check
in `bcur_parse_jade_message` and show the rejection to the user.

**`update_pinserver` was examined separately (the most dangerous of the three, because it changes
the trust anchor).** This QR is a candidate for changing the address and the **public key** of the
server that hands out the key which decrypts the wallet; had it been accepted silently, an attacker
could install their own oracle. The measured chain has three layers and is not silent
(`main/process/update_pinserver.c`):

1. **Format validation:** a protocol allowlist (`VALID_PROTOCOL`, 96-108), pubkey length and an
   on-curve check with `wally_ec_public_key_verify` (118-121); inconsistent requests such as "pubkey
   without URL" and "both set and reset" are rejected as well.
2. **The lock (in a production build):** while a wallet is set up on the device
   (`keychain_has_pin()`), a pubkey change is **rejected outright** ; "Cannot update initialized
   unit" (124-146). URL and certificate changes are allowed, the trust anchor is not.
3. **User confirmation:** the URLs and the hex of the pubkey are shown and explicit approval is
   asked (`initial_confirmation = true`, 155-163); a certificate change additionally requires
   comparing a digest (198-201). On refusal, `CBOR_RPC_USER_CANCELLED`.

**But layer 2 is inside `#ifndef CONFIG_DEBUG_MODE`** (line 124). This is the other face of the A2
finding: in a build that leaves `CONFIG_DEBUG_MODE` on, it is not only the debug RPCs that open ;
**the oracle pubkey lock on an initialised wallet falls too**. Our shipping build passes
`-DDEBUG_MODE=0`, so the lock is active (the same chain as in A2); this is the second and heavier
reason why turning that flag into a build option was the right decision.

### A4. Isolation of the mining component ; **the constraint holds**

The constraint: mining code calls nothing in keychain/wallet/storage and no sensitive function. The
measurement: `grep -rnE "keychain_|storage_|wallet_|SENSITIVE_|get_random|mnemonic" components/miner/*.c *.h`
returns **no match at all**. Template parsing validates its input and rejects a template without an
identity (`qrmode.c:2143-2178`).

### A5. Sensitive memory ; **the fork filled in what upstream left empty**

`sensitive_push/pop/clear_stack` are a real implementation in libjade (pthread TLS,
`libjade.c:392-434`); upstream libjade left them empty. Entropy enters the slot through a single
door and that door depends on the path by which the user supplied the words in this session
(`keychain.c:208-229`); an unsupported length is wiped with `wally_bzero`.

---

### A6. NO compiler hardening in the shipped binary ; **P2 (measured)**

The blog post lists "increased stack protection" among the measures Jade takes. On our Linux port
the situation works **in reverse**: all the hardening flags are bound to the `Debug` build and
`Release` is left bare (`libjade/CMakeLists.txt:42-45`):

```cmake
set(CMAKE_C_FLAGS_DEBUG   "-O0 -ggdb3 -DVERIFY -D_FORTIFY_SOURCE=3 -D_GLIBCXX_ASSERTIONS -fstack-protector-strong -fstack-clash-protection")
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")
```

The build we ship is `Release` (`pijade/images/build-armv6.sh:46` → `-DCMAKE_BUILD_TYPE=Release`),
so the `jade` binary that goes onto the card has neither `_FORTIFY_SOURCE`, nor
`-fstack-protector-strong`, nor `-fstack-clash-protection`. The second binary is in the same state:
`pijade-host` is compiled by hand with `-Wall -Wextra -O2` (`pijade/host/build.sh:19`) and takes no
hardening flag.

This is not a hole but a **missing defensive layer**: when a memory error occurs, nothing is there
to catch it. It is exactly the trade one does not want on a device that holds keys, and the cost is
two lines. Upstream binding these flags to the test build is a direct consequence of libjade being
designed as a development tool (see the C4 attribution note) ; we run it in production.

**Not measured:** it has not been verified that the cross compiler (armv6, inside Docker) supports
`_FORTIFY_SOURCE=3` and `-fstack-clash-protection`. `-fstack-clash-protection` is not present in
every GCC version on 32-bit ARM; before acting on this recommendation the compiler version must be
measured, rather than adding the flags blind.

---

## Independent second review ; cross-check

The same three fronts were given to an independent reviewer. **None of the findings in this
document were put in the brief**; only the system description, the threat model and the scope were
given. The result: 1 P1, 10 P2, 3 P3 (over the tree as it stood at that point).

### Overlapping findings (both reviews found these independently)

| Subject | Here | In the second review |
|---|---|---|
| The 1.0.41 RNG defence is inert in this port | B1 | B1 (P2) |
| Hardening flags absent from `Release` | A6 | B2 (P2), which additionally **verified** the absence of the symbol in the binary |
| No `get_random` mixing on the camera path, lower bound unproven | C2 | C1 (P2) |
| 99 dice rolls give 255.911 bits | C1 | C2 (P3), the same figure |
| `get_uniform_random_byte` modulo bias | C3 | C3 (P3) |
| No Coldcard-class downgrade path | B2 | The same conclusion inside B1 |

There is independent convergence at five separate points; that raises confidence in the
measurements themselves.

### What the second review found that is not in this document (its value is here)

| # | Finding | Status |
|---|---|---|
| A2 | A crafted UR frame can terminate the process: `stoul` is unbounded and production C++ is compiled with `-fno-exceptions` (`ur-decoder.cpp:145`, `CMakeLists.txt:93`) | **not verified**, plausible at source level |
| A3 | A one-byte out-of-bounds read on the `ur:bytes` path | **VERIFIED** (below) |
| A4 | Loading a scanned seed defaults to `Yes` and shows no fingerprint (`dialogs.c:925`) | **not verified** |
| A5 | Camera frame buffers and the BC-UR heap load are left unwiped | **not verified** |
| A6 | Two key buffers in `pinclient.c` are outside `SENSITIVE_PUSH` coverage | **not verified**; the exact target of remaining work 6 |
| A7 | The duress PIN is stored **in the clear** on the card; that removes its deniability | **not verified** |
| A8 | A `set_epoch` QR changes the clock without confirmation (P3) | **not verified** |
| B3 | The image chain does not enforce the authenticity of its inputs (an unpinned Docker tag, an unverified SHA256, unpinned `apk add`) | **not verified** |

**Verification of A3 (done in this round).** `handle_qr_bytes()` relies on a NUL terminator
contract and reads `strbytes[bytes_len]` (`main/qrmode.c:1805`). On the BC-UR path that buffer is
allocated **at exactly the right size** with `JADE_MALLOC_PREFER_SPIRAM(result_len + offset)`, with
no extra byte for the NUL (`main/bcur.c:745`); unlike the non-bc-ur branch, which does append a
terminator. And `bcur_parse_bytes()` returns not a copy but **a pointer into the CBOR buffer**
(`main/bcur.c:266-285`). If the byte string is the last element of the CBOR, the byte read is one
past the allocation. The value read does not escape (it is only compared `== '\0'`), so this is not
an information leak; but it is a defined out-of-bounds read triggered by attacker-controlled input.

### The second review's P1: reclassified ; **an already recorded constraint, not a new finding**

The reviewer treated the absence of boot-time verification of the product binaries on the SD card
as a shipping blocker. The threat is real, but it is not a P1, for three reasons:

1. **It is already written down and deliberately out of scope**: physical SD card write access is
   out of scope, and T7 makes no claim of signature verification or secure boot. The reviewer was
   not given that record in the brief, so it looked new to them.
2. **It is at parity with the reference**: SeedSigner also runs from an SD card and performs no
   signature verification at boot (there is no signing or verification under
   `seedsigner/src/seedsigner/hardware/`).
3. **A full solution is architecturally unavailable on this hardware**: the Pi Zero W offers no
   secure boot chain, and the code that would do the verification would sit on the same card.

The correct class is an **accepted architectural limit**, and the grounds for accepting it are
documented. If it is to be reopened, that is a threat-model decision (physical custody of the
card), not a shipping decision.

### One contradiction, recorded openly

In B3 this document listed 42 commits by title as "looking security-relevant" and wrote that it
**had not read the diffs**. The second review did read them and said that "the patch equivalents of
the URL decode, descriptor test and libjade deadlock changes are present in the fork; apart from B1
and B2 there is no verified new security fix" (and counted 61 patches against our count of 65).
That is a result in our favour, but **neither count has been independently verified**; remaining
work 1 closes this contradiction.

## Work NOT completed in this round (an honest record)

The following were not measured; they are written down not to say "there is no problem" but because
their turn did not come.

| # | Remaining work | Why it matters |
|---|---|---|
| 1 | Reading the diffs of the 42 upstream commits | The table in B3 came from titles alone |
| 2 | A scored rubric comparison against SeedSigner | The goal of "going beyond" should be measurable |
| 3 | Memory audit of the SeedQR import/export path | Key material reaches the screen and a QR |
| 4 | Audit of the BIP85 and passphrase paths | Derived secrets |
| 5 | Input audit of the PSBT signing path | The most complex parser |
| 6 | Completeness of `SENSITIVE_PUSH` coverage (a sweep) | An uncovered buffer is a secret left in memory |
| 7 | Symbol confirmation in the shipped binary (the last link of A2) | One minute, when the card is attached to the Mac |
| 8 | A full audit of the service/port surface | The masks in `prepare-image.sh` were read; no end-to-end count was made |
| 9 | Behaviour of the replay counter in a cloning scenario | A1 measured that the protection depends on the server; whether an additional obstacle can be built device-side was not examined |
| 10 | Hardening flag support in the cross compiler | A6's recommendation depends on this measurement |
| 11 | Camera characterisation (SP 800-90B, offline) | The third tool in C2c; a documented per-frame lower bound and a frame count derived from it |
| 12 | The seven unverified findings from the second review (A2, A4, A5, A6, A7, A8, B3) | They came from an independent review and were not verified one by one in this round |
| 13 | The fix for the A3 out-of-bounds read | Verified; the fix was not written (this round was read-only) |

---

## Phase 0 verification round (2026-09-03, no code changed)

After the plan was approved and before moving to the fixes, the unverified findings of the second
review and two open counts were measured. Every item was either verified or rejected.

| # | Subject | Result | Evidence |
|---|---|---|---|
| 0.1 | UR decoder `stoul` overflow | **VERIFIED** | `ur-decoder.cpp:150-151` calls `stoul(comps[0])`, `stoul(comps[1])`; nothing ever checks that the input is numeric (`:145` only looks at `comps.size() != 2`). `-fno-exceptions` is on in two places: `libjade/CMakeLists.txt:94` and `components/esp32_bc-ur/CMakeLists.txt:24`. A non-numeric or overflowing sequence component throws `std::invalid_argument` / `std::out_of_range`, there is no one to catch it, and the process dies through `std::terminate`. |
| 0.2 | Unbounded `seq_len` allocation | **VERIFIED** | `fountain-encoder.hpp:33` `is_valid()` only looks at `message_len_ && !data_.empty()`; there is no upper bound on `seq_len_`. `choose_fragments()` in `fountain-utils.cpp` does `indexes.reserve(seq_len)` with that value, and `fountain-decoder.cpp:205` calls `insert(i)` `seq_len` times. The value is an attacker-controlled `size_t`. |
| 0.3 | The seed loading confirmation | **VERIFIED** | `dashboard.c:876-877`, the text is only "Wallet QR identified." / "Load this wallet?"; the fourth argument is `true`, and `dialogs.c:930` makes that YES button the initial selection with `ftrbtns[default_selection ? 1 : 0]`. No fingerprint is shown. |
| 0.4 | The `esp_camera_deinit` call surface | **MEASURED** | The single production call is `main/camera.c:372`. The implementation is `libjade/esp_camera.c:61`, the QEMU wrapper `main/qemu/qemu_display.c:58`. Wiping can go in one place. |
| 0.5 | The upstream divergence count | **65, THE CONTRADICTION IS CLOSED** | `git rev-list --count fdb67a3f..upstream/master` = 65; with `--no-merges` also 65, so it is not a merge artefact. The figure of 61 from the second review could not be reproduced. **No missed security fix:** urldecode validation (`dd0d699d`), OTP URL validation (`b0552c8e`) and the libjade deadlock (`072dd3b1`) are present in the fork. The two commits not in the fork (`69627745` limited digit entry, `b5329010` removing `free_callback`) are a feature and a refactor; not security fixes. |
| 0.6 | Flag support in the cross compiler | **ALL SUPPORTED** | gcc 12.2.0 in the container (Raspbian 12.2.0-14+rpi1+deb12u1). `_FORTIFY_SOURCE=3`, `_FORTIFY_SOURCE=2`, `-fstack-protector-strong`, `-fstack-clash-protection`, `-Wl,-z,relro,-z,now`, `-fPIE -pie`, `_GLIBCXX_ASSERTIONS`: all compile. The `__stack_chk` symbol really is present in the produced binary (2 matches) and `readelf -d` shows `BIND_NOW`. There is no technical obstacle in front of A6's recommendation. |
| 0.7 | The interface that reads whether a duress PIN exists | **VERIFIED, HEAVIER THAN EXPECTED** | `dashboard.c:1337` does not merely read existence: if `storage_get_wallet_erase_pin()` succeeds it **prints the stored PIN on the screen in the clear** with `format_pin()` and offers Change / Delete. So the duress PIN has to be readable back; a one-way hash makes that screen unworkable. **Phase 3 outcome:** the screen was changed, the display was reduced to "set", and the field became a one-way verifier; the obstacle was the screen itself, not a requirement of the duress mechanism. |
| 0.8 | Behaviour on a magic number mismatch | **MEASURED** | `pijade_settings.c:359-361`: on a mismatch `pijade_settings_deserialize` returns `false`; the rejection is **per file, not per field**. The single call site is `libjade.c:738`, which passes the return value through unchanged. A card in an older format is not silently misread, but everything in it (the wallet blob, the counter, all preferences) goes together. |

### The consequence of 0.7: phase 3 is reopened for decision

The plan's phase 3 called for a "salt + hash verifier". The measurement shows this is not a side
effect but the **direct removal of a function**: today a user who forgets the duress PIN they set
can look it up in the menu. A hash is one-way; that screen drops to a "set / not set" indicator.

The same measurement affects 3.2: if the field is always present the interface will always think
"set", so a separate "is it set" flag is needed; and that flag reopens the very existence signal
one wanted to close.

**Decision (2026-09-03):** the hash is stored, the screen drops to a "set / not set" indicator;
Change and Delete keep working. A user who forgets their PIN cannot look it up and sets a new one.

The second consequence of this decision: **existence normalisation (item 3.2 of the plan) is
dropped.** Since the menu already shows whether the duress PIN is set, hiding the same information
on the card protects nothing; keeping the field always present would only have complicated the
code.

---

## Phase 1.2 ; the family sweep for key material wiping (2026-09-03)

The second review found `serverkey` by reading a single function. Patching one point would not
close the pattern error, so `SENSITIVE_PUSH`/`SENSITIVE_POP` coverage was swept systematically.

**Method.** Every named local buffer that could carry a key, a secret or decrypted data was listed
(`key|secret|seed|entropy|priv|master|shared|pin|passphrase|blind|nonce|hmac|token`), then those
cleared with `SENSITIVE_PUSH` or with a direct `bzero`/`memset` were removed. On the first pass the
sweep missed calls with an address-of operator such as `SENSITIVE_PUSH(&aeskey, ...)` and produced
false positives; after the pattern was fixed, **53** of 89 candidates came out uncovered.

**Threat model ; what this sweep is against.** Measured: swap is off (`prepare-image.sh:234-240`,
verified at `:565-572`), the root is read-only (`:386`, verified at `:583`), and there is no
coredump package in the image. So a residue left on the stack has **no path to disk**; the threat
is process-local. Its value appears in combination with a memory-read hole (the class fixed in
phase 1.1), that is, it is defence in depth.

**Reachability ; what the QR path accepts.** `handle_qr_bytes()` (`main/qrmode.c:1747`) does not
take arbitrary RPC; it recognises a narrow list of formats: a message to be signed (behind a flag),
an OTP URI, an OTP migrate URI, a multisig registration file, a mnemonic. There is no source
restriction in the method dispatch chain (`dashboard.c:560+`), but the only carrier that reaches
that chain is the serial message path, and that path is not fed in production: `libjade_send` is
unresolved in the production binary (0 symbols) and the host never calls it. That is why the
Liquid, identity and attestation handlers are unreachable on this device.
**This argument depends on the carrier: if serial or USB is ever opened, this table has to be read
again.**

**Result.** On the paths reachable over QR, no genuinely secret material was left unwiped. The
`serverkey` finding of the second review was the real exception; it and `decrypted_padded` were
fixed.

**The 53 excluded candidates, with their grounds** (all of them, so that no scope narrowing is
silent):

- **unreachable (13)** ; genuinely secret material, but no path on this device can call it
- **phase 3 (4)** ; duress PIN buffers; their storage format changes in that phase anyway
- **not a secret (31)** ; a public key, a fingerprint, a PEM, a MAC value, a code shown on screen
- **test vector (4)** ; `selfcheck.c`, public constant data
- **covered (1)** ; a struct member whose enclosing struct is PUSHed

| Location | Buffer | Why excluded |
|---|---|---|
| `main/process/debug_handshake.c:61` | `user_pin` | unreachable |
| `main/process/get_blinding_factor.c:53` | `master_blinding_key` | unreachable |
| `main/process/get_blinding_factor.c:59` | `blinding_factor` | unreachable |
| `main/process/get_blinding_key.c:30` | `master_blinding_key` | unreachable |
| `main/process/get_commitments.c:78` | `master_blinding_key` | unreachable |
| `main/process/get_identity_shared_key.c:52` | `shared_key` | unreachable |
| `main/process/get_receive_address.c:237` | `blinding_key` | unreachable |
| `main/process/get_receive_address.c:46` | `multisig_master_blinding_key` | unreachable |
| `main/process/get_registered_multisigs.c:19` | `master_blinding_key` | unreachable |
| `main/process/get_shared_nonce.c:84` | `master_blinding_key` | unreachable |
| `main/process/get_shared_nonce.c:91` | `shared_nonce` | unreachable |
| `main/rsa.c:26` | `entropy` | unreachable |
| `main/ui/multisig.c:143` | `blindingkeystr` | unreachable |
| `main/process/auth_user.c:36` | `pin_erase` | phase 3 |
| `main/process/dashboard.c:1298` | `pin` | phase 3 |
| `main/process/dashboard.c:1335` | `pin_erase` | phase 3 |
| `main/process/dashboard.c:1338` | `pinstr` | phase 3 |
| `libjade/nvs_flash.c:66` | `key` | not a secret |
| `main/assets.c:27` | `asset_entropy` | not a secret |
| `main/attestation/attestation.c:48` | `pubkey_pem` | not a secret |
| `main/descriptor.c:642` | `hmac_calculated` | not a secret |
| `main/keychain.c:656` | `hmac_calculated` | not a secret |
| `main/multisig.c:295` | `hmac_calculated` | not a secret |
| `main/otpauth.c:731` | `hmac` | not a secret |
| `main/process/dashboard.c:1677` | `token` | not a secret |
| `main/process/dashboard.c:683` | `pinstr` | not a secret |
| `main/process/get_bip85_entropy.c:25` | `pubkey` | not a secret |
| `main/process/get_bip85_pubkey.c:32` | `pubkey_pem` | not a secret |
| `main/process/get_blinding_key.c:36` | `public_blinding_key` | not a secret |
| `main/process/get_identity_pubkey.c:54` | `pubkey` | not a secret |
| `main/process/get_otp_code.c:80` | `token` | not a secret |
| `main/process/get_receive_address.c:72` | `pubkeys` | not a secret |
| `main/process/get_shared_nonce.c:65` | `blinding_pubkey` | not a secret |
| `main/process/pinclient.c:156` | `user_pubkey` | not a secret |
| `main/process/pinclient.c:157` | `hmac_tweak` | not a secret |
| `main/process/process_utils.c:414` | `key_type` | not a secret |
| `main/process/register_attestation.c:40` | `ext_pubkey_pem` | not a secret |
| `main/process/register_otp.c:97` | `token` | not a secret |
| `main/process/sign_attestation.c:13` | `pubkey_pem` | not a secret |
| `main/process/sign_identity.c:23` | `pubkey` | not a secret |
| `main/process/sign_psbt.c:276` | `pubkeys` | not a secret |
| `main/process/sign_psbt.c:78` | `key_fingerprint` | not a secret |
| `main/process/sign_tx.c:159` | `pubkeys` | not a secret |
| `main/process/update_pinserver.c:131` | `user_pubkey` | not a secret |
| `main/process/update_pinserver.c:32` | `pubkey` | not a secret |
| `main/qrmode.c:1456` | `pubkeys` | not a secret |
| `main/wallet.c:1039` | `pubkeys` | not a secret |
| `main/wallet.c:832` | `pubkeys` | not a secret |
| `main/selfcheck.c:191` | `aeskey` | test vector |
| `main/selfcheck.c:250` | `new_aeskey` | test vector |
| `main/selfcheck.c:307` | `aeskey` | test vector |
| `main/selfcheck.c:882` | `public_key_hash` | test vector |
| `main/process/pinclient.c:62` | `privkey` | covered |

---

## Phase 1 ; measurements (2026-09-03)

### Death without cleanup: the mechanism was measured

Phase 1.5(b) was a reading finding (`--wrap=abort` only redirects undefined `abort` references
within our own link unit; because `libjade.so` is dynamically linked against `libstdc++.so.6`, the
call inside `std::terminate` goes through the library's own PLT). The claim the fix rests on is
that a handler installed with `std::set_terminate` fires under this linkage too; that claim had not
been measured. The probe: in a translation unit compiled with `-fno-exceptions -fno-rtti`,
`set_terminate` is installed and a malformed UR string is handed to the old decoder.

| Measurement | Result |
|---|---|
| Without the handler installed (pre-phase-1 code) | `rc=134`, `terminate called after throwing an instance of 'std::invalid_argument'` |
| With the handler installed | `ISLEYICI KOSTU` (Turkish for "handler ran"), `rc=42` (printed verbatim by the probe used for that measurement; that probe is not in the repo) |
| The same binary's libstdc++ linkage | `libstdc++.so.6 => /lib/.../libstdc++.so.6` (dynamic) |

So a process that dies without entering the `jade_abort()` path now goes through the handler;
`keychain_clear()` and `sensitive_clear_stack()` run. This covers **every** future throw that the
gate in `main/bcur.c` misses, while that gate is the first line of defence.

### The measured basis of the phase 1 fixes

Two fixes rested on "it compiles" alone; both were measured.

`format_epoch` (`main/qrmode.c`) was run on five boundary values:

| Input | Result |
|---|---|
| `0` | accepted, `Thu Jan  1 00:00:00 1970` |
| `1700000000` | accepted, `Tue Nov 14 22:13:20 2023` |
| `253402300800` (year 10000) | rejected |
| `INT64_MAX` | rejected |
| `UINT64_MAX` | rejected |

The newline trimming was verified in the same run (untrimmed output would have raised a warning; it
did not).

The assumption behind wiping the host camera frame ; that the path handing the frame to libjade
takes a **copy** ; was proven: `libjade/esp_camera.c:41` does `memcpy(_cam_frame_buffer, data,
len)` and does not keep the pointer. Wiping the host-side buffer does not affect libjade's copy;
that copy is wiped separately inside `esp_camera_deinit()` (phase 1.3).

### The 1.1 out-of-bounds read: measured with ASAN (with a positive control)

Phase 1's only verified memory error rested, after the fix, on "it compiles" alone. The probe uses
the real decoder: `esp32_bc-ur` is built with ASAN, a single-part `ur:bytes` is produced and
decoded, the pointer returned by `urresult_ur_decoder` is put through the allocate-and-copy pattern
of `bcur_scan_qr`, and then the read from `handle_qr_bytes` (`qrmode.c:1805`,
`strbytes[bytes_len]`) is applied.

**The positive control is essential:** the same probe was also run against the old allocation. If
the old code had not produced the error, the probe would not be triggering the error at all and
the "clean" result would be worth nothing.

| Allocation | ASAN | Exit code |
|---|---|---|
| `result_len + offset` (old allocation, positive control) | `heap-buffer-overflow READ of size 1`, `0 bytes after 34-byte region` | `1` |
| `result_len + offset + 1` + NUL | no finding; `strbytes[32] = 0x00` | `0` |

No camera stream was needed; what was measured was not the scanning interface but the relationship
between the allocation size and the read index.

### 1.4 hardening: measured in the shipped binary (with a positive control)

Phase 0.6 had only measured that the cross compiler **accepts** the flags; that the flags actually
reach the shipped binary had not been measured. So the armv6 build was rerun from HEAD
(`pijade-armv6.tar.gz`, SHA256 `0113e102...5772a`) and the output examined directly. The existing
package was not trusted, because its timestamp (14:47) contradicted the timestamp of the 1.4 commit
(14:53).

| Measurement | Pre-phase-1 package | HEAD package |
|---|---|---|
| `pijade-host` ELF type | ; | `DYN (Position-Independent Executable file)` |
| `BIND_NOW` (both binaries) | **absent** | present, `FLAGS_1: NOW` |
| `GNU_RELRO` (both binaries) | ; | present |
| `__stack_chk_fail` (both binaries) | **absent** | present |
| FORTIFY `_chk` symbols | **absent** | host 5, libjade 7 |

None of the three protections is in the pre-phase-1 package; this independently confirms the second
review's failure to find `__stack_chk_fail` in the binary during the audit, and proves that the
measurement really shows a difference.

### 1.6 the appearance of the confirmation screen (2026-09-03, closed)

This item was waiting because the ASAN build is compiled with `-DCI=1`; in CI mode confirmation
screens are auto-approved and can never enter a frame. It was run with the `build_linux_nci_log`
build.

Getting onto the path was harder than expected on the first attempt, and that is a finding in
itself: `BTN_SCAN_QR` exists on the home screen only in the **Active/Unlocked** state
(`main/process/dashboard.c:108`), so on an uninitialised device the home-screen scan button enters
the SeedQR flow and an epoch QR cannot be scanned from there (log evidence: `mnemonic.c:1232`,
"0 matches for prefix: UR:JADE-EPOCH/..."). The device was brought to the Active state through
`debug_set_mnemonic` with the public test vector (`abandon` x11 + `about`, fingerprint `73C5DA0A`);
the epoch QR was produced with `pijade/tools/epoch_qr.py`.

**Finding: the confirmation screen was truncating the date.** `format_epoch` used the `ctime_r`
format; "Wed Jun 26 14:00:00 2030" is twenty-four characters, the 240-pixel screen prints
twenty-two of them, and the string was cut **in the middle of the year**. The user was shown
"Wed Jun 26 14:00:00 20" and asked to approve a clock change; that is, the very judgement the
confirmation screen exists for was being asked without the year ; the field an attacker would most
want to move ; being visible. Because it goes through the same helper, the success screen truncated
in the same way.

The fix moved the format to ISO (`localtime_r` + `strftime`, `"%Y-%m-%d %H:%M:%S"`, nineteen
characters). The font was not touched; for overflowing text one measures the layout first, one does
not change the font.

| Measurement | Result |
|---|---|
| Confirmation screen, before the fix | `Wed Jun 26 14:00:00 20` ; the year is cut |
| Confirmation screen, after the fix | `2030-06-26 14:00:00` ; fits fully, with space to the right |
| Default option | `No` (left, selected) |
| The `No` path | `User declined to set the clock`, return to the home screen, **no** error screen for the user |
| The `Yes` path | `Time set successfully` + `2026-09-03 16:46:51`, no truncation |

**The fix's own regression (second review, P2).** The first fix checked `strftime`'s return only
for success, but `%Y` is not fixed width: `ctime_r` rejected year 10000 and beyond as a side effect
of its own fixed layout, `%Y` does not, and the truncation came back. Measured directly (same libc,
a probe compiled in the container):

| epoch | `localtime_r` | `strftime` length | `ctime_r` |
|---|---|---|---|
| 1908712800 (2030) | ok | 19 | ok |
| 32503680000 (3000) | ok | 19 | ok |
| 253402300800 (10000) | ok | **20** | **NULL** |
| 9223372036854775 | ok | **24** | **NULL** |

So the finding was right: the rejection of `253402300800`, measured as a boundary value in phase 1,
had silently disappeared when the format changed. The check was made `== 19`; the fixed-width claim
now lives in the code itself. Verified end to end: a year-10000 QR is rejected with
`Invalid time in QR code`, and the 2030 QR still prints in full as `2030-06-26 14:00:00`.

Two notes, both from that end-to-end run. First, the value on the success screen comes from
`time(NULL)` and the system clock does not change in the emulator (that needs root), so what was
measured is the formatting path; actually setting the clock had already been closed on the device.
Second, when the user declines, the caller logs
`ERROR:qrmode.c:2501: Processing BC-UR as epoch failed`; there is no counterpart on screen, but a
deliberate refusal is logged at error level. Left out of scope, recorded.

---

## Phase 2 ; the entropy architecture (2026-09-03)

### 2.1 mixing in the device CSPRNG

The output of `get_random()` was added as the last input to `gather_camera_entropy`'s hash chain.
Before the mixing, the floor of the camera path was `macid` + the tick counter + frame content; if
the frames carried less entropy than expected there was nothing to say how many bits the result
had.

**What `get_random()` means in this product (measured):** on the libjade target `main/random.c` is
not compiled (it is not in `libjade/CMakeLists.txt`); `get_random` is the implementation at
`libjade/libjade.c:436-473` and it wraps the `getrandom(2)` system call directly. The `ENOSYS` path
`abort()`s, so it does not silently fall back to a weak source. The floor is therefore the kernel
CSPRNG.

**The frame count condition was kept.** The `ctx.nframes >= CAMERA_ENTROPY_FRAMES` check is in
place; the mixing does not turn a camera that gave no frames into a valid source, it only removes
the assumption that the frames alone carry 256 bits.

**Measurement (with a positive control), disassembly of `gather_camera_entropy`:**

| Source | Function lines | `get_random` calls | `sha256_update` calls |
|---|---|---|---|
| HEAD (mixing present) | 248 | **1** | 3 |
| mixing block removed (positive control) | 210 | **0** | 2 |
| restored | 248 | **1** | 3 |

The shipping build runs with `-DCAMERA=CAMERA -DCI=0 -DDEBUG_MODE=0`
(`pijade/images/build-armv6.sh:45-55`). The closest emulator build is `build_linux_nci`
(`CAMERA=1`, `CI=OFF`, `DEBUG_MODE=ON`, 240x240); every phase 2 measurement is made there, as is
the pending screen check of 1.6.

### The measurement was first run in the wrong build variant

The first probe was run against the `build_linux` build and returned zero in all three cases. Zero
did not mean "no calls": in that build `CAMERA:BOOL=0`, so `HAVE_CAMERA_ENTROPY` is undefined and
`gather_camera_entropy` is **not compiled at all**. The probe returned empty because it could not
find the function it was trying to measure.

Without the positive control this "0 calls" would have been read as the fix not working; all three
cases coming out the same showed that it was the probe that was faulty, not the difference. When
the probe was rewritten, the presence of the symbol and the length of the block were each made a
precondition, so it can never silently return zero again.

### The mechanisms measured for phase 2

| Question | Measurement |
|---|---|
| How does the SeedSigner pool work | It **keeps** the last 50 frames, drops the oldest when the 51st arrives, and hashes once at the end (`tools_screens.py:113-119`). The sliding window is not a design preference but a memory constraint: full `Image` objects are held in Python. Our chain is incremental, a frame is hashed and dropped, so going past 50 loses nothing |
| How does the user end the collection | `BTN_CAMERA_EXIT` ends the camera loop with `done = true` (`main/camera.c:556`); on exit `gather_camera_entropy` looks at the `nframes` condition and decides whether to produce a seed. So early exit is already supported; what changes is the callback stopping itself |
| Is on-screen feedback possible | The label node `label_node` is updated inside the function (`camera.c:547`, "Processing..."), but it is not exposed to the callback; `progress_bar`, on the other hand, is updated through `ctx` |
| How are synthetic frames fed to the emulator | The daemon RPC `set_camera_bytes` → `libjade_push_camera_frame` (`libjade/libjade.c:889-893`). The frame is 320x240 greyscale, 76800 bytes |

### 2.2 / 2.3 / 2.4 (and the threshold fix)

Three items closed in one commit: the change gate between consecutive frames, raising the pool to
50 with the user ending the collection, and the on-screen feedback.

**What the gate stores.** No full frame is stored (320x240 = 76800 bytes, with `ctx` on the stack
and inside `SENSITIVE_PUSH` coverage); the frame is reduced to an 8x8 grid and a 64-byte
block-average signature is kept. The difference between two signatures is measured as an L1
distance **after the mean difference is removed**, so that automatic exposure shifting the whole
scene by the same amount does not count as movement.

**The threshold was measured, not guessed.** The first write put 64 there and that value was wrong:
it sat inside the noise band it was supposed to eliminate. The measurement was made with a Python
twin of the `frame_signature` / `signature_change` pair in C (synthetic 320x240 frames):

| Case | Score | Outcome at 64 |
|---|---|---|
| Still camera, +/-8 LSB sensor noise, consecutive frames | at most 63 | rejected just below the threshold, no safety headroom |
| Still camera, +/-12 LSB | at most 78 | **false accept** |
| Still camera, +/-16 LSB | at most 114 | **false accept** |
| Still scene, +5 exposure steps, bright areas saturating | 65 | **false accept** |
| Still scene, exposure step, no saturation | **0** | correct reject |
| Real movement, 1 pixel shift | 89 - 128 | correct accept |
| Real movement, 2 pixels | 156 - 229 | correct accept |

The threshold was therefore raised to **128**. The number was also verified in the emulator by
running two builds; the same sequence of +/-16 LSB frames (40 frames) was driven at both
thresholds:

| Threshold | Frames accepted | Rejected |
|---|---|---|
| 64 | **7** (so 6 spurious frames entered the chain) | 28 |
| 128 | **1** (the first frame only; it has no predecessor to compare against) | 34 |

This is also the proof that the Python twin is valid: the twin predicted 8/39 accepts at 64, and
the measurement gave 7.

**Two boundary tests (emulator, `build_linux_nci_log`, `libjade_daemon` + `set_camera_bytes`).**

| Run | Fed | Counters | Result |
|---|---|---|---|
| (a) frozen sensor | 90 frames, same scene, +/-2 LSB noise per frame | 1 accepted, 155 flat, **575 unchanged**, 0 repeats | The progress bar **did not move** across 90 frames (the hash of the cropped region is the same), no seed was produced on exit, and the screen returned to the entropy menu |
| (b) real movement | 55 frames, the scene shifting 8 px each frame | **52 accepted**, 0 flat, 245 unchanged, 0 repeats | At 50 the label became "Enough - exit to finish" and the bar filled; collection **did not stop**, it continued until the user exited (52 > 50); a seed was produced on exit |

In (b) 52 of 55 frames counted: the emulator's camera loop sometimes cannot read the buffer before
it is overwritten. Frames drop on a real sensor too; since collection is now ended by the user
rather than by a counter, this costs nothing.

**A third run: replay ; the two gates complement each other.** In the first two runs
`nrejected_repeat` came out zero, which invited the reading "repeat rejection is in the shadow of
the other gate". The reading was wrong, because neither run had ever exercised the real scenario
for repeat rejection. `last_signature` is updated only on an **accepted** frame; so when
A(accepted) → B(accepted) → A arrives, the comparison is made through `change(B, A)`, and that
score is large, so the change gate lets A through. Only `frame_digests` can catch it. This is the
signature not of a frozen sensor but of a source **replaying frames**.

The third run exercised that directly: two different frames (the score between them 3673 in both
directions, more than twenty times the threshold) were fed alternately for 25 rounds.

| Fed | Counters | Reading |
|---|---|---|
| 2 frames x 25 rounds = 50 submissions | **2 accepted**, 0 flat, 22 unchanged, **21 repeats** | Only the first two frames entered the chain; every later A was eliminated by `frame_digests`, every later B by the change gate |

So the two gates close different threats and neither is in the other's shadow: the change gate
stops a **motionless** scene, repeat rejection stops a **looped** frame sequence. For a source to
pass both, the frames have to be both different from one another and never seen before.

**A process note about the measurement itself.** The first two attempts were silently measuring the
wrong screen: navigation driven by a fixed key sequence landed on the **QR scan** screen rather
than the camera entropy screen, and because the counter line was never written this looked less
like an error than like "the feature does not work". There were two separate causes. (1) In the
`build_linux_nci` build `LOG` is off, that is `CONFIG_LOG_DEFAULT_LEVEL_NONE` is defined and
`JADE_LOGI` prints nothing; the counters are visible only in the `build_linux_nci_log` build.
(2) There is an instruction screen between the home screen and Setup Type, and in the first
hand-driven run the press that passed it was never written into the sequence; a single missing
press shifted the whole navigation. The fix was to bind navigation to **a hash of the destination
screen** (`/probe/gotocam.py`): if the hash does not match, no frame is sent and no measurement is
made. A probe that says "0 findings" may be saying it does not work; what closed that possibility
in this round was the destination check.

### 2.5 the `Combined` mode

A fourth item entered the entropy menu. The design's only real decision was **where** the two
sources would be combined: a shared `mbedtls_sha256_context` could have been given to both
collectors, or each collector could have been left as it is today and only the outputs combined.
The second was chosen.

The reason is architectural rather than cryptographic: what gives the pure dice path its value is
that someone with paper, dice and sha256 can **reproduce the result on their own**. A shared hasher
would have required replacing the single `wally_sha256(rolls)` call inside `gather_dice_entropy`
with an incremental chain; the dice path would not have lost its verifiability at that moment, but
it would have stopped being a function that stands on its own. Combining at the output level
touches neither. Since both halves are already 256-bit digests, the result of
`sha256(dice || second)` is no weaker than the stronger half; this is the standard way of combining
two independent digests.

If there is no camera the second half comes from the device CSPRNG and the menu label becomes
`Dice + Device`. This is the concrete form of the ban on "silently working with something missing":
rather than offering something weaker under the same name, we change the name. That the label
really is bound to the build was measured at the binary level:

| Build | `Combined` | `Dice + Device` |
|---|---|---|
| `build_linux_nci_log` (CAMERA=1) | 1 | 0 |
| `build_linux` (CAMERA=0) | 0 | 1 |

It has to be written plainly that the measurement **stayed at the binary level**: in the
camera-less build the setup tree ends at the "Connect Jade to a compatible wallet app" screen and
the entropy menu cannot be reached through the interface at all. So the `Dice + Device` label was
**not seen** on screen; that it is bound to the build was verified in both directions with
`strings`. Checking it on screen is work for a device round.

Item (c) of the plan ; writing the result into the entropy slot so that eligibility for SeedQR
export is preserved ; needed no separate code: `Combined` shares the **same** `mnemonic_new` call
site with the dice path (`main/process/mnemonic.c`), only the collector differs. The eligibility
comes from the structure, not from an added branch.

The ceremony order is dice first, then camera. If the user leaves the camera before enough frames
are collected, **no seed is produced and the dice rolls are dropped too**. This is deliberate:
accepting the dice half on its own would mean handing the user a single-source wallet from under a
menu item that promised two sources.

**Emulator measurement.** All four runs were 12 words, dice input fifty presses of '1' (because the
wheel starts on the first face, fifty clicks give a deterministic sequence), and the same 55-frame
panning scene on the camera runs. No screen was ever displayed; the raw buffer was hashed inside
the container and wiped there, and only the digests were compared.

| Run | Path | Digest of screen 1 | Camera counters |
|---|---|---|---|
| A | `Dice Rolls` | `358dd88f113bf05e` | ; |
| B | `Dice Rolls` (same input) | `358dd88f113bf05e` | ; |
| C | `Combined` | `fe8ec9208b034836` | 54 accepted, 5 flat, 2 unchanged |
| D | `Combined` (same input) | `4bd590bb0b09c31b` | 55 accepted, 4 flat, 2 unchanged |
| E | `Combined`, camera abandoned | no seed, returned to the entropy menu | 0 accepted, 9 flat |

This says three things at once: **A = B**, so the pure dice path is deterministic; **C != A**, so
with the same dice sequence the camera and device contributions do enter the output; **C != D**, so
even with the same dice and the same frames the result changes, and only the device CSPRNG can do
that. That the dice path was **not changed at all** in this round is proven not by A = B but by
`git diff`: the body of `gather_dice_entropy` was not touched. Determinism and untouchedness are
two separate claims resting on two separate proofs.
The first screen (`57ef8f3d1b2d8988`) was identical in all four runs; it is a static warning
screen, which is why the comparison starts from the second screen.

**Side finding: the camera-less build was already broken.** While trying to compile the
camera-less branch of `Combined`, it turned out that the `build_linux` (CAMERA=0) build had **not
been compiling before this round either**. The cause was phase 2.3 itself: it added the
`label_out` parameter to the `jade_camera_process_images` signature, `main/camera.c` and its
callers were updated, but the definition in the `#else` (camera-less) half of
`libjade/esp_camera.c` was left unchanged. Because only the `nci` variants were being compiled, it
never surfaced. The same class of error appears for the second time in this round (the first being
the counter line never printing while `LOG` was off): **measuring in one build variant and drawing
a general conclusion.** The stub was fixed, and from now on all three builds (nci, nci_log,
camera-less) are compiled at the end of every phase.

**The 24-word run.** All five runs above were 12 words, so the layout in which the second half of
the `parts` buffer starts at offset 32 had passed only the compiler and the `JADE_ASSERT` check; it
had never run. A separate run was made and the chain of evidence closed as follows: `24 Words`
selected was verified by screenshot, then **99 dice rolls were fully consumed** (had 12 words been
selected it would have ended at fifty per `DICE_ROLLS_12WORD` and the remaining clicks would have
fallen onto the camera screen), the camera half completed with the counters **55 accepted, 164
flat, 10 unchanged, 0 repeats**, and the ceremony reached the result screen without tripping an
assert. So on the `entropy_len = 32` path `parts` was used as 64 bytes, the second half was written
at offset 32 and read back from there.

The digest of the result screen was `57ef8f3d1b2d8988`, which is **exactly the same** as the digest
of the static warning screen in the 12-word runs. A single equality says two things: that screen
does not depend on the seed (so the decision to start the comparison from the second screen was
right), and the 24-word flow arrives at the same screen as the 12-word flow.

---

### Phase 2 closing review

**Gate B (`codex review`): CLEAN.** No findings. The reviewer's wording: against the merge base
it was given, no actionable correctness defect was found; the updated camera API is applied
consistently at its call sites, and the entropy collection and cancellation paths are consistent.

**Adversarial review (`codex exec -s read-only`, scoped to the phase 2 diff, limited to seven files): 0 P1, 0 P2, 1 P3.** The
brief widened the threat model explicitly: the attacker can put a scene of their own choosing (a
screen, a printed page) in front of the camera, can freeze the sensor and replay it, and can read
the SD card offline afterwards. The focus list was where the combination happens, truncation in the
12-word case, the abandon path, the lifetime of sensitive buffers, the frame gates, and integer and
array bounds. Five of those six areas came back **clean**; even though the brief pointed directly
at the output-level combination decision, no separate defect came out of it.

#### P3: the frame gates are not a proof of entropy

The finding is concrete and was verified. An attacker can build a checkerboard A whose 8x8 block
averages alternate between 64 and 192, and its inverse B, and feed `A0, B0, A1, B1, ...`; shifting
two pixels in a single block by `+k` and `-k` on each repetition makes the frame unique at byte
level **without changing** the block averages. The result: the contrast threshold is passed easily;
`signature_change` gives 64 blocks x 128 difference = **8192** on every transition, sixty-four
times the `CAMERA_FRAME_MIN_CHANGE` value of 128; and because every frame's SHA256 differs, repeat
rejection never fires. All fifty frames are accepted and the collection counts as "enough", while
the attacker knows the entire sequence. A second sub-finding: because the digest history is written
into a ring buffer with `nframes % CAMERA_ENTROPY_FRAMES`, a deterministic loop of fifty-two frames
is accepted indefinitely.

No code change was made, and that is not a deferral but the decision itself. The grounds have four
layers:

First, the claim the gates make in the plan is about **sensor failure**, not an attacker-controlled
scene. Phase 2.2 added the gate so that a half-frozen sensor would be fail-closed; that is
SP 800-90B's continuous health test posture, and health tests by definition do not measure entropy,
they only catch obvious failure. What widened the threat model to an attacker-chosen scene was the
brief itself; the finding is a product of that widening, not a refutation of the design's own
claim.

Second, a gate of this class **cannot be closed in code**. Entropy is not a property of the data
but a function of the attacker's knowledge; against an input the attacker chooses, no
within-frame or between-frame statistical test can give a proof of unpredictability. Raising the
threshold, adding a periodicity test, lengthening the history ; all of them are beaten by the
attacker increasing their period by one. Writing them would be a direct violation of the principle
that the simplest implementation which fully meets the current requirement is the right one, and
that speculative layers are banned.

Third, the floor was already laid by phase 2.1. Even if the frames are entirely known, the last
input to the chain is `get_random()`, so the seed remains unpredictable; the reviewer classifying
the finding as P3 rather than P2 rests on exactly that. There is no hole from the point of view of
stored seeds; what is lost is not the secrecy of the seed but the **claim** that "the camera added
this much unpredictability".

Fourth, the ring buffer sub-finding is already sufficient within its own threat model: a frozen
sensor returns a small number of stale frames and the fifty-frame window catches it. A loop longer
than fifty is no longer a stopping pattern.

Two comment lines were corrected against this, because they were **wrong**: the phrase "check the
whole set" in `entropy_sources.c` is not true after fifty frames (what is checked is the last fifty
digests), and why truncation in the 12-word case is lossless was written down nowhere.

This is the camera's version of the **no-claims note** phase 3 wrote for the duress PIN: the
mechanism claims no more than it delivers. The only place where a legitimate bit figure could be
written remains phase 2.6 (camera characterisation on real hardware), and that measurement too
measures sensor noise; the attacker scenario here is outside its scope as well.

The closing commit contains only these two comments and the documents. Gate B and the
adversarial review examined the diff before those closing changes; because a comment and
document change cannot produce a P1 or P2, the review was not rerun.

---

## Phase 3 ; duress PIN storage hygiene (2026-09-03)

### What this phase does NOT do (the no-claims note)

This note is part of the fix, not a document to be written afterwards. Without it the code below
produces a false assurance.

The duress PIN is six digits, that is about 20 bits. An attacker who takes the card gets
`pijade-settings.bin`, reads the salt and tries all 10^6 candidates on their own machine; whatever
the PBKDF2 iteration count, that search takes minutes, not hours. A slow KDF slows down entry on the
device, not the attacker's desktop. The same search also answers the question "is a duress PIN
set", so hiding its existence is broken on its own as well (it is not hidden anyway, by decision).

**The conclusion: no storage transformation makes a six-digit secret deniable against someone who
reads the card.** Upstream Jade gets this for free from ESP32 flash encryption; piJade lost it in
moving to an SD card. This is the same class as the absence of secure boot: a gap coming from the
hardware, not an error coming from the code.

The **only** thing this phase gains: someone opening the card in a hex editor can no longer read
the PIN by eye, and the device no longer prints the stored PIN on its own screen. Nothing more
should be claimed.

### 3.1 Salt and verifier

The field grew from six raw digits to `WALLET_ERASE_PIN_RECORD_LEN` = 48 bytes: a 16-byte random
salt (`get_random`) and the 32-byte output of `wally_pbkdf2_hmac_sha256`. The storage API **lost
its getter** (remove the path that is being superseded); in its place came
`storage_verify_wallet_erase_pin()` and `storage_wallet_erase_pin_exists()`. The comparison stays
constant time (`sodium_memcmp`).

The iteration count is **2048** and deliberately low. Measured (x86-64 container, `-O2`, mean of 50
repetitions): cost=1000 → 3.995 ms, **cost=2048 → 8.132 ms**, cost=4096 → 16.233 ms. This number
was not measured on the Pi Zero W and is not being guessed; it can be measured in a device round,
but the decision does not change even if it is not, because **this is not a security parameter**:
the device already allows three attempts, the gain against an offline attacker is zero, and so a
high count would only slow down the legitimate user's PIN entry.

The timing side effect is recorded: if a duress PIN is **not set** the hash is never computed, so
the wrong-PIN path takes a few ms longer when one is set. That opens a new channel for the question
"is a duress PIN set". It was **not** normalised by running a dummy hash: the menu already shows
whether one is set (by decision), so there is nothing to hide, and adding a speculative layer would
go against keeping the implementation the simplest one that meets the requirement.

### 3.2 The screen dropped to "set"

The options screen in `main/process/dashboard.c` printed the stored PIN with `format_pin()`; that
was the real obstacle phase 0.7 found, not the duress mechanism itself. The screen now looks at the
result of `storage_wallet_erase_pin_exists()` and the row in `main/ui/dashboard.c` carries the
fixed text "Enabled". The layout (a three-way `vsplit`, 35/35/30) was **not changed**; the label
became "Wallet-erase PIN:" instead of "Wallet-erase PIN set:", that is, shorter. Change and Delete
work as before. `format_pin()` stays, because it has another caller (the PIN change screen).

The menu condition (`hw_pin_unlocked`) was **kept**, but the reason in the comment changed: the old
reason was "the screen prints the PIN in the clear" and it fell away. The reason that replaces it
is more fundamental: this screen offers to change and to delete the duress PIN, so whoever can open
it can remove the protection the stored wallet rests on. A device unlocked with a temporary wallet
should not pass that test.

### 3.4 Card format `PIJADES3` → `PIJADES4`

The schema changed because the field width changed. The rejection is **per file**: when an old card
is rejected it is not the duress PIN that goes but **the stored wallet blob and all the settings**,
and the device comes up as if it were not set up. No migration layer was written. **Existing cards
are re-prepared by hand**; this is the plan's only item that invalidates existing cards.

The digit range check in `libjade/pijade_settings.c` (`p[i] > 9` → reject) was **removed**. That
removal was mandatory and was the first scenario of the pre-mortem: the field now carries hash
bytes, and had the check stayed, every card with a duress PIN set would have been rejected on first
boot and would have silently lost everything. The `networktype` and `antireplay` checks stay in
place; the asserts they rest on are still reachable.

### 3.5 The behaviour of the duress path

`check_wallet_erase_pin()` in `main/process/auth_user.c` changed only its **call**
(`storage_verify_wallet_erase_pin` instead of `storage_get_wallet_erase_pin` + `sodium_memcmp`).
The screen text, the "Internal Error" rejection, `power_shutdown()` and the fact that an erase
failure only goes to the log all stay exactly as they were. The indistinguishability of this path
is a deliberate design and it was not touched in this phase.

### Measurements

**The storage layer (a new permanent test, `pijade/tools/duress_pin_test.c`, 15 checks, all
passed).** `pijade/tools/settings_test.c` verifies the width of the field but cannot look at its
content, because it never goes through `main/storage.c`. The new test sets a PIN through the real
storage path and reads the raw record back from the NVS layer: the record is 48 bytes; the sequence
`{1,2,3,4,5,6}` appears **nowhere** in the record; the correct PIN verifies, a wrong PIN and a
short PIN do not; setting the same PIN a second time produces a **different** record (the salt
really is random) and still verifies; after deletion neither existence nor verification remains.

**The settings file (`settings_test.c`, 89 checks, all passed).** `walleterasepin` goes out and
comes back unchanged at 48 bytes; a file headed `PIJADES3` is now **rejected** (a new case); the
digit range case was removed, because the rule itself was removed.

**The test's own copy of the constant broke.** When the version was raised to `S4`,
`settings_test.c` failed seven checks; none of the failures had anything to do with the duress
field (multisig, descriptor, OTP, HOTP, capacity). The root cause was `settings_test.c:158`: in its
own file builder the test **hand-copied** the magic number as `"PIJADES3"`, that is, a second copy
of the format constant existed. A positive control was set up (the change was reverted with
`git stash` and the test rerun: **all passed**), so it was measured rather than guessed that the
breakage came from me. A family sweep was done: there is no other copy of the string `PIJADES` in
the code or the scripts.

**All three builds** (`build_linux`, `build_linux_nci`, `build_linux_nci_log`) compile without
error. The two warnings are the known `noreturn` debt at `libjade.c:217`, unrelated to this phase.

**No screen round was run in the emulator; the obstacle itself was measured.** The menu gate of the
duress screen is `hw_pin_unlocked` (`main/process/dashboard.c:2590`), which requires
`keychain_has_pin()`. That in turn means `has_encrypted_blob` (`main/keychain.c:898`), and the flag
is only born when `storage_set_encrypted_blob()` runs (`main/keychain.c:696`); the input to that
path is the `aeskey` coming from the pinserver (`main/process/pinclient.c`). In an airgapped fork
there is no pinserver, so this screen cannot be reached in the emulator; the obstacle is not an
assumption copied from one round to the next. Because the storage layer was measured directly by
the test above and the screen change is nothing but static text, the visual round was recorded as
**debt for a device round**.

**The ESP32 target does not build in this fork and is out of scope.** `main/storage.c` also
compiles in upstream's ESP32 build; there is no `PIJADES4` rejection there, because there is no
card file and the field lives in NVS. If an old six-byte NVS record is encountered,
`read_blob_fixed()` sees the length mismatch, returns `false` and logs it (`main/storage.c`), so
the duress PIN reads as "not set"; there is no crash path. Since piJade produces no ESP32 image
this behaviour was not measured, only read from the code.

## Phase 5 ; improvements that go beyond the reference (2026-09-04)

Four of the phase's five items are code (5.1, 5.3, 5.4, 5.5) and one is documentation (5.2). The
code items converge in one place: the path by which a scanned wallet QR enters the device.

### The shared change: the confirmation is asked after the fingerprint is known

The old order was inside `handle_mnemonic_qr()`: the question "Wallet QR identified. / Load this
wallet?" (default `Yes`), then `derive_keychain()`. Because the question was asked before the words
were processed, it could say nothing beyond "this wallet". In the new order `derive_keychain()`
derives first with an empty passphrase (`main/process/mnemonic.c:1602-1607`), so the fingerprint is
known, and the confirmation is asked after that (`:1686-1694`).

The structural consequence: loading can now end without loading and without an error (the user
declined, or the wallet was already loaded). Since a `bool` cannot express that third ending, the
return type became three-valued (`main/process/process_utils.h:151-165`) and the caller separates
all three (`main/process/dashboard.c:887-897`): the error screen only on the `FAILED` path, the
carrier handover only on the `OK` path. Because the setup path (`initialise_with_mnemonic`) sees
none of these screens, there any result other than `OK` is an error as before
(`mnemonic.c:1956-1961`).

The pre-derivation with an empty passphrase does two jobs at once and is therefore not repeated: if
no passphrase is entered it is the result itself, and if one is entered it was only the input to
the duplicate check and the real wallet is derived again (`mnemonic.c:1650-1656`).

### 5.1 The load confirmation: fingerprint, default `No`, provenance warning

The screen: title `Load Wallet`, body `Load wallet <8-digit fingerprint>?` + "Whoever made this QR"
+ "knows this wallet.", footer `blkstrm.com/temporary`, initial selection `No`
(`mnemonic.c:1680-1697`). The fingerprint is produced as uppercase hex from the first
`BIP32_KEY_FINGERPRINT_LEN` bytes of `keydata.xpriv.hash160`, so it is the same value that appears
in the Session list.

The reasoning has two parts: (a) the user can only answer "is this the wallet I expected" if they
see an identity; (b) a wallet QR comes from outside and whoever prepared it knows the words, so
loading it is not a harmless default. Upstream's default was `Yes`; in SeedSigner this warning does
not exist at all.

### 5.3 The order of the wallet menu

The requested order was written into the code: Scan QR, Address Explorer, Export Xpub, Sign
Message, Registered Wallets, BIP85, Backup, Forget (`dashboard.c:3104-3140`). The conditions under
which rows appear did not change; only the order did. The array size was reinterpreted as `4 + 4`
(`dashboard.c:3058-3065`): four unconditional, four conditional (BIP85 and Sign Message on a
feature flag, Backup on entropy, Forget on a temporary wallet), and in a camera-less build three
drop out at compile time, so the size is an upper bound rather than a count.

### 5.4 When the same wallet is scanned a second time

The old behaviour: slot selection found the first empty slot, with no comparison against loaded
wallets; scanning the same SeedQR twice showed the same fingerprint twice in the Session list and
the user could think they had two separate wallets.

The new flow (`mnemonic.c:1577-1615`):
1. Derive with an empty passphrase and compare the resulting wallet against the loaded slots
   (`keychain_find_slot()`, `main/keychain.c:179-199`).
2. If there is no match the flow continues as before.
3. If there is a match and the device does not ask for a passphrase (`PASSPHRASE_NEVER`): the
   message "This wallet is / already loaded" and a switch to that slot (`switch_to_held_wallet()`,
   `:1548-1552`), result `ABORTED`. With that setting no other wallet can come out of those words,
   so there is nothing to ask.
4. If there is a match and the device does ask for a passphrase: a yes/no question, title
   `Add Wallet`, body "This wallet is already / loaded. Add it again / with a passphrase?", initial
   selection `No` (`:1605-1610`). `No` → switch to the slot, `ABORTED`. `Yes` → straight to the
   passphrase entry screen; the question was asked once and is not asked again
   (`passphrase_asked`, `:1613-1622`).

**The identity criterion is NOT the fingerprint but the master key** (a review finding). In
the first write the comparison was over the first four bytes of `xpriv.hash160` and the reason
given was "this is the value shown to the user". That reason was wrong: the fingerprint is 32 bits
and a collision can be sought deliberately, so a crafted QR could make the device say "this is
already loaded" and force **a different** wallet to be activated. The comparison is now made over
`priv_key` + `chain_code` (`keychain.c:192-196`); those are the wallet itself, and two wallets that
agree on both are the same wallet. Every comparison is constant time (`sodium_memcmp`); the loop
itself is not, because what it leaks (whether there is a match, and which) is exactly what the next
screen says. The fingerprint remains only as the value shown on screen.

**The second check is unconditional** (`:1647-1651`). Its reason was measured: if the user says
`Yes` and then leaves the passphrase screen empty (upstream's `<no passphrase>` confirmation), the
same wallet the first check found comes out. The check is two memcmps over keys already in memory,
which is why it was not narrowed to only the cases that need it.

**The capacity rejection is in one place** (`:1653-1663`): after the second identity check and
before the confirmation screen. This is the first point at which the device knows a slot is really
needed; every ending above it leaves the table as it is. The cost is written in the code: the
capacity rejection is sometimes shown after the passphrase has been entered. The alternative was to
reject scans that need no slot at all, and two review rounds showed exactly that error.

### 5.5 Passphrase: a two-option question before the keyboard

`get_passphrase()` no longer opens the keyboard directly; it first asks a screen titled
`Passphrase`: "Enter a passphrase?", `Enter` on the right, `Skip` on the left, initial selection
`Skip` (`mnemonic.c:1531-1541`). If the frequency is `PASSPHRASE_NEVER` the screen never opens and
the behaviour is as before (`:1525-1529`).

The reasoning: continuing without a passphrase was possible before too (leave the keyboard empty
and pass the `<no passphrase>` confirmation), but the keyboard read as "type something"; the
"Always Ask" setting looked like an obligation. `Skip` is the initial selection because it is the
ending that leaves the wallet as it is.

The entry screen itself was split out as `enter_passphrase()` (`:1501-1517`), because the `Yes`
branch of 5.4 has already asked its own question and must not ask the same one twice.

**The scope is two places and both go through the same function.** The two production callers of
`get_passphrase()`: unlocking with a PIN (`main/process/auth_user.c:241`, behind
`keychain_requires_passphrase()`, `:233`) and loading a wallet (`mnemonic.c:1640`). So the screen
appears on both paths; that is evidence from the code, while screen evidence could only be obtained
for the second path (see the constraint below).

### Emulator evidence (the 2026-09-03 and 2026-09-04 rounds)

The device was set up with public test vectors (`debug_set_mnemonic`; `abandon` x11 + `about` →
`73C5DA0A`, `abandon` x23 + `art` → `5436D724`). `Options > Preferences > Passphrase >
Frequency = Always Ask`; that setting is persistent because it is `PASSPHRASE_ALWAYS` ("Next Login
Only" is not persistent: `keychain_persist_key_flags()` writes only the AUTO_DEFAULT flag, and
`finalise_slot()` reloads the flags from storage; that is why the first attempt yielded no
measurement).

| What was measured | Screen | Frame |
|---|---|---|
| The 5.5 choice screen | `Passphrase` / "Enter a passphrase?" / `[Skip]` selected, `Enter` on the right | `z1_2` |
| The 5.1 confirmation screen | `Load Wallet` / "Load wallet 5436D724?" / "Whoever made this QR" / "knows this wallet." / `No` selected | `z2_0` |
| The 5.1 accept path | `Yes` → the load completes, a new slot in Session | `z3` |
| The 5.4 duplicate question | `Add Wallet` / "This wallet is already / loaded. Add it again / with a passphrase?" / `No` selected | `r2` |
| The 5.4 `Yes` branch | Straight to the `Enter Passphrase` keyboard; the second question is not asked | `z6` |
| The 5.4 second check | Empty keyboard → `<no passphrase>` confirmation → "This wallet is already loaded"; no new slot opened | `z8`, `z9` |
| The resulting state | Exactly two fingerprints in the Session list (73C5DA0A active, 5436D724) | `zb` |
| The 5.3 menu order | Scan QR, Address Explorer, Export Xpub, Sign Message, Registered Wallets, BIP85, Backup, Forget | `m5_0`, `m5_5`, `m6_1` |

**The half that could not be measured:** the PIN-unlock side of 5.5 cannot be run in the emulator.
`keychain_requires_passphrase()` is only true for a wallet unlocked with a PIN, which depends on
`storage_set_encrypted_blob()`, which depends on the `aeskey` from the pinserver; an airgapped fork
has no pinserver. The same obstacle as phase 3's duress screen; recorded as **debt for a device
round**.

### Screen dim swallows the first press (a trap for every emulator measurement)

In the first verification round the `right` selection did not move and `click` pressed the wrong
button (`No` instead of `Yes`). The root cause is not in the GUI: when the screen has dimmed, the
first event to arrive only wakes the screen. `idletimer_register_activity(true)` returns `true`
while the screen is dim, and `select_next_right()` and `gui_front_click()` return early
(`main/idletimer.c`, `main/gui.c:2576-2590`). The evidence is in the log:
`INFO:idletimer.c:154: Activity while screen disabled - powering screen`, exactly on the tick of
the swallowed event. The fix is on the measurement side, not in the code: `Screen Timeout` was
raised to 10 minutes and a disposable wake-up press was put at the head of the sequences. This is a
trap that affects every emulator measurement driven by blind key sequences.

### 5.2 The card cloning record (documentation; NO code change is recommended)

The question left open in A1 was: the persistent wallet's resistance to cloning depends on the
server's monotonicity check; can an additional obstacle be built device-side?

**The measured state.** The only persistent place the device can write to is the card. The attempt
counter and the replay counter are NVS fields (`counter`, `antireplay`; `main/storage.c:27-28`) and
both are in the list of fields written to the card (`PERSISTED_FIELDS` in
`libjade/pijade_settings.c`); the file is `/boot/firmware/pijade-settings.bin`
(`pijade/images/prepare-image.sh:107`, `pijade/host/pijade_host.c:718`). So a bit-for-bit copy of
the card rewinds both counters. This is already written in the code
(`pijade_settings.c:11-16`).

**Conclusion: no concrete device-side obstacle emerged.** For an obstacle to work it needs a
monotonic state, off the card, that a copy cannot rewind. Every writable state piJade has today is
on the card; no counter that sits on the card can meet that condition, because what is copied is
the counter itself. The only candidate off the card is the SoC's one-time-programmable memory
(OTP); it was **not measured** in this round: `vcgencmd` is in the image
(`prepare-image.sh:702`), but whether the OTP is readable, whether it is writable, how many bits it
has, and how many boots a counter there would survive were not measured, so it cannot be
recommended as a mechanism. Writing a recommendation without measuring is exactly what was avoided
this round.

So A1's sentence is not updated but confirmed: **the persistent wallet's resistance to card cloning
does not end on the device, it rests on the pinserver's replay counter check.** SeedSigner does not
have this threat because it has no persistent storage; in piJade this is the price of the
persistent wallet's convenience and it is paid knowingly. On the temporary wallet (SeedQR) path
nothing at all sits on the card, so the threat does not apply.

### The measurement of the capacity and duplicate branches (2026-09-04)

Because filling eight slots would require eight separate SeedQRs, the measurement was run on a
`build_linux_nci_log` build with `MAX_SEED_SLOTS` temporarily reduced to **1**; the constant only
determines the size of the table, and the logic under measurement is the same. The constant was
restored after the measurement and all three builds recompiled (no trace in `git diff`).

| Case (table full) | Expected | Seen on screen | Frame |
|---|---|---|---|
| QR of a loaded wallet, frequency `Never` | Switch to the slot | "This wallet is already loaded" | `n3_2` |
| QR of a new wallet, frequency `Never` | Capacity rejection | "No free wallet slot - forget one or log out" | `n6_2` |
| QR of a loaded wallet, frequency `Always Ask` | The offer screen should appear | `Add Wallet` / "This wallet is already loaded. Add it again with a passphrase?" / `No` selected | `q5_1` |
| `Yes` on that offer + passphrase `a` | The new wallet needs a slot and is rejected | "No free wallet slot - forget one or log out" | `q9_0` |

The last row is the measure of the accepted cost: the capacity rejection comes after the passphrase
has been entered. In exchange, none of the first three rows rejects a scan that needs no slot.

---

## Registered Wallets ; record ownership across multiple slots (2026-09-04)

The question raised during phase 5: how do registered wallets work, or how should they work, with a
seed loaded by PIN versus a temporary seed loaded from a SeedQR? A read-only audit was done first,
the findings were presented as a decision, and after approval two changes were made.

### What the feature is and how it behaves under the two ways of loading

A record is a multisig (or descriptor) definition the device has sealed to a single wallet. The seal
is `wallet_hmac_with_master_key()` (`main/wallet.c:1340-1352`): an HMAC-SHA256 produced with the
active wallet's master key. A record is therefore valid only for the wallet that registered it.

**There is NO behavioural difference between a wallet unlocked with a PIN and a temporary wallet
loaded from a SeedQR.** Both seal in the same way and in both cases the record is written
persistently to the card. The only difference is in the consequence: a temporary wallet is gone
when the session ends, its record stays on the card, and nothing says that it is orphaned. The
carousel marker added this round (below) is exactly that indication.

### The measured state

| Finding | Evidence |
|---|---|
| The record key is the name alone; there is no wallet scoping | `main/storage.c:822-843` (`MULTISIG_NAMESPACE` is a flat namespace), `:846-870` (the descriptor equivalent) |
| Overwriting looks only at the name | `main/process/register_multisig.c:99` `storage_multisig_name_exists()`; no identity check |
| The overwrite warning does not name the owner | `make_final_multisig_summary_activities()` in `main/ui/multisig.c`: `"WARNING"` / `"Overwriting existing"` |
| The menu lists every record and only says whose it is once you go in | `main/process/dashboard.c:1112-1125` (the name list), `main/ui/multisig.c:47-56` and `main/ui/descriptor.c:50-60` (`"Not valid for"` / `"current wallet"`) |
| The QR, address explorer and signing paths already use only valid records | `main/qrmode.c:688,693,1278,1286`; `main/process/sign_psbt.c:411-440,455-470`; the RPC listing `main/process/get_registered_multisigs.c:96-118` ("Corrupt or for another wallet - just log and skip") |
| Capacity is device-wide, not per wallet | `main/multisig.h:13` and `main/descriptor.h:14`, both 16 |
| Records sit unencrypted on the card | `libjade/pijade_settings.c:92-99`: multisig and descriptor entries are `modulo 0` (they do not have OTP's `modulo 16` AES); `main/multisig.c:296-300` is an HMAC gate, not decryption |
| **A descriptor record can never be created on this device** | The QR path recognises only a multisig file (`main/qrmode.c:1789-1800`); the serial carrier is an empty function in piJade (`libjade/libjade.c:290`). The descriptor branch in the menu is always empty on a production device |

There is NO risk of signing with the wrong wallet: signing skips a record that fails the HMAC. The
problem lies on the axes of visibility, data loss, capacity and privacy.

### The two changes made

**(1) The overwrite gate** ; `main/process/register_multisig.c`, `main/process/register_descriptor.c`

If an existing record cannot be read by this wallet, a question comes BEFORE the confirmation
screens of the registration flow: `Name In Use` / "Existing record is not readable by this wallet.
Replace it?" ; initial selection `No`. On `Yes` the flow continues into upstream's own
"WARNING / Overwriting existing" warning, so two warnings stand one after the other: one says the
record cannot be read, the other says what the operation is.

The question is asked only for an unreadable record. If the same wallet updates its own record
(a valid HMAC) the old behaviour continues unchanged; an identical record already returns early
(`register_multisig.c:104`).

**Why the text does not name the owner.** The first draft said "This name belongs to another
wallet". An unreadable record either belongs to another wallet or is this wallet's own record
corrupted (a bit flip on the card, a half write); the HMAC cannot tell the two apart. Text that
names an owner would give the user plainly wrong information in the second case ; someone trying to
recover their own record would read that a second wallet exists which does not. The new text says
only what is observed: the record cannot be read with this wallet. For the same reason the log line
makes no attribution either.

Even though the descriptor equivalent is unreachable on this device, it was fixed alongside for
code symmetry (a family sweep). On both branches the loaded record goes on the heap rather than the
stack: `descriptor_data_t` is around 3 KB, `multisig_data_t` is over a kilobyte, and both run on
the stack of a process task.

**(2) An ownership marker in the carousel** ; `main/ui/select_registered_wallet.c`, `main/process/dashboard.c`, `main/qrmode.c`

In the `Registered Wallets` carousel, the type label is replaced by `"Not This Wallet"` when the
record does not belong to this wallet. Validity is computed with `multisig_get_valid_record_names()`
and `descriptor_get_valid_record_names()` (both already existed), and the result is passed to the
selector as `bool` arrays. The selector's signature grew by two parameters; passing `NULL` means
"the list has already been reduced to the valid ones", and the caller at `main/qrmode.c:702` uses
that, because its list is already filtered.

**The list was deliberately not filtered.** Showing only valid records would also have closed the
only way of deleting a record left behind by a wallet that is not loaded.

The `UPDATE_WALLET_CAROUSEL` macro in the same file was fixed to read the name from its own index
rather than from `selected`. The two were the same value at every call site, so nothing wrong was
appearing on screen; they no longer have to be the same.

### Emulator evidence (2026-09-04, public test vectors)

Setup: `73C5DA0A` (wallet A) and `5436D724` (wallet B), `build_linux_nci_log`, registration done
over RPC (`register_multisig`, mainnet 2of2 `wsh(multi(k))`; both signers are different derivation
paths of the current wallet, because `validate_signers()` looks for the device's own fingerprint in
the quorum).

| Step | Expected | Seen on screen | Frame |
|---|---|---|---|
| A registers "Vault" | The normal flow | `Register Multisig` summary → signers → confirmation, `Success` | `r8_scr0`, `ra_1` |
| B opens the `Registered Wallets` carousel | The ownership marker | `View Wallet` / **"Not This Wallet"** / `Vault` | `sf_car` |
| B tries to register under the same name | The question saying it is unreadable, `No` selected | `Name In Use` / "Existing record is not readable by this wallet. Replace it?" / `No` | `t9_gate` |
| `No` is chosen | The registration is rejected, A's record stands | Log: `register_multisig.c:131` "existing record not readable by current wallet: Cannot de-serialise multisig wallet data" + `jade_pushing out reject` | `ta_no` |
| `Yes` on the same question | Upstream's warning follows | `Register Multisig` / "WARNING" / "Overwriting existing" | `sq_1` |
| B's carousel after the overwrite | Now B's record | `View Wallet` / **"Multisig Wallet"** / `Vault` | `ss_car` |

The gate rows (`t9_gate`, `ta_no`) were re-measured AFTER the text was corrected; the other rows
are from the first round and those screens did not change.

### Side finding: the home screen's fingerprint goes stale after `debug_set_mnemonic`

Seen during the measurement: with wallet A loaded, when wallet B was loaded through
`debug_set_mnemonic`, the bottom line of the home screen kept saying `73C5DA0A` (A) while the
`Session` menu correctly showed `5436D724` (B) (`t6_home`, `t7_session`). Entering and leaving the
menu does not fix it either (`t8_home`).

**Why.** The home screen's label is written only by `UPDATE_HOME_SCREEN`
(`main/process/dashboard.c:3564-3570`), which runs after `do_dashboard()` returns; and
`do_dashboard` does not return as long as `keychain_get() == initial_keychain`
(`dashboard.c:3468`). Every call to `keychain_set()` empties and refills **slot 0**
(`main/keychain.c:104-108`), and `occupy_slot()` sets `keychain_data` to
`&keychain_slots[0].keydata` (`keychain.c:70`) ; so even when the wallet changes, **the pointer is
the same address**. The loop sees no change and the screen is not rewritten.

**Unreachable in production; measured.** The only production call that hands `keychain_set()` a
fresh keydata is `main/process/mnemonic.c:1697`, and it is only entered with
`into_free_slot == false`, which is the setup path (`mnemonic.c:1944`): there the previous pointer
is `NULL`, so it changes and the screen is refreshed. The path that adds a second wallet while one
is loaded uses `keychain_load_into_free_slot()` (`mnemonic.c:1691`) and fills **an empty slot**, so
the address changes. Every other `keychain_set()` call passes `keychain_get()` as its argument
(`auth_user.c:259,332,393`, `dashboard.c:654,743,904`), which never enters the function's copying
branch. This count first missed `main/keychain.c` itself; there are two more calls inside that file
that hand over fresh keydata (`keychain.c:608` passphrase derivation, `keychain.c:872` opening the
blob with a PIN) and both are on the PIN path. Both give the same result: their only production
callers are inside `get_pin_load_keys()`, and that function rejects a loaded wallet at entry with
`JADE_ASSERT(!keychain_get())` (`auth_user.c:202`), so unlocking with a PIN always starts from a
`NULL` pointer, the address changes and the screen is refreshed. `debug_set_mnemonic` is not in the
production image and has no screen-related call either (the file contains no `gui_` or `dashboard`).

Conclusion: this is NOT a regression of the phase 5 multi-slot work, it is specific to the debug
path. No fix was made; changing the exit condition of the main loop for behaviour that is invisible
in production would carry more risk than it gains.

### What was not done, and why (debt)

**Encrypting the records on the card ; CLOSED (2026-09-06).** This was debt when the section was
written and the reason given was wrong; both stand here because the wrong reason teaches more than
the record itself.

What was written that day: someone who takes the card can read the vault name, the xpubs and the
derivation paths of a wallet whose seed was never written to that card, and it cannot be encrypted
because **there is no at-rest key waiting on the card for a temporary wallet**. The second clause
was mistaken. The key does not need to wait on the card: the record body is sealed with a key
derived from the wallet's master key while the wallet is in memory
(`main/registration_seal.h`), so the record is readable only while that wallet is loaded. A
temporary wallet works exactly like that. What was being looked for was a key sitting on the card,
when what was needed was for nothing to sit on the card at all.

**The remaining residue:** record **names stay in the clear**, so someone reading the card still
sees the names and the number of records; encrypting the name too would have closed the way of
deleting a record left behind by a wallet that is not loaded. And the seal is exactly as strong as
the secrecy of the seed. **No-claims note (in force):** this change fixes the card privacy of
record CONTENT; it does NOT fix the hardware gap described for the duress PIN, which stands
unchanged.

**Scoping records per wallet.** Measured and found closed: the NVS key is 16 bytes
(`libjade/include/nvs.h:4`) and the record name is also 16 bytes (`main/multisig.h:10`), so a
fingerprint prefix does not fit in the name. A separate namespace would change the fixed
`PERSISTED_NAMESPACES` table, that is, the card format would go `PIJADES4` → `S5` and existing
cards would be invalidated again. Not worth this round's gain.

**Deleting a temporary wallet's records when it is forgotten.** Feasible (there is a per-slot
temporariness flag: `keychain_slot_is_temporary()`), but it has two costs and both are real:
someone using the device only with SeedQRs would have to rescan the vault file every session, and
it is not a guarantee ; if the device is powered off without `Forget`, the records stay on the
card. Not done, by decision.

**Splitting capacity per wallet.** The device-wide limit of 16 is a product limit, not a defect; on
an eight-slot device it fills up sooner, that is all.

---

## Phase 4 ; authenticity of the image chain (2026-09-04)

The second review's B3: the build and image preparation chain does not enforce the authenticity of
its inputs. The plan's four items were met by measurement; two of them did not survive as written.

### The measured state of the chain (before starting)

| # | Step | Fingerprint of the input | In the repo |
|---|---|---|---|
| 1 | `2026-06-18-raspios-bookworm-armhf-lite.img.xz` | present (`images/img.sha256`) | outside the repo |
| 2 | Extract partition 2 → `raspios-rootfs.tar` → `docker import` | **absent** | **the command was in no script**, only in prose |
| 3 | `Dockerfile.armv6-build` | a tag | **outside the repo** |
| 4 | `build-armv6.sh` → `pijade-armv6.tar.gz` + `.sha256` | was produced | in the repo |
| 5 | `prepare-image.sh` unpacked the tar | **the `.sha256` was never read** | in the repo |
| 6 | The `t7_chain.sh` test copy | `cp`, **no hash was recorded** | in the repo |

### 4.1 The build environment's recipe was brought into the repo (decision, 2026-09-04)

The plan said "pin the base image by digest" and pointed at `build-armv6.sh:7`; there is no `FROM`
line there. The measurement led somewhere else: the base container does not sit in a registry, it
is produced locally with `docker import` (`docker history`: top layer `RUN apt-get`, base layer
`Imported from -`), and that import step was written nowhere in runnable form. Since an image that
is not in a registry has no digest, the root to pin is not that line but the fingerprint of the
source Raspberry Pi OS image.

Two files were written: `pijade/images/bootstrap-builder.sh` (verifies the source image's
fingerprint fail-closed, reads the partition geometry from the MBR with `sfdisk -d`, tars the root
filesystem, imports it, builds `Dockerfile.armv6-build` with an empty build context, and writes the
fingerprint of everything it produces into `bootstrap-record.txt`) and
`pijade/images/Dockerfile.armv6-build` (the copy that lived on the Mac, brought into the repo, with
a comment on why the base image is called by tag).

**Verification, in two layers.** Environment equality: the container built from the recipe gave the
same package list as the old container (`dpkg -l` hash `b998565c9643f589`, 623 packages, gcc
12.2.0, cmake 3.25.1). Output equality (the real proof): the same source tree was built separately
in both containers and the binaries came out **bit for bit identical** ; `pijade-host`
`55c43768c3af298b...`, `libjade.so` `bdbf5131289658...`.

Two measured details are worth recording: `losetup -P` does not produce partition devices in this
environment (the Docker VM), so mounting is done with offset/sizelimit ; that is also why
`prepare-image.sh` takes the same route. And the build context is deliberately not given: because
the Dockerfile copies no file, a contextless build produced the same config digest
(`ef6c6e5a6e87...`), whereas passing `.` would have sent the entire images directory to docker.

### 4.2 The package's fingerprint is now verified (GATE 0)

`build-armv6.sh:109` produced the `.sha256` and `prepare-image.sh` never read it. The gate runs
before a single touch is made to the image, and before the other package gates. The order of
sources: if the `PKG_SHA` environment variable is given it is mandatory, otherwise the sidecar
file; if neither exists the gate closes.

What it gains is written plainly in the code: a sidecar file **does not provide authenticity**
(whoever changes the tar changes the `.sha256` beside it), it catches staleness and corruption ;
which is exactly what happened on 2026-08-25. If authenticity is wanted, `PKG_SHA` is supplied by
hand. Three scenarios were measured: the correct sidecar passed (RC=0), a wrong `PKG_SHA` stopped
it (RC=1), and the absence of any hash source stopped it (RC=1).

### 4.3 The input image's hash: the item was rejected, but a real finding came out in its place

`prepare-image.sh`'s own comment said the input hash cannot be pinned there, with its reason (the
script modifies the image in place, so the input of a second run is the output of the first) ; that
is correct and the item was rejected. But the next sentence of the comment said the acceptance
runner "records the two starting hashes", and `t7_chain.sh:36` was only doing a `cp`. The comment
was describing an assurance that did not exist. The record was actually added (the main image plus
the package, before the run touches anything; a record, not a comparison) and the
`prepare-image.sh` comment was corrected to say that the package's situation is different.

### 4.4 Alpine pinned to a digest, apk version pinning rejected

`alpine:3.20` is a branch tag and it moves on point releases: when measured on 2026-09-04 it
pointed at 3.20.10, so the same command was running with different tools at different times. All
four real calls (`t7_chain.sh`) were bound to the multi-architecture list digest; the example
command in `bootstrap-builder.sh` and `prepare-image.sh` uses the same digest.

Pinning package versions was rejected, and the reason was written into the code: a pin of the form
`util-linux=2.40.1-r1` goes stale on every point release of the 3.20 branch and `apk` breaks the
chain with "not found"; and its gain touches only the intermediate product, not the product,
because those four packages are used while preparing the image and are never written to the card.

### Remaining work #8 (the service/port surface): no extra code was needed

Measured and found closed. The service surface: GATE 2 of `prepare-image.sh` counts the three
persistent unit search paths (`/etc`, `/usr/local/lib`, `/usr/lib`) and compares them against a
nine-unit allowlist, failing on any difference. The port surface: at boot it runs `ss -H -tlnu` and
errors if the output is not empty (`prepare-image.sh:630-637`), and GATE 11 verifies that this
script exists. Unix sockets are deliberately out of scope (`-x` is not passed), with the reason
written in the script.

### An end-to-end run of the chain (after the changes)

`t7_chain.sh`: twin fidelity passed, the starting hashes were recorded, `PREPARE_EXIT=0` (20
gates), the independent check gave 0 errors, sabotage caught 44/44, boot-verify 0.
`CHAIN_DONE P=0 V=0 Z=0 B=0` (the literal the script prints).

### Package determinism (the plan's acceptance criterion)

The plan's verification table asks this of phase 4: *"The build is run twice; the same input gives
the same SHA256."* On the first measurement the binaries came out bit for bit identical but
`pijade-armv6.tar.gz` came out different, so the criterion was not met. This was first written down
as "a side finding, out of scope"; that was wrong, it was the phase's own acceptance criterion, and
it was closed.

The difference was only in file mtimes: packaging normalised ownership (`--owner=0 --group=0`) but
not the time nor the directory read order. What was added: `--sort=name` (directory read order
comes from the file system and is not stable), `--mtime=@0`, `--numeric-owner` (name resolution
depends on the container's `passwd` file).

It was measured that NO extra flag is needed for the gzip header; the claim first written ("the
gzip header carries a timestamp too") was wrong. `tar -z` invokes gzip over a pipe, and since gzip
reading from stdin has no source file name or time, it writes zero into the MTIME field: the header
of both runs is `1f 8b 08 00 00 00 00 00`. Adding `gzip -n` would have required a shell pipe and
`pipefail`, its gain was zero, and it was not added.

**The criterion is met.** Two independent container runs from the same source tree (2026-09-04
13:32 and 13:35), into separate output directories:

```
5a27eebc74ce47ff386b7f8eb1f201d1eed810ecf0ed9ab8da57cfc71fe9c819  armv6-out/pijade-armv6.tar.gz
5a27eebc74ce47ff386b7f8eb1f201d1eed810ecf0ed9ab8da57cfc71fe9c819  armv6-out-det/pijade-armv6.tar.gz
```

The second half of the criterion ("preparation with a bad hash is rejected") had already been
measured by GATE 0's three scenarios: the correct sidecar RC=0, a wrong `PKG_SHA` RC=1, the absence
of a hash source RC=1.

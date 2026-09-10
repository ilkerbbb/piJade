#ifndef AMALGAMATED_BUILD
#include "descriptor_text.h"
#include "jade_assert.h"
#include "jade_wally_verify.h"
#include "utils/malloc_ext.h"
#include "utils/util.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <wally_crypto.h>

// BBB-AIRGAP: tr( is not detected: a taproot tree puts keys after '{', which the tokenizer below
// does not treat as a key start, and no fixture measures it (spec A1, out of scope).
static const char* const DESCRIPTOR_PREFIXES[] = { "wsh(", "sh(", "wpkh(", "pkh(" };
static const char ERR_INVALID[] = "Invalid descriptor";
static const char ERR_UNSUPPORTED[] = "Unsupported descriptor";
static const char ERR_TOO_LARGE[] = "Descriptor too large";
static const char ERR_KEY_TOO_LONG[] = "Descriptor key too long";
static const char ERR_TOO_MANY[] = "Too many signers";

// BBB-AIRGAP: verify BIP380 locally before rewriting the original text into a policy template.
// Wally verifies checksums when parsing, but has no raw-string checksum API and cannot parse
// the Specter '{0,1}' paths accepted here. Normalising those paths first changes the checksum.
static uint64_t descriptor_checksum_polymod(uint64_t chk, const uint8_t value)
{
    static const uint64_t generators[]
        = { 0xf5dee51989ULL, 0xa9fdca3312ULL, 0x1bab10e32dULL, 0x3706b1677aULL, 0x644d626ffdULL };
    const uint64_t top = chk >> 35;
    chk = ((chk & 0x7ffffffffULL) << 5) ^ value;
    for (size_t i = 0; i < 5; ++i) {
        if ((top >> i) & 1) {
            chk ^= generators[i];
        }
    }
    return chk;
}

static bool descriptor_checksum_valid(const char* text, const size_t text_len, const char* checksum)
{
    static const char input_charset[] = "0123456789()[],'/*abcdefgh@:$%{}IJKLMNOPQRSTUVWXYZ&+-.;<=>?!^_|~"
                                        "ijklmnopqrstuvwxyzABCDEFGH`#\"\\ ";
    static const char checksum_charset[] = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
    uint64_t chk = 1;
    uint8_t group = 0;
    size_t group_len = 0;
    for (size_t i = 0; i < text_len; ++i) {
        const char* const pos = memchr(input_charset, (unsigned char)text[i], sizeof(input_charset) - 1);
        if (!pos) {
            return false;
        }
        const size_t value = pos - input_charset;
        chk = descriptor_checksum_polymod(chk, value & 31);
        group = group * 3 + (value >> 5);
        if (++group_len == 3) {
            chk = descriptor_checksum_polymod(chk, group);
            group = 0;
            group_len = 0;
        }
    }
    if (group_len) {
        chk = descriptor_checksum_polymod(chk, group);
    }
    for (size_t i = 0; i < 8; ++i) {
        chk = descriptor_checksum_polymod(chk, 0);
    }
    chk ^= 1;
    for (size_t i = 0; i < 8; ++i) {
        if (checksum[i] != checksum_charset[(chk >> (5 * (7 - i))) & 31]) {
            return false;
        }
    }
    return true;
}

static const char* skip_ws(const char* p, const char* const end)
{
    while (p < end && isspace((unsigned char)*p)) {
        ++p;
    }
    return p;
}

bool descriptor_text_is_descriptor(const char* text, const size_t text_len)
{
    JADE_ASSERT(text);
    const char* const end = text + text_len;
    const char* const p = skip_ws(text, end);
    if (p < end && *p == '{') {
        // BBB-AIRGAP: a false positive is harmless: parsing rejects it with "Invalid descriptor".
        // Valid Specter exports always contain '"descriptor"' ignoring case, so there are no false negatives.
        return strncasestr(p, "\"descriptor\"", end - p) != NULL;
    }
    for (size_t i = 0; i < sizeof(DESCRIPTOR_PREFIXES) / sizeof(DESCRIPTOR_PREFIXES[0]); ++i) {
        const size_t len = strlen(DESCRIPTOR_PREFIXES[i]);
        if ((size_t)(end - p) >= len && !strncmp(p, DESCRIPTOR_PREFIXES[i], len)) {
            return true;
        }
    }
    return false;
}

// BBB-AIRGAP: skip strings without decoding escapes so an unusable optional label cannot
// prevent finding the required descriptor member.
static const char* json_skip_string(const char* p, const char* const end)
{
    ++p; // opening quote
    while (p < end) {
        const char c = *p++;
        if (c == '"') {
            return p;
        }
        if (c == '\\') {
            if (p >= end) {
                return NULL;
            }
            ++p;
        }
    }
    return NULL;
}

// BBB-AIRGAP: skip unrelated member values iteratively, including nested objects/arrays;
// quoted brackets do not affect depth and no new depth limit is imposed on valid exports.
static const char* json_skip_value(const char* p, const char* const end)
{
    size_t depth = 0;
    while (p < end) {
        if (*p == '"') {
            p = json_skip_string(p, end);
            if (!p) {
                return NULL;
            }
            if (!depth) {
                return p;
            }
            continue;
        }
        if (*p == '{' || *p == '[') {
            ++depth;
        } else if (*p == '}' || *p == ']') {
            if (!depth) {
                return p;
            }
            if (!--depth) {
                return p + 1;
            }
        } else if (*p == ',' && !depth) {
            return p;
        }
        ++p;
    }
    return NULL;
}

// Copy the JSON string value following '"key"' into 'out'; only \" \\ and \/ escapes are accepted.
// Returns false if the key is absent or the value is malformed/too long.
static bool json_get_string(
    const char* json, const char* const end, const char* key, char* out, const size_t out_len, size_t* written)
{
    JADE_INIT_OUT_SIZE(written);
    const size_t key_len = strlen(key);
    const char* p = skip_ws(json, end);
    if (p >= end || *p != '{') {
        return false;
    }
    // BBB-AIRGAP: only top-level member names match; a label value may itself be "descriptor".
    while (true) {
        p = skip_ws(p + 1, end); // opening brace or member separator
        if (p >= end || *p != '"') {
            return false;
        }
        const char* const name = p + 1;
        p = json_skip_string(p, end);
        if (!p) {
            return false;
        }
        // BBB-AIRGAP: compare raw names ignoring case; escaped member names are out of scope.
        const bool matches = (size_t)(p - name - 1) == key_len && !strncasecmp(name, key, key_len);
        p = skip_ws(p, end);
        if (p >= end || *p != ':') {
            return false;
        }
        p = skip_ws(p + 1, end);
        if (matches) {
            break;
        }
        p = json_skip_value(p, end);
        if (!p) {
            return false;
        }
        p = skip_ws(p, end);
        if (p >= end || *p != ',') {
            return false;
        }
    }
    if (p >= end || *p != '"') {
        return false;
    }
    ++p;
    size_t n = 0;
    while (p < end && *p != '"') {
        char c = *p++;
        if (c == '\\') {
            if (p >= end) {
                return false;
            }
            c = *p++;
            if (c != '"' && c != '\\' && c != '/') {
                return false;
            }
        }
        if (n + 1 >= out_len) {
            return false;
        }
        out[n++] = c;
    }
    if (p >= end) {
        return false; // unterminated string
    }
    out[n] = '\0';
    *written = n;
    return true;
}

// Parse '<a;b>', '{a,b}' or '**' into a canonical '/<a;b>/*' child path.  Anything else (a single
// path, a range, a hardened pair, no child path at all) is unsupported: register_descriptor()
// needs a change branch (multi_index 1) to accept the record (main/process/register_descriptor.c).
static bool canonical_child(const char* rest, const size_t rest_len, char* child, const size_t child_len)
{
    JADE_ASSERT(child_len >= sizeof("/<4294967295;4294967295>/*"));
    if (rest_len == 3 && !strncmp(rest, "/**", 3)) {
        strcpy(child, "/<0;1>/*");
        return true;
    }
    if (rest_len < sizeof("/<0;1>/*") - 1 || rest[0] != '/' || (rest[1] != '<' && rest[1] != '{')) {
        return false;
    }
    const char open = rest[1];
    const char sep = open == '<' ? ';' : ',';
    const char close = open == '<' ? '>' : '}';
    const char* p = rest + 2;
    const char* const end = rest + rest_len;
    // Accumulate in 64 bits and bound each branch by the unhardened BIP32 range.  A uint32_t
    // accumulator wraps silently: ten digits fit the digit guard, so '4294967296' would come out
    // as 0 and the record would be written as a DIFFERENT, valid descriptor rather than refused.
    // The same limit is what bcur.c enforces on the UR path.
    uint64_t a = 0;
    uint64_t b = 0;
    size_t digits = 0;
    while (p < end && isdigit((unsigned char)*p) && digits < 10) {
        a = a * 10 + (uint64_t)(*p++ - '0');
        ++digits;
    }
    if (!digits || a > BIP32_MAX_CHILD_INDEX || p >= end || *p != sep) {
        return false;
    }
    ++p;
    digits = 0;
    while (p < end && isdigit((unsigned char)*p) && digits < 10) {
        b = b * 10 + (uint64_t)(*p++ - '0');
        ++digits;
    }
    if (!digits || b > BIP32_MAX_CHILD_INDEX || p >= end || *p != close) {
        return false;
    }
    ++p;
    if (end - p != 2 || p[0] != '/' || p[1] != '*') {
        return false;
    }
    const int n = snprintf(child, child_len, "/<%" PRIu64 ";%" PRIu64 ">/*", a, b);
    return n > 0 && (size_t)n < child_len;
}

// Canonicalise one key expression '[fp/path]xpub/<a;b>/*': value '[fp/path]xpub' with a lowercase
// fingerprint and 'h' hardening, child '/<a;b>/*'.  An origin is required: descriptor_get_signers()
// refuses keys without one (WALLY_MS_IS_PARENTED, main/descriptor.c).
static bool canonical_key(const char* tok, const size_t tok_len, char* value, const size_t value_len, char* child,
    const size_t child_len, const char** errmsg)
{
    const char* const end = tok + tok_len;
    const char* p = tok;
    size_t n = 0;
    if (*p != '[') {
        *errmsg = ERR_UNSUPPORTED;
        return false;
    }
    const char* const close = memchr(p, ']', tok_len);
    if (!close || close - p < 9 || (p[9] != '/' && p[9] != ']')) {
        *errmsg = ERR_INVALID;
        return false;
    }
    // '[' + 8 hex fingerprint
    for (size_t i = 0; i < 9; ++i) {
        const char c = p[i];
        if (i && !isxdigit((unsigned char)c)) {
            *errmsg = ERR_INVALID;
            return false;
        }
        if (n + 1 >= value_len) {
            *errmsg = ERR_KEY_TOO_LONG;
            return false;
        }
        value[n++] = (char)tolower((unsigned char)c);
    }
    p += 9;
    // path components up to ']'
    while (p <= close) {
        char c = *p++;
        if (c == '\'') {
            c = 'h';
        } else if (!(isdigit((unsigned char)c) || c == '/' || c == 'h' || c == ']')) {
            *errmsg = ERR_INVALID;
            return false;
        }
        if (n + 1 >= value_len) {
            *errmsg = ERR_KEY_TOO_LONG;
            return false;
        }
        value[n++] = c;
    }
    // key body up to '/' or end
    const char* const key_start = p;
    while (p < end && *p != '/') {
        ++p;
    }
    if (p == key_start) {
        *errmsg = ERR_INVALID;
        return false;
    }
    if (n + (size_t)(p - key_start) + 1 > value_len) {
        *errmsg = ERR_KEY_TOO_LONG;
        return false;
    }
    memcpy(value + n, key_start, p - key_start);
    n += p - key_start;
    value[n] = '\0';

    if (!canonical_child(p, end - p, child, child_len)) {
        *errmsg = ERR_UNSUPPORTED;
        return false;
    }
    return true;
}

// Advance to the end of a key token: the first ',' or ')' outside [ ], < > and { } groups
static const char* token_end(const char* p, const char* const end)
{
    int group = 0;
    for (; p < end; ++p) {
        if (*p == '[' || *p == '<' || *p == '{') {
            ++group;
        } else if (*p == ']' || *p == '>' || *p == '}') {
            --group;
        } else if (!group && (*p == ',' || *p == ')')) {
            break;
        }
    }
    return p;
}

static bool is_key_start(const char* p, const char* const end)
{
    return p < end && (*p == '[' || (end - p >= 4 && (!strncmp(p, "xpub", 4) || !strncmp(p, "tpub", 4))));
}

static bool make_hash_name(const descriptor_data_t* descriptor, char* name, const size_t name_len)
{
    // SHA256 over script and values, in order; the name is 'desc-' + first 4 bytes as hex
    const size_t total = descriptor->script_len + string_values_len(descriptor->values, descriptor->num_values);
    uint8_t* const buf = JADE_MALLOC(total);
    size_t n = 0;
    memcpy(buf + n, descriptor->script, descriptor->script_len);
    n += descriptor->script_len;
    for (size_t i = 0; i < descriptor->num_values; ++i) {
        memcpy(buf + n, descriptor->values[i].key, descriptor->values[i].key_len);
        n += descriptor->values[i].key_len;
        memcpy(buf + n, descriptor->values[i].value, descriptor->values[i].value_len);
        n += descriptor->values[i].value_len;
    }
    JADE_ASSERT(n == total);
    uint8_t hash[SHA256_LEN];
    JADE_WALLY_VERIFY(wally_sha256(buf, total, hash, sizeof(hash)));
    free(buf);
    const int written = snprintf(name, name_len, "desc-%02x%02x%02x%02x", hash[0], hash[1], hash[2], hash[3]);
    return written > 0 && (size_t)written < name_len;
}

bool descriptor_text_parse(const char* text, const size_t text_len, descriptor_data_t* descriptor, char* name,
    const size_t name_len, const char** errmsg)
{
    JADE_ASSERT(text);
    JADE_ASSERT(text_len);
    JADE_ASSERT(descriptor);
    JADE_ASSERT(name);
    JADE_ASSERT(name_len >= MAX_DESCRIPTOR_NAME_SIZE);
    JADE_INIT_OUT_PPTR(errmsg);

    descriptor->script_len = 0;
    descriptor->num_values = 0;
    descriptor->type = DESCRIPTOR_TYPE_UNKNOWN;
    name[0] = '\0';

    // Working copy: at most the whole payload.  Specter JSON is unwrapped into it first.
    const size_t work_len = text_len + 1;
    char* const work = JADE_MALLOC(work_len);
    char label[MAX_DESCRIPTOR_NAME_SIZE * 4] = { 0 }; // longer labels are trimmed below
    bool ret = false;
    size_t len = 0;

    const char* const end = text + text_len;
    const char* p = skip_ws(text, end);
    if (p < end && *p == '{') {
        size_t label_len = 0;
        if (!json_get_string(p, end, "descriptor", work, work_len, &len)) {
            *errmsg = ERR_INVALID;
            goto cleanup;
        }
        if (!json_get_string(p, end, "label", label, sizeof(label), &label_len) || !label_len) {
            // Absent, malformed, or longer than the buffer: the hash name below is used instead
            JADE_LOGI("Specter JSON label not usable as a record name, using the hash name");
            label[0] = '\0';
        }
    } else {
        memcpy(work, p, end - p);
        len = end - p;
        work[len] = '\0';
    }

    // BBB-AIRGAP: trim only trailing whitespace before checking the original text. JSON escapes
    // are already decoded; any leading whitespace in the JSON value is part of the checksum.
    while (len && isspace((unsigned char)work[len - 1])) {
        --len;
    }
    work[len] = '\0';
    const char* const hash = memchr(work, '#', len);
    if (hash) {
        if (len - (hash - work) != 9 || !descriptor_checksum_valid(work, hash - work, hash + 1)) {
            *errmsg = ERR_INVALID;
            goto cleanup;
        }
        len = hash - work;
        work[len] = '\0';
    }
    // The checksum is optional. Strip internal whitespace only after validating it, if present.
    size_t w = 0;
    for (size_t i = 0; i < len; ++i) {
        if (!isspace((unsigned char)work[i])) {
            work[w++] = work[i];
        }
    }
    work[w] = '\0';
    len = w;
    if (!len) {
        *errmsg = ERR_INVALID;
        goto cleanup;
    }

    // Walk the text, replacing each key expression with '@<index>' + canonical child path
    const char* const wend = work + len;
    p = work;
    char prev = '(';
    while (p < wend) {
        if ((prev == '(' || prev == ',') && is_key_start(p, wend)) {
            const char* const tend = token_end(p, wend);
            char value[sizeof(descriptor->values[0].value)];
            char child[32];
            if (!canonical_key(p, tend - p, value, sizeof(value), child, sizeof(child), errmsg)) {
                goto cleanup;
            }
            // Reuse the index of an identical value (Liana style '@0/<0;1>/*' with '@0/<2;3>/*')
            size_t index = descriptor->num_values;
            for (size_t i = 0; i < descriptor->num_values; ++i) {
                if (!strcmp(descriptor->values[i].value, value)) {
                    index = i;
                    break;
                }
            }
            if (index == descriptor->num_values) {
                if (index >= MAX_ALLOWED_SIGNERS) {
                    *errmsg = ERR_TOO_MANY;
                    goto cleanup;
                }
                string_value_t* const sv = &descriptor->values[index];
                const int klen = snprintf(sv->key, sizeof(sv->key), "@%u", (unsigned)index);
                JADE_ASSERT(klen > 0 && (size_t)klen < sizeof(sv->key));
                sv->key_len = (uint16_t)klen;
                strcpy(sv->value, value);
                sv->value_len = (uint16_t)strlen(value);
                ++descriptor->num_values;
            }
            const int n = snprintf(descriptor->script + descriptor->script_len,
                sizeof(descriptor->script) - descriptor->script_len, "@%u%s", (unsigned)index, child);
            if (n <= 0 || (size_t)n >= sizeof(descriptor->script) - descriptor->script_len) {
                *errmsg = ERR_TOO_LARGE;
                goto cleanup;
            }
            descriptor->script_len += (uint16_t)n;
            p = tend;
            continue;
        }
        if (descriptor->script_len + 1 >= sizeof(descriptor->script)) {
            *errmsg = ERR_TOO_LARGE;
            goto cleanup;
        }
        prev = *p;
        descriptor->script[descriptor->script_len++] = *p++;
    }
    descriptor->script[descriptor->script_len] = '\0';
    if (!descriptor->num_values) {
        *errmsg = ERR_UNSUPPORTED; // no origin-carrying key at all
        goto cleanup;
    }

    // Name: sanitised label (as register_multisig_file() does for its 'Name' line), else hash
    size_t nlen = 0;
    for (const char* c = label; *c && nlen + 1 < MAX_DESCRIPTOR_NAME_SIZE; ++c) {
        if (*c == ' ') {
            name[nlen++] = '_';
        } else if (isgraph((unsigned char)*c)) {
            name[nlen++] = *c;
        }
    }
    name[nlen] = '\0';
    if (!nlen && !make_hash_name(descriptor, name, name_len)) {
        *errmsg = ERR_INVALID;
        goto cleanup;
    }
    ret = true;

cleanup:
    free(work);
    return ret;
}
#endif // AMALGAMATED_BUILD

#ifndef UTILS_URLDECODE_H_
#define UTILS_URLDECODE_H_

#include <stdbool.h>
#include <stddef.h>

#include "../jade_assert.h"

// Simple urldecode function - any triple that looks like "%XX" is replaced by the character
// specified, a '+' is replaced by a space, and any printable characters are copied verbatim.
// Both nul-terminated and length-specified input strings are supported but in latter case they must
// be nul-padded.
// The function returns true in case of success and false if there is an error in buffer sizes or
// invalid encoding, including: embedded nul, or hex encoded characters outside 0x20-0x7f range.
// In case of success the output string is always nul-terminated (although the input need not be).
WARN_UNUSED_RESULT bool urldecode(const char* src, size_t src_len, char* dest, size_t dest_len);

// Simple urlencode function - special chars are replaced by %XX, space is replaced by '+',
// and anything else is copied verbatim.
// The output string is always nul-terminated (although the input need not be).
WARN_UNUSED_RESULT bool urlencode(const char* src, size_t src_len, char* dest, size_t dest_len);

// Simple validation function for URL encoding, designed to operate in the same way as urldecode()
// with an optional capability to filter decoded characters with a user-provided callback
// function (can be NULL). The argument 'max_len' is used to limit the number of characters checked.
// Both nul-terminated and length-specified input strings are supported but in latter case they must
// be nul-padded.
WARN_UNUSED_RESULT bool is_valid_urlencoding(const char* src, size_t src_len, size_t max_len, int (*check_fn)(int));

#endif /* UTILS_URLDECODE_H_ */

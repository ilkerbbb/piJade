#ifndef DESCRIPTOR_TEXT_H_
#define DESCRIPTOR_TEXT_H_

#include <stdbool.h>
#include <stddef.h>

#include "descriptor.h"
#include "jade_assert.h"

// BBB-AIRGAP: descriptor text as scanned from a QR (Sparrow/Specter text, or a Specter JSON
// wallet export) reduced to the policy-template form register_descriptor() persists:
// script with '@0'..'@n' key placeholders and their '[fingerprint/path]xpub' values.

// True if the payload starts like a descriptor or is a Specter JSON export
bool descriptor_text_is_descriptor(const char* text, size_t text_len);

// Parse the text into canonical form and derive the registration name (label or 'desc-' + hash).
// 'name_len' must be at least MAX_DESCRIPTOR_NAME_SIZE.  On failure 'errmsg' names the reason.
WARN_UNUSED_RESULT bool descriptor_text_parse(const char* text, size_t text_len, descriptor_data_t* descriptor,
    char* name, size_t name_len, const char** errmsg);

#endif /* DESCRIPTOR_TEXT_H_ */

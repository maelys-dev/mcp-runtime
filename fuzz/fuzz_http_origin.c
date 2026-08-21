/*
 * http-origin: Origin, Host and request-target values against the validators.
 *
 * These three are the checks that run before anything else on a request, and
 * they are the ones a rebinding attack, a virtual-host confusion and a routing
 * bypass respectively have to get past. What is asserted is not "the validator
 * agrees with itself" but the properties the design states, each re-derived
 * here independently of the implementation:
 *
 *  - a loopback bind is strictly narrower than a public one, so anything a
 *    loopback bind accepts as a Host a public bind accepts too;
 *  - an Origin is accepted only when it byte-equals a configured entry, and an
 *    absent Origin only on a loopback bind;
 *  - a normalized target is NUL-terminated inside its buffer, carries no
 *    control byte and contains no `..` segment - so a target that survives
 *    normalization cannot be a traversal.
 *
 * The Mcp-Name half of this target's remit in the design belongs with the
 * Base64 sentinel, which lands with the header/body comparison in H2 and is
 * covered there by http-headers.
 */
#include "host/http_parser.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SLICE_CAPACITY 512u
#define PATH_CAPACITY 256u

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 2u) return 0;
    /* Two slices out of one input: a value and one allowlist entry. */
    size_t split = 1u + (size_t)(data[0] % (size - 1u ? size - 1u : 1u));
    if (split >= size) split = size - 1u;
    const char *value = (const char *)data + 1u;
    size_t value_length = split;
    if (value_length > SLICE_CAPACITY) value_length = SLICE_CAPACITY;

    char allowed_entry[SLICE_CAPACITY + 1u];
    size_t allowed_length = size - 1u - value_length;
    if (allowed_length > SLICE_CAPACITY) allowed_length = SLICE_CAPACITY;
    memcpy(allowed_entry, data + 1u + value_length, allowed_length);
    allowed_entry[allowed_length] = '\0';
    /* An allowlist entry is a configured C string, so a NUL inside the slice
     * truncates it exactly as a configured one would. */
    allowed_length = strlen(allowed_entry);
    const char *const allowed[] = {allowed_entry};

    /* Host. A loopback bind is a strict narrowing of a public one. */
    int loopback_ok = maelys_http_host_valid(value, value_length, 1);
    int public_ok = maelys_http_host_valid(value, value_length, 0);
    assert(!loopback_ok || public_ok);
    /* An empty Host is never valid on either. */
    if (!value_length) assert(!loopback_ok && !public_ok);

    /* Origin. */
    int with_list = maelys_http_origin_allowed(value, value_length, allowed, 1u, 0);
    int without_list = maelys_http_origin_allowed(value, value_length, NULL, 0u, 0);
    /* The allowlist is empty by default, and an empty allowlist accepts no
     * present Origin at all. */
    assert(!without_list);
    /* A present Origin is accepted only when it byte-equals the entry - checked
     * against memcmp here rather than against the validator's own comparison. */
    if (with_list) {
        assert(value_length == allowed_length);
        assert(memcmp(value, allowed_entry, value_length) == 0);
    }
    /* Absent depends only on the bind. */
    assert(maelys_http_origin_allowed(NULL, 0, allowed, 1u, 1) == 1);
    assert(maelys_http_origin_allowed(NULL, 0, allowed, 1u, 0) == 0);

    /* The request target, resolved against the same slice used as a Host. */
    char path[PATH_CAPACITY];
    memset(path, 0x7f, sizeof(path));
    if (maelys_http_target_normalize(value, value_length,
        allowed_entry, allowed_length, path, sizeof(path))) {
        size_t length = strnlen(path, sizeof(path));
        /* NUL-terminated strictly inside the buffer. */
        assert(length < sizeof(path));
        /* A path, always. */
        assert(length > 0 && path[0] == '/');
        size_t segment_start = 0;
        for (size_t index = 0; index <= length; ++index) {
            if (index < length) {
                unsigned char byte = (unsigned char)path[index];
                /* No control byte survives normalization, whether it was
                 * literal or percent-encoded. */
                assert(byte >= 0x20u && byte != 0x7fu);
            }
            if (index == length || path[index] == '/') {
                size_t segment_length = index - segment_start;
                assert(!(segment_length == 2u && path[segment_start] == '.' &&
                    path[segment_start + 1u] == '.'));
                segment_start = index + 1u;
            }
        }
    }
    return 0;
}

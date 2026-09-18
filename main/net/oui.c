/*
 * IEEE OUI lookup.
 *
 * The table is generated (see tools/gen_oui.py) and sorted ascending, so the
 * lookup is a plain binary search over flash. No allocation, no locking, safe
 * from any task.
 */
#include "oui.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t    prefix;     /* first three MAC bytes, big-endian order */
    const char *vendor;
} oui_entry_t;

static const oui_entry_t s_oui_table[] = {
#include "oui_table.inc"
};

#define OUI_TABLE_LEN (sizeof(s_oui_table) / sizeof(s_oui_table[0]))

const char *oui_lookup(const uint8_t mac[6])
{
    if (mac == NULL) {
        return NULL;
    }

    /*
     * Bit 1 of the first octet marks a locally administered address: modern
     * phones randomise those per network, so the prefix means nothing.
     * Bit 0 marks multicast, which is never a host address.
     */
    if (mac[0] & 0x03) {
        return NULL;
    }

    const uint32_t key = ((uint32_t)mac[0] << 16) | ((uint32_t)mac[1] << 8) | (uint32_t)mac[2];

    size_t lo = 0;
    size_t hi = OUI_TABLE_LEN;      /* exclusive */

    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        const uint32_t probe = s_oui_table[mid].prefix;

        if (probe == key) {
            return s_oui_table[mid].vendor;
        } else if (probe < key) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    return NULL;
}

size_t oui_table_size(void)
{
    return OUI_TABLE_LEN;
}

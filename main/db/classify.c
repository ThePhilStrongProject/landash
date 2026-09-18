/*
 * Device classification: vendor + advertised services + hostname keywords to
 * a netdash_type_t. Pure function, no locking, no I/O - see classify.h.
 *
 * The name tables are real: the REST API and the web UI depend on these exact
 * strings, so they live here from day one.
 */
#include "classify.h"

#include <ctype.h>
#include <stdbool.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "classify";

static const char *const s_type_names[NETDASH_TYPE_MAX] = {
    [NETDASH_TYPE_UNKNOWN]   = "unknown",
    [NETDASH_TYPE_ROUTER]    = "router",
    [NETDASH_TYPE_MESH_NODE] = "mesh_node",
    [NETDASH_TYPE_SWITCH]    = "switch",
    [NETDASH_TYPE_NAS]       = "nas",
    [NETDASH_TYPE_TV]        = "tv",
    [NETDASH_TYPE_HUB]       = "hub",
    [NETDASH_TYPE_PHONE]     = "phone",
    [NETDASH_TYPE_PC]        = "pc",
    [NETDASH_TYPE_IOT]       = "iot",
    [NETDASH_TYPE_PRINTER]   = "printer",
    [NETDASH_TYPE_CAST]      = "cast",
};

/* Indexed by bit position, must match the NETDASH_SVC_ defines. */
static const char *const s_service_names[NETDASH_SVC_COUNT] = {
    "http", "https", "ssh", "smb", "hap", "ha",
    "cast", "airplay", "printer", "ssdp", "workstation",
};

/* Case-insensitive substring search; no allocation. */
static bool icontains(const char *haystack, const char *needle)
{
    if (haystack == NULL || needle == NULL || needle[0] == '\0') {
        return false;
    }
    const size_t hlen = strlen(haystack);
    const size_t nlen = strlen(needle);
    if (nlen > hlen) {
        return false;
    }
    for (size_t i = 0; i + nlen <= hlen; i++) {
        size_t j = 0;
        for (; j < nlen; j++) {
            if (tolower((unsigned char)haystack[i + j]) != tolower((unsigned char)needle[j])) {
                break;
            }
        }
        if (j == nlen) {
            return true;
        }
    }
    return false;
}

static inline bool vendor_is(const netdash_device_t *dev, const char *needle)
{
    return icontains(dev->vendor, needle);
}

static inline bool hostname_has(const netdash_device_t *dev, const char *needle)
{
    return icontains(dev->hostname, needle);
}

static inline bool has_svc(const netdash_device_t *dev, uint16_t bit)
{
    return (dev->services & bit) != 0;
}

netdash_type_t classify_device(const netdash_device_t *dev, uint32_t gateway_ip)
{
    if (dev == NULL) {
        return NETDASH_TYPE_UNKNOWN;
    }

    ESP_LOGD(TAG, "classify %02x:%02x:%02x:%02x:%02x:%02x vendor='%s' host='%s' svc=0x%03x",
             dev->mac[0], dev->mac[1], dev->mac[2], dev->mac[3], dev->mac[4], dev->mac[5],
             dev->vendor, dev->hostname, (unsigned)dev->services);

    /* 1. Whatever answers at the gateway address is the router. */
    if (gateway_ip != 0 && dev->ip == gateway_ip) {
        return NETDASH_TYPE_ROUTER;
    }

    /*
     * 2. ASUS gear: a MiniUPnPd SSDP signature right at the gateway is also a
     *    router (belt-and-braces alongside rule 1, in case the gateway IP is
     *    not known yet); any other ASUS box on the LAN is an AiMesh node.
     */
    if (vendor_is(dev, "asustek")) {
        if (has_svc(dev, NETDASH_SVC_SSDP) && gateway_ip != 0 && dev->ip == gateway_ip) {
            return NETDASH_TYPE_ROUTER;
        }
        return NETDASH_TYPE_MESH_NODE;
    }

    /* 3. A Netgear box advertising nothing at all is almost always a dumb switch. */
    if (vendor_is(dev, "netgear") && dev->services == 0) {
        return NETDASH_TYPE_SWITCH;
    }

    /* 4. NAS: SMB plus a telltale hostname, or a dedicated NAS-vendor OUI. */
    if ((has_svc(dev, NETDASH_SVC_SMB) &&
         (hostname_has(dev, "truenas") || hostname_has(dev, "omv") ||
          hostname_has(dev, "openmediavault") || hostname_has(dev, "nas"))) ||
        vendor_is(dev, "synology") || vendor_is(dev, "qnap")) {
        return NETDASH_TYPE_NAS;
    }

    /*
     * 5. TV: LG (webOS) by vendor or hostname, or a cast/mirroring receiver
     * that also looks like a television.
     *
     * Casting alone is deliberately not enough. A Chromecast dongle and a Nest
     * speaker both advertise Google Cast without being televisions, and they
     * are caught by the dedicated cast rule further down.
     */
    if (vendor_is(dev, "lg electronics") || hostname_has(dev, "webos") ||
        hostname_has(dev, "lgtv") || hostname_has(dev, "bravia") ||
        ((has_svc(dev, NETDASH_SVC_CAST) || has_svc(dev, NETDASH_SVC_AIRPLAY)) &&
         (hostname_has(dev, "tv") || vendor_is(dev, "sony") ||
          vendor_is(dev, "samsung") || vendor_is(dev, "hisense") ||
          vendor_is(dev, "tcl") || vendor_is(dev, "vizio") ||
          vendor_is(dev, "panasonic") || vendor_is(dev, "roku")))) {
        return NETDASH_TYPE_TV;
    }

    /* 6. Smart-home hub: HomeKit/Home Assistant service or hostname. */
    if (has_svc(dev, NETDASH_SVC_HA) || has_svc(dev, NETDASH_SVC_HAP) ||
        hostname_has(dev, "homeassistant") || hostname_has(dev, "hass")) {
        return NETDASH_TYPE_HUB;
    }

    /* 7. PC: SMB workstation service, or a desktop/laptop/pc-ish hostname. */
    if (has_svc(dev, NETDASH_SVC_WORKSTATION) ||
        hostname_has(dev, "desktop") || hostname_has(dev, "laptop") ||
        hostname_has(dev, "pc")) {
        return NETDASH_TYPE_PC;
    }

    /* 8. IoT silicon/brand vendors. */
    if (vendor_is(dev, "espressif") || vendor_is(dev, "tuya") ||
        vendor_is(dev, "shelly") || vendor_is(dev, "sonoff")) {
        return NETDASH_TYPE_IOT;
    }

    /* 9. Printer: IPP/LPD service, or a known printer-vendor OUI. */
    if (has_svc(dev, NETDASH_SVC_PRINTER) ||
        vendor_is(dev, "hp") || vendor_is(dev, "brother") ||
        vendor_is(dev, "canon") || vendor_is(dev, "epson")) {
        return NETDASH_TYPE_PRINTER;
    }

    /* 10. A cast receiver that did not already match the TV rule above. */
    if (has_svc(dev, NETDASH_SVC_CAST)) {
        return NETDASH_TYPE_CAST;
    }

    /* 11. Phone: a consumer-mobile OUI with nothing else advertised. */
    if (dev->services == 0 &&
        (vendor_is(dev, "apple") || vendor_is(dev, "samsung") ||
         vendor_is(dev, "google") || vendor_is(dev, "xiaomi") ||
         vendor_is(dev, "oneplus"))) {
        return NETDASH_TYPE_PHONE;
    }

    return NETDASH_TYPE_UNKNOWN;
}

const char *netdash_type_name(netdash_type_t type)
{
    if ((int)type < 0 || type >= NETDASH_TYPE_MAX || s_type_names[type] == NULL) {
        return "unknown";
    }
    return s_type_names[type];
}

netdash_type_t netdash_type_from_name(const char *name)
{
    if (name == NULL) {
        return NETDASH_TYPE_UNKNOWN;
    }
    for (int i = 0; i < NETDASH_TYPE_MAX; i++) {
        if (s_type_names[i] != NULL && strcmp(s_type_names[i], name) == 0) {
            return (netdash_type_t)i;
        }
    }
    return NETDASH_TYPE_UNKNOWN;
}

const char *netdash_service_name(int bit_index)
{
    if (bit_index < 0 || bit_index >= NETDASH_SVC_COUNT) {
        return NULL;
    }
    return s_service_names[bit_index];
}

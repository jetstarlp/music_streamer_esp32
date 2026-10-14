/* Scan result normalisation for the setup portal's network list. */
#include "net_pure.h"

#include <stdio.h>
#include <string.h>

size_t net_scan_normalize(net_ap_info_t *aps, size_t n)
{
    if (aps == NULL) {
        return 0;
    }
    /* 1. Remove hidden networks and merge duplicates, keeping the first
     *    occurrence's position but the strongest signal. O(n^2) is fine for
     *    the few dozen entries a scan returns. */
    size_t m = 0;
    for (size_t i = 0; i < n; i++) {
        net_ap_info_t cur = aps[i];
        cur.ssid[NET_SSID_MAX] = '\0';
        if (cur.ssid[0] == '\0') {
            continue;
        }
        size_t j = 0;
        while (j < m && strcmp(aps[j].ssid, cur.ssid) != 0) {
            j++;
        }
        if (j < m) {
            if (cur.rssi > aps[j].rssi) {
                aps[j].rssi = cur.rssi;
                aps[j].channel = cur.channel;
                aps[j].secure = cur.secure;
            }
        } else {
            aps[m++] = cur;
        }
    }
    /* 2. Stable insertion sort, strongest first. */
    for (size_t i = 1; i < m; i++) {
        net_ap_info_t key = aps[i];
        size_t j = i;
        while (j > 0 && aps[j - 1].rssi < key.rssi) {
            aps[j] = aps[j - 1];
            j--;
        }
        aps[j] = key;
    }
    return m;
}

int net_rssi_bars(int rssi)
{
    if (rssi >= -55) {
        return 4;
    }
    if (rssi >= -67) {
        return 3;
    }
    if (rssi >= -78) {
        return 2;
    }
    return 1;
}

int net_scan_find(const net_ap_info_t *aps, size_t n, const char *ssid)
{
    if (aps == NULL || ssid == NULL || ssid[0] == '\0') {
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        if (strncmp(aps[i].ssid, ssid, NET_SSID_MAX + 1) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static bool usable_channel(uint8_t ch)
{
    return ch >= 1 && ch <= 13;
}

uint8_t net_pick_ap_channel(const net_ap_info_t *aps, size_t n, const char *const *known, size_t n_known)
{
    const net_ap_info_t *best_known = NULL;
    const net_ap_info_t *best_any = NULL;
    for (size_t i = 0; aps != NULL && i < n; i++) {
        const net_ap_info_t *ap = &aps[i];
        if (!usable_channel(ap->channel)) {
            continue;
        }
        if (best_any == NULL || ap->rssi > best_any->rssi) {
            best_any = ap;
        }
        for (size_t k = 0; known != NULL && k < n_known; k++) {
            if (known[k] != NULL && strncmp(known[k], ap->ssid, NET_SSID_MAX + 1) == 0 &&
                (best_known == NULL || ap->rssi > best_known->rssi)) {
                best_known = ap;
            }
        }
    }
    if (best_known != NULL) {
        return best_known->channel;
    }
    return best_any != NULL ? best_any->channel : 1;
}

size_t net_portal_ssid(const char *prefix, const uint8_t mac[6], bool mac_suffix, char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    if (prefix == NULL || prefix[0] == '\0' || (mac_suffix && mac == NULL)) {
        return 0;
    }
    char suffix[8] = "";
    if (mac_suffix) {
        snprintf(suffix, sizeof(suffix), "-%02X%02X", mac[4], mac[5]);
    }
    size_t slen = strlen(suffix);
    size_t plen = 0;
    while (plen < NET_SSID_MAX && prefix[plen] != '\0') {
        plen++;
    }
    if (plen + slen > NET_SSID_MAX) {
        plen = NET_SSID_MAX - slen;
    }
    /* do not cut a UTF-8 sequence in half */
    while (plen > 0 && ((unsigned char)prefix[plen] & 0xC0) == 0x80 && prefix[plen] != '\0') {
        plen--;
    }
    if (plen == 0 || plen + slen + 1 > cap) {
        return 0;
    }
    memcpy(out, prefix, plen);
    memcpy(out + plen, suffix, slen + 1);
    return plen + slen;
}

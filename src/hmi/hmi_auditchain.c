#include "hmi_auditchain.h"
#include "hmi_mac.h"
#include "hmi_platform.h"
#include "hmi_util.h" /* hmi_wipe */
#include <stdio.h>
#include <string.h>

static const char ZERO_SEAL[HMI_CHAIN_HEX + 1] = "00000000000000000000000000000000";

void hmi_chain_derive_key(const uint8_t *cfg_key, size_t cfg_key_len, uint8_t out[HMI_CHAIN_KEY_LEN])
{
    static const char LABEL[] = "prometheus99 audit chain v1";
    uint8_t mac[HMI_MAC_LEN];
    hmi_mac(cfg_key, cfg_key_len, (const uint8_t *)LABEL, sizeof(LABEL) - 1, mac);
    memcpy(out, mac, HMI_CHAIN_KEY_LEN);
    hmi_wipe(mac, sizeof(mac));
}

static bool is_hex_lower(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }

bool hmi_chain_has_seal(const char *line, size_t len, size_t *body_len)
{
    if (len < HMI_CHAIN_SEAL_LEN) return false;
    const char *s = line + len - HMI_CHAIN_SEAL_LEN;
    if (memcmp(s, " mac=", 5) != 0) return false;
    for (size_t i = 5; i < HMI_CHAIN_SEAL_LEN; i++)
        if (!is_hex_lower(s[i])) return false;
    if (body_len) *body_len = len - HMI_CHAIN_SEAL_LEN;
    return true;
}

bool hmi_chain_last_seal(const char *text, size_t len, char *prev)
{
    /* walk back over complete lines (each ends in '\n'); an unfinished last line is skipped */
    size_t end = len;
    while (end > 0 && text[end - 1] != '\n') end--;
    while (end > 0) {
        size_t stop = end - 1; /* the newline of this line */
        size_t start = stop;
        while (start > 0 && text[start - 1] != '\n') start--;
        if (hmi_chain_has_seal(text + start, stop - start, NULL)) {
            memcpy(prev, text + stop - HMI_CHAIN_HEX, HMI_CHAIN_HEX);
            prev[HMI_CHAIN_HEX] = '\0';
            return true;
        }
        end = start;
    }
    return false;
}

void hmi_chain_compute(const uint8_t key[HMI_CHAIN_KEY_LEN], const char *prev, const char *line, size_t body_len,
                       char *out)
{
    static const char HEX[] = "0123456789abcdef";
    hmi_mac_ctx_t c;
    uint8_t mac[HMI_MAC_LEN];
    hmi_mac_init(&c, key, HMI_CHAIN_KEY_LEN);
    hmi_mac_update(&c, (const uint8_t *)prev, HMI_CHAIN_HEX);
    hmi_mac_update(&c, (const uint8_t *)line, body_len);
    hmi_mac_final(&c, mac);
    for (size_t i = 0; i < HMI_CHAIN_HEX / 2; i++) {
        out[2 * i] = HEX[mac[i] >> 4];
        out[2 * i + 1] = HEX[mac[i] & 15];
    }
    out[HMI_CHAIN_HEX] = '\0';
}

static bool last_seal_of(const char *path, char *prev, bool *empty)
{
    char tail[HMI_CHAIN_TAIL];
    size_t len = 0;
    bool cut = false;
    *empty = true;
    if (hmi_fs_read_tail(path, tail, sizeof(tail), &len, &cut) != HMI_FS_READ_OK || len == 0) return false;
    *empty = false;
    return hmi_chain_last_seal(tail, len, prev);
}

void hmi_chain_prev_for(const char *path, char *prev)
{
    bool empty;
    memcpy(prev, ZERO_SEAL, sizeof(ZERO_SEAL));
    if (last_seal_of(path, prev, &empty) || !empty) return; /* found, or a file whose tail has no seal: zeros */
    char older[300];
    if (snprintf(older, sizeof(older), "%s.1", path) >= (int)sizeof(older)) return;
    (void)last_seal_of(older, prev, &empty);
}

size_t hmi_chain_seal(const uint8_t key[HMI_CHAIN_KEY_LEN], const char *prev, const char *rec, size_t len, char *out,
                      size_t cap)
{
    if (len == 0 || rec[len - 1] != '\n' || len + HMI_CHAIN_SEAL_LEN > cap) return 0;
    char mac[HMI_CHAIN_HEX + 1];
    hmi_chain_compute(key, prev, rec, len - 1, mac);
    if (out != rec) memmove(out, rec, len - 1);
    memcpy(out + len - 1, " mac=", 5);
    memcpy(out + len + 4, mac, HMI_CHAIN_HEX);
    out[len + 4 + HMI_CHAIN_HEX] = '\n';
    return len + HMI_CHAIN_SEAL_LEN;
}

/* First occurrence of word in line[0..len), or NULL. */
static const char *find_in(const char *line, size_t len, const char *word)
{
    size_t w = strlen(word);
    for (size_t i = 0; i + w <= len; i++)
        if (memcmp(line + i, word, w) == 0) return line + i;
    return NULL;
}

/* The value after "prev=" in a LOG_ROTATED line, if it is 32 hex digits. */
static bool rotated_prev(const char *line, size_t len, char *prev)
{
    const char *p = find_in(line, len, " prev=");
    if (!p || !find_in(line, len, " LOG_ROTATED ") || (size_t)(p + 6 + HMI_CHAIN_HEX - line) > len) return false;
    p += 6;
    for (size_t i = 0; i < HMI_CHAIN_HEX; i++)
        if (!is_hex_lower(p[i])) return false;
    memcpy(prev, p, HMI_CHAIN_HEX);
    prev[HMI_CHAIN_HEX] = '\0';
    return true;
}

void hmi_chain_verify_text(const uint8_t key[HMI_CHAIN_KEY_LEN], const char *text, size_t len, char *prev,
                           hmi_chain_report_t *r)
{
    bool first = true;
    uint32_t n = 0;
    size_t pos = 0;
    if (prev[0] == '\0') memcpy(prev, ZERO_SEAL, sizeof(ZERO_SEAL));
    bool sequence_start = memcmp(prev, ZERO_SEAL, HMI_CHAIN_HEX) == 0;
    while (pos < len) {
        size_t s = pos;
        while (pos < len && text[pos] != '\n') pos++;
        if (pos >= len) break; /* an unfinished last line is not a line yet */
        size_t l = pos - s;
        pos++;
        n++;
        r->lines++;
        /* only the first line of the first file may say where the chain started */
        if (first && sequence_start) (void)rotated_prev(text + s, l, prev);
        first = false;
        size_t body;
        if (!hmi_chain_has_seal(text + s, l, &body)) {
            r->unsealed++;
            if (!r->first_bad_line) r->first_bad_line = n;
            continue; /* the chain continues from the last line that has a seal */
        }
        char want[HMI_CHAIN_HEX + 1];
        hmi_chain_compute(key, prev, text + s, body, want);
        if (memcmp(want, text + s + body + 5, HMI_CHAIN_HEX) == 0) r->verified++;
        else {
            r->failed++;
            if (!r->first_bad_line) r->first_bad_line = n;
        }
        memcpy(prev, text + s + body + 5, HMI_CHAIN_HEX); /* the next line was sealed after this one as written */
        prev[HMI_CHAIN_HEX] = '\0';
    }
}

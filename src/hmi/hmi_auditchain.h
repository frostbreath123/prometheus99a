/**
 * @file hmi_auditchain.h
 * @brief Keyed hash chain over the lines of the audit file (tamper evidence).
 *
 * Every line of a chained audit file ends with a seal:
 *
 *   <the record as before> mac=<32 hex digits>
 *
 * The 32 digits are the first 128 bits of HMAC-SHA-512(audit key, <previous mac as 32 hex digits> || <the line up to,
 * not including, " mac=">). The previous mac is the seal of the last earlier line that carries a well-formed seal,
 * in the same file or, for the first line of a file, in the file before it (name.1 for the live file); a brand-new log
 * starts from 32 zeros. The first line of a rotated file, "LOG_ROTATED ... prev=<32 hex>", names the value it continues
 * from, so one file can be checked on its own.
 *
 * The audit key is derived from the configuration key (hmi_chain_derive_key), so the device needs no second key file;
 * whoever holds the configuration key can check the log (p99_sign verify-log) and could also forge it.
 *
 * What the chain shows: an edited, inserted, deleted or reordered line, and a removed seal, make that line or the next
 * one fail. A line torn by a power cut fails on its own and the chain continues after it. What it cannot show: lines
 * cut off the end of the newest file, or a whole file removed (the next file's prev= then names a seal nobody has).
 *
 * Three writers seal lines the same way, each reading the previous seal from the file just before it appends: the
 * storage worker (hmi_store.c), the start-up record (hmi_boot.c) and the root helper p99_cfg. Two of them appending at
 * the same instant could both continue from the same seal; the second line then fails verification (a visible fork,
 * never a silent one). This is recorded in docs/STORAGE.md.
 */
#ifndef HMI_AUDITCHAIN_H
#define HMI_AUDITCHAIN_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define HMI_CHAIN_KEY_LEN 32
#define HMI_CHAIN_HEX 32          /* hex digits of one seal */
#define HMI_CHAIN_SEAL_LEN 37     /* " mac=" and the 32 digits */
#define HMI_CHAIN_TAIL 1024       /* bytes read from the end of a file to find the previous seal */

/* The audit key: the first 32 bytes of HMAC-SHA-512(configuration key, "prometheus99 audit chain v1"). */
void hmi_chain_derive_key(const uint8_t *cfg_key, size_t cfg_key_len, uint8_t out[HMI_CHAIN_KEY_LEN]);

/* True if the line (len without its newline) ends in a well-formed seal; *body_len is then the length before " mac=". */
bool hmi_chain_has_seal(const char *line, size_t len, size_t *body_len);

/* The seal of the last line in text[0..len) that has a well-formed one (lines end in '\n'; an unfinished last line
 * is ignored). Returns false, and leaves prev unchanged, if no line has one. prev is HMI_CHAIN_HEX + 1 bytes. */
bool hmi_chain_last_seal(const char *text, size_t len, char *prev);

/* The seal that line[0..body_len) gets after prev (32 hex digits); out is HMI_CHAIN_HEX + 1 bytes. */
void hmi_chain_compute(const uint8_t key[HMI_CHAIN_KEY_LEN], const char *prev, const char *line, size_t body_len,
                       char *out);

/* The previous seal for a line appended to path: the last seal in path, or if path is empty or absent the last seal
 * of path.1, or 32 zeros. */
void hmi_chain_prev_for(const char *path, char *prev);

/* Seals a finished record (len including its newline) into out: the record, " mac=<seal after prev>", the newline.
 * Returns the new length, or 0 if it does not fit in cap or does not end in a newline. */
size_t hmi_chain_seal(const uint8_t key[HMI_CHAIN_KEY_LEN], const char *prev, const char *rec, size_t len, char *out,
                      size_t cap);

/* Result of checking one or more files in order (hmi_chain_verify_text). */
typedef struct {
    uint32_t lines;          /* complete lines looked at */
    uint32_t verified;       /* lines whose seal is right */
    uint32_t failed;         /* lines with a seal that does not match */
    uint32_t unsealed;       /* lines without a well-formed seal */
    uint32_t first_bad_line; /* 1-based line number (in the file being checked) of the first failed or unsealed line, 0 if none */
} hmi_chain_report_t;

/* Checks text (one file) against the key, continuing from prev (HMI_CHAIN_HEX + 1 bytes, updated to the file's last
 * seal). For the first file of a sequence pass prev as an empty string: the start is then the prev= of a LOG_ROTATED
 * first line, or 32 zeros. Adds to *r; first_bad_line is set only if it was 0. */
void hmi_chain_verify_text(const uint8_t key[HMI_CHAIN_KEY_LEN], const char *text, size_t len, char *prev,
                           hmi_chain_report_t *r);

#endif /* HMI_AUDITCHAIN_H */

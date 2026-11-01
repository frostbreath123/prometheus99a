/**
 * @file hmi_auditfmt.h
 * @brief Text format of the audit records and the wall-clock stamp they carry.
 *
 * Every line of the audit file starts with the monotonic time in milliseconds
 * since boot (the order of events inside one boot) and ends with a wall-clock
 * stamp and the clock's synchronisation state. A device without a battery
 * clock can boot with a wrong date; a line stamped "sync=no" must not be
 * trusted for its date, and the monotonic time and the BOOT marker lines
 * still order the events.
 *
 *   <ms> BOOT wall=<ISO time> sync=ok|no
 *   <ms> <EVENT> id=<n> tag=<i> role=<r> eng=<v> raw=0x.. rb=0x.. exc=<c> user=<u> old=<v> oldq=<q> wall=.. sync=..
 *   <ms> ALARM_ACK role=<r> action=ACK|ACK_ALL result=GRANTED|DENIED n=<k> points=<names> user=<u> wall=.. sync=..
 *
 * The formatting functions are pure: they take the wall clock as an argument
 * so tests can fix it. They return the length written (including the
 * newline), or 0 if the line does not fit; a caller treats 0 as "cannot be
 * recorded" and fails closed.
 */
#ifndef HMI_AUDITFMT_H
#define HMI_AUDITFMT_H

#include "hmi_writes.h"
#include "hmi_alarm.h"

typedef struct {
    uint64_t unix_ms;  /* milliseconds since 1970-01-01 UTC */
    bool valid;        /* the clock could be read */
    bool synced;       /* the system says it was set from a time source */
} hmi_wall_t;

/* Reads the platform wall clock. valid is false if it cannot be read. */
void hmi_wall_now(hmi_wall_t *w);

/* "wall=2026-10-22T10:15:03.120Z sync=ok", or "wall=- sync=no" when not valid.
 * Returns the length, or 0 if cap is too small. */
size_t hmi_wall_format(const hmi_wall_t *w, char *buf, size_t cap);

size_t hmi_audit_format_boot(char *buf, size_t cap, uint64_t mono_ms, const hmi_wall_t *w);
size_t hmi_audit_format_write(char *buf, size_t cap, const hmi_write_rec_t *r, const char *event, const hmi_wall_t *w);
/* names[i] is the name of tag i; a point equal to n_names is the PLC link alarm. */
size_t hmi_audit_format_ack(char *buf, size_t cap, const hmi_ack_audit_t *rec, const char *const *names,
                            size_t n_names, const hmi_wall_t *w);

/* One alarm journal entry, with a CRC-32 over the text so that a line torn by a
 * power cut or damaged on disk is recognised when the journal is read back:
 *   <ms> ALARM_EVENT seq=<n> point=<name> event=<e> kind=<k> state=<s> value=<v> wall=.. sync=.. crc=<8 hex>
 * names[i] is the name of tag i; the point equal to n_names is written PLC_LINK. */
size_t hmi_audit_format_journal(char *buf, size_t cap, const hmi_alarm_log_t *e, const char *const *names,
                                size_t n_names, const hmi_wall_t *w);
/* Reads one journal line back (len excludes the newline). Strict: false for a
 * wrong CRC, a missing or unknown field, an unknown point name, or anything
 * else that is not exactly what hmi_audit_format_journal produces. Only the
 * fields of the entry are filled; the wall stamp is checked for presence only. */
bool hmi_audit_parse_journal(const char *line, size_t len, const char *const *names, size_t n_names,
                             hmi_alarm_log_t *out);

/* Adds " <wall stamp>" in front of the final newline of a finished line of
 * length len (the newline is required), for records formatted elsewhere such as
 * configuration lines. Returns the new length, or 0 and leaves the
 * line unchanged if it does not fit in cap or does not end in a newline. */
size_t hmi_audit_stamp_line(char *line, size_t len, size_t cap, const hmi_wall_t *w);

/* Bytes the stamp can take at most, for callers that reserve room. */
#define HMI_WALL_STAMP_MAX 48

#endif /* HMI_AUDITFMT_H */

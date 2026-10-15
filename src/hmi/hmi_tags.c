#include "hmi_tags.h"
#include "hmi_decode.h"
#include <string.h>

#define HMI_MAX_GROUP_GAP 4u /* registers of unused gap we will read through */

uint32_t hmi_tag_stale_limit_ms(const hmi_tag_cfg_t *c)
{
    uint32_t v = c->poll_ms * 3u;
    return v < 1000u ? 1000u : v;
}

int hmi_tagdb_find(const hmi_tagdb_t *db, const char *name)
{
    for (size_t i = 0; i < db->n_tags; i++) {
        if (strncmp(db->cfg[i].name, name, HMI_NAME_LEN) == 0) return (int)i;
    }
    return -1;
}

bool hmi_tagdb_is_ready(const hmi_tagdb_t *db) { return db && db->ready; }

size_t hmi_tagdb_count(const hmi_tagdb_t *db) { return db->n_tags; }

const hmi_tag_cfg_t *hmi_tagdb_cfg(const hmi_tagdb_t *db, size_t idx)
{
    return idx < db->n_tags ? &db->cfg[idx] : NULL;
}

size_t hmi_tagdb_group_count(const hmi_tagdb_t *db) { return db->n_groups; }

const hmi_group_t *hmi_tagdb_group(const hmi_tagdb_t *db, size_t g)
{
    return g < db->n_groups ? &db->groups[g] : NULL;
}

void hmi_tagdb_group_set_due(hmi_tagdb_t *db, size_t g, uint64_t due_ms)
{
    if (g < db->n_groups) db->groups[g].next_due_ms = due_ms;
}

void hmi_tagdb_groups_set_all_due(hmi_tagdb_t *db, uint64_t due_ms)
{
    for (size_t g = 0; g < db->n_groups; g++) db->groups[g].next_due_ms = due_ms;
}

static bool cfg_valid(const hmi_tag_cfg_t *c)
{
    if (c->name[0] == '\0' || memchr(c->name, '\0', HMI_NAME_LEN) == NULL) return false;
    if (memchr(c->units, '\0', HMI_UNIT_LEN) == NULL) return false;
    if ((unsigned)c->area > HMI_AREA_INPUT || (unsigned)c->dtype > HMI_DT_F32) return false;
    if ((unsigned)c->address + hmi_dtype_regs(c->dtype) > 65536u) return false;
    if (c->poll_ms < 50u || c->poll_ms > 3600000u) return false;
    if (!(c->scale != 0.0) || !(c->eng_min <= c->eng_max)) return false;
    if (c->write_allowed) {
        if (c->area != HMI_AREA_HOLDING) return false; /* input registers are read-only */
        if (!(c->write_min <= c->write_max)) return false;
    }
    return true;
}

bool hmi_tagdb_init(hmi_tagdb_t *db, const hmi_tag_cfg_t *cfg, size_t n, uint64_t now_ms)
{
    memset(db, 0, sizeof(*db));
    if (n == 0 || n > HMI_MAX_TAGS) return false;
    for (size_t i = 0; i < n; i++) {
        if (!cfg_valid(&cfg[i])) return false;
        for (size_t j = 0; j < i; j++) {
            if (strncmp(cfg[i].name, cfg[j].name, HMI_NAME_LEN) == 0) return false;
        }
    }
    memcpy(db->cfg, cfg, n * sizeof(cfg[0]));
    db->n_tags = n;

    /* order[] = tag indices sorted by (area, address, poll_ms) */
    for (size_t i = 0; i < n; i++) db->order[i] = (uint8_t)i;
    for (size_t i = 1; i < n; i++) {
        uint8_t k = db->order[i];
        size_t j = i;
        while (j > 0) {
            const hmi_tag_cfg_t *a = &db->cfg[db->order[j - 1]], *b = &db->cfg[k];
            bool gt = (a->area > b->area) || (a->area == b->area && a->address > b->address) ||
                      (a->area == b->area && a->address == b->address && a->poll_ms > b->poll_ms);
            if (!gt) break;
            db->order[j] = db->order[j - 1];
            j--;
        }
        db->order[j] = k;
    }

    /* merge into groups */
    for (size_t i = 0; i < n; i++) {
        const hmi_tag_cfg_t *c = &db->cfg[db->order[i]];
        unsigned w = hmi_dtype_regs(c->dtype);
        hmi_group_t *g = db->n_groups ? &db->groups[db->n_groups - 1] : NULL;
        bool join = false;
        if (g && g->area == c->area && g->period_ms == c->poll_ms) {
            unsigned end = (unsigned)g->start + g->count;
            unsigned c_end = (unsigned)c->address + w;
            unsigned new_end = c_end > end ? c_end : end;
            bool gap_ok = c->address <= end + HMI_MAX_GROUP_GAP;
            join = gap_ok && (new_end - g->start) <= HMI_MAX_REGS_PER_READ;
            if (join) g->count = (uint16_t)(new_end - g->start);
        }
        if (join) {
            g->n_tags++;
        } else {
            if (db->n_groups >= HMI_MAX_GROUPS) return false;
            g = &db->groups[db->n_groups++];
            g->area = c->area;
            g->start = c->address;
            g->count = (uint16_t)w;
            g->period_ms = c->poll_ms;
            g->next_due_ms = now_ms;
            g->first = (uint8_t)i;
            g->n_tags = 1;
        }
    }

    for (size_t i = 0; i < n; i++) {
        db->val[i].quality = HMI_Q_UNAVAILABLE;
        db->val[i].reason = HMI_REASON_NOT_POLLED;
    }
    if (!hmi_mutex_init(&db->lock)) return false;
    db->ready = true;
    return true;
}

void hmi_tagdb_destroy(hmi_tagdb_t *db)
{
    if (db->ready) hmi_mutex_destroy(&db->lock);
    db->ready = false;
}

/* caller holds lock */
static void set_state(hmi_tagdb_t *db, size_t i, uint32_t raw, double eng, hmi_quality_t q,
                      hmi_reason_t r, bool value_valid)
{
    hmi_tag_value_t *v = &db->val[i];
    bool changed = (v->quality != q) || (v->reason != r) || (value_valid && (v->raw != raw));
    if (value_valid) {
        v->raw = raw;
        v->eng = eng;
    }
    v->quality = q;
    v->reason = r;
    if (changed) {
        v->version++;
        db->global_version++;
    }
}

void hmi_tagdb_group_ok(hmi_tagdb_t *db, size_t g, const uint16_t *regs, uint64_t now_ms)
{
    if (g >= db->n_groups) return;
    const hmi_group_t *grp = &db->groups[g];
    hmi_mutex_lock(&db->lock);
    for (unsigned k = 0; k < grp->n_tags; k++) {
        size_t i = db->order[grp->first + k];
        const hmi_tag_cfg_t *c = &db->cfg[i];
        unsigned off = (unsigned)c->address - grp->start;
        uint32_t raw = hmi_regs_to_raw(&regs[off], c->dtype, c->word_order, c->byte_order);
        double num;
        if (!hmi_raw_to_number(raw, c->dtype, &num)) {
            set_state(db, i, raw, 0.0, HMI_Q_BAD, HMI_REASON_PROTOCOL, false);
            continue;
        }
        double eng = num * c->scale + c->offset;
        bool in_range = (eng >= c->eng_min && eng <= c->eng_max);
        set_state(db, i, raw, eng, in_range ? HMI_Q_GOOD : HMI_Q_UNCERTAIN,
                  in_range ? HMI_REASON_NONE : HMI_REASON_OUT_OF_RANGE, true);
        db->val[i].ts_ms = now_ms;
    }
    hmi_mutex_unlock(&db->lock);
}

void hmi_tagdb_group_fail(hmi_tagdb_t *db, size_t g, hmi_reason_t reason)
{
    if (g >= db->n_groups) return;
    const hmi_group_t *grp = &db->groups[g];
    hmi_mutex_lock(&db->lock);
    for (unsigned k = 0; k < grp->n_tags; k++) {
        size_t i = db->order[grp->first + k];
        set_state(db, i, 0, 0.0, HMI_Q_BAD, reason, false);
    }
    hmi_mutex_unlock(&db->lock);
}

void hmi_tagdb_degrade(hmi_tagdb_t *db)
{
    hmi_mutex_lock(&db->lock);
    for (size_t i = 0; i < db->n_tags; i++) {
        if (db->val[i].quality == HMI_Q_GOOD) {
            set_state(db, i, 0, 0.0, HMI_Q_UNCERTAIN, HMI_REASON_MISSED_POLL, false);
        }
    }
    hmi_mutex_unlock(&db->lock);
}

void hmi_tagdb_link_lost(hmi_tagdb_t *db)
{
    hmi_mutex_lock(&db->lock);
    for (size_t i = 0; i < db->n_tags; i++) {
        if (db->val[i].quality != HMI_Q_UNAVAILABLE) {
            set_state(db, i, 0, 0.0, HMI_Q_BAD, HMI_REASON_COMM_LOSS, false);
        }
    }
    hmi_mutex_unlock(&db->lock);
}

void hmi_tagdb_age(hmi_tagdb_t *db, uint64_t now_ms)
{
    hmi_mutex_lock(&db->lock);
    for (size_t i = 0; i < db->n_tags; i++) {
        hmi_tag_value_t *v = &db->val[i];
        if (v->quality == HMI_Q_GOOD || v->quality == HMI_Q_UNCERTAIN) {
            if (now_ms - v->ts_ms > hmi_tag_stale_limit_ms(&db->cfg[i])) {
                set_state(db, i, 0, 0.0, HMI_Q_BAD, HMI_REASON_STALE, false);
            }
        }
    }
    hmi_mutex_unlock(&db->lock);
}

bool hmi_tagdb_get(hmi_tagdb_t *db, size_t idx, hmi_tag_value_t *out)
{
    if (idx >= db->n_tags) return false;
    hmi_mutex_lock(&db->lock);
    *out = db->val[idx];
    hmi_mutex_unlock(&db->lock);
    return true;
}

uint32_t hmi_tagdb_snapshot(hmi_tagdb_t *db, hmi_tag_value_t *out, size_t cap)
{
    hmi_mutex_lock(&db->lock);
    size_t n = db->n_tags < cap ? db->n_tags : cap;
    memcpy(out, db->val, n * sizeof(out[0]));
    uint32_t gv = db->global_version;
    hmi_mutex_unlock(&db->lock);
    return gv;
}

uint32_t hmi_tagdb_global_version(hmi_tagdb_t *db)
{
    hmi_mutex_lock(&db->lock);
    uint32_t gv = db->global_version;
    hmi_mutex_unlock(&db->lock);
    return gv;
}

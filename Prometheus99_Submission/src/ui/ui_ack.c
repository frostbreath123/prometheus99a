#include "ui_priv.h"

#define MAX_LABELS 3

static lv_obj_t *s_status[MAX_LABELS];
static size_t s_n_status;
static char t_ack[UI_TXT * 2];
static bool s_wait;        /* an acknowledgement is waiting for its audit record */
static uint32_t s_seq0;    /* hmi_alarm_last_ack() counter when it was requested */

void ui_ack_reset(void)
{
    memset(s_status, 0, sizeof(s_status));
    s_n_status = 0;
    memset(t_ack, 0, sizeof(t_ack));
    s_wait = false;
    s_seq0 = 0;
}

void ui_ack_bind(lv_obj_t *status_label)
{
    if (s_n_status < MAX_LABELS) s_status[s_n_status++] = status_label;
}

/* The text in t_ack goes to every bound label; tone is optional (NULL keeps the colour). */
static void show(const hmi_tone_t *tone)
{
    for (size_t i = 0; i < s_n_status; i++) {
        lv_label_set_text(s_status[i], t_ack);
        if (tone) lv_obj_set_style_text_color(s_status[i], ui_tone_color(*tone), 0);
    }
}

static void show_result(hmi_ack_result_t r, uint32_t seq0)
{
    hmi_tone_t tone = hmi_view_ack_result_tone(r);
    s_wait = (r == HMI_ACK_PENDING); /* the binder shows the outcome when the record is durable */
    s_seq0 = seq0;
    snprintf(t_ack, sizeof(t_ack), "%s", hmi_view_ack_result_text(r));
    show(&tone);
}

/* The core decides (hmi_alarm_ack_all_as checks the role and audits); this only
 * shows the outcome. */
void ui_ack_all(void)
{
    if (!ui_rt || !s_n_status) return;
    uint64_t now = hmi_now_ms();
    uint32_t seq0 = hmi_alarm_last_ack(&ui_rt->alarms, NULL);
    hmi_ack_result_t r = hmi_alarm_ack_all_as(&ui_rt->alarms, now, ui_current_role(now), ui_current_actor(now));
    show_result(r, seq0);
}

void ui_ack_all_cb(lv_event_t *e)
{
    (void)e;
    ui_ack_all();
}

void ui_ack_point(size_t point, const hmi_alarm_point_t *shown)
{
    if (!ui_rt || !s_n_status) return;
    hmi_alarm_point_t cur;
    hmi_alarm_get(&ui_rt->alarms, point, &cur);
    if (cur.state != shown->state || cur.kind != shown->kind || cur.since_ms != shown->since_ms) {
        hmi_tone_t tone = HMI_TONE_WARN;
        s_wait = false;
        snprintf(t_ack, sizeof(t_ack), "%s", hmi_view_ack_changed_text());
        show(&tone);
        return;
    }
    uint64_t now = hmi_now_ms();
    uint32_t seq0 = hmi_alarm_last_ack(&ui_rt->alarms, NULL);
    hmi_ack_result_t r = hmi_alarm_ack_as(&ui_rt->alarms, point, now, ui_current_role(now), ui_current_actor(now));
    show_result(r, seq0);
}

void ui_ack_role_changed(bool may_ack, hmi_role_t role)
{
    if (!s_n_status) return;
    if (!may_ack) snprintf(t_ack, sizeof(t_ack), "ACK DISABLED: %s role", hmi_role_name(role));
    else t_ack[0] = '\0'; /* a new role: an outcome shown for the previous one no longer applies */
    show(NULL);
}

void ui_ack_poll(void)
{
    if (!s_wait || !s_n_status || !ui_rt) return;
    hmi_ack_result_t ar = HMI_ACK_PENDING;
    if (hmi_alarm_last_ack(&ui_rt->alarms, &ar) != s_seq0) {
        hmi_tone_t tone = hmi_view_ack_result_tone(ar);
        s_wait = false;
        snprintf(t_ack, sizeof(t_ack), "%s", hmi_view_ack_result_text(ar));
        show(&tone);
    }
}

const char *ui_ack_text(void)
{
    return t_ack;
}

#include "ui_priv.h"

#define TAB_Y 44
#define TAB_H 36

#define C_TAB_ON  C_ACCENT
#define C_TAB_OFF C_PANEL

/* The screens, in tab order. A new screen is one more line here. */
#ifdef P99_LITE
static const ui_screen_ops_t *const SCREENS[] = { &ui_screen_overview, &ui_screen_alarms, &ui_screen_settings };
#else
static const ui_screen_ops_t *const SCREENS[] = { &ui_screen_overview, &ui_screen_alarms, &ui_screen_trends, &ui_screen_settings };
#endif
#define N_SCREENS (sizeof(SCREENS) / sizeof(SCREENS[0]))

static lv_obj_t *s_page[N_SCREENS];
static lv_obj_t *s_tab[N_SCREENS];
static bool s_built[N_SCREENS];
static size_t s_cur;

size_t ui_nav_count(void) { return N_SCREENS; }
size_t ui_nav_current(void) { return s_cur; }
bool ui_nav_visible(size_t index) { return index == s_cur && index < N_SCREENS && s_page[index] != NULL; }

void ui_nav_reset(void)
{
    for (size_t i = 0; i < N_SCREENS; i++) {
        s_page[i] = NULL;
        s_tab[i] = NULL;
        s_built[i] = false;
        SCREENS[i]->reset();
    }
    s_cur = 0;
}

static void show_current(void)
{
    for (size_t i = 0; i < N_SCREENS; i++) {
        if (!s_page[i]) continue;
        if (i == s_cur) lv_obj_remove_flag(s_page[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_page[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(s_tab[i], i == s_cur ? C_TAB_ON : C_TAB_OFF, 0);
    }
}

bool ui_nav_select(size_t index)
{
    if (index >= N_SCREENS || !s_page[index]) return false;
    if (index == s_cur) return true;
    if (!s_built[index]) { /* a lazy screen is built on its first visit */
        SCREENS[index]->build(s_page[index]);
        s_built[index] = true;
    }
    s_cur = index;
    show_current();
    ui_nav_update();   /* draw it now rather than at the next tick */
    return true;
}

static void tab_cb(lv_event_t *e)
{
    ui_nav_select((size_t)(intptr_t)lv_event_get_user_data(e));
}

void ui_nav_build(lv_obj_t *scr)
{
    int w = 800 / (int)N_SCREENS;
    for (size_t i = 0; i < N_SCREENS; i++) {
        s_tab[i] = ui_mk_button(scr, (int)i * w, TAB_Y, w, TAB_H, SCREENS[i]->title, tab_cb, (void *)(intptr_t)i);
        lv_obj_set_style_radius(s_tab[i], 0, 0);
    }
    for (size_t i = 0; i < N_SCREENS; i++) {
        lv_obj_t *p = lv_obj_create(scr);
        lv_obj_set_size(p, 800, UI_PAGE_H);
        lv_obj_set_pos(p, 0, UI_PAGE_Y);
        lv_obj_set_style_bg_opa(p, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(p, 0, 0);
        lv_obj_set_style_radius(p, 0, 0);
        lv_obj_set_style_pad_all(p, 0, 0);
        lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
        s_page[i] = p;
        if (!SCREENS[i]->lazy) {
            SCREENS[i]->build(p);
            s_built[i] = true;
        }
    }
    s_cur = 0;
    show_current();
}

/* Only the visible screen is updated: a hidden screen costs nothing per tick.
 * Each screen refreshes everything that changed while it was hidden when it is
 * next updated, because it compares against what it last showed. */
void ui_nav_update(void)
{
    if (s_cur < N_SCREENS && s_page[s_cur]) SCREENS[s_cur]->update();
}

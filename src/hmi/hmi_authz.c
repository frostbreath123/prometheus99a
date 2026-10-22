#include "hmi_authz.h"
#include <string.h>

static const char *const ROLE_NAMES[] = { "viewer", "operator", "engineer", "admin" };
#define N_ROLES (sizeof(ROLE_NAMES) / sizeof(ROLE_NAMES[0]))

bool hmi_authz_allows(hmi_role_t role, hmi_action_t action, hmi_role_t tag_min_role)
{
    if ((unsigned)role >= N_ROLES) return false; /* a corrupt role value is never privileged */
    switch (action) {
        case HMI_ACT_ACK_ALARM: return role >= HMI_ROLE_OPERATOR;
        case HMI_ACT_WRITE_TAG: return role >= tag_min_role;
        case HMI_ACT_MANAGE_USERS: return role >= HMI_ROLE_ADMIN;
        case HMI_ACT_EDIT_SETTINGS: return role >= HMI_ROLE_ENGINEER;
    }
    return false; /* unknown action: deny */
}

const char *hmi_role_name(hmi_role_t role)
{
    return (unsigned)role < N_ROLES ? ROLE_NAMES[role] : "invalid";
}

bool hmi_role_parse(const char *name, hmi_role_t *out)
{
    if (!name) return false;
    for (size_t i = 0; i < N_ROLES; i++) {
        if (strcmp(name, ROLE_NAMES[i]) == 0) {
            *out = (hmi_role_t)i;
            return true;
        }
    }
    return false;
}

/**
 * @file hmi_authz.h
 * @brief The single authorization decision point of the core.
 *
 * The role passed to every privileged call is a CLAIMED role: today it comes
 * from the --role command-line option of the launching process. It is not an
 * authenticated identity and this module does not make it one. Its job is to
 * keep the "who may do what" rule in one place so that the UI cannot be the
 * only thing enforcing it.
 */
#ifndef HMI_AUTHZ_H
#define HMI_AUTHZ_H

#include "hmi_types.h"

typedef enum {
    HMI_ACT_ACK_ALARM,   /* acknowledge one alarm or all alarms: Operator and above */
    HMI_ACT_WRITE_TAG    /* write a tag: role must reach that tag's configured minimum */
} hmi_action_t;

/* tag_min_role is only used for HMI_ACT_WRITE_TAG and ignored otherwise. */
bool hmi_authz_allows(hmi_role_t role, hmi_action_t action, hmi_role_t tag_min_role);

/* Exact, lower-case names: "viewer", "operator", "engineer", "admin". */
const char *hmi_role_name(hmi_role_t role);
/* Strict: NULL, empty, wrong case, extra characters all fail and leave *out alone. */
bool hmi_role_parse(const char *name, hmi_role_t *out);

#endif /* HMI_AUTHZ_H */

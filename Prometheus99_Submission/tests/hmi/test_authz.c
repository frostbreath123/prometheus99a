/* Tests for hmi_authz: the single authorization decision point. */
#include "test_support.h"
#include "../../src/hmi/hmi_authz.h"

static void test_roles(void)
{
    SECTION("authz: role names and strict parsing");
    hmi_role_t r = HMI_ROLE_ADMIN;
    const char *names[] = { "viewer", "operator", "engineer", "admin" };
    for (int i = 0; i < 4; i++) {
        CHECK(strcmp(hmi_role_name((hmi_role_t)i), names[i]) == 0);
        r = HMI_ROLE_VIEWER;
        CHECK(hmi_role_parse(names[i], &r) && (int)r == i);   /* round trip */
    }
    r = HMI_ROLE_ENGINEER;                                      /* failures must not touch *out */
    CHECK(!hmi_role_parse(NULL, &r) && r == HMI_ROLE_ENGINEER);
    CHECK(!hmi_role_parse("", &r) && r == HMI_ROLE_ENGINEER);
    CHECK(!hmi_role_parse("Operator", &r));                     /* case sensitive */
    CHECK(!hmi_role_parse("OPERATOR", &r));
    CHECK(!hmi_role_parse("operatr", &r));                      /* typo */
    CHECK(!hmi_role_parse("operator ", &r));                    /* trailing space */
    CHECK(!hmi_role_parse(" operator", &r));
    CHECK(!hmi_role_parse("root", &r));
    CHECK(!hmi_role_parse("viewer,admin", &r));
    CHECK(strcmp(hmi_role_name((hmi_role_t)99), "invalid") == 0);
}

static void test_ack_rule(void)
{
    SECTION("authz: alarm acknowledgement needs Operator or above");
    CHECK(!hmi_authz_allows(HMI_ROLE_VIEWER, HMI_ACT_ACK_ALARM, HMI_ROLE_VIEWER));
    CHECK(hmi_authz_allows(HMI_ROLE_OPERATOR, HMI_ACT_ACK_ALARM, HMI_ROLE_ADMIN)); /* tag_min ignored */
    CHECK(hmi_authz_allows(HMI_ROLE_ENGINEER, HMI_ACT_ACK_ALARM, HMI_ROLE_VIEWER));
    CHECK(hmi_authz_allows(HMI_ROLE_ADMIN, HMI_ACT_ACK_ALARM, HMI_ROLE_VIEWER));
    CHECK(!hmi_authz_allows((hmi_role_t)99, HMI_ACT_ACK_ALARM, HMI_ROLE_VIEWER)); /* corrupt role */
    CHECK(!hmi_authz_allows((hmi_role_t)-1, HMI_ACT_ACK_ALARM, HMI_ROLE_VIEWER));
    CHECK(!hmi_authz_allows(HMI_ROLE_ADMIN, (hmi_action_t)99, HMI_ROLE_VIEWER));  /* unknown action */
}

static void test_write_rule(void)
{
    SECTION("authz: tag write rule is role >= the tag's minimum (unchanged)");
    for (int role = 0; role < 4; role++) {
        for (int min = 0; min < 4; min++) {
            bool want = role >= min;
            CHECK(hmi_authz_allows((hmi_role_t)role, HMI_ACT_WRITE_TAG, (hmi_role_t)min) == want);
        }
    }
}

static void test_settings_rule(void)
{
    SECTION("authz: asking for a settings change needs Engineer");
    for (int role = 0; role < 4; role++) {
        for (int min = 0; min < 4; min++) {          /* the tag minimum is ignored */
            CHECK(hmi_authz_allows((hmi_role_t)role, HMI_ACT_EDIT_SETTINGS, (hmi_role_t)min) == (role >= HMI_ROLE_ENGINEER));
        }
    }
    CHECK(!hmi_authz_allows((hmi_role_t)99, HMI_ACT_EDIT_SETTINGS, HMI_ROLE_VIEWER));      /* a corrupt role gets nothing */
    CHECK(!hmi_authz_allows((hmi_role_t)-1, HMI_ACT_EDIT_SETTINGS, HMI_ROLE_VIEWER));
}

int main(void)
{
    test_settings_rule();
    test_roles();
    test_ack_rule();
    test_write_rule();
    return test_finish();
}

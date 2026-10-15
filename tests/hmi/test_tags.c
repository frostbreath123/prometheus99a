/* Tests for hmi_tags: validation and poll group merge.
 * Part of the HMI core host tests; shared fixtures are in test_support.h. */
#include "test_support.h"

static void test_group_building(void)
{
    SECTION("tags: validation and poll group merge");
    fx_t *f = fx_new(false);
    CHECK(f->db.n_groups == 3);
    CHECK(f->db.groups[0].start == 0 && f->db.groups[0].count == 2 && f->db.groups[0].n_tags == 2);
    CHECK(f->db.groups[1].start == 10 && f->db.groups[1].count == 2);
    CHECK(f->db.groups[2].area == HMI_AREA_INPUT && f->db.groups[2].period_ms == 500);

    /* read-only accessors agree with the table and bound-check */
    CHECK(hmi_tagdb_is_ready(&f->db) && !hmi_tagdb_is_ready(NULL));
    CHECK(hmi_tagdb_count(&f->db) == f->db.n_tags && hmi_tagdb_count(&f->db) > 0);
    CHECK(hmi_tagdb_cfg(&f->db, 0) == &f->db.cfg[0]);
    CHECK(hmi_tagdb_cfg(&f->db, hmi_tagdb_count(&f->db)) == NULL);
    CHECK(hmi_tagdb_group_count(&f->db) == 3);
    CHECK(hmi_tagdb_group(&f->db, 1) == &f->db.groups[1]);
    CHECK(hmi_tagdb_group(&f->db, 3) == NULL);
    hmi_tagdb_group_set_due(&f->db, 1, 4242);
    CHECK(f->db.groups[1].next_due_ms == 4242 && hmi_tagdb_group(&f->db, 1)->next_due_ms == 4242);
    hmi_tagdb_group_set_due(&f->db, 3, 1); /* out of range: ignored, no write */
    CHECK(f->db.groups[0].next_due_ms != 1 && f->db.groups[2].next_due_ms != 1);
    hmi_tagdb_groups_set_all_due(&f->db, 777);
    CHECK(f->db.groups[0].next_due_ms == 777 && f->db.groups[1].next_due_ms == 777 &&
          f->db.groups[2].next_due_ms == 777);
    fx_free(f, false);

    hmi_tag_cfg_t t[2];
    size_t n;
    hmi_tag_cfg_t base[4];
    make_tags(base, &n);
    hmi_tagdb_t db;
    t[0] = base[0];
    t[1] = base[0]; /* duplicate name */
    CHECK(!hmi_tagdb_init(&db, t, 2, 0));
    t[1] = base[1];
    t[1].address = 65535; t[1].dtype = HMI_DT_U32; /* span overflows 16 bit address space */
    CHECK(!hmi_tagdb_init(&db, t, 2, 0));
    t[1] = base[1];
    t[1].scale = 0.0;
    CHECK(!hmi_tagdb_init(&db, t, 2, 0));
    CHECK(!hmi_tagdb_init(&db, t, 0, 0));
    t[1] = base[1];
    t[1].area = HMI_AREA_INPUT; t[1].write_allowed = true; t[1].write_max = 1;
    CHECK(!hmi_tagdb_init(&db, t, 2, 0)); /* input registers cannot be writable */
}


int main(void)
{
    test_group_building();
    return test_finish();
}

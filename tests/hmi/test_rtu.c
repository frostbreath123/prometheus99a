/* Tests for the Modbus RTU configuration (HMI_RTU): plc.transport "rtu", the serial settings, the device allow-list
 * and the round trip through the writer. The serial link itself is tested end to end in test_rtu_e2e.c (Linux).
 * Part of the HMI core host tests; built with -DHMI_RTU like every test program. */
#include "test_support.h"

static const char *TAGS = "\"tags\":[{\"name\":\"LVL\",\"address\":0,\"type\":\"u16\",\"poll_ms\":200}]";

static bool rtu(const char *plc, hmi_app_cfg_t *c, char *err)
{
    char text[1024];
    snprintf(text, sizeof(text), "{\"version\":1,\"plc\":{%s},%s}", plc, TAGS);
    return parse(text, c, err);
}

static void test_rtu_parse(void)
{
    SECTION("rtu: plc.transport rtu with a device, defaults and every rejection");
    static hmi_app_cfg_t c, c2;
    char err[128];

    CHECK(rtu("\"transport\":\"rtu\",\"device\":\"/dev/ttyUSB0\",\"unit\":3,\"allowed_devices\":[\"/dev/ttyUSB0\"]", &c, err));
    CHECK(c.transport == HMI_TRANSPORT_RTU && strcmp(c.device, "/dev/ttyUSB0") == 0 && c.unit == 3);
    CHECK(c.baud == 19200 && c.parity == 'E' && c.stop_bits == 1);            /* the Modbus serial defaults */
    CHECK(c.response_timeout_ms == 500 && c.connect_timeout_ms == 1000);
    CHECK(hmi_config_has_allow_list(&c) && hmi_config_host_allowed(&c));
    CHECK(c.host[0] == '\0' && c.n_allowed == 0);

    CHECK(rtu("\"transport\":\"rtu\",\"device\":\"/dev/ttyS1\",\"baud\":9600,\"parity\":\"none\",\"stop_bits\":2,\"unit\":1,"
              "\"response_timeout_ms\":800,\"allowed_devices\":[\"/dev/ttyS0\",\"/dev/ttyS1\"]", &c, err));
    CHECK(c.baud == 9600 && c.parity == 'N' && c.stop_bits == 2 && c.response_timeout_ms == 800);
    CHECK(c.n_allowed_devices == 2 && hmi_config_host_allowed(&c));
    CHECK(rtu("\"transport\":\"rtu\",\"device\":\"/dev/ttyS1\",\"parity\":\"odd\",\"unit\":1", &c, err) && c.parity == 'O');
    CHECK(!hmi_config_has_allow_list(&c) && !hmi_config_host_allowed(&c));   /* no allow-list: a signature is refused */
    CHECK(rtu("\"transport\":\"rtu\",\"device\":\"/dev/ttyS1\",\"unit\":1,\"allowed_devices\":[\"/dev/ttyS0\"]", &c, err));
    CHECK(!hmi_config_host_allowed(&c));                                      /* the device is not on the list */

    /* the round trip keeps every serial setting (the on-device editor rewrites the configuration) */
    CHECK(rtu("\"transport\":\"rtu\",\"device\":\"/dev/ttyS1\",\"baud\":38400,\"parity\":\"none\",\"stop_bits\":2,\"unit\":7,"
              "\"allowed_devices\":[\"/dev/ttyS1\"]", &c, err));
    static char out[HMI_CONFIG_MAX_BYTES + 1];
    size_t n = hmi_config_serialize(&c, out, sizeof(out));
    CHECK(n > 0 && hmi_config_parse(out, n, &c2, err, sizeof(err)) && hmi_config_equal(&c, &c2));
    CHECK(strstr(out, "\"transport\":\"rtu\"") != NULL && strstr(out, "\"host\"") == NULL);

    /* rejections */
    const char *bad[] = {
        "\"transport\":\"rtu\",\"unit\":1",                                                  /* no device */
        "\"transport\":\"rtu\",\"device\":\"ttyS1\",\"unit\":1",                             /* not under /dev/ */
        "\"transport\":\"rtu\",\"device\":\"/dev/../etc/passwd\",\"unit\":1",                /* climbs out of /dev */
        "\"transport\":\"rtu\",\"device\":\"/dev/ttyS1\"",                                   /* no unit */
        "\"transport\":\"rtu\",\"device\":\"/dev/ttyS1\",\"unit\":1,\"host\":\"10.0.0.1\"",  /* a host with rtu */
        "\"transport\":\"rtu\",\"device\":\"/dev/ttyS1\",\"unit\":1,\"port\":502",           /* a port with rtu */
        "\"transport\":\"rtu\",\"device\":\"/dev/ttyS1\",\"unit\":1,\"baud\":300",           /* baud too low */
        "\"transport\":\"rtu\",\"device\":\"/dev/ttyS1\",\"unit\":1,\"baud\":230400",        /* baud too high */
        "\"transport\":\"rtu\",\"device\":\"/dev/ttyS1\",\"unit\":1,\"parity\":\"mark\"",    /* unknown parity */
        "\"transport\":\"rtu\",\"device\":\"/dev/ttyS1\",\"unit\":1,\"stop_bits\":3",        /* stop bits */
        "\"transport\":\"serial\",\"device\":\"/dev/ttyS1\",\"unit\":1",                     /* unknown transport */
        "\"host\":\"127.0.0.1\",\"port\":502,\"unit\":1,\"device\":\"/dev/ttyS1\"",          /* serial keys with tcp */
        "\"transport\":\"rtu\",\"device\":\"/dev/ttyS1\",\"unit\":1,\"allowed_devices\":[]", /* empty allow-list */
        "\"transport\":\"rtu\",\"device\":\"/dev/ttyS1\",\"unit\":1,\"allowed_devices\":[\"/dev/a\",\"/dev/a\"]",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) CHECK(!rtu(bad[i], &c, err));

    /* TCP is unchanged: the default, and explicit */
    CHECK(rtu("\"host\":\"127.0.0.1\",\"port\":502,\"unit\":1", &c, err) && c.transport == HMI_TRANSPORT_TCP);
    CHECK(rtu("\"transport\":\"tcp\",\"host\":\"127.0.0.1\",\"port\":502,\"unit\":1", &c, err) &&
          c.transport == HMI_TRANSPORT_TCP);
    CHECK(parse(GOOD_CFG, &c, err) && c.transport == HMI_TRANSPORT_TCP);
}

int main(void)
{
    test_rtu_parse();
    return test_finish();
}

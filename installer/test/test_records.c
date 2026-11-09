// Host tests for installer/main/records.c (installer/SPEC.md, Tests 1): join,
// leave, expiry, DNS names and the stock label, and the download ring.

#include <string.h>

#include "check.h"
#include "records.h"

static const uint8_t RADIO[6] = {0x5C, 0xCF, 0x7F, 0x01, 0x02, 0x03};
static const uint8_t PHONE[6] = {0xAA, 0xBB, 0xCC, 0x00, 0x00, 0x01};
static const uint8_t IP2[4] = {192, 168, 4, 2};
static const uint8_t IP3[4] = {192, 168, 4, 3};

static const rec_device *dev(const records *r, const uint8_t mac[6])
{
    for (int i = 0; i < REC_DEVICES; i++) {
        if (r->dev[i].used && memcmp(r->dev[i].mac, mac, 6) == 0) {
            return &r->dev[i];
        }
    }
    return NULL;
}

static int used(const records *r)
{
    int n = 0;
    for (int i = 0; i < REC_DEVICES; i++) {
        n += r->dev[i].used;
    }
    return n;
}

static void test_devices(void)
{
    static records r;
    rec_init(&r);
    rec_join(&r, RADIO, 1000);
    rec_ip(&r, RADIO, IP2);
    rec_join(&r, PHONE, 2000);
    rec_ip(&r, PHONE, IP3);
    const rec_device *d = dev(&r, RADIO);
    CHECK(d && d->present && d->joined_ms == 1000 && memcmp(d->ip, IP2, 4) == 0 && !d->stock);

    rec_dns(&r, IP2, "pool.ntp.org");
    rec_dns(&r, IP2, "iotc.kuailece.cn");
    d = dev(&r, RADIO);
    CHECK(d->stock);
    CHECK(d->name_count == 2 && strcmp(d->names[0], "iotc.kuailece.cn") == 0 &&
          strcmp(d->names[1], "pool.ntp.org") == 0);
    CHECK(!dev(&r, PHONE)->stock);
    // A repeated name moves to the front without a second entry.
    rec_dns(&r, IP2, "pool.ntp.org");
    CHECK(d->name_count == 2 && strcmp(d->names[0], "pool.ntp.org") == 0);

    // The last 8 distinct names are kept.
    char name[16];
    for (int i = 0; i < 12; i++) {
        snprintf(name, sizeof(name), "n%d.example", i);
        rec_dns(&r, IP3, name);
    }
    const rec_device *p = dev(&r, PHONE);
    CHECK(p->name_count == REC_NAMES);
    CHECK(strcmp(p->names[0], "n11.example") == 0 && strcmp(p->names[7], "n4.example") == 0);
    // A long name is cut to fit.
    char longname[300];
    memset(longname, 'a', sizeof(longname) - 1);
    longname[sizeof(longname) - 1] = '\0';
    rec_dns(&r, IP3, longname);
    CHECK(strlen(p->names[0]) == REC_NAME_MAX - 1);

    // A query from an address no station has is not recorded.
    static const uint8_t other[4] = {192, 168, 4, 9};
    rec_dns(&r, other, "x.example");
    CHECK(used(&r) == 2);

    // Leaving: still listed, greyed, until REC_LEFT_KEEP_MS.
    rec_leave(&r, RADIO, 5000);
    d = dev(&r, RADIO);
    CHECK(d && !d->present && d->left_ms == 5000 && d->stock);
    rec_expire(&r, 5000 + REC_LEFT_KEEP_MS - 1);
    CHECK(dev(&r, RADIO) != NULL);
    rec_expire(&r, 5000 + REC_LEFT_KEEP_MS);
    CHECK(dev(&r, RADIO) == NULL);
    CHECK(dev(&r, PHONE) != NULL);  // present stations never expire

    // Rejoining keeps the record and its names.
    rec_leave(&r, PHONE, 6000);
    rec_join(&r, PHONE, 7000);
    p = dev(&r, PHONE);
    CHECK(p->present && p->joined_ms == 7000 && p->name_count == REC_NAMES);
    rec_expire(&r, 7000 + REC_LEFT_KEEP_MS);
    CHECK(dev(&r, PHONE) != NULL);

    // An address handed to a new station leaves the old one.
    rec_join(&r, RADIO, 8000);
    rec_ip(&r, RADIO, IP3);
    CHECK(memcmp(dev(&r, PHONE)->ip, "\0\0\0\0", 4) == 0);
    rec_dns(&r, IP3, "iotc.kuailece.cn");
    CHECK(dev(&r, RADIO)->stock && !dev(&r, PHONE)->stock);
}

static void test_full_table(void)
{
    static records r;
    rec_init(&r);
    uint8_t mac[6] = {2, 0, 0, 0, 0, 0};
    for (int i = 0; i < REC_DEVICES; i++) {
        mac[5] = (uint8_t)i;
        rec_join(&r, mac, (uint32_t)i);
        if (i != 3) {
            rec_leave(&r, mac, 100);
        }
    }
    CHECK(used(&r) == REC_DEVICES);
    // A new station replaces the oldest one that left (0), never a present one.
    mac[5] = 0x40;
    rec_join(&r, mac, 200);
    CHECK(used(&r) == REC_DEVICES && dev(&r, mac) != NULL);
    mac[5] = 0;
    CHECK(dev(&r, mac) == NULL);
    mac[5] = 3;
    CHECK(dev(&r, mac) != NULL);
}

static void test_downloads(void)
{
    static records r;
    rec_init(&r);
    uint32_t a = rec_dl_start(&r, IP2, 770048, 100);
    CHECK(a == 1);
    rec_dl_update(&r, a, "", 0);
    rec_dl_update(&r, a, "5C:CF:7F:01:02:03", 65536);
    const rec_download *d = NULL;
    for (int i = 0; i < REC_DOWNLOADS; i++) {
        if (r.dl[i].id == a) {
            d = &r.dl[i];
        }
    }
    CHECK(d && d->outcome == DL_RUNNING && d->sent == 65536 && d->total == 770048 &&
          strcmp(d->mac, "5C:CF:7F:01:02:03") == 0 && d->start_ms == 100 && memcmp(d->ip, IP2, 4) == 0);
    rec_dl_update(&r, a, "5C:CF:7F:01:02:03", 770048);
    rec_dl_end(&r, a, DL_OK, 9000);
    CHECK(d->outcome == DL_OK && d->sent == 770048 && d->end_ms == 9000);
    CHECK(strcmp(rec_outcome_name(DL_RUNNING), "in progress") == 0);
    CHECK(strcmp(rec_outcome_name(DL_OK), "sent every byte") == 0);
    CHECK(strcmp(rec_outcome_name(DL_STOPPED), "stopped early") == 0);
    CHECK(strcmp(rec_outcome_name(DL_REFUSED), "refused: no image") == 0);

    // The last 10 are kept; updates to a dropped session are ignored.
    for (int i = 0; i < 12; i++) {
        uint32_t id = rec_dl_start(&r, IP3, 0, 200 + (uint32_t)i);
        rec_dl_end(&r, id, DL_REFUSED, 201 + (uint32_t)i);
    }
    uint32_t lo = 0xFFFFFFFFu, hi = 0;
    for (int i = 0; i < REC_DOWNLOADS; i++) {
        lo = r.dl[i].id < lo ? r.dl[i].id : lo;
        hi = r.dl[i].id > hi ? r.dl[i].id : hi;
    }
    CHECK(lo == 4 && hi == 13);
    rec_dl_update(&r, a, "XX", 1);
    rec_dl_end(&r, a, DL_STOPPED, 1);
    for (int i = 0; i < REC_DOWNLOADS; i++) {
        CHECK(r.dl[i].id != a && strcmp(r.dl[i].mac, "XX") != 0);
    }
}

static void test_mac_text(void)
{
    char t[REC_MAC_TEXT];
    rec_mac_text(RADIO, t);
    CHECK(strcmp(t, "5C:CF:7F:01:02:03") == 0);
}

int main(void)
{
    test_devices();
    test_full_table();
    test_downloads();
    test_mac_text();
    return check_report("records");
}

#include "hal/hal_gpio.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/util/util.h"
#include "os/os.h"
#include "platform.h"
#include "protocol.h"
#include "security.h"
#include "services/gap/ble_svc_gap.h"
#include "storage.h"
#include "sysinit/sysinit.h"
#include <assert.h>
#include <string.h>

/* The BLE event task owns link state and the slot allocator. The worker owns
 * the panel and protocol session. Event queues + semaphore transfer ownership;
 * no SPI or BUSY waits occur inside NimBLE callbacks. */
static const ble_uuid128_t uuid = BLE_UUID128_INIT(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
                                                   0x00, 0x10, 0x00, 0x00, 0x46, 0x24, 0x00, 0x00);
static uint16_t value_handle, connection = BLE_HS_CONN_HANDLE_NONE;
static uint8_t address_type;
static bool subscribed;
static bool slow_advertising;
static volatile uint32_t generation;
static char name[9] = "OD000000";
/* Company ID 0x2446. No invented voltage or temperature measurements. */
static uint8_t msd[16] = {0x46, 0x24};
static struct os_task display_task;
/* Cortex-M0 stack sizes are os_stack_t words: 384 words = 1536 bytes. */
OS_TASK_STACK_DEFINE(display_stack, 384);
static struct os_eventq display_queue;
static struct os_callout tx_retry, idle_timeout, adv_retry;
static struct od_session session;
struct command {
    struct os_event work, done;
    bool used;
    uint32_t generation;
    uint16_t len, mtu;
    uint8_t bytes[OD_MAX_COMMAND];
};
static struct command commands[2];
static struct os_event abort_event, idle_abort_event;
static struct {
    struct os_event event;
    struct os_sem finished;
    uint32_t generation;
    uint8_t data[OD_MAX_RESPONSE + 29];
    uint16_t len, retries;
    int result;
} tx;

static void advertise(void);
static int set_advertisement_data(void);
static int gap_event(struct ble_gap_event *event, void *arg);

static void notify(struct os_event *ev) {
    int rc = BLE_HS_ENOTCONN;
    if (tx.generation == generation && subscribed && connection != BLE_HS_CONN_HANDLE_NONE) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(tx.data, tx.len);
        rc = om ? ble_gatts_notify_custom(connection, value_handle, om) : BLE_HS_ENOMEM;
        /* notify_custom consumes om even on failure. */
    }
    if ((rc == BLE_HS_ENOMEM || rc == BLE_HS_EAGAIN) && ++tx.retries < 100) {
        os_callout_reset(&tx_retry, os_time_ms_to_ticks32(50) + 1);
        return;
    }
    tx.result = rc;
    os_sem_release(&tx.finished);
}
static int send_response(const uint8_t *data, size_t len, void *arg) {
    const struct command *c = arg;
    assert(len <= sizeof tx.data);
    memcpy(tx.data, data, len);
    tx.len = len;
    tx.generation = c->generation;
    tx.retries = 0;
    os_eventq_put(os_eventq_dflt_get(), &tx.event);
    os_sem_pend(&tx.finished, OS_TIMEOUT_NEVER);
    return tx.result;
}
static void command_done(struct os_event *ev) {
    struct command *c = ev->ev_arg;
    bool telemetry = c->len == 0;
    if (telemetry) {
        if (c->mtu) { /* Internal job result: a complete, successful sample. */
            memcpy(msd, c->bytes, sizeof msd);
            msd[15] = (msd[15] & ~8u) | (od_security_enabled() ? 8 : 0);
            if (ble_gap_adv_active())
                set_advertisement_data();
        }
        memset(c->bytes, 0, sizeof c->bytes);
    }
    c->used = false;
    if (!telemetry && connection == BLE_HS_CONN_HANDLE_NONE && ble_gap_adv_active()) {
        ble_gap_adv_stop();
        advertise();
    }
    if (!telemetry && c->generation == generation && connection != BLE_HS_CONN_HANDLE_NONE)
        os_callout_reset(&idle_timeout, 30 * OS_TICKS_PER_SEC);
}
static void command_work(struct os_event *ev) {
    struct command *c = ev->ev_arg;
    if (c->len == 0) {
        /* Reuse an idle command slot: no extra timer, task, or RAM buffer. */
        c->mtu = c->generation == generation && !od_read_msd(c->bytes);
        os_eventq_put(os_eventq_dflt_get(), &c->done);
        return;
    }
    if (c->generation == generation) {
        if (c->len >= 2 && c->bytes[0] == 0 && c->bytes[1] == 0x44) {
            uint8_t sample[16];
            if (od_read_msd(sample)) {
                const uint8_t error[] = {0xff, 0x44};
                send_response(error, sizeof error, c);
                goto done;
            }
            memcpy(msd, sample, sizeof msd);
        }
        msd[15] = (msd[15] & ~8u) | (od_security_enabled() ? 8 : 0);
        od_secure_command(&session, c->bytes, c->len, msd, c->mtu, send_response, c);
        msd[15] = (msd[15] & ~8u) | (od_security_enabled() ? 8 : 0);
    } else
        od_abort(&session);
done:
    memset(c->bytes, 0, sizeof c->bytes);
    os_eventq_put(os_eventq_dflt_get(), &c->done);
}
static void abort_work(struct os_event *ev) {
    od_abort(&session);
    od_store_abort();
    od_security_reset();
}
static void idle_abort_work(struct os_event *ev) {
    od_abort(&session);
    od_store_abort();
}
static void expire(struct os_event *ev) {
    /* Worker may legitimately be waiting for a slow panel. Its own waits are
     * bounded; only expire when no commands are running or queued. */
    if (commands[0].used || commands[1].used) {
        os_callout_reset(&idle_timeout, OS_TICKS_PER_SEC);
        return;
    }
    os_eventq_put(&display_queue, &idle_abort_event);
    if (connection != BLE_HS_CONN_HANDLE_NONE) {
        if (ble_gap_terminate(connection, BLE_ERR_REM_USER_CONN_TERM))
            os_callout_reset(&idle_timeout, OS_TICKS_PER_SEC);
    }
}
static void display_main(void *arg) {
    epd_gpio_init();
    /* Reset establishes a known command state without powering the booster. */
    hal_gpio_write(MYNEWT_VAL(EPD_RESET), 0);
    os_time_delay(os_time_ms_to_ticks32(10) + 1);
    hal_gpio_write(MYNEWT_VAL(EPD_RESET), 1);
    os_time_delay(os_time_ms_to_ticks32(10) + 1);
    epd_off();
    while (1)
        os_eventq_run(&display_queue);
}
static int access_value(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt,
                        void *arg) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
        return BLE_ATT_ERR_READ_NOT_PERMITTED;
    unsigned len = OS_MBUF_PKTLEN(ctxt->om);
    if (len < 2 || len > OD_MAX_COMMAND)
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (conn != connection || !subscribed)
        return BLE_ATT_ERR_UNLIKELY;
    for (unsigned i = 0; i < 2; ++i) {
        struct command *c = &commands[i];
        if (c->used)
            continue;
        if (os_mbuf_copydata(ctxt->om, 0, len, c->bytes))
            return BLE_ATT_ERR_UNLIKELY;
        c->len = len;
        c->mtu = ble_att_mtu(conn);
        c->generation = generation;
        c->used = true;
        os_callout_stop(&idle_timeout);
        os_eventq_put(&display_queue, &c->work);
        return 0;
    }
    return BLE_ATT_ERR_INSUFFICIENT_RES;
}
static const struct ble_gatt_svc_def services[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = &uuid.u,
     .characteristics =
         (struct ble_gatt_chr_def[]){
             {.uuid = &uuid.u,
              .access_cb = access_value,
              .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
              .val_handle = &value_handle},
             {0}}},
    {0}};
static void sample_telemetry(void) {
    if (connection != BLE_HS_CONN_HANDLE_NONE || commands[0].used || commands[1].used)
        return;
    /* Zero length cannot arrive through GATT; it identifies an internal job. */
    struct command *c = &commands[0];
    c->len = c->mtu = 0;
    c->generation = generation;
    c->used = true;
    os_eventq_put(&display_queue, &c->work);
}
static void advertise_retry(struct os_event *ev) {
    if (connection != BLE_HS_CONN_HANDLE_NONE)
        return;
    if (ble_gap_adv_active()) {
        sample_telemetry();
        if (slow_advertising) {
            os_callout_reset(&adv_retry, 300 * OS_TICKS_PER_SEC);
            return;
        }
        slow_advertising = true;
        if (ble_gap_adv_stop()) {
            os_callout_reset(&adv_retry, OS_TICKS_PER_SEC);
            return;
        }
    }
    advertise();
}
static int set_advertisement_data(void) {
    struct ble_hs_adv_fields f = {0};
    /* 3 flags + 18 MSD + 10 name = 31 bytes. UUID goes in scan response. */
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (uint8_t *)name;
    f.name_len = 8;
    f.name_is_complete = 1;
    f.mfg_data = msd;
    f.mfg_data_len = sizeof msd;
    return ble_gap_adv_set_fields(&f);
}
static void advertise(void) {
    struct ble_hs_adv_fields f = {0};
    struct ble_gap_adv_params p = {0};
    if (connection != BLE_HS_CONN_HANDLE_NONE || !ble_hs_synced())
        return;
    int rc = set_advertisement_data();
    if (rc)
        goto retry;
    memset(&f, 0, sizeof f);
    f.uuids128 = &uuid;
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&f);
    if (rc)
        goto retry;
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    p.itvl_min = slow_advertising ? 4800 : 1600;
    p.itvl_max = slow_advertising ? 8000 : 1920; /* slow: 3--5 s */
    rc = ble_gap_adv_start(address_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
retry:
    if (rc)
        os_callout_reset(&adv_retry, OS_TICKS_PER_SEC);
    else if (!slow_advertising)
        os_callout_reset(&adv_retry, 30 * OS_TICKS_PER_SEC);
    else
        os_callout_reset(&adv_retry, 300 * OS_TICKS_PER_SEC);
}
static void reset_link(void) {
    ++generation;
    connection = BLE_HS_CONN_HANDLE_NONE;
    subscribed = false;
    os_callout_stop(&idle_timeout);
    os_eventq_put(&display_queue, &abort_event);
}
static int gap_event(struct ble_gap_event *e, void *arg) {
    switch (e->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (e->connect.status == 0) {
            ++generation;
            connection = e->connect.conn_handle;
            subscribed = false;
            os_callout_stop(&adv_retry);
            os_callout_reset(&idle_timeout, 30 * OS_TICKS_PER_SEC);
        } else
            advertise();
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        reset_link();
        slow_advertising = false;
        advertise();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (e->subscribe.attr_handle == value_handle) {
            subscribed = e->subscribe.cur_notify;
            if (!subscribed) {
                ++generation;
                os_eventq_put(&display_queue, &abort_event);
            }
        }
        break;
    default:
        break;
    }
    return 0;
}
static void on_reset(int reason) { reset_link(); }
static void on_sync(void) {
    msd[15] = (msd[15] & ~8u) | (od_security_enabled() ? 8 : 0);
    uint8_t addr[6];
    static const char hex[] = "0123456789ABCDEF";
    int rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);
    rc = ble_hs_id_infer_auto(0, &address_type);
    assert(rc == 0);
    rc = ble_hs_id_copy_addr(address_type, addr, NULL);
    assert(rc == 0);
    for (unsigned i = 0; i < 3; ++i) {
        name[2 + 2 * i] = hex[addr[2 - i] >> 4];
        name[3 + 2 * i] = hex[addr[2 - i] & 15];
    }
    rc = ble_svc_gap_device_name_set(name);
    assert(rc == 0);
    sample_telemetry();
    advertise();
}
int mynewt_main(int argc, char **argv) {
    int rc;
    sysinit();
    od_store_init();
    os_eventq_init(&display_queue);
    os_sem_init(&tx.finished, 0);
    tx.event.ev_cb = notify;
    abort_event.ev_cb = abort_work;
    idle_abort_event.ev_cb = idle_abort_work;
    os_callout_init(&tx_retry, os_eventq_dflt_get(), notify, NULL);
    os_callout_init(&idle_timeout, os_eventq_dflt_get(), expire, NULL);
    os_callout_init(&adv_retry, os_eventq_dflt_get(), advertise_retry, NULL);
    for (unsigned i = 0; i < 2; ++i) {
        commands[i].work.ev_cb = command_work;
        commands[i].work.ev_arg = &commands[i];
        commands[i].done.ev_cb = command_done;
        commands[i].done.ev_arg = &commands[i];
    }
    rc = os_task_init(&display_task, "epd", display_main, NULL, MYNEWT_VAL(OS_MAIN_TASK_PRIO) + 1,
                      OS_WAIT_FOREVER, display_stack,
                      sizeof display_stack / sizeof display_stack[0]);
    assert(rc == 0);
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    rc = ble_gatts_count_cfg(services);
    assert(rc == 0);
    rc = ble_gatts_add_svcs(services);
    assert(rc == 0);
    while (1)
        os_eventq_run(os_eventq_dflt_get());
}

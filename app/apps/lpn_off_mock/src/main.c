
#include <bluetooth/mesh/models.h>
#include <stdint.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/mesh.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/timer/nrf_grtc_timer.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/sys/reboot.h>

#include "model_handler.h"
#include "retained.h"

/* Wewnetrzny stan sieci mesh - stad bierzemy i odtwarzamy licznik seq */
#include "net.h"

LOG_MODULE_REGISTER(lpn_off_mock, LOG_LEVEL_INF);

/* Adresacja i klucze - musza zgadzac sie z apps/friend */
#define NET_IDX     0x0000
#define APP_IDX     0x0000
#define FRIEND_ADDR 0x0001

static const uint8_t net_key[16] = {
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
};
static const uint8_t app_key[16] = {
    0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00,
};
/* DevKey po cieplym starcie - Friend nie wysyla juz Configa, wiec moze byc staly */
static const uint8_t lpn_dev_key[16] = {
    0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf,
};

/* UUID w unprovisioned beacon - Friend provisionuje tylko ten wezel */
static const uint8_t dev_uuid[16] = {0x1b, 0x7a, 0x0c, 0x54};

/* Interwal publikacji temperatury - tu zarazem czas w System OFF (patrz Kconfig) */
#ifndef CONFIG_LPN_SENSOR_INTERVAL_S
#define CONFIG_LPN_SENSOR_INTERVAL_S 10
#endif

/* Co ile pytac, czy Friend domknal konfiguracje */
#define CFG_POLL_INTERVAL K_MSEC(200)
/* Zapas na retry Frienda po zgubionym Status (patrz Kconfig) */
#define CFG_SETTLE_DELAY K_MSEC(CONFIG_LPN_CFG_SETTLE_MS)

static K_SEM_DEFINE(sem_provisioned, 0, 1);
static K_SEM_DEFINE(sem_friendship, 0, 1);

/* Provisioning zakonczony - wezel ma adres i klucze, czekamy na konfiguracje */
static void prov_complete(uint16_t net_idx, uint16_t addr)
{
    LOG_INF("Sprovisionowany przez Frienda - adres 0x%04x", addr);

    retained.addr = addr;
    k_sem_give(&sem_provisioned);
}

static const struct bt_mesh_prov prov = {
    .uuid = dev_uuid,
    .complete = prov_complete,
};

/* Callbacki LPN */
static void lpn_established(uint16_t net_idx, uint16_t friend_addr, uint8_t queue_size,
                            uint8_t recv_window)
{
    LOG_INF("Friendship nawiazany z Friendem 0x%04x (queue %u, recv_window %u ms)", friend_addr,
            queue_size, recv_window);

    k_sem_give(&sem_friendship);
}

static void lpn_terminated(uint16_t net_idx, uint16_t friend_addr)
{
    LOG_WRN("Friendship zerwany z Friendem 0x%04x", friend_addr);
}

BT_MESH_LPN_CB_DEFINE(lpn_cb) = {
    .established = lpn_established,
    .terminated = lpn_terminated,
};

/* Odklada stan do retencji i odcina zasilanie; GRTC (z LFXO) budzi nas z OFF */
static void enter_system_off(void)
{
    /* seq musi rosnac monotonicznie, inaczej Friend odrzuci nas na replay protection */
    retained.iv_index = bt_mesh.iv_index;
    retained.seq = bt_mesh.seq;
    retained.cycles++;
    retained_save();

    int err = z_nrf_grtc_wakeup_prepare((uint64_t)CONFIG_LPN_SENSOR_INTERVAL_S * USEC_PER_SEC);

    if (err) {
        /* Bez uzbrojonej pobudki System OFF bylby wieczny - lepiej sie zrestartowac */
        LOG_ERR("Uzbrojenie pobudki GRTC nieudane (err %d) - restart", err);
        k_sleep(K_SECONDS(CONFIG_LPN_SENSOR_INTERVAL_S));
        sys_reboot(SYS_REBOOT_COLD);
    }

    LOG_INF("System OFF na %d s (cykl %u)", CONFIG_LPN_SENSOR_INTERVAL_S, retained.cycles);
    sys_poweroff();
}

/* Sciezka zimnego startu: pelny provisioning i konfiguracja od Frienda */
static int join_network_cold(void)
{
    int err = bt_mesh_prov_enable(BT_MESH_PROV_ADV);

    if (err) {
        LOG_ERR("Wlaczenie provisioningu (beacon) nieudane (err %d)", err);
        return err;
    }

    LOG_INF("Czekam na provisioning od Frienda...");
    err = k_sem_take(&sem_provisioned, K_SECONDS(CONFIG_LPN_PROV_TIMEOUT_S));
    if (err) {
        LOG_ERR("Provisioning nie doszedl do skutku w %d s", CONFIG_LPN_PROV_TIMEOUT_S);
        return err;
    }

    /* Adres publikacji ustawia ostatni krok konfiguracji - to nasz znacznik konca */
    for (int i = 0; i < CONFIG_LPN_PROV_TIMEOUT_S * 5; i++) {
        if (model_handler_is_configured()) {
            LOG_INF("Konfiguracja zakonczona (publikacja ustawiona)");
            k_sleep(CFG_SETTLE_DELAY);
            retained.provisioned = 1;
            return 0;
        }
        k_sleep(CFG_POLL_INTERVAL);
    }

    LOG_ERR("Friend nie domknal konfiguracji");
    return -ETIMEDOUT;
}

/* Sciezka cieplego startu: siec i konfiguracja odtworzone z retencji, bez radia */
static int join_network_warm(void)
{
    int err = bt_mesh_provision(net_key, NET_IDX, 0, retained.iv_index, retained.addr, lpn_dev_key);

    if (err) {
        LOG_ERR("Odtworzenie provisioningu nieudane (err %d)", err);
        return err;
    }

    /* Kontynuuj licznik seq z zapasem na cokolwiek, co wyszlo po ostatnim zapisie */
    bt_mesh.seq = retained.seq + CONFIG_LPN_SEQ_MARGIN;

    err = model_handler_restore_config(NET_IDX, APP_IDX, app_key, FRIEND_ADDR);
    if (err) {
        return err;
    }

    LOG_INF("Siec odtworzona z retencji - adres 0x%04x, seq %u", retained.addr, bt_mesh.seq);
    return 0;
}

int main(void)
{
    uint32_t reset_cause = 0;
    int err;

    (void)hwinfo_get_reset_cause(&reset_cause);
    (void)hwinfo_clear_reset_cause();

    /* Waznosc bloku retencyjnego rozstrzyga, ktora sciezka startu nas czeka */
    bool warm = retained_load();

    LOG_INF("Start lpn_off_mock: %s (cykl %u, reset 0x%08x)",
            warm ? "cieply start z retencji RAM" : "zimny start", retained.cycles, reset_cause);

    err = bt_enable(NULL);
    if (err) {
        LOG_ERR("bt_enable nieudany (err %d)", err);
        enter_system_off();
    }

    err = bt_mesh_init(&prov, model_handler_init());
    if (err) {
        LOG_ERR("Inicjalizacja mesh nieudana (err %d)", err);
        enter_system_off();
    }

    if (warm && retained.provisioned) {
        err = join_network_warm();
    }
    else {
        err = join_network_cold();
    }

    if (err) {
        /* Nie udalo sie wejsc do sieci - i tak zasnij, zeby pomiar szedl dalej */
        enter_system_off();
    }

    if (IS_ENABLED(CONFIG_LPN_FRIENDSHIP)) {
        err = bt_mesh_lpn_set(true);
        if (err) {
            LOG_ERR("Wlaczenie LPN nieudane (err %d)", err);
        }
        else if (k_sem_take(&sem_friendship, K_SECONDS(CONFIG_LPN_FRIENDSHIP_TIMEOUT_S))) {
            LOG_WRN("Brak Frienda w %d s - publikuje bez friendship",
                    CONFIG_LPN_FRIENDSHIP_TIMEOUT_S);
        }
    }

    /* Jedyna rzecz, po ktora sie budzimy */
    err = model_handler_publish_temp();
    if (err) {
        LOG_WRN("Publikacja nieudana (err %d)", err);
    }

    /* Reklama musi zdazyc wyjsc w eter przed odcieciem zasilania */
    k_sleep(K_MSEC(CONFIG_LPN_DRAIN_MS));

    enter_system_off();

    return 0;
}

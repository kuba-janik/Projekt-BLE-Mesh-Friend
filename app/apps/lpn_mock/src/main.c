
#include <bluetooth/mesh/models.h>
#include <ram_pwrdn.h>
#include <stdint.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/mesh.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "model_handler.h"

LOG_MODULE_REGISTER(lpn_mock, LOG_LEVEL_INF);

/* UUID w unprovisioned beacon - ten sam co w apps/lpn (mock jest zamiennikiem) */
static const uint8_t dev_uuid[16] = {0x1b, 0x7a, 0x0c, 0x54};

/* W tryb LPN wchodzimy dopiero po konfiguracji - spiacy wezel nie ACK-uje segmentow */
#define CONFIG_POLL_INTERVAL K_SECONDS(2) /* jak czesto sprawdzac stan konfiguracji */
#define CONFIG_SETTLE_DELAY  K_SECONDS(8) /* zapas na retry Frienda po zgubionym Status */

static struct k_work_delayable lpn_start_work;
static bool config_detected;

/* Interwal publikacji temperatury - ustawiany flaga builda (patrz Kconfig) */
#ifndef CONFIG_LPN_SENSOR_INTERVAL_S
#define CONFIG_LPN_SENSOR_INTERVAL_S 10
#endif
#define SENSOR_READ_INTERVAL K_SECONDS(CONFIG_LPN_SENSOR_INTERVAL_S)

static struct k_work_delayable sensor_read_work;

static void sensor_read_work_handler(struct k_work *work)
{
    model_handler_publish_temp();

    /* Przeplanuj siebie - odczyt i publikacja co SENSOR_READ_INTERVAL */
    k_work_reschedule(&sensor_read_work, SENSOR_READ_INTERVAL);
}

/* Czeka az Friend skonfiguruje wezel, odczekuje zapas i wlacza tryb LPN */
static void lpn_start_handler(struct k_work *work)
{
    if (!config_detected) {
        if (!model_handler_is_configured()) {
            /* Konfiguracja jeszcze niekompletna - pytaj dalej */
            k_work_reschedule(&lpn_start_work, CONFIG_POLL_INTERVAL);
            return;
        }

        config_detected = true;
        LOG_INF("Konfiguracja zakonczona (publikacja ustawiona) - za chwile LPN");
        k_work_reschedule(&lpn_start_work, CONFIG_SETTLE_DELAY);
        return;
    }

    /* Konfiguracja gotowa - teraz mozna spac */
    int err = bt_mesh_lpn_set(true);
    if (err) {
        LOG_ERR("Wlaczenie LPN nieudane (err %d)", err);
    }
    else {
        LOG_INF("Tryb LPN wlaczony - szukam Frienda");
    }
}

/* Provisioning zakonczony - wezel ma adres i klucze, czekamy na konfiguracje */
static void prov_complete(uint16_t net_idx, uint16_t addr)
{
    LOG_INF("Sprovisionowany przez Frienda - adres 0x%04x", addr);
    k_work_reschedule(&lpn_start_work, CONFIG_POLL_INTERVAL);
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

    /* Rusz cykliczne wysylanie pomiarow */
    k_work_reschedule(&sensor_read_work, K_NO_WAIT);
}

static void lpn_terminated(uint16_t net_idx, uint16_t friend_addr)
{
    LOG_WRN("Friendship zerwany z Friendem 0x%04x", friend_addr);

    /* Brak Frienda - nie ma gdzie publikowac, wiec zatrzymaj pomiary */
    k_work_cancel_delayable(&sensor_read_work);
}

static void lpn_polled(uint16_t net_idx, uint16_t friend_addr, bool retry)
{
    LOG_DBG("Poll do Frienda 0x%04x%s", friend_addr, retry ? " (retry)" : "");
}

BT_MESH_LPN_CB_DEFINE(lpn_cb) = {
    .established = lpn_established,
    .terminated = lpn_terminated,
    .polled = lpn_polled,
};

int main(void)
{
    int err;

    LOG_INF("Start wezla LPN Mock (BTZ_EndDevice, czujnik falszowany)");

    /* Kontrola wkompilowanego Kconfig: LPN_AUTO=1 znaczy, ze prj.conf nie wszedl */
    LOG_INF("KONFIG: LPN_AUTO=%d LOW_POWER=%d (wymagane LPN_AUTO=0)",
            IS_ENABLED(CONFIG_BT_MESH_LPN_AUTO), IS_ENABLED(CONFIG_BT_MESH_LOW_POWER));

    /* Inicjalizacja Bluetooth */
    err = bt_enable(NULL);
    if (err) {
        LOG_ERR("bt_enable nieudany (err %d)", err);
        return 1;
    }

    LOG_INF("Bluetooth zainicjalizowany");

    /* Worki: publikacja pomiarow oraz wejscie w LPN po konfiguracji */
    k_work_init_delayable(&sensor_read_work, sensor_read_work_handler);
    k_work_init_delayable(&lpn_start_work, lpn_start_handler);

    err = bt_mesh_init(&prov, model_handler_init());
    if (err) {
        LOG_ERR("Inicjalizacja mesh nieudana (err %d)", err);
        return 1;
    }

    /* Wystaw unprovisioned beacon i czekaj na provisioning od Frienda */
    err = bt_mesh_prov_enable(BT_MESH_PROV_ADV);
    if (err) {
        LOG_ERR("Wlaczenie provisioningu (beacon) nieudane (err %d)", err);
        return 1;
    }
    LOG_INF("Czekam na provisioning od Frienda...");

    /* Sekcje RAM za obrazem aplikacji nie sa uzywane - odetnij im zasilanie.
     * Wymaga COMMON_LIBC_MALLOC_ARENA_SIZE > 0, inaczej gasilibysmy arene malloc. */
    if (IS_ENABLED(CONFIG_RAM_POWER_DOWN_LIBRARY)) {
        power_down_unused_ram();
    }

    return 0;
}

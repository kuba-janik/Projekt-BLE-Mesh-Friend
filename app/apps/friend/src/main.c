#include <bluetooth/mesh/models.h>
#include <stdint.h>
#include <string.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/mesh.h>
#include <zephyr/bluetooth/mesh/cdb.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(node_friend, LOG_LEVEL_INF);

/* Czyszczenie Replay Protection List */
extern void bt_mesh_rpl_clear(void);

/* Reset kontekstow odbioru segmentow (seq_auth) - API wewnetrzne stosu mesh */
extern void bt_mesh_rx_reset(void);

#define LED_NODE DT_ALIAS(led0)

/* Adresacja sieci */
#define NET_IDX     0x0000 /* index sieci */
#define APP_IDX     0x0000 /* index aplikacji */
#define FRIEND_ADDR 0x0001 /* staly adres Friend (provisioner) */
#define LPN_ADDR    0x0002 /* staly adres przydzielany LPN-owi */

/* KLUCZE */
static const uint8_t net_key[16] = {
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
};
static const uint8_t app_key[16] = {
    0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00,
};
/* DevKey */
static const uint8_t dev_key[16] = {
    0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf,
};

/* UUID Frienda */
static const uint8_t dev_uuid[16] = {0xdd, 0xdd};

/* UUID naszego LPN-a - inne beacony ignorujemy (musi zgadzac sie z apps/lpn) */
static const uint8_t lpn_uuid[16] = {0x1b, 0x7a, 0x0c, 0x54};

/* Wypisanie klucza dla Wiresharka */
static void log_sniff_key(const char *name, const uint8_t key[16])
{
    char hex[33];

    bin2hex(key, 16, hex, sizeof(hex));
    LOG_INF("  %s: %s", name, hex);
}

static struct bt_mesh_cfg_cli cfg_cli;

static void health_current_status(struct bt_mesh_health_cli *cli, uint16_t addr, uint8_t test_id,
                                  uint16_t cid, uint8_t *faults, size_t fault_count)
{
    size_t i;

    LOG_INF("Health Current Status from 0x%04x", addr);

    if (!fault_count) {
        LOG_INF("Health Test ID 0x%02x Company ID 0x%04x: no faults", test_id, cid);
        return;
    }

    LOG_WRN("Health Test ID 0x%02x Company ID 0x%04x Fault Count %zu:", test_id, cid, fault_count);

    for (i = 0; i < fault_count; i++) {
        LOG_WRN("\t0x%02x", faults[i]);
    }
}

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED_NODE, gpios);

#define BLINK_INTERVAL K_MSEC(250)
static struct k_work_delayable blink_work;

static void blink_handler(struct k_work *work)
{
    gpio_pin_toggle_dt(&led);
    k_work_reschedule(&blink_work, BLINK_INTERVAL);
}

static struct bt_mesh_health_cli health_cli = {
    .current_status = health_current_status,
};

/* Sensor Client: odbiera temperature publikowana przez LPN */
static void sensor_cli_data_cb(struct bt_mesh_sensor_cli *cli, struct bt_mesh_msg_ctx *ctx,
                               const struct bt_mesh_sensor_type *sensor,
                               const struct bt_mesh_sensor_value *value)
{
    if (sensor->id != bt_mesh_sensor_present_amb_temp.id) {
        return;
    }

    LOG_INF("Temperatura: %s C", bt_mesh_sensor_ch_str(value));

    /* Odszyfrowana publikacja z LPN dowodzi, ze konfiguracja przeszla */
    if (ctx->addr != LPN_ADDR) {
        return;
    }

    struct bt_mesh_cdb_node *node = bt_mesh_cdb_node_get(ctx->addr);
    if (node && !atomic_test_and_set_bit(node->flags, BT_MESH_CDB_NODE_CONFIGURED)) {
        LOG_INF("LPN 0x%04x publikuje - konfiguracja potwierdzona", ctx->addr);
    }
}

/* Handlery Sensor Clienta */
static const struct bt_mesh_sensor_cli_handlers sensor_cli_handlers = {
    .data = sensor_cli_data_cb,
};

static struct bt_mesh_sensor_cli sensor_cli = BT_MESH_SENSOR_CLI_INIT(&sensor_cli_handlers);

static const struct bt_mesh_model root_models[] = {
    BT_MESH_MODEL_CFG_SRV,                 /* Configuration Server */
    BT_MESH_MODEL_CFG_CLI(&cfg_cli),       /* Configuration Client (loopback self-config) */
    BT_MESH_MODEL_HEALTH_CLI(&health_cli), /* Health Client */
    BT_MESH_MODEL_SENSOR_CLI(&sensor_cli), /* Sensor Client */
};

static const struct bt_mesh_elem elements[] = {
    BT_MESH_ELEM(0, root_models, BT_MESH_MODEL_NONE),
};

/* Kompozycja wezla dla bt_mesh_init */
static const struct bt_mesh_comp mesh_comp = {
    .cid = BT_COMP_ID_LF,               /* Local Fictional Company ID */
    .elem = elements,                   /* Elementy sieci */
    .elem_count = ARRAY_SIZE(elements), /* Liczba elementow */
};

/* Adres LPN-a z ostatniego provisioningu - dla watku konfiguracji */
static uint16_t provisioned_lpn_addr;
/* Blokada: nie ruszaj kolejnego provisioningu, gdy jeden juz trwa */
static bool prov_in_progress;

/* Konfiguracja rusza zaraz po provisioningu - spiacy LPN nie ACK-uje segmentow
 */
#define LPN_CFG_DELAY      K_SECONDS(6) /* zwloka na domkniecie linku PB-ADV */
#define LPN_CFG_RETRIES    5            /* LPN bywa nieresponsywny do ~30 s po provisioningu */
#define LPN_CFG_TIMEOUT_MS 5000         /* timeout Config Client - obudzony LPN odpowiada szybko */

/* Watchdog: brak node_added w tym czasie zwalnia blokade do ponowienia */
/* Na granicznym linku PB-ADV domyka sie dluzej niz 10 s - za krotki watchdog
 * zwalnial blokade i drugi beacon startowal provisioning na zywym linku */
#define PROV_WATCHDOG K_SECONDS(40)

static struct k_work_delayable prov_watchdog_work;

/* Cykliczny zrzut licznikow komunikacji stosu (ile PDU zeszlo do LPN) */
#define STAT_DUMP_INTERVAL K_SECONDS(60)
static struct k_work_delayable stat_dump_work;

/* Semafor budzacy watek konfiguracji - podnoszony w node_added */
K_SEM_DEFINE(sem_node_added, 0, 1);

/* Czy LPN nawiazal friendship (== spi) - do diagnozy bledow konfiguracji */
static volatile bool lpn_in_friendship;

/* Szukanie wezla w CDB po UUID - do wykrycia restartu LPN */
struct cdb_uuid_search {
    const uint8_t *uuid;
    struct bt_mesh_cdb_node *found;
};

static uint8_t cdb_uuid_match(struct bt_mesh_cdb_node *node, void *user_data)
{
    struct cdb_uuid_search *s = user_data;

    if (memcmp(node->uuid, s->uuid, 16) == 0) {
        s->found = node;
        return BT_MESH_CDB_ITER_STOP;
    }

    return BT_MESH_CDB_ITER_CONTINUE;
}

static struct bt_mesh_cdb_node *cdb_node_by_uuid(const uint8_t uuid[16])
{
    struct cdb_uuid_search s = {.uuid = uuid, .found = NULL};

    bt_mesh_cdb_node_foreach(cdb_uuid_match, &s);
    return s.found;
}

/* Zdalna konfiguracja LPN: AppKey, bindy, publikacja. Idempotentna - mozna
 * ponawiac */
static int configure_lpn(uint16_t addr)
{
    uint8_t status;
    int err;

    err = bt_mesh_cfg_cli_app_key_add(NET_IDX, addr, NET_IDX, APP_IDX, app_key, &status);
    if (err || status) {
        LOG_WRN("AppKey add na LPN nieudane (err %d, status %d)", err, status);
        return err ? err : -EIO;
    }

    const uint16_t bind_ids[] = {
        BT_MESH_MODEL_ID_SENSOR_SRV,
        BT_MESH_MODEL_ID_HEALTH_SRV,
    };

    for (size_t i = 0; i < ARRAY_SIZE(bind_ids); i++) {
        err = bt_mesh_cfg_cli_mod_app_bind(NET_IDX, addr, addr, APP_IDX, bind_ids[i], &status);
        if (err || status) {
            LOG_WRN("Bind modelu 0x%04x na LPN nieudany (err %d, status %d)", bind_ids[i], err,
                    status);
            return err ? err : -EIO;
        }
    }

    /* Sensor Server LPN publikuje pomiary na Frienda */
    struct bt_mesh_cfg_cli_mod_pub pub = {
        .addr = FRIEND_ADDR,
        .app_idx = APP_IDX,
        .ttl = 7,
        .period = 0,
        .transmit = BT_MESH_TRANSMIT(0, 30),
    };

    err = bt_mesh_cfg_cli_mod_pub_set(NET_IDX, addr, addr, BT_MESH_MODEL_ID_SENSOR_SRV, &pub,
                                      &status);
    if (err || status) {
        LOG_WRN("Publikacja Sensor Srv na LPN nieudana (err %d, status %d)", err, status);
        return err ? err : -EIO;
    }

    /* Oznacz wezel jako skonfigurowany w CDB */
    struct bt_mesh_cdb_node *node = bt_mesh_cdb_node_get(addr);
    if (node) {
        atomic_set_bit(node->flags, BT_MESH_CDB_NODE_CONFIGURED);
    }

    return 0;
}

/* Watek konfiguracji - blokujacy Config Client poza system workqueue (SAR-TX
 * zyje) */
static void config_thread(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    while (1) {
        k_sem_take(&sem_node_added, K_FOREVER);

        /* Daj czas na domkniecie linku provisioningu */
        k_sleep(LPN_CFG_DELAY);

        uint16_t addr = provisioned_lpn_addr;
        int err = -EIO;

        /* Nie konfiguruj drugi raz - wezel juz oznaczony w CDB */
        struct bt_mesh_cdb_node *cfg_node = bt_mesh_cdb_node_get(addr);
        if (cfg_node && atomic_test_bit(cfg_node->flags, BT_MESH_CDB_NODE_CONFIGURED)) {
            LOG_INF("LPN 0x%04x juz skonfigurowany - pomijam", addr);
            continue;
        }

        bool confirmed = false;

        for (int attempt = 1; attempt <= LPN_CFG_RETRIES && err; attempt++) {
            LOG_INF("Zdalna konfiguracja LPN 0x%04x (proba %d/%d)...", addr, attempt,
                    LPN_CFG_RETRIES);
            err = configure_lpn(addr);

            /* Zgubiony Status daje blad, choc LPN moze byc juz gotowy - sprawdz flage
             */
            if (err) {
                cfg_node = bt_mesh_cdb_node_get(addr);
                if (cfg_node && atomic_test_bit(cfg_node->flags, BT_MESH_CDB_NODE_CONFIGURED)) {
                    LOG_INF(
                        "LPN 0x%04x potwierdzil konfiguracje publikacja "
                        "- przerywam ponawianie",
                        addr);
                    confirmed = true;
                    break;
                }
            }
        }

        if (confirmed) {
            /* Sukces potwierdzony publikacja - komunikat juz wypisany */
            continue;
        }

        if (err) {
            LOG_ERR("Konfiguracja LPN 0x%04x nieudana ostatecznie (err %d)", addr, err);
            if (lpn_in_friendship) {
                LOG_ERR(
                    "PRZYCZYNA: LPN nawiazal friendship i SPI - nie "
                    "odbiera segmentow. LPN musi zostac pelnym wezlem "
                    "do konca konfiguracji.");
            }
            else {
                LOG_ERR(
                    "LPN nie odpowiada mimo ze NIE spi - sprawdz zasieg "
                    "i log strony LPN (SAR discard?).");
            }
            continue;
        }

        LOG_INF("LPN 0x%04x skonfigurowany - publikuje na 0x%04x", addr, FRIEND_ADDR);
    }
}

#define CONFIG_THREAD_STACK 2560
#define CONFIG_THREAD_PRIO  7
K_THREAD_DEFINE(config_tid, CONFIG_THREAD_STACK, config_thread, NULL, NULL, NULL,
                CONFIG_THREAD_PRIO, 0, 0);

/* Zrzut licznikow - pod #if, bo bt_mesh_stat_get istnieje tylko z
 * BT_MESH_STATISTIC */
static void stat_dump_work_handler(struct k_work *work)
{
#if defined(CONFIG_BT_MESH_STATISTIC)
    struct bt_mesh_statistic st;

    bt_mesh_stat_get(&st);
    LOG_INF("Stat: rx_adv=%u | tx_friend plan=%u ok=%u | tx_local plan=%u ok=%u", st.rx_adv,
            st.tx_friend_planned, st.tx_friend_succeeded, st.tx_local_planned,
            st.tx_local_succeeded);
#endif
    k_work_reschedule(&stat_dump_work, STAT_DUMP_INTERVAL);
}

/* Provisioning nie zakonczyl sie w czasie - zwolnij blokade do ponowienia */
static void prov_watchdog_handler(struct k_work *work)
{
    if (prov_in_progress) {
        LOG_WRN(
            "Provisioning LPN nie zakonczyl sie w czasie - ponowie przy "
            "kolejnym beaconie");
        prov_in_progress = false;
    }
}

/* Odebrano beacon nieprovisionowanego urzadzenia */
static void unprovisioned_beacon(uint8_t uuid[16], bt_mesh_prov_oob_info_t oob_info,
                                 uint32_t *uri_hash)
{
    /* Reaguj tylko na nasz LPN - obce urzadzenia ignorujemy */
    if (memcmp(uuid, lpn_uuid, 16) != 0) {
        return;
    }

    if (prov_in_progress) {
        return;
    }

    /* LPN sie zrestartowal - sprzataj po starej instancji przed ponownym
     * provisioningiem */
    struct bt_mesh_cdb_node *old = cdb_node_by_uuid(uuid);
    if (old) {
        uint16_t old_addr = old->addr;

        LOG_INF("LPN 0x%04x wrocil - ponowny provisioning", old_addr);

        /* Zerwij stary friendship - inaczej konfiguracja utknie w Friend Queue */
        int terr = bt_mesh_friend_terminate(old_addr);
        if (terr == 0) {
            LOG_INF("Zerwano stary friendship z LPN 0x%04x - adres wolny", old_addr);
        }
        else {
            LOG_INF("Brak aktywnego friendship z LPN 0x%04x (err %d)", old_addr, terr);
        }

        bt_mesh_cdb_node_del(old, false);
        bt_mesh_rpl_clear();

        /* Zrestartowany LPN liczy seq od nowa - bez tego jego odpowiedzi sa
         * odrzucane */
        bt_mesh_rx_reset();
        LOG_INF("Wyczyszczono konteksty seg_rx (stary seq_auth LPN)");
    }

    prov_in_progress = true;

    int err = bt_mesh_provision_adv(uuid, NET_IDX, LPN_ADDR, 0);
    if (err) {
        LOG_ERR("Provisioning LPN nieudany (err %d)", err);
        prov_in_progress = false;
    }
    else {
        LOG_INF("Rozpoczeto provisioning LPN -> 0x%04x", LPN_ADDR);
        /* Uzbroj watchdog na wypadek nieukonczonego provisioningu */
        k_work_reschedule(&prov_watchdog_work, PROV_WATCHDOG);
    }
}

/* Provisioning LPN zakonczony - konfiguruj od razu, zanim wezel zasnie */
static void node_added(uint16_t net_idx, uint8_t uuid[16], uint16_t addr, uint8_t num_elem)
{
    LOG_INF("LPN sprovisionowany @ 0x%04x (elementow: %u) - konfiguruje", addr, num_elem);

    k_work_cancel_delayable(&prov_watchdog_work);
    prov_in_progress = false;
    provisioned_lpn_addr = addr;
    /* Obudz watek konfiguracji */
    k_sem_give(&sem_node_added);
}

/* Rola w provisioningu: Friend jest provisionerem */
static const struct bt_mesh_prov prov = {
    .uuid = dev_uuid,
    .unprovisioned_beacon = unprovisioned_beacon,
    .node_added = node_added,
};

/* Callbacki dla Friendship */
static void friend_established(uint16_t net_idx, uint16_t lpn_addr, uint8_t recv_delay,
                               uint32_t polltimeout)
{
    LOG_INF("Friendship z LPN 0x%04x nawiazany: recv_delay=%u ms, poll_timeout=%u ms", lpn_addr,
            recv_delay, polltimeout);
    lpn_in_friendship = true;

    k_work_cancel_delayable(&blink_work);
    gpio_pin_set_dt(&led, 1);

    /* Tu nie konfigurujemy - LPN juz spi. Friendship przed konfiguracja = blad na
     * LPN */
    struct bt_mesh_cdb_node *node = bt_mesh_cdb_node_get(lpn_addr);
    if (node && !atomic_test_bit(node->flags, BT_MESH_CDB_NODE_CONFIGURED)) {
        LOG_WRN(
            "Friendship PRZED zakonczeniem konfiguracji - LPN uspil sie za "
            "wczesnie (zla firmware LPN?). Konfiguracja segmentowana nie "
            "przejdzie.");
    }
}

static void friend_terminated(uint16_t net_idx, uint16_t lpn_addr)
{
    LOG_WRN("Friendship z LPN 0x%04x zerwana - czekam na ponowny Friend Request", lpn_addr);
    lpn_in_friendship = false;

    k_work_reschedule(&blink_work, K_NO_WAIT);

    /* Wyczysc RPL */
    bt_mesh_rpl_clear();
    LOG_INF("Wyczyszczono RPL");
}

/* Friend Poll od LPN - pokazuje kadencje odpytywania i ze kierunek LPN->Friend
 * zyje */
static void friend_polled(uint16_t net_idx, uint16_t lpn_addr)
{
    LOG_INF("Friend Poll <- LPN 0x%04x", lpn_addr);
}

BT_MESH_FRIEND_CB_DEFINE(friend_cb) = {
    .established = friend_established,
    .terminated = friend_terminated,
    .polled = friend_polled,
};

/* Konfiguracja samego siebie przez loopback Config Client */
static void configure_self(void)
{
    uint8_t status;
    int err;

    LOG_INF("Konfiguracja lokalna (loopback do 0x%04x)...", FRIEND_ADDR);

    /* Dodaje AppKey do wezla */
    err = bt_mesh_cfg_cli_app_key_add(NET_IDX, FRIEND_ADDR, NET_IDX, APP_IDX, app_key, &status);
    if (err || status) {
        LOG_ERR("Dodanie AppKey nieudane (err %d, status %d)", err, status);
        return;
    }

    /* Podpina AppKey pod model Health Client */
    err = bt_mesh_cfg_cli_mod_app_bind(NET_IDX, FRIEND_ADDR, FRIEND_ADDR, APP_IDX,
                                       BT_MESH_MODEL_ID_HEALTH_CLI, &status);
    if (err || status) {
        LOG_ERR("Bind Health Cli nieudany (err %d, status %d)", err, status);
        return;
    }

    /* Podpina AppKey do Sensor Clienta - bez tego nie odszyfruje danych z LPN */
    err = bt_mesh_cfg_cli_mod_app_bind(NET_IDX, FRIEND_ADDR, FRIEND_ADDR, APP_IDX,
                                       BT_MESH_MODEL_ID_SENSOR_CLI, &status);
    if (err || status) {
        LOG_ERR("Bind Sensor Cli nieudany (err %d, status %d)", err, status);
        return;
    }

    LOG_INF("Konfiguracja zakonczona");
}

static int bt_ready(void)
{
    int err;

    /* Marker firmware: brak tej linii w logu = dziala stary build */
    LOG_INF("FW Friend: real-provisioning (config w node_added + diagnostyka)");

    LOG_INF("=== KLUCZE DO SNIFFOWANIA (Wireshark), IV index = 0 ===");
    log_sniff_key("NetKey", net_key);
    log_sniff_key("AppKey", app_key);

    /* prov = kim jestem i jak provisionuje, mesh_comp = jakie modele udostepniam
     */
    err = bt_mesh_init(&prov, &mesh_comp);
    if (err) {
        LOG_ERR("Initializing mesh failed (err %d)", err);
        return err;
    }

    LOG_INF("BLE Mesh initialized");

    /* Ustaw timeout Config Client */
    bt_mesh_cfg_cli_timeout_set(LPN_CFG_TIMEOUT_MS);

    /* CDB - baza wezlow i ich DevKey; w RAM, wiec powstaje od nowa po restarcie
     */
    err = bt_mesh_cdb_create(net_key);
    if (err && err != -EALREADY) {
        LOG_ERR("Utworzenie CDB nieudane (err %d)", err);
        return err;
    }

    /* Friend jako provisioner sam wchodzi do wlasnej sieci na staly adres */
    err = bt_mesh_provision(net_key, NET_IDX, 0, 0, FRIEND_ADDR, dev_key);
    if (err) {
        LOG_ERR("Self-provisioning nieudany (err %d)", err);
        return err;
    }
    LOG_INF("Self-provisioned - jestem w sieci @ 0x%04x", FRIEND_ADDR);

    /* Konfiguracja lokalna */
    configure_self();

    /* Wlacz funkcje Friend */
    err = bt_mesh_friend_set(BT_MESH_FEATURE_ENABLED);
    if (err && err != -EALREADY) {
        LOG_ERR("Wlaczenie Friend nieudane (err %d)", err);
    }
    else {
        LOG_INF("Friend wlaczony (real provisioning)");
    }

    return 0;
}

int main(void)
{
    int err;
    /* Inicjalizacja LED */
    err = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
    if (err != 0) {
        LOG_ERR("Error %d: failed to configure %s pin %d", err, led.port->name, led.pin);
        return 0;
    }

    LOG_INF("Initializing...");
    for (size_t i = 0; i < 50; i++) {
        gpio_pin_toggle_dt(&led);
        k_sleep(K_MSEC(100));
    }

    k_work_init_delayable(&blink_work, blink_handler);
    k_work_init_delayable(&prov_watchdog_work, prov_watchdog_handler);
    k_work_init_delayable(&stat_dump_work, stat_dump_work_handler);

    /* Inicjalizacja Bluetooth */
    err = bt_enable(NULL);
    if (err) {
        LOG_ERR("Bluetooth init failed (err %d)", err);
        return 0;
    }

    LOG_INF("Bluetooth initialized");

    if (bt_ready()) {
        LOG_ERR("Bluetooth mesh not ready");
        return 0;
    }

    /* LED zapalony na stale = wezel dziala i jest w sieci */
    gpio_pin_set_dt(&led, 1);

    /* Rusz cykliczny zrzut licznikow komunikacji */
    k_work_reschedule(&stat_dump_work, STAT_DUMP_INTERVAL);

    return 0;
}

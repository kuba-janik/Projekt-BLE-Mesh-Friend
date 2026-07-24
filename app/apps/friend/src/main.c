#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/mesh.h>
#include <bluetooth/mesh/models.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <stdint.h>
#include <string.h>

LOG_MODULE_REGISTER(node_friend, LOG_LEVEL_INF);

/* Czyszczenie Replay Protection List */
extern void bt_mesh_rpl_clear(void);

#define LED_NODE	DT_ALIAS(led0)


/* Provisioning na sztywno */
#define NET_IDX		0x0000 		/* Index sieci */
#define APP_IDX		0x0000 		/* Index aplikacji */
#define FRIEND_ADDR	0x0001		/* staly adres Friend */

/* KLUCZE */
static const uint8_t net_key[16] = {
	0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
	0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
};
static const uint8_t app_key[16] = {
	0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0x99, 0x88,
	0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00,
};
/* DevKey */
static const uint8_t dev_key[16] = {
	0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
	0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf,
};

/* UUID */
static const uint8_t dev_uuid[16] = { 0xdd, 0xdd };

/* Wypianie klucza dla Wireshark */
static void log_sniff_key(const char *name, const uint8_t key[16])
{
	char hex[33];

	bin2hex(key, 16, hex, sizeof(hex));
	LOG_INF("  %s: %s", name, hex);
}

static struct bt_mesh_cfg_cli cfg_cli;

static void health_current_status(struct bt_mesh_health_cli *cli, uint16_t addr,
				  uint8_t test_id, uint16_t cid, uint8_t *faults,
				  size_t fault_count)
{
	size_t i;

	LOG_INF("Health Current Status from 0x%04x", addr);

	if (!fault_count) {
		LOG_INF("Health Test ID 0x%02x Company ID 0x%04x: no faults",
		       test_id, cid);
		return;
	}

	LOG_WRN("Health Test ID 0x%02x Company ID 0x%04x Fault Count %zu:",
	       test_id, cid, fault_count);

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
static void sensor_cli_data_cb(struct bt_mesh_sensor_cli *cli,
			       struct bt_mesh_msg_ctx *ctx,
			       const struct bt_mesh_sensor_type *sensor,
			       const struct bt_mesh_sensor_value *value)
{
	if (sensor->id == bt_mesh_sensor_present_amb_temp.id) {
		LOG_INF("Temperatura: %s C", bt_mesh_sensor_ch_str(value));
	}
}

/* Handler na */
static const struct bt_mesh_sensor_cli_handlers sensor_cli_handlers = {
	.data = sensor_cli_data_cb,
};

static struct bt_mesh_sensor_cli sensor_cli =
	BT_MESH_SENSOR_CLI_INIT(&sensor_cli_handlers);

static const struct bt_mesh_model root_models[] = {
	BT_MESH_MODEL_CFG_SRV, /* Configuration Server */
	BT_MESH_MODEL_CFG_CLI(&cfg_cli), /* Configuration Client (loopback self-config) */
	BT_MESH_MODEL_HEALTH_CLI(&health_cli), /* Health Client */
	BT_MESH_MODEL_SENSOR_CLI(&sensor_cli), /* Sensor Client */
};

static const struct bt_mesh_elem elements[] = {
	BT_MESH_ELEM(0, root_models, BT_MESH_MODEL_NONE),
};

/* Kompozycja wezla dla bt_mesh_init. */
static const struct bt_mesh_comp mesh_comp = {
	.cid = BT_COMP_ID_LF, /* Local Fictional Company ID */
	.elem = elements, /* Elementy sieci */
	.elem_count = ARRAY_SIZE(elements), /* Liczba elementow */
};

/* Konfiguracja roli podczas provisioningu */
static const struct bt_mesh_prov prov = {
	.uuid = dev_uuid,
};

/* Callbacki dla Friendship */
static void friend_established(uint16_t net_idx, uint16_t lpn_addr,
			       uint8_t recv_delay, uint32_t polltimeout)
{
	LOG_INF("Friendship z LPN nawiazany");

	k_work_cancel_delayable(&blink_work);
	gpio_pin_set_dt(&led, 1); 
}

static void friend_terminated(uint16_t net_idx, uint16_t lpn_addr)
{
	LOG_WRN("Friendship z LPN 0x%04x zerwana", lpn_addr);

	k_work_reschedule(&blink_work, K_NO_WAIT); 

	/* Wyczysc RPL */
	bt_mesh_rpl_clear();
	LOG_INF("Wyczyszczono RPL");
}

BT_MESH_FRIEND_CB_DEFINE(friend_cb) = {
	.established = friend_established,
	.terminated = friend_terminated,
};

/* Konfiguracja lokalna */
static void configure_self(void)
{
	uint8_t status;
	int err;

	LOG_INF("Konfiguracja lokalna (loopback do 0x%04x)...", FRIEND_ADDR);

	/* Dodaje AppKey do wezla*/
	err = bt_mesh_cfg_cli_app_key_add(NET_IDX, FRIEND_ADDR, NET_IDX, APP_IDX,
					  app_key, &status);
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

	/* Podpina AppKey do Sensor Client; bez tego nie odszyfruje odbieranych danych. */
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

	/* Wypisz klucze do sniffowania */
	LOG_INF("=== KLUCZE DO SNIFFOWANIA (Wireshark), IV index = 0 ===");
	log_sniff_key("NetKey", net_key);
	log_sniff_key("AppKey", app_key);

	/* 
	prov - kim jestem i jak sie provisionuje
	mesh_comp - jakie modele i funkcje udostepniam
	*/
	err = bt_mesh_init(&prov, &mesh_comp);
	if (err) {
		LOG_ERR("Initializing mesh failed (err %d)", err);
		return err;
	}

	LOG_INF("BLE Mesh initialized");

	/* reczny provisioning, sztuczne odtworzenie efektu koncowego normalnego provisioningu */
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
	} else {
		LOG_INF("Friend wlaczony");
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

	/* LED zapalony na stale = wezel dziala i jest w sieci. */
	gpio_pin_set_dt(&led, 1);

	return 0;
}

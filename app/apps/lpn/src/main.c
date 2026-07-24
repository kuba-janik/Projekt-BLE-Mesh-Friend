
#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/mesh.h>
#include <bluetooth/mesh/models.h>
#include <zephyr/logging/log.h>

#include "model_handler.h"

LOG_MODULE_REGISTER(lpn_node, LOG_LEVEL_INF);

/* Provisioning na sztywno */
#define NET_IDX		0x0000
#define APP_IDX		0x0000
#define LPN_ADDR	0x0002		/* staly adres LPN */
#define FRIEND_ADDR	0x0001		/* adres Frienda */

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
	0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7,
	0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf,
};

/* UUID */
static const uint8_t dev_uuid[16] = { 0x1b, 0x7a, 0x0c, 0x54 };

/* Cykliczna publikacja temperatury. Interwal sterowany build-time przez
 * CONFIG_LPN_SENSOR_INTERVAL_S (patrz Kconfig; domyslnie 10 s). Fallback na
 * wypadek builda bez app-level Kconfig - zachowuje dotychczasowe 10 s. */
#ifndef CONFIG_LPN_SENSOR_INTERVAL_S
#define CONFIG_LPN_SENSOR_INTERVAL_S 10
#endif
#define SENSOR_READ_INTERVAL   K_SECONDS(CONFIG_LPN_SENSOR_INTERVAL_S)

static struct k_work_delayable sensor_read_work;

static void sensor_read_work_handler(struct k_work *work)
{
	model_handler_publish_temp();

	/* Przeplanuj siebie -> odczyt i publikacja co SENSOR_READ_INTERVAL. */
	k_work_reschedule(&sensor_read_work, SENSOR_READ_INTERVAL);
}

static const struct bt_mesh_prov prov = {
	.uuid = dev_uuid,
};

/* Callbacki LPN */
static void lpn_established(uint16_t net_idx, uint16_t friend_addr,
			    uint8_t queue_size, uint8_t recv_window)
{
	LOG_INF("Friendship nawiazany z Friendem 0x%04x (queue %u, recv_window %u ms)",
		friend_addr, queue_size, recv_window);

	/* Wysyłanie pomiarów */
	k_work_reschedule(&sensor_read_work, K_NO_WAIT);
}

static void lpn_terminated(uint16_t net_idx, uint16_t friend_addr)
{
	LOG_WRN("Friendship zerwany z Friendem 0x%04x", friend_addr);

	/* Anuluj publikacje pomiarow, w przypadku zerwania friendship */
	k_work_cancel_delayable(&sensor_read_work);
}

static void lpn_polled(uint16_t net_idx, uint16_t friend_addr, bool retry)
{
	/* Logowanie Friend-Poll */
	LOG_DBG("Poll do Frienda 0x%04x%s", friend_addr, retry ? " (retry)" : "");
}

BT_MESH_LPN_CB_DEFINE(lpn_cb) = {
	.established = lpn_established,
	.terminated = lpn_terminated,
	.polled = lpn_polled,
};

/* Konfiguracja lokalna */
static void configure_self(void)
{
	uint8_t status;
	int err;

	LOG_INF("Konfiguracja lokalna (loopback do 0x%04x)...", LPN_ADDR);

	err = bt_mesh_cfg_cli_app_key_add(NET_IDX, LPN_ADDR, NET_IDX, APP_IDX,
					  app_key, &status);
	if (err || status) {
		LOG_ERR("Dodanie AppKey nieudane (err %d, status %d)", err, status);
		return;
	}

	/* Bind AppKey do modeli serwerowych. */
	const uint16_t bind_ids[] = {
		BT_MESH_MODEL_ID_SENSOR_SRV,
		BT_MESH_MODEL_ID_HEALTH_SRV,
	};

	for (size_t i = 0; i < ARRAY_SIZE(bind_ids); i++) {
		err = bt_mesh_cfg_cli_mod_app_bind(NET_IDX, LPN_ADDR, LPN_ADDR, APP_IDX,
						   bind_ids[i], &status);
		if (err || status) {
			LOG_ERR("Bind modelu 0x%04x nieudany (err %d, status %d)",
				bind_ids[i], err, status);
		}
	}

	/* Ustaw publikacje Sensor Servera na Frienda */
	struct bt_mesh_cfg_cli_mod_pub pub = {
		.addr = FRIEND_ADDR,
		.app_idx = APP_IDX,
		.ttl = 7,
		.period = 0,
		.transmit = BT_MESH_TRANSMIT(0, 30),
	};

	err = bt_mesh_cfg_cli_mod_pub_set(NET_IDX, LPN_ADDR, LPN_ADDR,
					  BT_MESH_MODEL_ID_SENSOR_SRV, &pub, &status);
	if (err || status) {
		LOG_ERR("Ustawienie publikacji Sensor Srv nieudane (err %d, status %d)",
			err, status);
		return;
	}

	LOG_INF("Publikacja Sensor Server -> 0x%04x", FRIEND_ADDR);
	LOG_INF("Konfiguracja zakonczona");
}

int main(void)
{
	int err;

	LOG_INF("Start wezla LPN (BTZ_EndDevice)");

	/* Inicjalizacja Bluetooth */
	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("bt_enable nieudany (err %d)", err);
		return 1;
	}

	LOG_INF("Bluetooth zainicjalizowany");

	/* Inicjalizacja worku publikacji */
	k_work_init_delayable(&sensor_read_work, sensor_read_work_handler);

	err = bt_mesh_init(&prov, model_handler_init());
	if (err) {
		LOG_ERR("Inicjalizacja mesh nieudana (err %d)", err);
		return 1;
	}

	/* Provisioning na sztywno */
	err = bt_mesh_provision(net_key, NET_IDX, 0, 0, LPN_ADDR, dev_key);
	if (err) {
		LOG_ERR("Self-provisioning nieudany (err %d)", err);
		return 1;
	}
	LOG_INF("Self-provisioned - jestem w sieci @ 0x%04x", LPN_ADDR);

	/* Lokalna konfiguracja */
	configure_self();

	/* Wlacz tryb LPN */
	err = bt_mesh_lpn_set(true);
	if (err) {
		LOG_ERR("Wlaczenie LPN nieudane (err %d)", err);
	} else {
		LOG_INF("Tryb LPN wlaczony - szukam Frienda");
	}

	return 0;
}

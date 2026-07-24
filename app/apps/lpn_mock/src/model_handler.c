#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/mesh.h>
#include <bluetooth/mesh/models.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include "model_handler.h"

LOG_MODULE_DECLARE(lpn_mock);


/* Config Client */
static struct bt_mesh_cfg_cli cfg_cli;

/* Health Server */
static const struct bt_mesh_health_srv_cb health_srv_cb;   /* puste callbacki */
static struct bt_mesh_health_srv health_srv = {
	.cb = &health_srv_cb,
};

BT_MESH_HEALTH_PUB_DEFINE(health_pub, 0);

/* Sensor Server (temperatura STS4X) */
#define MOCK_TEMP_MICRO_C 22500000   /* 22.5 C */

/* Fake wartosc w formacie mesh Present Ambient Temperature. */
static int temp_sample(struct bt_mesh_sensor_value *rsp)
{
	/* Konwersja wartosci czujnika do formatu Mesh. */
	int err = bt_mesh_sensor_value_from_micro(
		bt_mesh_sensor_present_amb_temp.channels[0].format,
		MOCK_TEMP_MICRO_C, rsp);
	if (err && err != -ERANGE) {   /* -ERANGE = przycieto do zakresu, OK */
		LOG_ERR("Kodowanie wartosci nieudane (err %d)", err);
		return err;
	}

	return 0;
}

/* Callback modelu: wolany, gdy klient wysle Sensor Get (odpytanie). */
static int temp_get(struct bt_mesh_sensor_srv *srv, struct bt_mesh_sensor *sensor,
		    struct bt_mesh_msg_ctx *ctx, struct bt_mesh_sensor_value *rsp)
{
	int err = temp_sample(rsp);

	if (!err) {
		LOG_INF("Sensor Get -> temperatura: %s C", bt_mesh_sensor_ch_str(rsp));
	}
	return err;
}

static struct bt_mesh_sensor temp_sensor = {
	.type = &bt_mesh_sensor_present_amb_temp,
	.get = temp_get,
};

static struct bt_mesh_sensor *const sensors[] = {
	&temp_sensor,
};

static struct bt_mesh_sensor_srv sensor_srv =
	BT_MESH_SENSOR_SRV_INIT(sensors, ARRAY_SIZE(sensors));

/* Kompozycja wezla */
static const struct bt_mesh_elem elements[] = {
	BT_MESH_ELEM(0,
		BT_MESH_MODEL_LIST(
			BT_MESH_MODEL_CFG_SRV,
			BT_MESH_MODEL_CFG_CLI(&cfg_cli),
			BT_MESH_MODEL_HEALTH_SRV(&health_srv, &health_pub),
			BT_MESH_MODEL_SENSOR_SRV(&sensor_srv)),
		BT_MESH_MODEL_NONE),
};

static const struct bt_mesh_comp comp = {
	.cid = BT_COMP_ID_LF,
	.elem = elements,
	.elem_count = ARRAY_SIZE(elements),
};




int model_handler_publish_temp(void)
{
	struct bt_mesh_sensor_value val;
	int err;

	err = temp_sample(&val);
	if (err) {
		return err;
	}

	LOG_INF("Temperatura: %s C", bt_mesh_sensor_ch_str(&val));

	/* Publikacja wartosci czujnika. */
	err = bt_mesh_sensor_srv_pub(&sensor_srv, NULL, &temp_sensor, &val);
	if (err) {
		LOG_DBG("Publikacja pominieta (err %d) - brak adresu publikacji?", err);
	}

	return err;
}

const struct bt_mesh_comp *model_handler_init(void)
{
	return &comp;
}

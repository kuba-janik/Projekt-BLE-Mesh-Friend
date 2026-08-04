#include "model_handler.h"

#include <bluetooth/mesh/models.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/mesh.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(lpn_mock);

/* Health Server */
static const struct bt_mesh_health_srv_cb health_srv_cb; /* puste callbacki */
static struct bt_mesh_health_srv health_srv = {
    .cb = &health_srv_cb,
};

BT_MESH_HEALTH_PUB_DEFINE(health_pub, 0);

/* Sensor Server wersja MOCK: bez STS4X i bez bramki zasilania, stala temperatura */
#define MOCK_TEMP_MICRO_C 22500000 /* 22.5 C */

/* Zwraca sztuczna wartosc w formacie mesh Present Ambient Temperature */
static int temp_sample(struct bt_mesh_sensor_value *rsp)
{
    int err = bt_mesh_sensor_value_from_micro(bt_mesh_sensor_present_amb_temp.channels[0].format,
                                              MOCK_TEMP_MICRO_C, rsp);
    if (err == -ERANGE) {
        err = 0; /* -ERANGE = przycieto do zakresu, OK */
    }
    else if (err) {
        LOG_ERR("Kodowanie wartosci nieudane (err %d)", err);
    }

    return err;
}

/* Callback modelu: wolany, gdy klient wysle Sensor Get */
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

static struct bt_mesh_sensor_srv sensor_srv = BT_MESH_SENSOR_SRV_INIT(sensors, ARRAY_SIZE(sensors));

/* Kompozycja wezla - identyczna jak w apps/lpn */
static const struct bt_mesh_elem elements[] = {
    BT_MESH_ELEM(0,
                 BT_MESH_MODEL_LIST(BT_MESH_MODEL_CFG_SRV,
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

    /* Publikacja wartosci czujnika do Frienda */
    err = bt_mesh_sensor_srv_pub(&sensor_srv, NULL, &temp_sensor, &val);
    if (err) {
        LOG_DBG("Publikacja pominieta (err %d) - brak adresu publikacji?", err);
    }

    return err;
}

bool model_handler_is_configured(void)
{
    /* Adres publikacji ustawia ostatni krok konfiguracji Frienda (mod_pub_set) */
    return sensor_srv.pub.addr != BT_MESH_ADDR_UNASSIGNED;
}

const struct bt_mesh_comp *model_handler_init(void)
{
    /* Wersja mock - brak czujnika do inicjalizacji */
    return &comp;
}

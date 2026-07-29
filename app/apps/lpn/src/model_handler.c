#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/mesh.h>
#include <bluetooth/mesh/models.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include "model_handler.h"

LOG_MODULE_DECLARE(lpn_node);

  /* Health Server */
  static const struct bt_mesh_health_srv_cb health_srv_cb;   /* puste callbacki */
  static struct bt_mesh_health_srv health_srv = {
        .cb = &health_srv_cb,
  };

  BT_MESH_HEALTH_PUB_DEFINE(health_pub, 0);

  /* Sensor Server (temperatura STS4X) */
  static const struct device *const sensor_dev = DEVICE_DT_GET(DT_NODELABEL(sts4x));

  /* Bramka zasilania czujnikow, włączona tylko na czas pomiaru */
  static const struct device *const sensor_pwr = DEVICE_DT_GET(DT_NODELABEL(sensor_pwr));
  static bool sensor_pwr_ready;

  /* Czas na power-up szyny i reset STS4X przed pomiarem. */
  #define STS4X_POWERUP_MS 2

  /* Odczyt z STS4X i zakodowanie do struktury wartosci czujnika mesh. */
  static int temp_sample(struct bt_mesh_sensor_value *rsp)
  {
        struct sensor_value val;
        int err;

        /* Zasilamy szyne czujnikow tylko na czas odczytu. */
        if (sensor_pwr_ready) {
                err = regulator_enable(sensor_pwr);
                if (err) {
                        LOG_ERR("Zalaczenie sensor_pwr nieudane (err %d)", err);
                        return err;
                }
                k_msleep(STS4X_POWERUP_MS);   /* power-up rail + reset STS4X */
        }

        err = sensor_sample_fetch(sensor_dev);
        if (err) {
                LOG_ERR("sensor_sample_fetch nieudany (err %d)", err);
                goto power_off;
        }

        err = sensor_channel_get(sensor_dev, SENSOR_CHAN_AMBIENT_TEMP, &val);
        if (err) {
                LOG_ERR("sensor_channel_get nieudany (err %d)", err);
                goto power_off;
        }

        /* Konwersja wartosci czujnika do formatu Mesh. */
        err = bt_mesh_sensor_value_from_sensor_value(
                bt_mesh_sensor_present_amb_temp.channels[0].format, &val, rsp);
        if (err == -ERANGE) {
                err = 0;   /* -ERANGE = przycieto do zakresu, OK */
        } else if (err) {
                LOG_ERR("Kodowanie wartosci nieudane (err %d)", err);
        }

  power_off:
        /* Wylaczamy zasilanie czujnika. */
        if (sensor_pwr_ready) {
                int derr = regulator_disable(sensor_pwr);
                if (derr) {
                        LOG_ERR("Wylaczenie sensor_pwr nieudane (err %d)", derr);
                }
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

  static struct bt_mesh_sensor_srv sensor_srv =
        BT_MESH_SENSOR_SRV_INIT(sensors, ARRAY_SIZE(sensors));

  /* Kompozycja wezla */
  static const struct bt_mesh_elem elements[] = {
        BT_MESH_ELEM(0,
                BT_MESH_MODEL_LIST(
                        BT_MESH_MODEL_CFG_SRV,
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

bool model_handler_is_configured(void)
{
	/* mod_pub_set (ostatni krok konfiguracji Frienda) ustawia adres publikacji
	 * Sensor Servera. Dopoki jest UNASSIGNED, konfiguracja nie jest kompletna. */
	return sensor_srv.pub.addr != BT_MESH_ADDR_UNASSIGNED;
}

const struct bt_mesh_comp *model_handler_init(void)
{
	if (!device_is_ready(sensor_dev)) {
		LOG_ERR("STS4X niegotowy - Sensor Server nie bedzie mial danych");
	}

	/* Wylaczamy zasilanie czujnika po inicjalizacji. */
	sensor_pwr_ready = device_is_ready(sensor_pwr);
	if (sensor_pwr_ready) {
		int err = regulator_disable(sensor_pwr);
		if (err) {
			LOG_ERR("Wylaczenie sensor_pwr po init nieudane (err %d)", err);
		} else {
			LOG_INF("sensor_pwr OFF - budzony tylko na czas odczytu STS4X");
		}
	} else {
		LOG_WRN("sensor_pwr niegotowy - szyna zostanie na stale");
	}

	return &comp;
}

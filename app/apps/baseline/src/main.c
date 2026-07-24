/*
 * Baseline: pomiar floora pradu (bez BLE Mesh.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

/* Szyna zasilania czujnikow (sensor_pwr, P0.00, active-low).
 *   1 = WLACZONA
 *   0 = WYLACZONA
*/
#define SENSOR_RAIL_ON 0

/* blysk LED na starcie */
#define STARTUP_BLINK 0

static const struct gpio_dt_spec sensor_pwr =
	GPIO_DT_SPEC_GET(DT_NODELABEL(sensor_pwr), enable_gpios);

#if STARTUP_BLINK
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
#endif

int main(void)
{
	/* Ustaw szyne czujnikow */
	if (gpio_is_ready_dt(&sensor_pwr)) {
		gpio_pin_configure_dt(&sensor_pwr,
			SENSOR_RAIL_ON ? GPIO_OUTPUT_ACTIVE : GPIO_OUTPUT_INACTIVE);
	}

#if STARTUP_BLINK
	if (gpio_is_ready_dt(&led)) {
		gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
		for (int i = 0; i < 6; i++) {
			gpio_pin_toggle_dt(&led);
			k_sleep(K_MSEC(120));
		}
		/* Odlacz pin LED */
		gpio_pin_configure_dt(&led, GPIO_DISCONNECTED);
	}
#endif

	/* Oddajemy procesor watkowi idle (WFI / tickless).*/
	k_sleep(K_FOREVER);
	return 0;
}

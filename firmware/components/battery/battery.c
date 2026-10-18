#include "max17043.h"
#include "max17043_config.h"

#include "freertos/FreeRTOS.h"
#include "battery.h"
#include "esp_check.h"
#include "freertos/queue.h"
#include "sdkconfig.h"

static const char *TAG = "MAX17043";

// The gauge refines SOC continuously; this is just how often we look.
#define BATTERY_POLL_MS 5000

typedef struct {
	max17043_handle_t device;
	QueueHandle_t queue;
} battery_callback_data_t;

// No sleep/wake and no quick start here. The gauge only tracks charge while
// awake (~50 uA, nothing worth saving), and a quick start throws away what
// it has learned and re-guesses SOC from the loaded cell voltage. Also, the
// v1.1.1 driver's sleep/wake write a single byte at CONFIG, i.e. RCOMP rather
// than the SLEEP bit, which skews SOC instead of sleeping.
static void max17043_read_task(void *arg0)
{
	battery_callback_data_t *arg = (battery_callback_data_t *)arg0;
	max17043_handle_t device = arg->device;

	while (1) {
		// A FreeRTOS task must never return, so read errors are logged by
		// the driver and simply retried on the next poll.
		uint8_t soc = 0;
		if (max17043_read_soc(device, &soc) == ESP_OK) {
			// SOC can read slightly above 100 on a full cell.
			if (soc > 100) {
				soc = 100;
			}
			xQueueOverwrite(arg->queue, &soc);
		}

		uint16_t voltage;
		if (max17043_read_voltage(device, &voltage) == ESP_OK) {
			ESP_LOGD(TAG, "SOC %u%%, %u mV", soc, voltage);
		}

		vTaskDelay(pdMS_TO_TICKS(BATTERY_POLL_MS));
	}
}

esp_err_t battery_sensor_init(QueueHandle_t *q) {
	QueueHandle_t queue = NULL;
	max17043_handle_t sensor_handle = NULL;

	ESP_LOGI(TAG, "Starting MAX17043");

#ifdef CONFIG_PACER_LCD_BL_GPIO
	// The component's default SCL is GPIO1, which is the LCD backlight on
	// this board; claiming it for I2C silently kills the backlight PWM.
	if (MAX17043_I2C_SDA_PIN == CONFIG_PACER_LCD_BL_GPIO ||
	    MAX17043_I2C_SCL_PIN == CONFIG_PACER_LCD_BL_GPIO) {
		ESP_LOGE(TAG, "SDA/SCL (%d/%d) clash with the backlight on GPIO%d; "
			 "set MAX17043_I2C_SDA/SCL in menuconfig",
			 MAX17043_I2C_SDA_PIN, MAX17043_I2C_SCL_PIN,
			 CONFIG_PACER_LCD_BL_GPIO);
		return ESP_ERR_INVALID_ARG;
	}
#endif

	// Any free port, not MAX17043_I2C_PORT: the touch panel already holds
	// port 0, and asking for a taken port fails rather than sharing it.
	i2c_master_bus_config_t i2c_bus2_config = {
		.clk_source = I2C_CLK_SRC_DEFAULT,
		.glitch_ignore_cnt = 7,
		.i2c_port = -1,
		.sda_io_num = MAX17043_I2C_SDA_PIN,
		.scl_io_num = MAX17043_I2C_SCL_PIN,
		.flags.enable_internal_pullup = true,
	};

	esp_err_t err = ESP_OK;
	i2c_master_bus_handle_t i2c_bus;
	err = i2c_new_master_bus(&i2c_bus2_config, &i2c_bus);
	ESP_RETURN_ON_ERROR(err, TAG, "Failed to initialize I2C bus (SDA %d, SCL %d)",
			    MAX17043_I2C_SDA_PIN, MAX17043_I2C_SCL_PIN);

	/* Probe the sensor first to check if it is connected to the bus */
	ESP_RETURN_ON_ERROR(i2c_master_probe(i2c_bus, MAX17043_I2C_ADDRESS, 200),
		TAG, "MAX17043 not found at address 0x%02X", MAX17043_I2C_ADDRESS);

	ESP_LOGI(TAG, "MAX17043 detected on I2C bus");

	queue = xQueueCreate(1, sizeof(uint8_t));
	if (!queue) {
		ESP_LOGE(TAG, "Failed to create queue");
		return ESP_ERR_INVALID_STATE;
	}

	sensor_handle = max17043_create(i2c_bus, MAX17043_I2C_ADDRESS);
	if (!sensor_handle) {
		ESP_LOGE(TAG, "Failed to create MAX17043 device");
		return ESP_ERR_INVALID_STATE;
	}

	ESP_LOGI(TAG, "MAX17043 device created successfully");

	battery_callback_data_t *args = (battery_callback_data_t *)malloc(sizeof(battery_callback_data_t));
	args->device = sensor_handle;
	args->queue = queue;

	/* Create sensor reader task */
	if (pdPASS != xTaskCreate(
		max17043_read_task,
		"max17043_read_task",
		4096,
		args,
		5,
		NULL
	)) {
		ESP_LOGE(TAG, "Failed to create sensor reader task");
		vQueueDelete(queue);
		max17043_delete(sensor_handle);
		return ESP_ERR_INVALID_STATE;
	}

	*q = queue;

	return err;
}

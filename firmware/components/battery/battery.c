#include "max17043.h"
#include "max17043_config.h"

#include "freertos/FreeRTOS.h"
#include "battery.h"
#include "esp_check.h"
#include "freertos/queue.h"

static const char *TAG = "MAX17043";

typedef struct {
	max17043_handle_t device;
	QueueHandle_t queue;
} battery_callback_data_t;

static void max17043_read_task(void *arg0)
{
	battery_callback_data_t *arg = (battery_callback_data_t *)arg0;
	max17043_handle_t device = arg->device;

	while (1) {
		esp_err_t ret = ESP_OK;

		// wakeup
		ESP_LOGI(TAG, "Waking up...");
		ret = max17043_wake(device);
		ESP_RETURN_VOID_ON_ERROR(ret, TAG, "Failed to wake device: %s", esp_err_to_name(ret));
		ESP_LOGI(TAG, "✓ Device awake");

		// quick start
		ESP_LOGI(TAG, "Performing quick start...");
		ret = max17043_quick_start(device);
		ESP_RETURN_VOID_ON_ERROR(ret, TAG, "Failed to quick start: %s", esp_err_to_name(ret));
		ESP_LOGI(TAG, "✓ Quick start complete");

		// give it some time
		vTaskDelay(pdMS_TO_TICKS(1000));

		uint8_t soc;
		ret = max17043_read_soc(device, &soc);
		if (ret == ESP_OK) {
			xQueueOverwrite(arg->queue, &soc);
		}

		uint16_t voltage;
		ret = max17043_read_voltage(device, &voltage);
		if (ret == ESP_OK) {
			ESP_LOGI(TAG, "Voltage: %u mV", voltage);
		}

		// back to sleep
		ESP_LOGI(TAG, "Entering sleep mode...");
		ret = max17043_sleep(device);
		ESP_RETURN_VOID_ON_ERROR(ret, TAG, "Failed to sleep: %s", esp_err_to_name(ret));
		ESP_LOGI(TAG, "✓ Device sleeping");

		vTaskDelay(pdMS_TO_TICKS(3000));
	}
}

esp_err_t battery_sensor_init(QueueHandle_t *q) {
	QueueHandle_t queue = NULL;
	max17043_handle_t sensor_handle = NULL;

	ESP_LOGI(TAG, "Starting MAX17043");

	ESP_LOGI(TAG, "Initialize I2C bus port %d", MAX17043_I2C_PORT);
	i2c_master_bus_config_t i2c_bus2_config = {
		.clk_source = I2C_CLK_SRC_DEFAULT,
		.glitch_ignore_cnt = 7,
		.i2c_port = MAX17043_I2C_PORT,
		.sda_io_num = MAX17043_I2C_SDA_PIN,
		.scl_io_num = MAX17043_I2C_SCL_PIN,
		.flags.enable_internal_pullup = true,
	};

	esp_err_t err = ESP_OK;
	i2c_master_bus_handle_t i2c_bus;
	err = i2c_new_master_bus(&i2c_bus2_config, &i2c_bus);
	ESP_RETURN_ON_ERROR(err, TAG, "Failed to initialize I2C bus port %d", MAX17043_I2C_PORT);

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

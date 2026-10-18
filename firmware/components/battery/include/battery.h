#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_err.h"

#if __cplusplus
extern "C" {
#endif
esp_err_t battery_sensor_init(QueueHandle_t *q);
#if __cplusplus
}
#endif
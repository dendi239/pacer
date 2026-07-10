#include <stdlib.h>
#include <sys/cdefs.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_commands.h"
#include "esp_log.h"

#include "esp_lcd_nv3041a.h"

#define LCD_OPCODE_WRITE_CMD        (0x02ULL)
#define LCD_OPCODE_READ_CMD         (0x03ULL)
#define LCD_OPCODE_WRITE_COLOR      (0x32ULL)

static const char *TAG = "nv3041a";

static esp_err_t panel_nv3041a_del(esp_lcd_panel_t *panel);
static esp_err_t panel_nv3041a_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_nv3041a_init(esp_lcd_panel_t *panel);
static esp_err_t panel_nv3041a_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data);
static esp_err_t panel_nv3041a_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_nv3041a_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_nv3041a_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t panel_nv3041a_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t panel_nv3041a_disp_on_off(esp_lcd_panel_t *panel, bool off);

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    int x_gap;
    int y_gap;
    uint8_t fb_bits_per_pixel;
    uint8_t madctl_val; // save current value of LCD_CMD_MADCTL register
    uint8_t colmod_val; // save surrent value of LCD_CMD_COLMOD register
    const nv3041a_lcd_init_cmd_t *init_cmds;
    uint16_t init_cmds_size;
    struct {
        unsigned int use_qspi_interface: 1;
        unsigned int reset_level: 1;
    } flags;
} nv3041a_panel_t;

esp_err_t esp_lcd_new_panel_nv3041a(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config, esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, TAG, "invalid argument");

    esp_err_t ret = ESP_OK;
    nv3041a_panel_t *nv3041a = NULL;
    nv3041a = calloc(1, sizeof(nv3041a_panel_t));
    ESP_GOTO_ON_FALSE(nv3041a, ESP_ERR_NO_MEM, err, TAG, "no mem for nv3041a panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        gpio_config_t io_conf = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configure GPIO for RST line failed");
    }

    switch (panel_dev_config->rgb_ele_order) {
    case LCD_RGB_ELEMENT_ORDER_RGB:
        nv3041a->madctl_val = 0;
        break;
    case LCD_RGB_ELEMENT_ORDER_BGR:
        nv3041a->madctl_val |= LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported color element order");
        break;
    }

    switch (panel_dev_config->bits_per_pixel) {
    case 16: // RGB565
        nv3041a->colmod_val = 0x55;
        nv3041a->fb_bits_per_pixel = 16;
        break;
    case 18: // RGB666
        nv3041a->colmod_val = 0x66;
        // each color component (R/G/B) should occupy the 6 high bits of a byte, which means 3 full bytes are required for a pixel
        nv3041a->fb_bits_per_pixel = 24;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported pixel width");
        break;
    }

    nv3041a->io = io;
    nv3041a->reset_gpio_num = panel_dev_config->reset_gpio_num;
    nv3041a->flags.reset_level = panel_dev_config->flags.reset_active_high;
    nv3041a_vendor_config_t *vendor_config = (nv3041a_vendor_config_t *)panel_dev_config->vendor_config;
    if (vendor_config) {
        nv3041a->init_cmds = vendor_config->init_cmds;
        nv3041a->init_cmds_size = vendor_config->init_cmds_size;
        nv3041a->flags.use_qspi_interface = vendor_config->flags.use_qspi_interface;
    }
    nv3041a->base.del = panel_nv3041a_del;
    nv3041a->base.reset = panel_nv3041a_reset;
    nv3041a->base.init = panel_nv3041a_init;
    nv3041a->base.draw_bitmap = panel_nv3041a_draw_bitmap;
    nv3041a->base.invert_color = panel_nv3041a_invert_color;
    nv3041a->base.set_gap = panel_nv3041a_set_gap;
    nv3041a->base.mirror = panel_nv3041a_mirror;
    nv3041a->base.swap_xy = panel_nv3041a_swap_xy;
    nv3041a->base.disp_on_off = panel_nv3041a_disp_on_off;
    *ret_panel = &(nv3041a->base);
    ESP_LOGD(TAG, "new nv3041a panel @%p", nv3041a);

    return ESP_OK;

err:
    if (nv3041a) {
        if (panel_dev_config->reset_gpio_num >= 0) {
            gpio_reset_pin(panel_dev_config->reset_gpio_num);
        }
        free(nv3041a);
    }
    return ret;
}

static esp_err_t tx_param(nv3041a_panel_t *nv3041a, esp_lcd_panel_io_handle_t io, int lcd_cmd, const void *param, size_t param_size)
{
    if (nv3041a->flags.use_qspi_interface) {
        lcd_cmd &= 0xff;
        lcd_cmd <<= 8;
        lcd_cmd |= LCD_OPCODE_WRITE_CMD << 24;
    }
    return esp_lcd_panel_io_tx_param(io, lcd_cmd, param, param_size);
}

static esp_err_t tx_color(nv3041a_panel_t *nv3041a, esp_lcd_panel_io_handle_t io, int lcd_cmd, const void *param, size_t param_size)
{
    if (nv3041a->flags.use_qspi_interface) {
        lcd_cmd &= 0xff;
        lcd_cmd <<= 8;
        lcd_cmd |= LCD_OPCODE_WRITE_COLOR << 24;
    }
    return esp_lcd_panel_io_tx_color(io, lcd_cmd, param, param_size);
}

static esp_err_t panel_nv3041a_del(esp_lcd_panel_t *panel)
{
    nv3041a_panel_t *nv3041a = __containerof(panel, nv3041a_panel_t, base);

    if (nv3041a->reset_gpio_num >= 0) {
        gpio_reset_pin(nv3041a->reset_gpio_num);
    }
    ESP_LOGD(TAG, "del nv3041a panel @%p", nv3041a);
    free(nv3041a);
    return ESP_OK;
}

static esp_err_t panel_nv3041a_reset(esp_lcd_panel_t *panel)
{
    nv3041a_panel_t *nv3041a = __containerof(panel, nv3041a_panel_t, base);
    esp_lcd_panel_io_handle_t io = nv3041a->io;

    // Perform hardware reset
    if (nv3041a->reset_gpio_num >= 0) {
        gpio_set_level(nv3041a->reset_gpio_num, nv3041a->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(nv3041a->reset_gpio_num, !nv3041a->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(120));
    } else { // Perform software reset
        ESP_RETURN_ON_ERROR(tx_param(nv3041a, io, LCD_CMD_SWRESET, NULL, 0), TAG, "send command failed");
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    return ESP_OK;
}

static const nv3041a_lcd_init_cmd_t vendor_specific_init_default[] = {
    {0xff, (uint8_t []){0xa5}, 1, 0},
    {0xE7, (uint8_t []){0x10}, 1, 0},
    // TEOFF: the board routes the panel's TE line to GPIO0 (boot strap); with TE
    // enabled the panel holds it low across a warm reset and the chip enters
    // ROM download mode. Sent actively to clear TE latched on from a prior run.
    {0x34, (uint8_t []){0x00}, 0, 0},
    {0x36, (uint8_t []){0xc0}, 1, 0},
    {0x3A, (uint8_t []){0x01}, 1, 0}, // 01---565，00---666
    {0x40, (uint8_t []){0x01}, 1, 0},
    {0x41, (uint8_t []){0x03}, 1, 0}, // 01--8bit, 03-16bit
    {0x44, (uint8_t []){0x15}, 1, 0},
    {0x45, (uint8_t []){0x15}, 1, 0},
    {0x7d, (uint8_t []){0x03}, 1, 0},
    {0xc1, (uint8_t []){0xbb}, 1, 0},
    {0xc2, (uint8_t []){0x05}, 1, 0},
    {0xc3, (uint8_t []){0x10}, 1, 0},
    {0xc6, (uint8_t []){0x3e}, 1, 0},
    {0xc7, (uint8_t []){0x25}, 1, 0},
    {0xc8, (uint8_t []){0x11}, 1, 0},
    {0x7a, (uint8_t []){0x5f}, 1, 0},
    {0x6f, (uint8_t []){0x44}, 1, 0},
    {0x78, (uint8_t []){0x70}, 1, 0},
    {0xc9, (uint8_t []){0x00}, 1, 0},
    {0x67, (uint8_t []){0x21}, 1, 0},

    {0x51, (uint8_t []){0x0a}, 1, 0},
    {0x52, (uint8_t []){0x76}, 1, 0},
    {0x53, (uint8_t []){0x0a}, 1, 0},
    {0x54, (uint8_t []){0x76}, 1, 0},

    {0x46, (uint8_t []){0x0a}, 1, 0},
    {0x47, (uint8_t []){0x2a}, 1, 0},
    {0x48, (uint8_t []){0x0a}, 1, 0},
    {0x49, (uint8_t []){0x1a}, 1, 0},
    {0x56, (uint8_t []){0x43}, 1, 0},
    {0x57, (uint8_t []){0x42}, 1, 0},
    {0x58, (uint8_t []){0x3c}, 1, 0},
    {0x59, (uint8_t []){0x64}, 1, 0},
    {0x5a, (uint8_t []){0x41}, 1, 0},
    {0x5b, (uint8_t []){0x3c}, 1, 0},
    {0x5c, (uint8_t []){0x02}, 1, 0},
    {0x5d, (uint8_t []){0x3c}, 1, 0},
    {0x5e, (uint8_t []){0x1f}, 1, 0},
    {0x60, (uint8_t []){0x80}, 1, 0},
    {0x61, (uint8_t []){0x3f}, 1, 0},
    {0x62, (uint8_t []){0x21}, 1, 0},
    {0x63, (uint8_t []){0x07}, 1, 0},
    {0x64, (uint8_t []){0xe0}, 1, 0},
    {0x65, (uint8_t []){0x02}, 1, 0},
    {0xca, (uint8_t []){0x20}, 1, 0},
    {0xcb, (uint8_t []){0x52}, 1, 0},
    {0xcc, (uint8_t []){0x10}, 1, 0},
    {0xcD, (uint8_t []){0x42}, 1, 0},

    {0xD0, (uint8_t []){0x20}, 1, 0},
    {0xD1, (uint8_t []){0x52}, 1, 0},
    {0xD2, (uint8_t []){0x10}, 1, 0},
    {0xD3, (uint8_t []){0x42}, 1, 0},
    {0xD4, (uint8_t []){0x0a}, 1, 0},
    {0xD5, (uint8_t []){0x32}, 1, 0},

    {0xf8, (uint8_t []){0x03}, 1, 0},
    {0xf9, (uint8_t []){0x20}, 1, 0},

    {0x80, (uint8_t []){0x00}, 1, 0},
    {0xA0, (uint8_t []){0x00}, 1, 0},

    {0x81, (uint8_t []){0x07}, 1, 0},
    {0xA1, (uint8_t []){0x06}, 1, 0},

    {0x82, (uint8_t []){0x02}, 1, 0},
    {0xA2, (uint8_t []){0x01}, 1, 0},

    {0x86, (uint8_t []){0x11}, 1, 0},
    {0xA6, (uint8_t []){0x10}, 1, 0},

    {0x87, (uint8_t []){0x27}, 1, 0},
    {0xA7, (uint8_t []){0x27}, 1, 0},

    {0x83, (uint8_t []){0x37}, 1, 0},
    {0xA3, (uint8_t []){0x37}, 1, 0},

    {0x84, (uint8_t []){0x35}, 1, 0},
    {0xA4, (uint8_t []){0x35}, 1, 0},

    {0x85, (uint8_t []){0x3f}, 1, 0},
    {0xA5, (uint8_t []){0x3f}, 1, 0},

    {0x88, (uint8_t []){0x0b}, 1, 0},
    {0xA8, (uint8_t []){0x0b}, 1, 0},

    {0x89, (uint8_t []){0x14}, 1, 0},
    {0xA9, (uint8_t []){0x14}, 1, 0},

    {0x8a, (uint8_t []){0x1a}, 1, 0},
    {0xAa, (uint8_t []){0x1a}, 1, 0},

    {0x8b, (uint8_t []){0x0a}, 1, 0},
    {0xAb, (uint8_t []){0x0a}, 1, 0},

    {0x8c, (uint8_t []){0x14}, 1, 0},
    {0xAc, (uint8_t []){0x08}, 1, 0},

    {0x8d, (uint8_t []){0x17}, 1, 0},
    {0xAd, (uint8_t []){0x07}, 1, 0},

    {0x8e, (uint8_t []){0x16}, 1, 0},
    {0xAe, (uint8_t []){0x06}, 1, 0},

    {0x8f, (uint8_t []){0x1B}, 1, 0},
    {0xAf, (uint8_t []){0x07}, 1, 0},

    {0x90, (uint8_t []){0x04}, 1, 0},
    {0xB0, (uint8_t []){0x04}, 1, 0},

    {0x91, (uint8_t []){0x0A}, 1, 0},
    {0xB1, (uint8_t []){0x0A}, 1, 0},

    {0x92, (uint8_t []){0x16}, 1, 0},
    {0xB2, (uint8_t []){0x15}, 1, 0},

    {0xff, (uint8_t []){0x00}, 1, 0},
    {0x11, (uint8_t []){0x00}, 1, 700},
    {0x29, (uint8_t []){0x00}, 1, 100},
};

static esp_err_t panel_nv3041a_init(esp_lcd_panel_t *panel)
{
    nv3041a_panel_t *nv3041a = __containerof(panel, nv3041a_panel_t, base);
    esp_lcd_panel_io_handle_t io = nv3041a->io;
    const nv3041a_lcd_init_cmd_t *init_cmds = NULL;
    uint16_t init_cmds_size = 0;
    bool is_user_set = true;
    bool is_cmd_overwritten = false;

    ESP_RETURN_ON_ERROR(tx_param(nv3041a, io, LCD_CMD_MADCTL, (uint8_t[]) {
        nv3041a->madctl_val,
    }, 1), TAG, "send command failed");
    ESP_RETURN_ON_ERROR(tx_param(nv3041a, io, LCD_CMD_COLMOD, (uint8_t[]) {
        nv3041a->colmod_val,
    }, 1), TAG, "send command failed");

    // vendor specific initialization, it can be different between manufacturers
    // should consult the LCD supplier for initialization sequence code
    if (nv3041a->init_cmds) {
        init_cmds = nv3041a->init_cmds;
        init_cmds_size = nv3041a->init_cmds_size;
    } else {
        init_cmds = vendor_specific_init_default;
        init_cmds_size = sizeof(vendor_specific_init_default) / sizeof(nv3041a_lcd_init_cmd_t);
    }

    for (int i = 0; i < init_cmds_size; i++) {
        // Check if the command has been used or conflicts with the internal
        if (is_user_set && (init_cmds[i].data_bytes > 0)) {
            switch (init_cmds[i].cmd) {
            case LCD_CMD_MADCTL:
                is_cmd_overwritten = true;
                nv3041a->madctl_val = ((uint8_t *)init_cmds[i].data)[0];
                break;
            case LCD_CMD_COLMOD:
                is_cmd_overwritten = true;
                nv3041a->colmod_val = ((uint8_t *)init_cmds[i].data)[0];
                break;
            default:
                is_cmd_overwritten = false;
                break;
            }

            if (is_cmd_overwritten) {
                is_cmd_overwritten = false;
                ESP_LOGW(TAG, "The %02Xh command has been used and will be overwritten by external initialization sequence", init_cmds[i].cmd);
            }
        }

        // Send command
        ESP_RETURN_ON_ERROR(tx_param(nv3041a, io, init_cmds[i].cmd, init_cmds[i].data, init_cmds[i].data_bytes), TAG, "send command failed");
        vTaskDelay(pdMS_TO_TICKS(init_cmds[i].delay_ms));

    }
    ESP_LOGD(TAG, "send init commands success");

    return ESP_OK;
}

static esp_err_t panel_nv3041a_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data)
{
    nv3041a_panel_t *nv3041a = __containerof(panel, nv3041a_panel_t, base);
    assert((x_start < x_end) && (y_start < y_end) && "start position must be smaller than end position");
    esp_lcd_panel_io_handle_t io = nv3041a->io;

    x_start += nv3041a->x_gap;
    x_end += nv3041a->x_gap;
    y_start += nv3041a->y_gap;
    y_end += nv3041a->y_gap;

    // define an area of frame memory where MCU can access
    ESP_RETURN_ON_ERROR(tx_param(nv3041a, io, LCD_CMD_CASET, (uint8_t[]) {
        (x_start >> 8) & 0xFF,
        x_start & 0xFF,
        ((x_end - 1) >> 8) & 0xFF,
        (x_end - 1) & 0xFF,
    }, 4), TAG, "send command failed");
    ESP_RETURN_ON_ERROR(tx_param(nv3041a, io, LCD_CMD_RASET, (uint8_t[]) {
        (y_start >> 8) & 0xFF,
        y_start & 0xFF,
        ((y_end - 1) >> 8) & 0xFF,
        (y_end - 1) & 0xFF,
    }, 4), TAG, "send command failed");
    // transfer frame buffer
    size_t len = (x_end - x_start) * (y_end - y_start) * nv3041a->fb_bits_per_pixel / 8;
    tx_color(nv3041a, io, LCD_CMD_RAMWR, color_data, len);

    return ESP_OK;
}

static esp_err_t panel_nv3041a_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    nv3041a_panel_t *nv3041a = __containerof(panel, nv3041a_panel_t, base);
    esp_lcd_panel_io_handle_t io = nv3041a->io;
    int command = 0;
    if (invert_color_data) {
        command = LCD_CMD_INVON;
    } else {
        command = LCD_CMD_INVOFF;
    }
    ESP_RETURN_ON_ERROR(tx_param(nv3041a, io, command, NULL, 0), TAG, "send command failed");
    return ESP_OK;
}

static esp_err_t panel_nv3041a_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    nv3041a_panel_t *nv3041a = __containerof(panel, nv3041a_panel_t, base);
    esp_lcd_panel_io_handle_t io = nv3041a->io;
    esp_err_t ret = ESP_OK;

    if (mirror_x) {
        nv3041a->madctl_val |= BIT(6);
    } else {
        nv3041a->madctl_val &= ~BIT(6);
    }
    if (mirror_y) {
        nv3041a->madctl_val |= BIT(7);
    } else {
        nv3041a->madctl_val &= ~BIT(7);
    }
    ESP_RETURN_ON_ERROR(tx_param(nv3041a, io, LCD_CMD_MADCTL, (uint8_t[]) {
        nv3041a->madctl_val
    }, 1), TAG, "send command failed");
    return ret;
}

static esp_err_t panel_nv3041a_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    nv3041a_panel_t *nv3041a = __containerof(panel, nv3041a_panel_t, base);
    esp_lcd_panel_io_handle_t io = nv3041a->io;
    if (swap_axes) {
        nv3041a->madctl_val |= LCD_CMD_MV_BIT;
    } else {
        nv3041a->madctl_val &= ~LCD_CMD_MV_BIT;
    }
    esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
        nv3041a->madctl_val
    }, 1);
    return ESP_OK;
}

static esp_err_t panel_nv3041a_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    nv3041a_panel_t *nv3041a = __containerof(panel, nv3041a_panel_t, base);
    nv3041a->x_gap = x_gap;
    nv3041a->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_nv3041a_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    nv3041a_panel_t *nv3041a = __containerof(panel, nv3041a_panel_t, base);
    esp_lcd_panel_io_handle_t io = nv3041a->io;
    int command = 0;

    if (on_off) {
        command = LCD_CMD_DISPON;
    } else {
        command = LCD_CMD_DISPOFF;
    }
    ESP_RETURN_ON_ERROR(tx_param(nv3041a, io, command, NULL, 0), TAG, "send command failed");
    return ESP_OK;
}

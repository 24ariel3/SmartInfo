#include "bsp.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/spi_master.h"
#include "esp_log.h"

static const char *TAG = "BSP";

esp_err_t bsp_init(void) {
    esp_err_t ret = ESP_OK;

    ESP_LOGI(TAG, "Initializing GPIOs...");
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BSP_LED_BLUE) | (1ULL << BSP_LCD_TOUCH_CS),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    
    // Set initial states
    gpio_set_level(BSP_LCD_TOUCH_CS, 1); // Deselect touch
    gpio_set_level(BSP_LED_BLUE, 0);

    ESP_LOGI(TAG, "Initializing SPI Bus...");
    spi_bus_config_t buscfg = {
        .mosi_io_num = BSP_LCD_SPI_MOSI,
        .miso_io_num = BSP_LCD_SPI_MISO,
        .sclk_io_num = BSP_LCD_SPI_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BSP_LCD_H_RES * 80 * sizeof(uint16_t), // Transfer buffer size
    };
    ESP_ERROR_CHECK(spi_bus_initialize(BSP_LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Initializing I2C Bus...");
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = BSP_I2C_SDA,
        .scl_io_num = BSP_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = BSP_I2C_FREQ_HZ,
    };
    i2c_param_config(BSP_I2C_NUM, &conf);
    ESP_ERROR_CHECK(i2c_driver_install(BSP_I2C_NUM, conf.mode, 0, 0, 0));

    return ret;
}

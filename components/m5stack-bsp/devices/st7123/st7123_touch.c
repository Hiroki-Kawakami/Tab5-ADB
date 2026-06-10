/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "st7123_touch.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_lcd_touch_st7123.h"

static const char *TAG = "ST7123_TP";

typedef struct {
    bsp_touch_t base;                 /* must be first — struct-inheritance vtable */
    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_touch_handle_t handle;
    SemaphoreHandle_t interrupt_semaphore;
} st7123_touch_t;

static void st7123_touch_interrupt_callback(esp_lcd_touch_handle_t tp) {
    st7123_touch_t *state = tp->config.user_data;
    BaseType_t pxHigherPriorityTaskWoken;
    xSemaphoreGiveFromISR(state->interrupt_semaphore, &pxHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(pxHigherPriorityTaskWoken);
}

static int read(bsp_touch_t *self, bsp_touch_point_t *points, uint8_t max_points) {
    st7123_touch_t *d = (st7123_touch_t *)self;
    if (max_points == 0) return 0;
    if (max_points > 5) max_points = 5;

    esp_err_t ret = esp_lcd_touch_read_data(d->handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read touch data: %s", esp_err_to_name(ret));
        return 0;
    }

    esp_lcd_touch_point_data_t raw[5];
    uint8_t count = 0;
    esp_lcd_touch_get_data(d->handle, raw, &count, max_points);
    for (uint8_t i = 0; i < count; i++) {
        points[i].x = raw[i].x;
        points[i].y = raw[i].y;
        points[i].strength = raw[i].strength;
        points[i].id = raw[i].track_id;
    }
    return count;
}

static void wait_interrupt(bsp_touch_t *self) {
    st7123_touch_t *d = (st7123_touch_t *)self;
    xSemaphoreTake(d->interrupt_semaphore, portMAX_DELAY);
}

static esp_err_t deinit(bsp_touch_t *self) {
    st7123_touch_t *d = (st7123_touch_t *)self;
    esp_lcd_touch_del(d->handle);
    esp_lcd_panel_io_del(d->io_handle);
    free(d);
    return ESP_OK;
}

esp_err_t st7123_touch_create(const bsp_touch_config_t *config, bsp_touch_t **out) {
    esp_err_t ret;

    st7123_touch_t *state = calloc(1, sizeof(st7123_touch_t));
    if (state == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for touch state");
        return ESP_ERR_NO_MEM;
    }
    state->base = (bsp_touch_t){ .read = read, .wait_interrupt = wait_interrupt, .deinit = deinit };

    // Setup IO
    esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_ST7123_CONFIG();
    io_config.scl_speed_hz = config->scl_speed_hz;

    ret = esp_lcd_new_panel_io_i2c(config->i2c_bus, &io_config, &state->io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create panel IO: %s", esp_err_to_name(ret));
        free(state);
        return ret;
    }

    // Init ST7123 Touch
    esp_lcd_touch_config_t touch_config = {
        .x_max = config->size.width,
        .y_max = config->size.height,
        .rst_gpio_num = config->rst_gpio,
        .int_gpio_num = config->int_gpio,
    };

    ret = esp_lcd_touch_new_i2c_st7123(state->io_handle, &touch_config, &state->handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize ST7123 touch: %s", esp_err_to_name(ret));
        esp_lcd_panel_io_del(state->io_handle);
        free(state);
        return ret;
    }

    if (config->interrupt) {
        state->interrupt_semaphore = xSemaphoreCreateBinary();
        if (!state->interrupt_semaphore) {
            esp_lcd_touch_del(state->handle);
            esp_lcd_panel_io_del(state->io_handle);
            free(state);
            return ESP_ERR_NO_MEM;
        }

        ret = gpio_config(&(gpio_config_t){
            .mode = GPIO_MODE_INPUT,
            .pin_bit_mask = 1 << config->int_gpio,
            .intr_type = GPIO_INTR_NEGEDGE,
        });
        if (ret != ESP_OK) {
            esp_lcd_touch_del(state->handle);
            esp_lcd_panel_io_del(state->io_handle);
            free(state);
            return ret;
        }

        ret = esp_lcd_touch_register_interrupt_callback_with_data(state->handle, st7123_touch_interrupt_callback, state);
        if (ret != ESP_OK) {
            esp_lcd_touch_del(state->handle);
            esp_lcd_panel_io_del(state->io_handle);
            free(state);
            return ret;
        }
    }

    *out = &state->base;
    return ESP_OK;
}

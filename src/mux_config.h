#pragma once
#include <esp_adc/adc_oneshot.h>
#include <driver/gpio.h>

// Mux ADC channel (the single physical pin the mux output connects to)
const adc_channel_t MUX_ADC_CHANNEL = ADC_CHANNEL_4;   // GPIO32

// Mux select lines (S0–S3) and enable pin
const gpio_num_t MUX_S0 = GPIO_NUM_13;
const gpio_num_t MUX_S1 = GPIO_NUM_12;
const gpio_num_t MUX_S2 = GPIO_NUM_14;
const gpio_num_t MUX_S3 = GPIO_NUM_27;
const gpio_num_t MUX_EN = GPIO_NUM_26;   // if EN is active low, adjust logic

// Sensor channels on the multiplexer (0–15)
const int MUX_CH_FSR    = 0;
const int MUX_CH_FLEX_0 = 1;
const int MUX_CH_FLEX_1 = 2;
// ... add more as needed
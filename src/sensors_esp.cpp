// ESP32 ESP-IDF version
#include <stdio.h>
#include <cstring>
#include <algorithm>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <nvs_flash.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include "wifi_credentials.h" // Wi‑Fi credentials (not committed to Git)
#include "calibration.h"

// ------------------------------------------------------------
// Sensor configuration
// ------------------------------------------------------------
const adc_channel_t FSR_CHANNEL = ADC_CHANNEL_4;    // GPIO32
const adc_channel_t FLEX_0_CHANNEL = ADC_CHANNEL_6; // GPIO34
const adc_channel_t FLEX_1_CHANNEL = ADC_CHANNEL_7; // GPIO35
const adc_unit_t ADC_UNIT = ADC_UNIT_1;
const int DELAY_MS = 100;
const float VCC = 3.3;
const int ADC_MAX = 4095;
const float R_DIV_FSR = 5000.0;
const float R_DIV_FLEX = 50000.0;
const float MAX_BEND_ANGLE = 180.0;
// const float FLEX_R_STRAIGHT_FALLBACK = 23130.0;
// const float FLEX_R_BENT_FALLBACK = 46400.0;

// Flex 0
const float FLEX0_R_STRAIGHT = 41000.0; // from your measurement
const float FLEX0_R_BENT = 107500.0;    // at 90° (or use fully bent value)

// Flex 1
const float FLEX1_R_STRAIGHT = 70450.0; // 1700 → Vout 1.370 V → R ≈ 70.45 k
const float FLEX1_R_BENT = 265000.0;    // 650 → Vout 0.524 V → R ≈ 265 k

adc_oneshot_unit_handle_t adc_handle;
static adc_cali_handle_t cali_handle = NULL;

float constrain(float value, float min_val, float max_val)
{
    if (value < min_val)
        return min_val;
    if (value > max_val)
        return max_val;
    return value;
}

int readAverage(adc_channel_t channel, int n)
{
    long sum = 0;
    int raw;
    for (int i = 0; i < n; i++)
    {
        adc_oneshot_read(adc_handle, channel, &raw);
        sum += raw;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return sum / n;
}

float get_voltage_from_adc(int raw)
{
    int voltage_mv;
    if (cali_handle != NULL)
    {
        esp_err_t err = adc_cali_raw_to_voltage(cali_handle, raw, &voltage_mv);
        if (err == ESP_OK)
        {
            return voltage_mv / 1000.0f; // convert mV to V
        }
    }
    // Fallback in case calibration is not available
    return (raw * VCC) / (float)ADC_MAX;
}

float getNewtonForce(int adc_val, float r_divider)
{
    if (adc_val <= 10)
        return 0.0;
    float v_out = get_voltage_from_adc(adc_val);
    if (v_out <= 0.01 || v_out >= VCC)
        return 0.0;
    float fsr_R = r_divider * (VCC / v_out - 1.0);
    if (fsr_R <= 0)
        return 0.0;
    float conductance = 1000000.0 / fsr_R;
    if (conductance <= 1000.0)
        return conductance / 80.0;
    else
        return conductance / 30.0;
}

float getFlexAngle(int adc_val, float r_divider, float r_straight, float r_bent)
{
    if (adc_val <= 10)
        return 0.0;
    float v_out = get_voltage_from_adc(adc_val);
    if (v_out <= 0.01 || v_out >= VCC)
        return 0.0;
    float flex_R = r_divider * (VCC / v_out - 1.0);
    float angle = (flex_R - r_straight) * MAX_BEND_ANGLE /
                  (r_bent - r_straight);
    return constrain(angle, 0.0, MAX_BEND_ANGLE);
}

// ------------------------------------------------------------
// Wi-Fi and TCP server
// ------------------------------------------------------------
#define TCP_PORT 1234

static int server_sock = -1;
int client_sock = -1;
SemaphoreHandle_t sock_mutex = NULL;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        esp_wifi_connect();
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        printf("Wi-Fi connected, IP: " IPSTR "\n", IP2STR(&event->ip_info.ip));
    }
}

static void wifi_init_sta(void)
{
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler, NULL, NULL);

    wifi_config_t wifi_config = {};
    strcpy((char *)wifi_config.sta.ssid, WIFI_SSID);
    strcpy((char *)wifi_config.sta.password, WIFI_PASS);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
}

static void tcp_server_task(void *arg)
{
    struct sockaddr_in server_addr = {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(TCP_PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    server_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_sock < 0)
    {
        printf("TCP socket create failed\n");
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        printf("TCP bind failed\n");
        close(server_sock);
        vTaskDelete(NULL);
        return;
    }

    if (listen(server_sock, 1) < 0)
    {
        printf("TCP listen failed\n");
        close(server_sock);
        vTaskDelete(NULL);
        return;
    }

    printf("TCP server listening on port %d\n", TCP_PORT);

    char rx_buffer[64];
    while (1)
    {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int new_sock = accept(server_sock, (struct sockaddr *)&client_addr, &addr_len);
        if (new_sock >= 0)
        {
            xSemaphoreTake(sock_mutex, portMAX_DELAY);
            if (client_sock >= 0)
            {
                close(client_sock); // replace previous client
            }
            client_sock = new_sock;
            xSemaphoreGive(sock_mutex);
            printf("Client connected\n");

            // Command loop (unchanged)
            char rx_buffer[64];
            while (1)
            {
                int len = recv(client_sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
                if (len <= 0)
                    break;
                rx_buffer[len] = '\0';
                char *cmd = strtok(rx_buffer, "\r\n");
                if (cmd && cmd[strlen(cmd) - 1] == '\r')
                    cmd[strlen(cmd) - 1] = '\0';
                while (cmd != NULL)
                {
                    if (strlen(cmd) > 0) // skip empty strings (e.g., leading \r\n)
                    {
                        printf("CMD: '%s'\n", cmd);
                        process_command(cmd);
                    }
                    cmd = strtok(NULL, "\r\n");
                }
            }
            // Client disconnected
            xSemaphoreTake(sock_mutex, portMAX_DELAY);
            close(client_sock);
            client_sock = -1;
            xSemaphoreGive(sock_mutex);
            printf("Client disconnected\n");
        }
    }
}

static void send_data(const char *str)
{
    xSemaphoreTake(sock_mutex, portMAX_DELAY);
    if (client_sock >= 0)
    {
        int len = strlen(str);
        int sent = send(client_sock, str, len, 0);
        if (sent < 0)
        {
            close(client_sock);
            client_sock = -1;
        }
    }
    xSemaphoreGive(sock_mutex);
}

// ------------------------------------------------------------
// Sensor task (unchanged logic, added Wi-Fi output)
// ------------------------------------------------------------
static void sensor_task(void *arg)
{
    while (true)
    {
        if (!streaming_enabled)
        {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        int fsr_adc = readAverage(FSR_CHANNEL, 10);
        float force_N = getNewtonForce(fsr_adc, R_DIV_FSR);

        int flex0_adc = readAverage(FLEX_0_CHANNEL, 10);
        int flex1_adc = readAverage(FLEX_1_CHANNEL, 10);

        float flex0_deg = get_calibrated_angle(flex0_adc, calib_flex0);
        if (flex0_deg < 0)
        {
            // Fallback to old method
            flex0_deg = getFlexAngle(flex0_adc, R_DIV_FLEX, FLEX0_R_STRAIGHT, FLEX0_R_BENT);
        }

        float flex1_deg = get_calibrated_angle(flex1_adc, calib_flex1);
        if (flex1_deg < 0)
        {
            flex1_deg = getFlexAngle(flex1_adc, R_DIV_FLEX, FLEX1_R_STRAIGHT, FLEX1_R_BENT);
        }

        // Serial output (debug)
        printf("FSR:%.2f N, Flex0:%04i => %.2f deg, Flex1:%04i => %.2f deg\n",
               force_N, flex0_adc, flex0_deg, flex1_adc, flex1_deg);

        // Wi-Fi output
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "FSR:%.2f N, Flex0:%04i => %.2f deg, Flex1:%04i => %.2f deg\r\n",
                 force_N, flex0_adc, flex0_deg, flex1_adc, flex1_deg);
        send_data(buf);

        vTaskDelay(pdMS_TO_TICKS(DELAY_MS));
    }
}

// ------------------------------------------------------------
// Main entry point
// ------------------------------------------------------------
extern "C" void app_main()
{
    // ADC1 initialisation
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT,
        .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    adc_oneshot_new_unit(&init_config, &adc_handle);

    adc_oneshot_chan_cfg_t chan_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    adc_oneshot_config_channel(adc_handle, FSR_CHANNEL, &chan_config);
    adc_oneshot_config_channel(adc_handle, FLEX_0_CHANNEL, &chan_config);
    adc_oneshot_config_channel(adc_handle, FLEX_1_CHANNEL, &chan_config);

    // === ADC calibration (line fitting) ===
    adc_cali_line_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
        .default_vref = 1100, // typical internal reference in mV
    };
    esp_err_t ret = adc_cali_create_scheme_line_fitting(&cali_config, &cali_handle);
    if (ret != ESP_OK)
    {
        printf("ADC calibration failed (0x%x), using raw conversion\n", ret);
        cali_handle = NULL;
    }

    // Load stored calibration tables from NVS
    load_calibration_from_nvs();

    // Create mutex for socket access
    sock_mutex = xSemaphoreCreateMutex();

    // Initialize Wi‑Fi
    wifi_init_sta();

    // Start TCP server task
    xTaskCreate(tcp_server_task, "tcp_server", 8192, NULL, 5, NULL);

    // Create sensor task
    xTaskCreate(sensor_task, "sensor_task", 8192, NULL, tskIDLE_PRIORITY + 1, NULL);
}
#include "calibration.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <lwip/sockets.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <cstdio>
#include <string>
#include <cstring>
#include <algorithm>
#include <esp_adc/adc_oneshot.h>

// --- External references ---
extern adc_oneshot_unit_handle_t adc_handle;
extern int client_sock; // from sensors_esp.cpp
extern SemaphoreHandle_t sock_mutex;

// Calibration tables (active)
std::vector<CalPoint> calib_flex0;
std::vector<CalPoint> calib_flex1;

// Buffers during active calibration
static std::vector<CalPoint> calib_buffer0;
static std::vector<CalPoint> calib_buffer1;

bool streaming_enabled = true;

// Helper to sort points by raw ADC ascending
static void sort_points(std::vector<CalPoint> &pts)
{
    std::sort(pts.begin(), pts.end(),
              [](const CalPoint &a, const CalPoint &b)
              { return a.raw < b.raw; });
}

// Send response with mutex
void send_response(const char *msg)
{
    xSemaphoreTake(sock_mutex, portMAX_DELAY);
    if (client_sock >= 0)
    {
        send(client_sock, msg, strlen(msg), 0);
        send(client_sock, "\n", 1, 0);
    }
    xSemaphoreGive(sock_mutex);
}

// Single raw reading (for calibration)
int read_sensor_raw(int channel)
{
    int raw;
    adc_oneshot_read(adc_handle, (adc_channel_t)channel, &raw);
    return raw;
}

// Parse "raw,angle;raw,angle;..." into vector
static void parse_points(const char *str, std::vector<CalPoint> &out)
{
    out.clear();
    std::string s(str);
    size_t pos = 0;
    while (pos < s.size())
    {
        size_t next = s.find(';', pos);
        std::string token = s.substr(pos, next - pos);
        size_t comma = token.find(',');
        if (comma != std::string::npos)
        {
            int raw = atoi(token.substr(0, comma).c_str());
            float angle = atof(token.substr(comma + 1).c_str());
            out.push_back({raw, angle});
        }
        pos = (next == std::string::npos) ? s.size() : next + 1;
    }
}

// Load from NVS (and sort immediately)
void load_calibration_from_nvs()
{
    nvs_handle_t handle;
    if (nvs_open("flex_calib", NVS_READONLY, &handle) != ESP_OK)
    {
        return;
    }

    size_t len;
    char buf[256];
    if (nvs_get_str(handle, "flex0", NULL, &len) == ESP_OK)
    {
        nvs_get_str(handle, "flex0", buf, &len);
        parse_points(buf, calib_flex0);
        sort_points(calib_flex0);

        printf("Loaded calibration: Flex0 points: ");
        for (auto &p : calib_flex0)
        {
            printf("%d->%.1f ", p.raw, p.angle);
        }
    }
    if (nvs_get_str(handle, "flex1", NULL, &len) == ESP_OK)
    {
        nvs_get_str(handle, "flex1", buf, &len);
        parse_points(buf, calib_flex1);
        sort_points(calib_flex1);

        printf("\nLoaded calibration: Flex1 points: ");
        for (auto &p : calib_flex1)
        {
            printf("%d->%.1f ", p.raw, p.angle);
        }
    }
    nvs_close(handle);
}

// Save buffers to NVS (sorted) and update active tables
static void save_calibration_to_nvs()
{
    // Sort the recorded buffers before storing
    sort_points(calib_buffer0);
    sort_points(calib_buffer1);

    nvs_handle_t handle;
    nvs_open("flex_calib", NVS_READWRITE, &handle);

    // Store flex0
    std::string str;
    for (auto &p : calib_buffer0)
    {
        str += std::to_string(p.raw) + "," + std::to_string(p.angle) + ";";
    }
    if (!str.empty())
    {
        nvs_set_str(handle, "flex0", str.c_str());
    }
    else
    {
        nvs_erase_key(handle, "flex0");
    }

    // Store flex1
    str.clear();
    for (auto &p : calib_buffer1)
    {
        str += std::to_string(p.raw) + "," + std::to_string(p.angle) + ";";
    }
    if (!str.empty())
    {
        nvs_set_str(handle, "flex1", str.c_str());
    }
    else
    {
        nvs_erase_key(handle, "flex1");
    }

    nvs_commit(handle);
    nvs_close(handle);

    // Update active tables with the sorted data
    calib_flex0 = calib_buffer0;
    calib_flex1 = calib_buffer1;
}

// Command parser
void process_command(const char *cmd)
{
    printf("CMD recv: '%s'\n", cmd);
    if (strcmp(cmd, "CAL:START") == 0)
    {
        streaming_enabled = false;
        calib_buffer0.clear();
        calib_buffer1.clear();
        send_response("CAL:READY");
    }
    else if (strncmp(cmd, "CAL:RECORD_FLEX0 ", 17) == 0)
    {
        float angle = atof(cmd + 17);
        int raw = read_sensor_raw(6); // FLEX_0_CHANNEL = ADC_CHANNEL_6
        calib_buffer0.push_back({raw, angle});
        char resp[64];
        snprintf(resp, sizeof(resp), "OK:%d@%.1f", raw, angle);
        send_response(resp);
    }
    else if (strncmp(cmd, "CAL:RECORD_FLEX1 ", 17) == 0)
    {
        float angle = atof(cmd + 17);
        int raw = read_sensor_raw(7); // FLEX_1_CHANNEL = ADC_CHANNEL_7
        calib_buffer1.push_back({raw, angle});
        char resp[64];
        snprintf(resp, sizeof(resp), "OK:%d@%.1f", raw, angle);
        send_response(resp);
    }
    else if (strcmp(cmd, "CAL:DONE") == 0)
    {
        save_calibration_to_nvs();
        streaming_enabled = true;
        send_response("CAL:SAVED");
    }
    else if (strcmp(cmd, "CAL:DUMP") == 0)
    {
        std::string dump = "Flex0: ";
        for (auto &p : calib_flex0)
        {
            dump += std::to_string(p.raw) + "->" + std::to_string(p.angle) + "; ";
        }
        dump += "| Flex1: ";
        for (auto &p : calib_flex1)
        {
            dump += std::to_string(p.raw) + "->" + std::to_string(p.angle) + "; ";
        }
        send_response(dump.c_str());
    }
    else if (strcmp(cmd, "CAL:RESET") == 0)
    {
        nvs_handle_t handle;
        nvs_open("flex_calib", NVS_READWRITE, &handle);
        nvs_erase_key(handle, "flex0");
        nvs_erase_key(handle, "flex1");
        nvs_commit(handle);
        nvs_close(handle);
        calib_flex0.clear();
        calib_flex1.clear();
        send_response("CAL:RESET");
    }
    else
    {
        send_response("UNKNOWN");
    }
}

// Interpolation (with sorted points)
float get_calibrated_angle(int raw, const std::vector<CalPoint> &calib)
{
    if (calib.empty())
    {
        return -1; // signal to use fallback
    }
    if (raw <= calib.front().raw)
        return calib.front().angle;
    if (raw >= calib.back().raw)
        return calib.back().angle;

    for (size_t i = 0; i < calib.size() - 1; i++)
    {
        if (raw >= calib[i].raw && raw <= calib[i + 1].raw)
        {
            float t = (float)(raw - calib[i].raw) / (calib[i + 1].raw - calib[i].raw);
            return calib[i].angle + t * (calib[i + 1].angle - calib[i].angle);
        }
    }
    return 0; // should never reach
}

void send_calibration_info(void) {
    std::string dump = "Calibration loaded: Flex0=";
    for (auto& p : calib_flex0) {
        dump += std::to_string(p.raw) + "->" + std::to_string(p.angle) + " ";
    }
    dump += "| Flex1=";
    for (auto& p : calib_flex1) {
        dump += std::to_string(p.raw) + "->" + std::to_string(p.angle) + " ";
    }
    send_response(dump.c_str());
    printf("%s\n", dump.c_str());
}
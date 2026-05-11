#ifndef CALIBRATION_H
#define CALIBRATION_H

#include <vector>

struct CalPoint {
    int raw;
    float angle;
};

// Global calibration tables (loaded from NVS)
extern std::vector<CalPoint> calib_flex0;
extern std::vector<CalPoint> calib_flex1;

// Calibration command processor
void process_command(const char *cmd);

// Load calibration tables from NVS
void load_calibration_from_nvs();

// Get angle from raw ADC using interpolation
float get_calibrated_angle(int raw, const std::vector<CalPoint>& calib);

// Helper to send response back to TCP client
void send_response(const char *msg);

// Helper to take a single raw ADC reading (used during calibration)
int read_sensor_raw(int channel);

// Send current calibration tables to the connected client
void send_calibration_info(void);

// Flag to enable/disable data streaming
extern bool streaming_enabled;

#endif
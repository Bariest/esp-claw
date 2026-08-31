#pragma once

#include <cstdint>

namespace robot {

/**
 * @brief IMU data snapshot: accelerometer (g) and gyroscope (dps).
 */
struct ImuData {
    float ax = 0.0f;  ///< Accel X (g)
    float ay = 0.0f;  ///< Accel Y (g)
    float az = 0.0f;  ///< Accel Z (g)
    float gx = 0.0f;  ///< Gyro X (dps)
    float gy = 0.0f;  ///< Gyro Y (dps)
    float gz = 0.0f;  ///< Gyro Z (dps)
};

/**
 * @brief Initialise the IMU (Bosch BMI270) over I2C.
 *
 * - Resolves the `imu_sensor` board device and the I2C bus behind it
 * - Uploads the BMI270 configuration blob and sets +/-16 g, +/-2000 dps,
 *   200 Hz -- the same ranges ESP-Claw's own BMI270 backend uses, so
 *   readings are comparable between the two
 * - Starts a background task polling at 20 Hz
 *
 * Implemented in mpx_imu.cc. The earlier MangDang board had a QMI8658C on
 * SPI2 sharing pins with the servo driver boards; this one does not, which
 * is why driver_board.h's bus-lock note no longer describes a live hazard.
 *
 * @return true on success, false on failure.
 */
bool imu_init();

/**
 * @brief Return a copy of the latest IMU sample (thread-safe).
 */
ImuData imu_read();

/**
 * @brief Print the latest IMU data via ESP_LOGI.
 */
void imu_print();

}  // namespace robot

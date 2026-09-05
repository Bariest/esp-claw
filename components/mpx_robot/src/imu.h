#pragma once

#include <cstdint>

namespace robot {

/**
 * @brief IMU data snapshot: accelerometer (g), gyroscope (dps) and, when the
 *        BMM150 behind the BMI270's auxiliary bus came up, magnetometer (uT).
 *
 * The magnetometer fields stay at zero and `mag_valid` false when the BMM150
 * was not found; accel and gyro are unaffected either way. The new fields are
 * appended, so code that only knows the first six floats (mpx_wasm's host
 * copy, cap_robot's status JSON) keeps working unchanged.
 */
struct ImuData {
    float ax = 0.0f;  ///< Accel X (g)
    float ay = 0.0f;  ///< Accel Y (g)
    float az = 0.0f;  ///< Accel Z (g)
    float gx = 0.0f;  ///< Gyro X (dps)
    float gy = 0.0f;  ///< Gyro Y (dps)
    float gz = 0.0f;  ///< Gyro Z (dps)
    float mx = 0.0f;  ///< Mag X (uT), compensated with the chip's trim data
    float my = 0.0f;  ///< Mag Y (uT)
    float mz = 0.0f;  ///< Mag Z (uT)
    bool  mag_valid = false;  ///< BMM150 present and this sample carries it
};

/**
 * @brief Initialise the IMU (Bosch BMI270) over I2C.
 *
 * - Resolves the `imu_sensor` board device and the I2C bus behind it
 * - Uploads the BMI270 configuration blob and sets +/-16 g, +/-2000 dps,
 *   200 Hz -- the same ranges ESP-Claw's own BMI270 backend uses, so
 *   readings are comparable between the two
 * - Brings up the BMM150 magnetometer through the BMI270's auxiliary I2C
 *   master (it is not on the main bus); a missing BMM150 is logged and
 *   ignored, accel and gyro still come up
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
 * @brief True once the BMM150 answered through the BMI270 and is being polled.
 */
bool imu_mag_ready();

/**
 * @brief Print the latest IMU data via ESP_LOGI.
 */
void imu_print();

}  // namespace robot

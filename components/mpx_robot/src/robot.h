#pragma once

#include "sdkconfig.h"
#include <cstdint>

namespace robot {

// ── Forward declarations ─────────────────────────────────────
struct ImuData;

}  // namespace robot

#include "imu.h"

namespace robot {

// ── Robot constants ──────────────────────────────────────────
constexpr float L1 = 50.0f;   // Upper leg length (mm)
constexpr float L2 = 56.0f;   // Lower leg length (mm)
constexpr float PI = 3.14159265358979f;

// ── Servo signal range ───────────────────────────────────────
// The AT32 driver boards sweep 0–270° over the raw range 0..1023
// (centre 511 = 135°). IK angles are commanded RELATIVE to the centre,
// so the usable command range is ±135°.
//
// CHANGED WITH THE DRIVER-BOARD SWAP. The Feetech SCSCL servos this
// firmware used to drive were 0–180° over the same 0..1023, giving
// 5.683 raw counts per degree. driver_board_sync_write() maps
// 0..1023 onto the AT32's 0..2700 deci-degrees, so a degree is now
// worth 3.789 counts. Leaving the old constant in place would have
// overdriven every joint by exactly 1.5x — the gait would still run,
// but every leg would swing half again as far as the IK asked for.
// 1/0.263 in the mpxesp test firmware is this same number.
//
// NOTE: the per-servo offsets in NVS were calibrated against the old
// scale. They are stored in DEGREES, so they are now applied 1.5x
// smaller than when you set them — recalibrate after first flash.
constexpr float SERVO_RANGE_DEG   = 270.0f;
constexpr float SERVO_DEG_TO_RAW  = 1023.0f / SERVO_RANGE_DEG;   // ≈ 3.789

// ── The two position frames, and how to move between them ────────
//
// There are TWO raw position frames in this firmware and they are mirror
// images of each other. Confusing them is not a compile error, so name the
// conversion instead of open-coding it:
//
//   GAIT frame  — what s_goal_pos[] and set_servo_angle() speak.
//                 0..1023, 511 = centre, positive `deg` increases the value.
//
//   AT32 frame  — what the driver boards, driver_board_stage(),
//                 driver_board_direct(), all feedback (fb_store) and Servo
//                 Studio speak. 0..1023 (or 0..270°), 511 ≈ centre.
//
// driver_board_sync_write() converts gait -> AT32 with a direction flip
// (`2700 - pos * 2700/1024`), and fb_store() deliberately does NOT flip on
// the way back, which is what makes the round trip exact. The net effect is
// simply that the two frames run in opposite directions:
//
//     at32_raw == 1024 - gait_raw
//
// Anything that compares a commanded position against a measured one MUST
// put both in the same frame first. read_moving() did not, and reported
// "still moving" for every pose except dead centre.
constexpr int gait_raw_to_at32_raw(int gait_raw) { return 1024 - gait_raw; }
constexpr int at32_raw_to_gait_raw(int at32_raw) { return 1024 - at32_raw; }

// ── Neutral stand calibration (like the reference minipupperesp) ─
// The robot is physically calibrated so ALL SERVOS CENTRED == the
// standing pose at this height.  The leg IK subtracts the neutral
// joint angles of this stance so that front_right_ik(0,0,NEUTRAL_Z)
// == centred servos == the power-on pose.  This makes the stand
// phase after start-up identical to the reference repo: the robot
// simply holds/settles into the stand with no repositioning jump.
constexpr float NEUTRAL_Z = 70.0f;

// ── Stanford walk parameters ─────────────────────────────────
// The walk speed itself is RUNTIME-ADJUSTABLE: Config::sg_speed
// (mm/s), set from the web UI "Walk speed" slider and persisted in
// NVS.  Full-stick maximum is 200 mm/s per the Mini Pupper BSP
// Config.py; the reference web default was 100 which is too fast
// for this robot, so the default here is 50.
constexpr float SG_SPEED_MAX_MM_S = 200.0f;   // full joystick stick
// The reference robot's leg roles are mirrored left<->right vs the
// physical legs, so lateral/yaw commands are flipped there.  Keep
// the same default; set to false if strafe/turn comes out mirrored.
constexpr bool  SG_MIRROR_LR = true;

// ── Web joystick (mini_pupper_web_controller style) ──────────
// Left pad: forward/strafe, right pad: turn.  Values -1..1, scaled
// by Config::sg_speed.  Fresh input drives the Stanford walk; when
// the pads are released (zeros / timeout) the robot steps in place.
constexpr float JOY_VY_MAX     = 200.0f;   // mm/s strafe at full stick
constexpr float JOY_WZ_MAX     = 2.0f;     // rad/s yaw at full stick
constexpr uint32_t JOY_TIMEOUT_MS = 600;

// ── Body geometry for attitude (look) poses ──────────────────
// NOTE: superseded — the look/imported-move poses now use the exact
// Stanford geometry (SG_ORIGIN_X/SG_ORIGIN_Y in stanford_gait.h).
// Kept only for reference.
constexpr float BODY_LX = 50.0f;   // half fore-aft hip spacing (mm)
constexpr float BODY_LY = 35.0f;   // half left-right hip spacing (mm)

// Direction sign flips for the look poses.  If the robot looks the
// OPPOSITE way to what a command says, flip that axis sign.
// (Verified in simulation: PITCH_SIGN = -1 makes LookUp raise the nose.)
constexpr float PITCH_SIGN = -1.0f; // LookUp raises the nose
constexpr float YAW_SIGN   =  1.0f; // LookRight aims head right (flip if reversed)
constexpr float ROLL_SIGN  =  1.0f; // positive roll leans right (flip if reversed)

// Default look amplitudes (degrees), matching StanfordQuadruped.
constexpr float LOOK_PITCH_DEG = 20.0f;
constexpr float LOOK_YAW_DEG   = 30.0f;

// ── Gait command enum ────────────────────────────────────────
enum class GaitCmd : uint8_t {
    None,
    Init,        // Initial pose (zero offsets)
    Step,        // In-place stepping (trot)
    Roll,        // Body roll
    Pitch,       // Body pitch
    Stretch,     // Body stretch (up/down)
    Advance,     // Walk forward
    Back,        // Walk backward
    Left,        // Strafe left
    Right,       // Strafe right
    TurnL,       // Turn left (spin)
    TurnR,       // Turn right (spin)
    Twerk,       // Twerk / wiggle
    Jump,        // Vertical jump
    JumpFwd,     // Jump forward
    TestSpeed,   // Servo speed test

    // ── Imported from StanfordQuadruped (body-attitude poses) ────
    // These hold a static "head aiming" pose by rotating the body
    // while the feet stay planted (Stanford's roll/pitch/yaw model
    // converted to this robot's per-leg IK).  All static-hold.
    LookUp,         // Pitch nose up
    LookDown,       // Pitch nose down
    LookLeft,       // Yaw head left
    LookRight,      // Yaw head right
    LookUpperLeft,  // Up + left
    LookUpperRight, // Up + right
    LookLowerLeft,  // Down + left
    LookLowerRight, // Down + right

    // ── Single-leg lifts (raise one paw while standing) ──────────
    ForelegLiftL,   // Lift front-left paw
    ForelegLiftR,   // Lift front-right paw
    BacklegLiftL,   // Lift rear-left paw
    BacklegLiftR,   // Lift rear-right paw

    // ── Static body height ───────────────────────────────────────
    HeightUp,       // Raise body
    HeightDown,     // Lower body

    // ── Static combined roll+pitch hold ──────────────────────────
    Balance,        // Hold a roll+pitch attitude

    // ── Level-3 choreography (dynamic, auto-return) ──────────────
    BowBack,        // Bow head down and shuffle backward
    BodyCycle,      // Body centre draws a circle (orientation fixed)
    HeadEllipse,    // Head draws an ellipse via body attitude

    // ── Diagonal trot walks ──────────────────────────────────────
    MoveLeftFront,  // Forward + strafe left
    MoveRightFront, // Forward + strafe right
    MoveLeftBack,   // Backward + strafe left
    MoveRightBack,  // Backward + strafe right

    // ── Stanford Pupper trot gait (exact StanfordQuadruped port) ─
    StanfordWalk,   // Continuous forward trot, Raibert swing/stance

    // ── FPC choreography (MangDang MovementGroups.py, Stanford IK) ─
    FrontKick,      // Rear up like a horse: front paws reach fwd+up, auto-return
    Wiggle,         // Butt up (pitch -22°), tail-wag yaw sweep while held
    ButtShrug,      // Nose up (pitch +20°), butt-shrug yaw sweep while held
    WiggleLeft,     // Butt up, yaw held to one side (FPC wiggle_left)
    WiggleRight,    // Butt up, yaw held to the other side (FPC wiggle_right)
    ButtShrugLeft,  // Nose up, yaw held to one side (FPC butt_shrug_left)
    ButtShrugRight, // Nose up, yaw held to the other side (FPC butt_shrug_right)

    // Direct SDK-controlled body attitude. Kept last so existing numeric
    // GaitCmd values remain backward compatible.
    BodyAttitude,   // Hold caller-provided roll, pitch and yaw angles
};

// ── Robot configuration ──────────────────────────────────────
struct Config {
    int period   = 80;   // Gait period (ms per phase)
    int height   = 70;   // Body height (mm)
    int up_height = 10;  // Foot lift height (mm)
    int stride   = 10;   // Stride length (mm)
    int tilt     = 10;   // Body tilt angle (degrees)
    int sg_speed = 50;   // Stanford walk / diagonal speed (mm/s, max 200)
};

// ── Servo bus configuration ──────────────────────────────────
//
// The servos are driven by four AT32F413 driver boards over SPI, three servos
// each (see driver_board.h). All the pins live there.
//
// This layer used to own one pin of its own -- SERVO_POWER_PIN, GPIO 8, the
// servo rail enable. The MP4 ESP32 CORE has no such pin: the four HC-PHD-2
// connectors are wired to Vbat+ directly, and GPIO 8 is IO8_PI_2 on the
// expansion header. The constant is gone rather than repointed, because
// there is nothing correct to repoint it at.

// Current cap sent with every position setpoint, in mA. The driver boards do
// position control with a current LIMIT rather than a speed - this is a
// ceiling, not a forced draw, so a lightly loaded leg still only sources what
// it needs to hold station.
constexpr uint16_t SERVO_CURRENT_MAX_MA = 1500;

// ── NVS ──────────────────────────────────────────────────────
//
// Servo offsets and Config live in their own namespace. ESP-Claw's app_config
// owns the default one, and a collision there would have the two layers
// silently overwriting each other's keys.
#define MPX_ROBOT_NVS_NAMESPACE "mpx_robot"

// ── Public API ───────────────────────────────────────────────

/**
 * @brief Initialise robot HAL.
 *
 * - Opens NVS handle
 * - Enables servo power (GPIO8 high)
 * - Brings up the SPI servo driver boards (bus shared with the IMU)
 * - Restores persisted offsets and config from NVS
 * - Spawns the gait task on core 1
 *
 * @return true on success.
 */
bool init();

/**
 * @brief Send a gait command (thread-safe).
 *
 * Overwrites any previous command.  The gait task picks it up
 * on its next iteration.
 */
void send_gait_cmd(GaitCmd cmd);

/* ── The one gait name table ──────────────────────────────────────────────
 *
 * This mapping existed in three hand-maintained copies -- the SDK's host
 * function, the HTTP handler, and the logging switch in gait_task() -- which
 * is three chances for a name to mean different things depending on who asked.
 * They now all come here.
 */

/** Look a wire name up. False if there is no such gait. */
bool gait_from_name(const char *name, GaitCmd &out);

/** The wire name for a gait, or "none". */
const char *gait_to_name(GaitCmd cmd);

/** How many named gaits there are, and the i-th name. For listing them. */
int         gait_name_count();
const char *gait_name_at(int index);

/**
 * @brief Return the currently active gait command.
 */
GaitCmd current_gait_cmd();

/**
 * @brief Web-joystick input (mini_pupper_web_controller style).
 *
 * f = forward/back, s = strafe (+left), t = turn (+left); each -1..1.
 * Any input beyond the dead-zone auto-starts the Stanford walk; the
 * velocities scale with Config::sg_speed.  Releasing the pads (zeros)
 * makes the robot step in place; leaving the walk parks it standing.
 */
void joy_input(float f, float s, float t);

/**
 * @brief Hold a Stanford-IK body attitude (degrees).
 *
 * Values are clamped to the same safe limits as the reference movement API:
 * roll +/-25, pitch +/-20 and yaw +/-30 degrees. Calling this again updates
 * the held pose; sending any gait command exits the pose normally.
 */
void set_body_attitude(float roll_deg, float pitch_deg, float yaw_deg);

/**
 * @brief Set the body-attitude slew speed in degrees/second.
 *
 * 0 (default) = instant: roll/pitch/yaw/attitude snap to the target.
 * >0 makes the held attitude glide toward the target at this speed, so
 * repeated pose updates ease smoothly instead of jumping. Negative values
 * are clamped to 0.
 */
void set_attitude_speed(float dps);

/**
 * @brief Set the body-attitude slew speed per axis, in degrees/second.
 *
 * Like set_attitude_speed() but with an independent speed for roll, pitch
 * and yaw. 0 on an axis = that axis snaps instantly; >0 = it glides at that
 * speed. Negative values are clamped to 0.
 */
void set_attitude_speed_xyz(float roll_dps, float pitch_dps, float yaw_dps);

/**
 * @brief Get the current robot configuration.
 */
Config get_config();

/**
 * @brief Overwrite the robot configuration and persist to NVS.
 */
void set_config(const Config &cfg);

// ── IMU API ─────────────────────────────────────────────────

/**
 * @brief Initialise the QMI8658C IMU over SPI (SPI2_HOST).
 *
 * Must be called once during boot, after NVS but before the IMU
 * background task is needed.  Spawns a low-priority polling task
 * on core 0 that reads 6-DOF data at ~20 Hz.
 *
 * @return true on success.
 */
bool imu_init();

/**
 * @brief Return a thread-safe copy of the latest IMU sample.
 */
ImuData imu_read();

/**
 * @brief Print the latest IMU data to the log.
 */
void imu_print();

/**
 * @brief Get the angular offset for a single servo (1‑12).
 */
float get_offset(int servo_id);

/**
 * @brief Set the angular offset for a single servo (1‑12) and
 *        persist it to NVS.
 */
void set_offset(int servo_id, float deg);

/**
 * @brief Reset all servo offsets to 0 and persist.
 */
void reset_offsets();

/**
 * @brief Read the current position of a servo, raw 0‑1023 in the AT32 FRAME.
 *        Returns -1 on failure.
 *
 * @note This is the frame the driver boards, servo_read()/servo_read_all()
 *       and Servo Studio use — NOT the frame set_servo_angle() accepts. The
 *       two run in opposite directions (see the frame note at the top of this
 *       header). To close a loop around set_servo_angle(), use
 *       read_angle_cdeg() instead; comparing this against a commanded angle
 *       will diverge.
 */
int read_position(int servo_id);

/**
 * @brief Read a servo's measured angle in the SAME frame set_servo_angle()
 *        takes: signed centidegrees relative to centre.
 *
 * This is the reader to close a control loop with — command with
 * set_servo_angle(id, deg), measure with read_angle_cdeg(id) / 100.0f, and
 * the error term has the sign you expect.
 *
 * @return Centidegrees from centre, or INT32_MIN on a bad servo id. The
 *         sentinel is out of band deliberately: every value in ±13500 is a
 *         legitimate reading, so -1 could not be used as an error code here.
 */
int read_angle_cdeg(int servo_id);

/**
 * @brief Read the current speed of a servo (signed).
 *        Returns -1 on failure.
 */
int read_speed(int servo_id);

/**
 * @brief Read the current load on a servo (signed, unit‑dependent).
 *        Returns -1 on failure.
 */
int read_load(int servo_id);

/**
 * @brief Read the servo supply voltage (0.1 V increments).
 *        Returns -1 on failure.
 *
 * @note Not reported by the AT32 driver boards - always returns -1.
 */
int read_voltage(int servo_id);

/**
 * @brief Read the servo NTC temperature (°C, truncated to a whole degree).
 *        Returns -1 if that servo has never answered.
 *
 * @note This DOES work on the driver boards - the AT32 samples one 10k NTC per
 *       servo and ships it in every feedback frame, so while the gait runs the
 *       value costs no extra SPI traffic. Use read_temperature_c() to keep the
 *       fractional part.
 */
int read_temperature(int servo_id);

/**
 * @brief Read the servo moving state (0 = stopped, 1 = moving).
 *        Returns -1 on failure.
 */
int read_moving(int servo_id);

/**
 * @brief Read the servo current draw (mA, signed).
 *        Returns -1 on failure.
 *
 * @note This DOES work on the driver boards - present motor current rides on
 *       the same feedback frame as position.
 */
int read_current(int servo_id);

/**
 * @brief Read the servo NTC temperature in °C, fractional part kept.
 *        Returns NAN if that servo has never answered.
 */
float read_temperature_c(int servo_id);

/**
 * @brief Ping a servo to verify communication.
 *
 * Probes the channel with a single parameter read over SPI. Returns 1 if the
 * driver board answered, or <= 0 on failure. (The Feetech bus returned a model
 * number here; the AT32 boards have no equivalent, so this is now a plain
 * reachable / not-reachable answer.)
 */
int ping_servo(int servo_id);

// ── Servo Studio / direct bus access ─────────────────────────

/**
 * @brief Park the gait task so another task can own the servo bus.
 *
 * Servo Studio parameter access needs unhurried use of the driver boards, and
 * a config request is a request/reply PAIR that must not be interleaved with
 * gait traffic. While studio mode is on the gait task stops calling flush()
 * and the servos simply hold their last commanded pose. Turning it off resumes
 * normal scheduling.
 */
void set_studio_mode(bool on);

/**
 * @brief Whether Servo Studio currently holds the bus.
 */
bool studio_mode();

/**
 * @brief Take the servo bus for a WASM skill (Unitree-style low-level control).
 *
 * Parks the gait exactly like studio mode does, but records a different owner
 * so the two cannot silently steal the bus from each other. Servo Studio wins:
 * this returns false while a human has the console open, rather than yanking
 * the bus out from under them.
 *
 * The sandbox force-releases this when the skill returns or is killed, so a
 * crashed skill cannot leave the robot parked — see release_skill_bus_lock().
 *
 * @return true if the lock was taken (or already held by a skill).
 */
bool servo_lock();

/**
 * @brief Release a skill's bus lock and resume the gait. No-op if a skill
 *        does not currently hold it.
 */
void servo_unlock();

/**
 * @brief Whether anything (Studio or a skill) currently holds the bus.
 *
 * @warning Do NOT use this to authorise a skill's servo writes — it is also
 *          true while Servo Studio holds the bus. Use servo_owned_by_skill().
 */
bool servo_locked();

/**
 * @brief Whether a WASM skill specifically holds the bus.
 *
 * This is the check every low-level servo host function should gate on: it
 * answers "did the caller take the lock", not "is the lock taken".
 */
bool servo_owned_by_skill();

/**
 * @brief Force-release a skill's lock. Called by the WASM sandbox after the
 *        skill's entry point returns, whatever the outcome.
 */
void release_skill_bus_lock();

// ── Low-level IK (exposed for WASM host functions) ───────────

/**
 * @brief Set one servo angle in degrees RELATIVE to centre (0 = 90°
 *        physical). Clamped to the raw range 0..1023, i.e. ±90° for
 *        this 0–180° servo. (Raw scale was 270°/1023 before; now
 *        180°/1023 to match the physical servo range. Old doc said (‑270, 270).
 */
void set_servo_angle(int servo_id, float deg);

/**
 * @brief Set movement speed for one servo (0 = max, larger = slower).
 *
 * @note Retained for API compatibility. The driver boards have no speed field
 *       - motion rate comes from the position command stream and the current
 *       cap - so this value is recorded but not sent.
 */
void set_servo_speed(int servo_id, uint16_t speed);

/**
 * @brief Set speed for all 12 servos at once.
 */
void set_all_servo_speed(uint16_t speed);

/**
 * @brief Flush all buffered servo commands to the driver boards
 *        (one SPI frame per board, 12 servos total).
 */
void flush();

/* ── Overlay: add to whatever is already driving the joints ────────────────
 *
 * The four control layers replace one another. A skill that wants "the
 * built-in walk, but with the tail wiggling" has, until now, had to
 * reimplement the walk -- there was no way to add to a frame the gait
 * generator produced. That is a large share of the reasons people fork this
 * firmware instead of writing a skill.
 *
 * The overlay is a per-joint offset in degrees, applied in flush() to the
 * outgoing frame only. It never touches s_goal_pos, so it does not
 * accumulate, and whatever owns the joints -- gait generator, IK, or a skill
 * -- keeps owning them.
 *
 * Deliberately clamped hard. An overlay is a garnish, not a control path: a
 * skill that wants authority over a joint should take it properly. The clamp
 * is what makes it safe to apply on top of a running gait without the two
 * combining into something that tips the robot over.
 *
 * Cleared when a skill ends, however it ends.
 */
constexpr float SERVO_OVERLAY_MAX_DEG = 20.0f;

/** Set one joint's overlay, in degrees. Clamped to +/-SERVO_OVERLAY_MAX_DEG. */
void set_overlay(int servo_id, float deg);

/** Read one joint's overlay back, in degrees. 0 for an invalid id. */
float get_overlay(int servo_id);

/** Drop every overlay. Called by the sandbox when a skill ends. */
void clear_overlay();

// ── Per‑leg IK helpers ───────────────────────────────────────

/**
 * Each leg IK takes three parameters:
 *   x    – forward/backward displacement (mm), positive = forward
 *   th0  – hip rotation (degrees), positive = outward
 *   z    – foot height (mm), measured from hip
 *
 * Leg‑to‑servo mapping
 *   Front Right (FR): servo 1 (hip),  2 (shoulder), 3 (knee)
 *   Front Left  (FL): servo 4 (hip),  5 (shoulder), 6 (knee)
 *   Rear Right  (RR): servo 7 (hip),  8 (shoulder), 9 (knee)
 *   Rear Left   (RL): servo 10 (hip), 11 (shoulder),12 (knee)
 */
void front_right_ik(float x, float th0, float z);
void front_left_ik(float x, float th0, float z);
void rear_right_ik(float x, float th0, float z);
void rear_left_ik(float x, float th0, float z);

}  // namespace robot

// Time-block pump schedule stored in NVS.
//
// Schedule blocks are time windows (start→end) with a target pump RPM and
// an optional salt-water generator relay toggle. Blocks are computed by the
// cloud dashboard (where the user creates their weekly schedule via a UI)
// and pushed to the device through the AWS IoT Device Shadow delta.
//
// Once downloaded, the JSON array is persisted to NVS so the schedule
// survives reboots and runs fully offline — no cloud connection is required
// during execution, only a valid SNTP-synced system clock (checked via
// cloudClockSynced() before any schedule evaluation).
//
// Overnight blocks (start > end, e.g., 22:00–06:00) wrap past midnight
// and are handled with simple comparison logic in pollSchedule().
//
// The schedule is capped at MAX_SCHEDULE_BLOCKS entries. Blocks beyond
// this limit are silently dropped during JSON parsing. A factory reset
// (full NVS erase) clears all stored schedule data.

#pragma once

#include <cstdint>

// A single schedule block: start time, end time, target RPM, and whether
// to engage the salt-water generator relay during this block.
// All time fields are in local time (not UTC). The device's timezone is
// configured via the cloud dashboard and applied server-side before blocks
// are pushed to the device.
struct ScheduleBlock {
  uint8_t startHour;
  uint8_t startMinute;
  uint8_t endHour;
  uint8_t endMinute;
  uint16_t rpm;
  bool swgRelay;
};

// Maximum number of schedule blocks. Chosen to comfortably fit a typical
// daily pool schedule (morning, afternoon, evening blocks) plus a few
// extra. More blocks increase NVS usage and JSON parse time — 6 is plenty
// for a residential pool controller.
static constexpr uint8_t MAX_SCHEDULE_BLOCKS = 6;

// Loads schedule blocks from NVS on boot. If no schedule exists (first boot
// or factory reset), sBlockCount will be 0 and pollSchedule() is a no-op.
// Called once from main during startup, before the cloud service and Matter
// are initialized.
void initializeSchedule();

// Evaluates the schedule against the current wall-clock time. Called from
// the main loop at ~100 Hz. Only acts on block boundary transitions — it
// does not re-issue the same RPM target every tick. Gates on SNTP sync
// (won't run with a bogus clock).
//
// When entering a new block, calls handlePumpRpm() and setRelay() for any
// relay configured as SaltWaterGenerator. When no block is active, logs
// the exit but does NOT turn the pump off (the pump keeps its last setting).
void pollSchedule();

// Receives a new schedule configuration from the cloud shadow delta.
// Persists to NVS immediately so the schedule survives a power loss that
// occurs before the next scheduled NVS commit. Resets the active block
// tracking so the next poll applies the new schedule on its next tick.
void applyScheduleConfig(const ScheduleBlock *blocks, uint8_t count);

// Copies the current schedule blocks into the caller-provided array.
// Returns the number of blocks written. Used by the cloud service to
// include the current schedule in telemetry reports so the dashboard
// can show the active schedule.
uint8_t getScheduleConfig(ScheduleBlock *blocks);

// Returns true if at least one schedule block is configured. Used by the
// Matter module and cloud service to determine whether schedule mode is
// active (schedule blocks override manual pump control).
bool isScheduleActive();

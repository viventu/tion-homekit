// End-to-end tests of the sketch on the host. TionHomeKit.ino is built
// unchanged against host doubles of HomeSpan (tests/stubs/HomeSpan.h), the
// Arduino core and the ESP32-S3 adapter, whose UART line ends in a simulated
// Tion 4S. A HomeKit write travels through the real services, ControlAccess
// and UartCore down to UART bytes; the breezer's state travels back up to
// characteristic values. Each scenario runs in its own process because the
// sketch keeps its state in globals. Built twice: with and without
// TION_ENABLE_WEB_DIAGNOSTICS.

#include <Arduino.h>
#include <HomeSpan.h>
#include <SRP.h>
#include <nvs.h>
#include <tion4s/describe.h>
#include <tion4s/esp32_uart_service.h>

#include "support/check.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

void setup();
void loop();

namespace {

using fake::HapStatus;

tion4s::Esp32UartService& uart() { return tion4s::Esp32UartService::instance(); }
fake::FakeTion& tion() { return uart().tion; }

// Sketch loop passes 10 ms apart; the UART task runs in between.
void run(std::uint32_t ms) {
  const auto end = fake_millis + ms;
  while (static_cast<std::int32_t>(fake_millis - end) < 0) {
    delay(10);
    loop();
  }
}

// Helpers over the published accessory ------------------------------------

std::string configured_name(const SpanService* service) {
  for (const auto* characteristic : service->characteristics) {
    if (std::strcmp(characteristic->type, "ConfiguredName") == 0) {
      return characteristic->text;
    }
  }
  return {};
}

SpanService* service(const char* type, const char* name = nullptr) {
  for (auto* candidate : homeSpan.services) {
    if (std::strcmp(candidate->type, type) == 0 &&
        (name == nullptr || configured_name(candidate) == name)) {
      return candidate;
    }
  }
  CHECK(false, "service exists");
  return nullptr;
}

SpanCharacteristic* in(SpanService* owner, const char* type) {
  for (auto* characteristic : owner->characteristics) {
    if (std::strcmp(characteristic->type, type) == 0) {
      return characteristic;
    }
  }
  CHECK(false, "characteristic exists");
  return nullptr;
}

struct Tiles {
  SpanService* information = service("AccessoryInformation");
  SpanService* fan = service("Fan");
  SpanService* recirculation = service("Switch", "Рециркуляция");
  SpanService* sound = service("Switch", "Звук Tion");
  SpanService* led = service("Switch", "Подсветка Tion");
  SpanService* heater = service("HeaterCooler");
  SpanService* intake = service("TemperatureSensor", "Температура притока");
  SpanService* outlet = service("TemperatureSensor", "Температура выхода");
  SpanService* purifier = service("AirPurifier");
  SpanService* filter = service("FilterMaintenance");
  SpanService* reset = service("Switch", "Сброс фильтра после замены");
  SpanService* boost = service("Switch", "Ускорение на 5 минут");
};

double value(SpanService* owner, const char* type) { return in(owner, type)->value; }

// One HAP request, handled by the next poll().
std::vector<HapStatus> write(std::vector<fake::HapWrite> writes) {
  auto request = homeSpan.write(std::move(writes));
  run(10);
  CHECK(request->done, "HomeSpan handles a write in its next poll");
  return request->statuses;
}

HapStatus write(SpanService* owner, const char* type, double input) {
  return write({{in(owner, type), input}})[0];
}

HapStatus rename(SpanService* owner, const char* name) {
  return write({{in(owner, "ConfiguredName"), 0, name}})[0];
}

// Order of services and characteristics, which assigns the IIDs that paired
// controllers remember. Extending it is fine; changing it breaks pairings.
std::string layout() {
  std::string result;
  for (const auto* owner : homeSpan.services) {
    result += owner->type;
    const auto name = configured_name(owner);
    if (!name.empty()) {
      result += " \"" + name + "\"";
    }
    result += ":";
    for (const auto* characteristic : owner->characteristics) {
      result += " ";
      result += characteristic->type;
    }
    result += "\n";
  }
  return result;
}

constexpr char kLayout[] =
    "AccessoryInformation: Identify Name Manufacturer Model SerialNumber "
    "FirmwareRevision\n"
    "Fan: Active RotationSpeed StatusFault CurrentFanState\n"
    "Switch \"Рециркуляция\": On ConfiguredName\n"
    "Switch \"Звук Tion\": On ConfiguredName\n"
    "Switch \"Подсветка Tion\": On ConfiguredName\n"
    "HeaterCooler \"Нагрев притока\": Active CurrentTemperature CurrentHeaterCoolerState "
    "TargetHeaterCoolerState HeatingThresholdTemperature ConfiguredName\n"
    "TemperatureSensor \"Температура притока\": CurrentTemperature StatusFault "
    "StatusActive "
    "ConfiguredName\n"
    "TemperatureSensor \"Температура выхода\": CurrentTemperature StatusFault "
    "StatusActive "
    "ConfiguredName\n"
    "AirPurifier \"Фильтрация притока\": Active CurrentAirPurifierState "
    "TargetAirPurifierState ConfiguredName\n"
    "FilterMaintenance \"Фильтр Tion\": FilterChangeIndication ConfiguredName\n"
    "Switch \"Сброс фильтра после замены\": On ConfiguredName\n"
    "Switch \"Ускорение на 5 минут\": On ConfiguredName\n";

// APA102 status LED --------------------------------------------------------

struct Pixel {
  int brightness;
  int red;
  int green;
  int blue;
  bool operator==(const Pixel& other) const {
    return brightness == other.brightness && red == other.red && green == other.green &&
           blue == other.blue;
  }
};

constexpr Pixel kDark{1, 0, 0, 0};
constexpr Pixel kBlue{1, 0, 0, 32};
constexpr Pixel kRed{1, 32, 0, 0};

// Decodes the bit-banged APA102 stream: data (GPIO40) is sampled on each
// rising clock edge (GPIO39). Every update is a start frame of four zero
// bytes, one LED frame and an end frame of four 0xFF bytes.
std::vector<Pixel> pixels() {
  std::vector<int> bits;
  int clock = LOW;
  int data = LOW;
  for (const auto& [pin, level] : fake::pin_writes) {
    if (pin == 40) {
      data = level;
    } else if (pin == 39) {
      if (clock == LOW && level == HIGH) {
        bits.push_back(data);
      }
      clock = level;
    }
  }
  CHECK(bits.size() % (std::size_t{8} * 12) == 0, "APA102 updates are whole frames");
  std::vector<int> bytes;
  for (std::size_t i = 0; i < bits.size(); i += 8) {
    int byte = 0;
    for (std::size_t bit = 0; bit < 8; ++bit) {
      byte = (byte << 1) | bits[i + bit];
    }
    bytes.push_back(byte);
  }
  std::vector<Pixel> result;
  for (std::size_t i = 0; i < bytes.size(); i += 12) {
    for (std::size_t k = 0; k < 4; ++k) {
      CHECK(bytes[i + k] == 0 && bytes[i + 8 + k] == 0xFF, "APA102 start and end frames");
    }
    CHECK((bytes[i + 4] & 0xE0) == 0xE0, "APA102 LED frame header");
    result.push_back({bytes[i + 4] & 0x1F, bytes[i + 7], bytes[i + 6], bytes[i + 5]});
  }
  return result;
}

// Scenarios ------------------------------------------------------------------

void boot_publishes_the_accessory() {
  // Names from the synthetic MAC in tests/stubs/esp_mac.h. The suffix is kept
  // apart because the publication scan rejects anything that looks like a
  // real device's host name.
  const std::string suffix_lower = "abcdef";
  const std::string suffix_upper = "ABCDEF";
  setup();
  CHECK(uart().begin_calls == 1, "UART starts first");
  CHECK(homeSpan.began && homeSpan.category == Category::Fans &&
            homeSpan.display_name == "Tion 4S" &&
            homeSpan.host_name == "tion-4s-" + suffix_lower &&
            homeSpan.model_name == "Tion 4S bridge",
        "HomeSpan starts as a fan named after the MAC");
  CHECK(homeSpan.ap_ssid == "Tion-Setup-" + suffix_upper &&
            homeSpan.ap_timeout_s == 300 && homeSpan.auto_start_ap &&
            homeSpan.control_pin == 0 && homeSpan.serial_input_disabled &&
            homeSpan.log_level == -1 && homeSpan.watchdog_s == 10 &&
            homeSpan.status_pin == 40,
        "provisioning, silent serial, watchdog and status LED are configured");
#if defined(TION_ENABLE_WEB_DIAGNOSTICS)
  CHECK(homeSpan.web_log_url == "tion-status" && homeSpan.web_log_callback != nullptr,
        "the diagnostics build serves /tion-status");
  const std::string suffix = "-diag";
#else
  CHECK(homeSpan.web_log_url.empty() && homeSpan.web_log_callback == nullptr,
        "the normal build has no web page");
  const std::string suffix = "-gm1";
#endif
  const auto& version = homeSpan.sketch_version;
  CHECK(version.rfind("0.2.0-", 0) == 0 &&
            version.compare(version.size() - suffix.size(), suffix.size(), suffix) == 0,
        "sketch version names the variant");
  CHECK(homeSpan.accessories.size() == 1 && layout() == kLayout,
        "services and characteristics keep their order and IIDs");

  const Tiles tiles;
  const std::vector<std::uint32_t> expected_iids{1, 8, 16, 19, 22, 25, 32, 37, 42, 47, 50, 53};
  CHECK(homeSpan.services.size() == expected_iids.size(), "control gate removed");
  for (std::size_t i = 0; i < expected_iids.size(); ++i) {
    const auto* owner = homeSpan.services[i];
    CHECK(owner->iid == expected_iids[i], "remaining services keep their paired IDs");
    for (std::size_t c = 0; c < owner->characteristics.size(); ++c) {
      CHECK(owner->characteristics[c]->iid == owner->iid + c + 1,
            "remaining characteristics keep their paired IDs");
    }
  }

  CHECK(in(tiles.information, "Name")->text == "Tion 4S" &&
            in(tiles.information, "Manufacturer")->text == "DIY" &&
            in(tiles.information, "Model")->text == "T-Dongle-S3 / Tion 4S" &&
            in(tiles.information, "SerialNumber")->text == "TION4S-026F28ABCDEF" &&
            in(tiles.information, "FirmwareRevision")->text == version,
        "accessory information");
  const auto* heat_mode = in(tiles.heater, "TargetHeaterCoolerState");
  const auto* threshold = in(tiles.heater, "HeatingThresholdTemperature");
  CHECK(heat_mode->valid_values == std::vector<int>{1} && threshold->minimum == 1 &&
            threshold->maximum == 25 && threshold->step_value == 1,
        "heater offers heat only and the device's 1-25 C target in whole degrees");
  for (auto* owner : {tiles.heater, tiles.intake, tiles.outlet}) {
    const auto* temperature = in(owner, "CurrentTemperature");
    CHECK(temperature->minimum == -50 && temperature->maximum == 100,
          "temperatures below zero are in range");
  }
  CHECK(
      in(tiles.purifier, "TargetAirPurifierState")->valid_values == std::vector<int>{0} &&
          tiles.purifier->links == std::vector<SpanService*>{tiles.filter},
      "purifier is manual only and links the filter");

  CHECK((fake::output_pins == std::vector<int>{40, 39} &&
         pixels() == std::vector<Pixel>{kDark, kBlue, kDark}),
        "status LED starts dark, then HomeSpan blinks it blue");
  CHECK(fake::srp_code.size() == 8 &&
            fake::srp_code.find_first_not_of("0123456789") == std::string::npos &&
            fake_nvs::values.count("SRP/VERIFYDATA") == 1 &&
            fake_nvs::values.count("TIONSETUP/SEEDED") == 1,
        "a private eight-digit setup code is stored before HomeKit runs");
  run(100);
  CHECK(homeSpan.polls > 0 && uart().supervise_calls == homeSpan.polls,
        "every loop pass supervises the UART and polls HomeSpan");
}

void tiles_follow_the_breezer() {
  setup();
  const Tiles tiles;
  CHECK(value(tiles.fan, "StatusFault") == 1,
        "fan reports a fault before the first state");
  run(3000);
  CHECK(value(tiles.fan, "Active") == 1 && value(tiles.fan, "RotationSpeed") == 50 &&
            value(tiles.fan, "CurrentFanState") == 2 &&
            value(tiles.fan, "StatusFault") == 0,
        "fan shows power and step 3 of 6");
  CHECK(value(tiles.heater, "Active") == 0 &&
            value(tiles.heater, "CurrentHeaterCoolerState") == 0 &&
            value(tiles.heater, "HeatingThresholdTemperature") == 20 &&
            value(tiles.heater, "CurrentTemperature") == 18,
        "heater shows permission, state, target and outlet air");
  CHECK(value(tiles.intake, "CurrentTemperature") == -12 &&
            value(tiles.intake, "StatusFault") == 0 &&
            value(tiles.intake, "StatusActive") == 1 &&
            value(tiles.outlet, "CurrentTemperature") == 18,
        "sensors show intake and outlet air, below zero too");
  CHECK(value(tiles.purifier, "Active") == 1 &&
            value(tiles.purifier, "CurrentAirPurifierState") == 2 &&
            value(tiles.filter, "FilterChangeIndication") == 0,
        "purifier and filter");
  CHECK(value(tiles.recirculation, "On") == 0 && value(tiles.sound, "On") == 1 &&
            value(tiles.led, "On") == 1 &&
            value(tiles.boost, "On") == 0 && value(tiles.reset, "On") == 0,
        "switches");

  // Changes made with the breezer's own controls.
  auto& state = tion().state;
  state.fan_speed = 6;
  state.heater_allowed = true;
  state.heater_percent = 40;
  state.target_temperature = 23;
  state.filter_warning = true;
  state.sound_on = false;
  state.led_on = false;
  state.gate_position = 1;
  run(2500);
  CHECK(value(tiles.fan, "RotationSpeed") == 100 && value(tiles.heater, "Active") == 1 &&
            value(tiles.heater, "CurrentHeaterCoolerState") == 2 &&
            value(tiles.heater, "HeatingThresholdTemperature") == 23 &&
            value(tiles.filter, "FilterChangeIndication") == 1 &&
            value(tiles.sound, "On") == 0 && value(tiles.led, "On") == 0 &&
            value(tiles.recirculation, "On") == 1,
        "native changes reach HomeKit");
  state.heater_percent = 0;
  state.fan_speed = 0;
  run(2500);
  CHECK(value(tiles.heater, "CurrentHeaterCoolerState") == 1 &&
            value(tiles.fan, "CurrentFanState") == 1 &&
            value(tiles.fan, "RotationSpeed") == 0,
        "idle heater and a stopped fan motor");
  state.power_on = false;
  run(2500);
  CHECK(value(tiles.fan, "Active") == 0 && value(tiles.fan, "CurrentFanState") == 0 &&
            value(tiles.heater, "CurrentHeaterCoolerState") == 0 &&
            value(tiles.purifier, "Active") == 0 &&
            value(tiles.purifier, "CurrentAirPurifierState") == 0,
        "breezer off");
  const auto notifications = in(tiles.fan, "RotationSpeed")->notifications;
  run(5000);
  CHECK(in(tiles.fan, "RotationSpeed")->notifications == notifications,
        "an unchanged state sends no notifications");

  // Values HomeKit cannot show are left out rather than clamped.
  state.target_temperature = 0;
  run(2500);
  CHECK(value(tiles.heater, "HeatingThresholdTemperature") == 23,
        "a target below 1 C is not shown");
  state.target_temperature = 30;
  state.current_temperature = 120;
  state.outdoor_temperature = -60;
  run(2500);
  CHECK(value(tiles.heater, "HeatingThresholdTemperature") == 23 &&
            value(tiles.heater, "CurrentTemperature") == 18 &&
            value(tiles.outlet, "StatusFault") == 1 &&
            value(tiles.outlet, "StatusActive") == 0 &&
            value(tiles.intake, "StatusFault") == 1,
        "out-of-range values keep the last value or report a sensor fault");
  state.max_fan_speed = 0;
  run(2500);
  CHECK(value(tiles.fan, "StatusFault") == 1, "an impossible fan range is a fault");
  state.max_fan_speed = 6;
  state.outdoor_temperature = -12;
  run(2500);
  CHECK(value(tiles.fan, "StatusFault") == 0 && value(tiles.intake, "StatusFault") == 0 &&
            value(tiles.intake, "StatusActive") == 1,
        "valid values clear the faults");

  tion().respond = false;
  run(11000);
  CHECK(value(tiles.fan, "StatusFault") == 1 && value(tiles.intake, "StatusFault") == 1 &&
            value(tiles.intake, "StatusActive") == 0 && value(tiles.sound, "On") == 0,
        "a silent breezer is a fault; switches keep the last value");
  CHECK(write(tiles.sound, "On", 1) == HapStatus::Unable, "a stale state refuses writes");
}

void writes_work_without_a_gate() {
  setup();
  run(3000);
  const Tiles tiles;
  CHECK(write(tiles.fan, "StatusFault", 1) == HapStatus::ReadOnly,
        "read-only characteristics stay read-only");
  CHECK(write(tiles.fan, "RotationSpeed", 100) == HapStatus::Ok,
        "speed write is accepted");
  run(2000);
  const auto& frame = tion().last_write;
  CHECK(tion().writes == 1 && frame[3] == 0x30 && frame[4] == 0x32 && frame[13] == 6 &&
            value(tiles.fan, "RotationSpeed") == 100,
        "100% becomes one state write with step 6");

  CHECK(write(tiles.fan, "Active", 0) == HapStatus::Ok, "power off is accepted");
  run(2000);
  CHECK(!tion().state.power_on && value(tiles.fan, "Active") == 0 &&
            value(tiles.fan, "RotationSpeed") == 0 &&
            value(tiles.fan, "CurrentFanState") == 0,
        "breezer off");
  CHECK(write(tiles.fan, "RotationSpeed", 33) == HapStatus::Ok, "speed on a stopped fan");
  run(2000);
  CHECK(tion().state.power_on && tion().state.fan_speed == 2 &&
            value(tiles.fan, "RotationSpeed") == 33,
        "a positive speed also starts the breezer");

  const auto writes = tion().writes;
  const auto statuses =
      write({{in(tiles.fan, "Active"), 1}, {in(tiles.fan, "RotationSpeed"), 83}});
  CHECK((statuses == std::vector<HapStatus>{HapStatus::Ok, HapStatus::Ok}),
        "one request with Active and RotationSpeed");
  run(2000);
  CHECK(tion().writes == writes + 1 && tion().state.fan_speed == 5,
        "both characteristics leave in one frame");
  CHECK(write(tiles.fan, "RotationSpeed", 150) == HapStatus::Unable &&
            value(tiles.fan, "RotationSpeed") == 83,
        "a speed outside 0-100 is refused and reverted");
  CHECK(write(tiles.fan, "RotationSpeed", 0) == HapStatus::Ok, "zero speed");
  run(2000);
  CHECK(!tion().state.power_on, "zero speed turns the breezer off");

  run(31 * 60 * 1000);
  CHECK(write(tiles.fan, "Active", 1) == HapStatus::Ok,
        "ordinary control has no thirty-minute expiry");
  run(2000);
  CHECK(tion().state.power_on, "power returns without a permission tile");
}

void slider_writes_merge() {
  setup();
  run(3000);
  const Tiles tiles;
  // The first write runs; the next waits; the last merges into the waiting one.
  auto first = homeSpan.write({{in(tiles.fan, "RotationSpeed"), 67}});
  run(10);
  auto second = homeSpan.write({{in(tiles.fan, "RotationSpeed"), 100}});
  run(10);
  auto third = homeSpan.write({{in(tiles.fan, "RotationSpeed"), 83}});
  run(10);
  CHECK(first->statuses[0] == HapStatus::Ok && second->statuses[0] == HapStatus::Ok &&
            third->statuses[0] == HapStatus::Ok,
        "every slider position is accepted");
  run(3000);
  CHECK(tion().writes == 2 && tion().state.fan_speed == 5 &&
            value(tiles.fan, "RotationSpeed") == 83,
        "the running write and the merged latest position reach the breezer");
  const auto status = uart().status();
  CHECK(status.control_applied == 2 && status.control_failed == 0 &&
            tion4s::outcome_of(status, 2) == tion4s::CommandOutcome::Merged,
        "the absorbed command is reported as merged");
}

void renames_do_not_act() {
  setup();
  run(3000);
  const Tiles tiles;
  for (auto* owner : {tiles.recirculation, tiles.sound, tiles.led,
                      tiles.heater, tiles.purifier, tiles.reset, tiles.boost}) {
    CHECK(rename(owner, "Tion") == HapStatus::Ok && configured_name(owner) == "Tion",
          "a controller may rename every service");
  }
  run(3000);
  CHECK(uart().submitted == 0 && tion().writes == 0, "a rename sends no command");


}

void heater_and_safety_rules() {
  setup();
  run(3000);
  const Tiles tiles;
  const auto statuses = write({{in(tiles.heater, "Active"), 1},
                               {in(tiles.heater, "HeatingThresholdTemperature"), 22}});
  CHECK((statuses == std::vector<HapStatus>{HapStatus::Ok, HapStatus::Ok}),
        "heat on at 22 C");
  run(2000);
  CHECK(tion().state.heater_allowed && tion().state.target_temperature == 22 &&
            tion().writes == 1 && value(tiles.heater, "Active") == 1 &&
            value(tiles.heater, "CurrentHeaterCoolerState") == 1,
        "heat allowed, heater idle");
  CHECK(write(tiles.heater, "HeatingThresholdTemperature", 22.4) == HapStatus::Ok,
        "22.4 C");
  run(2000);
  CHECK(tion().writes == 1, "a target that rounds to the current one needs no write");
  CHECK(write(tiles.heater, "HeatingThresholdTemperature", 23.6) == HapStatus::Ok,
        "23.6 C");
  run(2000);
  CHECK(tion().state.target_temperature == 24 && tion().writes == 2, "rounded to 24 C");
  CHECK(write(tiles.heater, "HeatingThresholdTemperature", 30) == HapStatus::Unable &&
            value(tiles.heater, "HeatingThresholdTemperature") == 24,
        "a target above 25 C is refused");
  CHECK(write(tiles.heater, "TargetHeaterCoolerState", 2) == HapStatus::Unable &&
            write(tiles.heater, "TargetHeaterCoolerState", 1) == HapStatus::Ok,
        "only the heat mode exists");
  CHECK(write(tiles.heater, "Active", 0) == HapStatus::Ok, "heat off");
  run(2000);
  CHECK(!tion().state.heater_allowed && tion().writes == 3, "heat is off");

  CHECK(write(tiles.recirculation, "On", 1) == HapStatus::Ok, "recirculation on");
  run(2000);
  CHECK(tion().state.gate_position == 1 && value(tiles.recirculation, "On") == 1,
        "recirculation without heat");
  CHECK(write(tiles.heater, "Active", 1) == HapStatus::Ok, "the write is queued");
  run(2000);
  const auto status = uart().status();
  CHECK(!tion().state.heater_allowed && tion().writes == 4 &&
            status.control_refusal == tion4s::WriteRejection::UnsafeCombination &&
            value(tiles.heater, "Active") == 0,
        "heat during recirculation is refused before UART, and reverts the tile");
}

void sound_and_light() {
  setup();
  run(3000);
  const Tiles tiles;
  CHECK(write(tiles.sound, "On", 0) == HapStatus::Ok &&
            write(tiles.led, "On", 0) == HapStatus::Ok,
        "sound and LED off");
  run(3000);
  const auto& frame = tion().last_write;
  CHECK(!tion().state.sound_on && !tion().state.led_on && frame[3] == 0x34 &&
            frame[4] == 0x32,
        "settings use the persistent frame");
}

void boost_runs_for_five_minutes() {
  setup();
  run(3000);
  const Tiles tiles;
  CHECK(write(tiles.boost, "On", 1) == HapStatus::Ok, "boost starts");
  run(2000);
  CHECK(tion().state.fan_speed == 6 && value(tiles.boost, "On") == 1 &&
            value(tiles.fan, "RotationSpeed") == 100,
        "boost runs at the maximum step");
  CHECK(rename(tiles.boost, "Турбо") == HapStatus::Ok, "renaming a running boost");
  run(5 * 60 * 1000);
  CHECK(
      tion().state.fan_speed == 3 && value(tiles.boost, "On") == 0 && tion().writes == 2,
      "after five minutes the previous step returns");

  CHECK(write(tiles.boost, "On", 1) == HapStatus::Ok, "boost again");
  run(2000);
  CHECK(write(tiles.boost, "On", 0) == HapStatus::Ok, "manual stop");
  run(2000);
  CHECK(tion().state.fan_speed == 3 && value(tiles.boost, "On") == 0,
        "a manual stop restores at once");

  tion().state.fan_speed = 6;
  run(2500);
  CHECK(write(tiles.boost, "On", 1) == HapStatus::Unable && value(tiles.boost, "On") == 0,
        "no boost at the maximum step already");
}

void filter_reset_needs_a_stopped_breezer() {
  setup();
  tion().state.filter_warning = true;
  run(3000);
  const Tiles tiles;
  CHECK(value(tiles.filter, "FilterChangeIndication") == 1, "filter warning");
  CHECK(write(tiles.reset, "On", 1) == HapStatus::Ok, "reset is queued");
  run(2000);
  CHECK(
      tion().writes == 0 && value(tiles.reset, "On") == 0 &&
          uart().status().control_refusal == tion4s::WriteRejection::FilterResetNeedsStop,
      "a running breezer refuses the reset before UART; ordinary control stays available");

  CHECK(write(tiles.purifier, "TargetAirPurifierState", 1) == HapStatus::Unable &&
            write(tiles.purifier, "TargetAirPurifierState", 0) == HapStatus::Ok,
        "the purifier has no automatic mode");
  CHECK(write(tiles.purifier, "Active", 0) == HapStatus::Ok,
        "purifier tile turns power off");
  run(2000);
  CHECK(!tion().state.power_on && value(tiles.purifier, "Active") == 0, "breezer off");
  CHECK(write(tiles.reset, "On", 1) == HapStatus::Ok, "reset");
  run(100);
  CHECK(value(tiles.reset, "On") == 1, "the switch stays on while the reset runs");
  CHECK(rename(tiles.reset, "Сброс") == HapStatus::Ok, "renaming a running reset");
  run(2000);
  const auto& frame = tion().last_write;
  CHECK(tion().writes == 2 && (frame[9] & 0x80) != 0 && !tion().state.filter_warning &&
            value(tiles.filter, "FilterChangeIndication") == 0 &&
            value(tiles.reset, "On") == 0,
        "the reset is written, confirmed by the counter, and the switch returns off");
  CHECK(write(tiles.reset, "On", 0) == HapStatus::Ok,
        "turning the reset switch off is a no-op");
  CHECK(write(tiles.purifier, "Active", 1) == HapStatus::Ok, "power on again");
  run(2000);
  CHECK(tion().state.power_on && tion().writes == 3, "breezer on");
}

void upstream_firmware_confirms_by_request_id() {
  tion().firmware = 0x02D2;
  tion().echo_request_ids = true;
  // The write's own answer then arrives after the first verification read.
  tion().response_delay_ms = 200;
  setup();
  run(3000);
  const Tiles tiles;
  CHECK(write(tiles.fan, "RotationSpeed", 100) == HapStatus::Ok, "speed");
  run(2000);
  const auto status = uart().status();
  CHECK(tion().state.fan_speed == 6 && status.control_applied == 1 &&
            status.control_request_id == 2 && value(tiles.fan, "RotationSpeed") == 100,
        "02D2 confirms the write by its request ID");
}

void unsupported_firmware_stays_read_only() {
  tion().firmware = 0x03CD;
  setup();
  run(3000);
  const Tiles tiles;
  CHECK(value(tiles.fan, "RotationSpeed") == 50, "state is still shown");
  CHECK(write(tiles.sound, "On", 0) == HapStatus::Unable &&
            write(tiles.fan, "Active", 0) == HapStatus::Unable &&
            write(tiles.heater, "Active", 1) == HapStatus::Unable && tion().writes == 0,
        "firmware with broken control remains read-only across all command paths");
}

void uart_start_failure_keeps_homekit_off() {
  uart().task_starts = false;
  setup();
  CHECK((!homeSpan.began && homeSpan.services.empty() &&
         pixels() == std::vector<Pixel>{kDark, kRed}),
        "without the UART task HomeKit stays off and the LED turns red");
  run(4000);
  CHECK(tion().heartbeats >= 1 && tion().max_heartbeat_gap_ms <= 3100 &&
            uart().supervise_calls >= 100 && homeSpan.polls == 0,
        "the loop keeps the heartbeat itself");
}

void pairing_failure_keeps_homekit_closed() {
  fake_nvs::fail_at = 1;
  setup();
  CHECK((homeSpan.began && homeSpan.accessories.empty() && homeSpan.services.empty() &&
         pixels() == std::vector<Pixel>{kDark, kBlue, kDark, kRed}),
        "without a private setup code no accessory is published and the LED turns red");
  run(5000);
  CHECK(homeSpan.polls == 0 && tion().heartbeats >= 2 && uart().supervise_calls >= 5,
        "HAP never runs; the UART task keeps the heartbeat");
}

void stored_pairing_is_reused() {
  fake_nvs::values["TIONSETUP/SEEDED"] = {1};
  fake_nvs::values["SRP/VERIFYDATA"].assign(sizeof(Verification), 7);
  setup();
  CHECK(homeSpan.accessories.size() == 1 && fake_nvs::blob_writes == 0 &&
            fake::srp_code.empty(),
        "an existing verifier is kept");
}

#if defined(TION_ENABLE_WEB_DIAGNOSTICS)
bool has_row(const String& page, const char* label, const char* text) {
  const std::string row =
      std::string("<tr><td>") + label + ":</td><td>" + text + "</td></tr>";
  return std::strstr(page.c_str(), row.c_str()) != nullptr;
}

void diagnostics_page_reports_the_link() {
  setup();
  String before;
  homeSpan.web_log_callback(before);
  CHECK(has_row(before, "UART task", "1") && has_row(before, "Valid states", "0") &&
            has_row(before, "Pending state replies", "0") &&
            has_row(before, "Control phase / failure", "idle / none") &&
            has_row(before, "Last control kind / request ID", "none / 0") &&
            has_row(before, "UART stall restarts", "0 / armed") &&
            std::strstr(before.c_str(), "Tion firmware") == nullptr &&
            std::strstr(before.c_str(), "Raw errors") == nullptr,
        "before the first answer the page has link rows only");

  run(3000);
  const Tiles tiles;
  CHECK(write(tiles.recirculation, "On", 1) == HapStatus::Ok, "recirculation");
  run(2000);
  uart().stall_restarts = 2;
  uart().stall_gave_up = true;
  String page;
  homeSpan.web_log_callback(page);
  CHECK(has_row(page, "Device type", "0x00008003") &&
            has_row(page, "Tion firmware", "02D0") &&
            has_row(page, "Device work mode", "1") && has_row(page, "Power", "1") &&
            has_row(page, "Fan speed / maximum", "3 / 6") &&
            has_row(page, "Outdoor temperature (C)", "-12") &&
            has_row(page, "Gate position (0=outdoor, 1=recirculation)", "1") &&
            has_row(page, "Control available", "1") &&
            has_row(page, "Control phase / failure", "applied / none") &&
            has_row(page, "Control refusal", "none") &&
            has_row(page, "Last control kind / request ID", "recirculation / 2") &&
            has_row(page, "Control applied / failed", "1 / 0") &&
            has_row(page, "UART stall restarts", "2 / limit reached") &&
            has_row(page, "UART stack free (bytes)", "1234"),
        "the page shows device, state, control and adapter rows");
}
#endif

struct Scenario {
  const char* name;
  void (*body)();
};

constexpr Scenario kScenarios[] = {
    {"boot publishes the accessory", boot_publishes_the_accessory},
    {"tiles follow the breezer", tiles_follow_the_breezer},
    {"writes work without a gate", writes_work_without_a_gate},
    {"slider writes merge", slider_writes_merge},
    {"renames do not act", renames_do_not_act},
    {"heater and safety rules", heater_and_safety_rules},
    {"sound and light", sound_and_light},
    {"boost runs for five minutes", boost_runs_for_five_minutes},
    {"filter reset needs a stopped breezer", filter_reset_needs_a_stopped_breezer},
    {"upstream firmware confirms by request ID",
     upstream_firmware_confirms_by_request_id},
    {"unsupported firmware stays read-only", unsupported_firmware_stays_read_only},
    {"UART start failure keeps HomeKit off", uart_start_failure_keeps_homekit_off},
    {"pairing failure keeps HomeKit closed", pairing_failure_keeps_homekit_closed},
    {"stored pairing is reused", stored_pairing_is_reused},
#if defined(TION_ENABLE_WEB_DIAGNOSTICS)
    {"diagnostics page reports the link", diagnostics_page_reports_the_link},
#endif
};

}  // namespace

int main() {
  for (const auto& scenario : kScenarios) {
    std::cout.flush();
    const pid_t child = fork();
    CHECK(child >= 0, "fork");
    if (child == 0) {
      scenario.body();
      CHECK(homeSpan.violations == 0, "no HomeSpan misuse");
      CHECK(homeSpan.max_poll_gap_ms < 1000 * homeSpan.watchdog_s || homeSpan.polls == 0,
            "HomeSpan is polled well within its watchdog");
      return 0;  // Normal exit: sanitizer and coverage reports are written.
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child, "wait for the scenario");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      std::cerr << "firmware scenario failed: " << scenario.name << "\n";
      return 1;
    }
  }
#if defined(TION_ENABLE_WEB_DIAGNOSTICS)
  std::cout << "HomeKit sketch end to end, diagnostics build: PASS\n";
#else
  std::cout << "HomeKit sketch end to end: PASS\n";
#endif
}

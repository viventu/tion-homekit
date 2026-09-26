#pragma once

// Host double of the HomeSpan 2.1.8 API the sketch uses. It keeps the rules
// the sketch relies on, taken from HomeSpan's Span::updateCharacteristics():
//  - services and characteristics register in creation order; that order
//    assigns the IIDs a paired controller remembers;
//  - a characteristic belongs to the service created last;
//  - a HAP write loads new values, calls the service's update() once, then
//    commits the values when it returns true and reverts them otherwise;
//  - read-only characteristics refuse writes; ranges are not enforced on
//    writes, only reported to the controller;
//  - setVal() inside update() on a characteristic being written is an error.
// Writes are queued by tests and processed in poll(), as on the device.

#include <Arduino.h>

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class Blinkable {
 public:
  virtual void on() = 0;
  virtual void off() = 0;
  virtual int getPin() = 0;

 protected:
  ~Blinkable() = default;
};

enum class Category : std::uint8_t { Other = 1, Bridges = 2, Fans = 3 };

struct SpanCharacteristic;

class SpanService {
 public:
  explicit SpanService(const char* service_type);
  virtual ~SpanService() = default;
  SpanService(const SpanService&) = delete;
  SpanService& operator=(const SpanService&) = delete;
  SpanService(SpanService&&) = delete;
  SpanService& operator=(SpanService&&) = delete;

  virtual boolean update() { return true; }
  virtual void loop() {}
  SpanService* addLink(SpanService* service) {
    links.push_back(service);
    return this;
  }

  // Host inspection.
  std::uint32_t iid = 0;
  const char* type;
  std::vector<SpanCharacteristic*> characteristics;
  std::vector<SpanService*> links;
};

class SpanAccessory {
 public:
  explicit SpanAccessory(std::uint32_t aid = 0);
};

enum class Format : std::uint8_t { Bool, Uint8, Float, String };

struct SpanCharacteristic {
  SpanCharacteristic(const char* characteristic_type, Format value_format, bool can_write,
                     double minimum_value, double maximum_value, double initial);
  SpanCharacteristic(const SpanCharacteristic&) = delete;
  SpanCharacteristic& operator=(const SpanCharacteristic&) = delete;
  SpanCharacteristic(SpanCharacteristic&&) = delete;
  SpanCharacteristic& operator=(SpanCharacteristic&&) = delete;
  ~SpanCharacteristic() = default;

  template <class T = int>
  T getVal() const {
    return static_cast<T>(value);
  }
  template <class T = int>
  T getNewVal() const {
    return static_cast<T>(new_value);
  }
  template <typename T>
  void setVal(T input, boolean notify = true);
  boolean updated() const { return updating; }

  template <typename A, typename B, typename S = int>
  SpanCharacteristic* setRange(A min, B max, S step = 0) {
    minimum = static_cast<double>(min);
    maximum = static_cast<double>(max);
    if (step > 0) {
      step_value = static_cast<double>(step);
    }
    return this;
  }

  // HomeSpan's own C-style signature.
  SpanCharacteristic* setValidValues(int count, ...) {  // NOLINT(cert-dcl50-cpp)
    va_list values;
    va_start(values, count);
    valid_values.clear();
    for (int i = 0; i < count; ++i) {
      valid_values.push_back(va_arg(values, int));
    }
    va_end(values);
    return this;
  }

  // Whether HomeSpan's loadUpdate() would accept the written JSON value.
  bool accepts(double number, const char* string) const {
    switch (format) {
      case Format::Bool:
        return string == nullptr && (number == 0 || number == 1);
      case Format::Uint8:
        return string == nullptr && number >= 0 && number <= 255 &&
               std::floor(number) == number;
      case Format::Float:
        return string == nullptr && std::isfinite(number);
      case Format::String:
        break;
    }
    return string != nullptr;
  }

  // Host inspection.
  std::uint32_t iid = 0;
  const char* type;
  Format format;
  bool writable;
  double minimum;
  double maximum;
  double step_value = 0;
  std::vector<int> valid_values;
  std::string text;
  std::string new_text;
  double value;
  double new_value;
  bool updating = false;
  unsigned notifications = 0;
  SpanService* service = nullptr;
};

namespace fake {

enum class HapStatus : std::uint8_t { Pending, Ok, Unable, ReadOnly, InvalidValue };

// A number, or a string for string characteristics such as ConfiguredName.
struct HapWrite {
  SpanCharacteristic* characteristic;
  double value = 0;
  const char* text = nullptr;
};

struct HapRequest {
  std::vector<HapWrite> writes;
  std::vector<HapStatus> statuses;
  bool done = false;
};

}  // namespace fake

class Span {
 public:
  Span& setStatusDevice(Blinkable* device) {
    status_device = device;
    return *this;
  }
  Span& setControlPin(std::uint8_t pin) {
    control_pin = pin;
    return *this;
  }
  Span& setSerialInputDisable(boolean disabled) {
    serial_input_disabled = disabled;
    return *this;
  }
  Span& setLogLevel(int level) {
    log_level = level;
    return *this;
  }
  Span& setApSSID(const char* ssid) {
    ap_ssid = ssid;
    return *this;
  }
  Span& setApTimeout(std::uint16_t seconds) {
    ap_timeout_s = seconds;
    return *this;
  }
  Span& enableAutoStartAP() {
    auto_start_ap = true;
    return *this;
  }
  Span& setSketchVersion(const char* version) {
    sketch_version = version;
    return *this;
  }
  Span& enableWebLog(std::uint16_t max_entries = 0, const char* server = nullptr,
                     const char* zone = "UTC", const char* url = "status") {
    static_cast<void>(max_entries);
    static_cast<void>(server);
    static_cast<void>(zone);
    web_log_url = url;
    return *this;
  }
  Span& setWebLogCallback(void (*callback)(String&)) {
    web_log_callback = callback;
    return *this;
  }
  Span& enableWatchdog(std::uint16_t seconds) {
    watchdog_s = seconds;
    return *this;
  }

  Span& resetIID(std::uint32_t new_iid) {
    iid_count = new_iid - 1;
    return *this;
  }

  void begin(Category id, const char* name, const char* host, const char* model) {
    began = true;
    category = id;
    display_name = name;
    host_name = host;
    model_name = model;
    // HomeSpan's Blinker reads the pin and blinks the status device.
    if (status_device != nullptr) {
      status_pin = status_device->getPin();
      status_device->on();
      status_device->off();
    }
  }

  void poll() {
    if (!began) {
      ++violations;
    }
    if (polls != 0 && millis() - last_poll_ms > max_poll_gap_ms) {
      max_poll_gap_ms = millis() - last_poll_ms;
    }
    last_poll_ms = millis();
    ++polls;
    while (!pending.empty()) {
      auto request = pending.front();
      pending.pop_front();
      process(*request);
    }
    for (auto* service : services) {
      service->loop();
    }
  }

  // Host side --------------------------------------------------------------
  // Queues one HAP PUT /characteristics request for the next poll().
  std::shared_ptr<fake::HapRequest> write(std::vector<fake::HapWrite> writes) {
    auto request = std::make_shared<fake::HapRequest>();
    request->writes = std::move(writes);
    pending.push_back(request);
    return request;
  }

  Blinkable* status_device = nullptr;
  int status_pin = -1;
  int control_pin = -1;
  bool serial_input_disabled = false;
  int log_level = 0;
  std::string ap_ssid;
  unsigned ap_timeout_s = 0;
  bool auto_start_ap = false;
  std::string sketch_version;
  std::string web_log_url;
  void (*web_log_callback)(String&) = nullptr;
  unsigned watchdog_s = 0;
  bool began = false;
  Category category = Category::Other;
  std::string display_name;
  std::string host_name;
  std::string model_name;
  std::vector<SpanAccessory*> accessories;
  std::vector<SpanService*> services;
  std::vector<SpanCharacteristic*> characteristics;
  // Calls HomeSpan would log as errors: poll() before begin(), a
  // characteristic outside any service, setVal() during its own update.
  std::uint32_t iid_count = 0;
  unsigned violations = 0;
  unsigned polls = 0;
  std::uint32_t last_poll_ms = 0;
  std::uint32_t max_poll_gap_ms = 0;

 private:
  static void process(fake::HapRequest& request) {
    const auto count = request.writes.size();
    request.statuses.assign(count, fake::HapStatus::Pending);
    for (std::size_t i = 0; i < count; ++i) {
      const auto& write = request.writes[i];
      auto* characteristic = write.characteristic;
      if (!characteristic->writable) {
        request.statuses[i] = fake::HapStatus::ReadOnly;
      } else if (!characteristic->accepts(write.value, write.text)) {
        request.statuses[i] = fake::HapStatus::InvalidValue;
      } else {
        characteristic->new_value = write.value;
        if (write.text != nullptr) {
          characteristic->new_text = write.text;
        }
        characteristic->updating = true;
      }
    }
    for (std::size_t i = 0; i < count; ++i) {
      if (request.statuses[i] != fake::HapStatus::Pending) {
        continue;
      }
      auto* service = request.writes[i].characteristic->service;
      const bool accepted = service->update();
      for (std::size_t j = i; j < count; ++j) {
        auto* characteristic = request.writes[j].characteristic;
        if (characteristic->service != service ||
            request.statuses[j] != fake::HapStatus::Pending) {
          continue;
        }
        request.statuses[j] = accepted ? fake::HapStatus::Ok : fake::HapStatus::Unable;
        if (accepted) {
          characteristic->value = characteristic->new_value;
          characteristic->text = characteristic->new_text;
        } else {
          characteristic->new_value = characteristic->value;
          characteristic->new_text = characteristic->text;
        }
        characteristic->updating = false;
      }
    }
    request.done = true;
  }

  std::deque<std::shared_ptr<fake::HapRequest>> pending;
};

// Never destroyed: the services the sketch creates with new stay registered
// until the process ends, as on the device.
inline Span& homeSpan = *new Span();

inline SpanService::SpanService(const char* service_type) : type(service_type) {
  iid = ++homeSpan.iid_count;
  homeSpan.services.push_back(this);
}

inline SpanAccessory::SpanAccessory(std::uint32_t) {
  homeSpan.iid_count = 0;
  homeSpan.accessories.push_back(this);
}

inline SpanCharacteristic::SpanCharacteristic(const char* characteristic_type,
                                              Format value_format, bool can_write,
                                              double minimum_value, double maximum_value,
                                              double initial)
    : type(characteristic_type),
      format(value_format),
      writable(can_write),
      minimum(minimum_value),
      maximum(maximum_value),
      value(initial),
      new_value(initial) {
  iid = ++homeSpan.iid_count;
  homeSpan.characteristics.push_back(this);
  if (homeSpan.services.empty()) {
    ++homeSpan.violations;
    return;
  }
  service = homeSpan.services.back();
  service->characteristics.push_back(this);
}

template <typename T>
void SpanCharacteristic::setVal(T input, boolean notify) {
  if (updating) {
    ++homeSpan.violations;
  }
  value = static_cast<double>(input);
  new_value = value;
  if (notify) {
    ++notifications;
  }
}

// Types, formats, write permission and default ranges from the HAP
// specification as HomeSpan 2.1.8 declares them.
#define FAKE_HAP_SERVICE(NAME)     \
  struct NAME : SpanService {      \
    NAME() : SpanService(#NAME) {} \
  };
#define FAKE_HAP_NUMBER(NAME, FORMAT, WRITABLE, MINIMUM, MAXIMUM, TYPE)         \
  struct NAME : SpanCharacteristic {                                            \
    explicit NAME(TYPE initial = 0, boolean nvs_store = false)                  \
        : SpanCharacteristic(#NAME, Format::FORMAT, WRITABLE, MINIMUM, MAXIMUM, \
                             static_cast<double>(initial)) {                    \
      static_cast<void>(nvs_store);                                             \
    }                                                                           \
  };
#define FAKE_HAP_TEXT(NAME, WRITABLE)                                    \
  struct NAME : SpanCharacteristic {                                     \
    explicit NAME(const char* initial)                                   \
        : SpanCharacteristic(#NAME, Format::String, WRITABLE, 0, 0, 0) { \
      text = initial;                                                    \
      new_text = text;                                                   \
    }                                                                    \
  };

namespace Service {
FAKE_HAP_SERVICE(AccessoryInformation)
FAKE_HAP_SERVICE(AirPurifier)
FAKE_HAP_SERVICE(Fan)
FAKE_HAP_SERVICE(FilterMaintenance)
FAKE_HAP_SERVICE(HeaterCooler)
FAKE_HAP_SERVICE(Switch)
FAKE_HAP_SERVICE(TemperatureSensor)
}  // namespace Service

namespace Characteristic {
FAKE_HAP_NUMBER(Active, Uint8, true, 0, 1, int)
FAKE_HAP_NUMBER(CurrentAirPurifierState, Uint8, false, 0, 2, int)
FAKE_HAP_NUMBER(CurrentFanState, Uint8, false, 0, 2, int)
FAKE_HAP_NUMBER(CurrentHeaterCoolerState, Uint8, false, 0, 3, int)
FAKE_HAP_NUMBER(CurrentTemperature, Float, false, 0, 100, double)
FAKE_HAP_NUMBER(FilterChangeIndication, Uint8, false, 0, 1, int)
FAKE_HAP_NUMBER(HeatingThresholdTemperature, Float, true, 0, 25, double)
FAKE_HAP_NUMBER(Identify, Bool, true, 0, 1, bool)
FAKE_HAP_NUMBER(On, Bool, true, 0, 1, bool)
FAKE_HAP_NUMBER(RotationSpeed, Float, true, 0, 100, double)
FAKE_HAP_NUMBER(StatusActive, Bool, false, 0, 1, bool)
FAKE_HAP_NUMBER(StatusFault, Uint8, false, 0, 1, int)
FAKE_HAP_NUMBER(TargetAirPurifierState, Uint8, true, 0, 1, int)
FAKE_HAP_NUMBER(TargetHeaterCoolerState, Uint8, true, 0, 2, int)
FAKE_HAP_TEXT(ConfiguredName, true)
FAKE_HAP_TEXT(FirmwareRevision, false)
FAKE_HAP_TEXT(Manufacturer, false)
FAKE_HAP_TEXT(Model, false)
FAKE_HAP_TEXT(Name, false)
FAKE_HAP_TEXT(SerialNumber, false)
}  // namespace Characteristic

#undef FAKE_HAP_SERVICE
#undef FAKE_HAP_NUMBER
#undef FAKE_HAP_TEXT

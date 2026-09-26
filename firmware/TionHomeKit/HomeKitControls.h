#pragma once

// HomeSpan services. Behavior lives in HomeKitLogic.h; these classes only map
// characteristics to it. Characteristic order inside each service is part of
// the pairing (IIDs): append, never reorder.
//
// HomeSpan calls update() once per HAP write to any writable characteristic
// of the service, including ConfiguredName when a controller renames it, so
// every update() acts only on the characteristics that were written. Outside
// a write getNewVal() equals getVal().

#include <HomeSpan.h>

#include "ControlAccess.h"
#include "HomeKitLogic.h"

namespace tion_homekit {

// Compares in the characteristic's own value type: getVal() defaults to int
// and would truncate fractional values.
template <typename CharacteristicType, typename ValueType>
void publish(CharacteristicType* characteristic, ValueType value) {
  if (characteristic->template getVal<ValueType>() != value) {
    characteristic->setVal(value);
  }
}

// Keep the first four characteristics and their order from firmware 0.1.1.
class FanService final : public Service::Fan {
 public:
  explicit FanService(ControlAccess& access) : access_(access) {
    active_ = new Characteristic::Active(0);
    speed_ = new Characteristic::RotationSpeed(0);
    fault_ = new Characteristic::StatusFault(1);
    current_ = new Characteristic::CurrentFanState(0);
  }

  void refresh(const tion4s::Snapshot& snapshot) {
    if (!fan_snapshot_valid(snapshot)) {
      publish(fault_, 1);
      return;
    }
    const auto& state = snapshot.state;
    publish(active_, state.power_on ? 1 : 0);
    const auto percent =
        state.power_on ? tion4s::speed_to_percent(state.fan_speed, state.max_fan_speed) : 0;
    publish(speed_, static_cast<float>(percent));
    publish(current_, !state.power_on ? 0 : (state.fan_speed == 0 ? 1 : 2));
    publish(fault_, 0);
  }

  // Only Active and RotationSpeed are writable here.
  boolean update() override {
    const auto command =
        fan_command(active_->updated(), active_->getNewVal() != 0, speed_->updated(),
                    speed_->getNewVal<float>(), access_.status().device);
    return command && access_.submit(*command) != 0;
  }

 private:
  ControlAccess& access_;
  Characteristic::Active* active_;
  Characteristic::RotationSpeed* speed_;
  Characteristic::StatusFault* fault_;
  Characteristic::CurrentFanState* current_;
};

class AuxiliarySwitch final : public Service::Switch {
 public:
  AuxiliarySwitch(ControlAccess& access, const Auxiliary& kind, const char* name)
      : access_(access), kind_(kind) {
    on_ = new Characteristic::On(false);
    new Characteristic::ConfiguredName(name);
  }

  boolean update() override {
    return !on_->updated() || access_.submit(kind_.command(on_->getNewVal())) != 0;
  }

  void refresh(const tion4s::Snapshot& snapshot) {
    if (snapshot.fresh) {
      publish(on_, kind_.reported(snapshot.state));
    }
  }

 private:
  ControlAccess& access_;
  const Auxiliary& kind_;
  Characteristic::On* on_;
};

class HeaterService final : public Service::HeaterCooler {
 public:
  explicit HeaterService(ControlAccess& access) : access_(access) {
    active_ = new Characteristic::Active(0);
    temperature_ = new Characteristic::CurrentTemperature(0);
    current_ = new Characteristic::CurrentHeaterCoolerState(0);
    target_ = new Characteristic::TargetHeaterCoolerState(1);
    target_->setValidValues(1, 1);  // Heat only; no Auto or cooling.
    temperature_->setRange(kMinTemperatureC, kMaxTemperatureC);
    threshold_ = new Characteristic::HeatingThresholdTemperature(20);
    threshold_->setRange(tion4s::kMinHeaterTargetC, tion4s::kMaxHeaterTargetC, 1);
    new Characteristic::ConfiguredName("Нагрев притока");
  }

  boolean update() override {
    if (target_->updated() && target_->getNewVal() != 1) {
      return false;
    }
    const bool active_updated = active_->updated();
    const bool target_updated = threshold_->updated();
    if (!active_updated && !target_updated) {
      return true;  // Heat-only mode or the name was written.
    }
    const auto command = heater_command(active_updated, active_->getNewVal() != 0,
                                        target_updated, threshold_->getNewVal<float>());
    return command && access_.submit(*command) != 0;
  }

  void refresh(const tion4s::Snapshot& snapshot) {
    if (!snapshot.fresh) {
      return;
    }
    const auto& state = snapshot.state;
    publish(active_, state.heater_allowed ? 1 : 0);
    publish(current_, !state.power_on || !state.heater_allowed
                          ? 0
                          : (state.heater_percent > 0 ? 2 : 1));
    if (state.target_temperature >= tion4s::kMinHeaterTargetC &&
        state.target_temperature <= tion4s::kMaxHeaterTargetC) {
      publish(threshold_, static_cast<float>(state.target_temperature));
    }
    if (temperature_in_range(state.current_temperature)) {
      publish(temperature_, static_cast<float>(state.current_temperature));
    }
  }

 private:
  ControlAccess& access_;
  Characteristic::Active* active_;
  Characteristic::CurrentTemperature* temperature_;
  Characteristic::CurrentHeaterCoolerState* current_;
  Characteristic::TargetHeaterCoolerState* target_;
  Characteristic::HeatingThresholdTemperature* threshold_;
};

class TemperatureService final : public Service::TemperatureSensor {
 public:
  explicit TemperatureService(const char* name) {
    temperature_ = new Characteristic::CurrentTemperature(0);
    temperature_->setRange(kMinTemperatureC, kMaxTemperatureC);
    fault_ = new Characteristic::StatusFault(1);
    active_ = new Characteristic::StatusActive(false);
    new Characteristic::ConfiguredName(name);
  }

  void refresh(bool valid, std::int8_t value) {
    if (!valid || !temperature_in_range(value)) {
      publish(fault_, 1);
      publish(active_, false);
      return;
    }
    publish(temperature_, static_cast<float>(value));
    publish(fault_, 0);
    publish(active_, true);
  }

 private:
  Characteristic::CurrentTemperature* temperature_;
  Characteristic::StatusFault* fault_;
  Characteristic::StatusActive* active_;
};

class FilterService final : public Service::AirPurifier {
 public:
  explicit FilterService(ControlAccess& access) : access_(access) {
    active_ = new Characteristic::Active(0);
    current_ = new Characteristic::CurrentAirPurifierState(0);
    target_ = new Characteristic::TargetAirPurifierState(0);
    target_->setValidValues(1, 0);  // The breezer has no verified Auto mode.
    new Characteristic::ConfiguredName("Фильтрация притока");
    filter_ = new Service::FilterMaintenance();
    warning_ = new Characteristic::FilterChangeIndication(0);
    new Characteristic::ConfiguredName("Фильтр Tion");
    addLink(filter_);
  }

  boolean update() override {
    if (target_->updated() && target_->getNewVal() != 0) {
      return false;
    }
    if (!active_->updated()) {
      return true;
    }
    return access_.submit(tion4s::ControlCommand::power(active_->getNewVal() != 0)) != 0;
  }

  void refresh(const tion4s::Snapshot& snapshot) {
    if (!snapshot.fresh) {
      return;
    }
    publish(active_, snapshot.state.power_on ? 1 : 0);
    publish(current_, snapshot.state.power_on ? 2 : 0);
    publish(warning_, snapshot.state.filter_warning ? 1 : 0);
  }

 private:
  ControlAccess& access_;
  Characteristic::Active* active_;
  Characteristic::CurrentAirPurifierState* current_;
  Characteristic::TargetAirPurifierState* target_;
  Service::FilterMaintenance* filter_;
  Characteristic::FilterChangeIndication* warning_;
};

// A momentary action: the switch stays on until its own command finishes.
class FilterResetSwitch final : public Service::Switch {
 public:
  explicit FilterResetSwitch(ControlAccess& access) : access_(access) {
    on_ = new Characteristic::On(false);
    new Characteristic::ConfiguredName("Сброс фильтра после замены");
  }

  boolean update() override {
    if (!on_->updated() || !on_->getNewVal()) {
      return true;
    }
    ticket_ = access_.submit(tion4s::ControlCommand::filter_reset());
    return ticket_ != 0;
  }

  void refresh(const tion4s::UartStatus& status) {
    if (on_->getVal() &&
        tion4s::outcome_of(status, ticket_) != tion4s::CommandOutcome::Pending) {
      publish(on_, false);
    }
  }

 private:
  ControlAccess& access_;
  Characteristic::On* on_;
  std::uint32_t ticket_ = 0;
};

class BoostSwitch final : public Service::Switch {
 public:
  explicit BoostSwitch(ControlAccess& access) : timer_(access) {
    on_ = new Characteristic::On(false);
    new Characteristic::ConfiguredName("Ускорение на 5 минут");
  }

  boolean update() override {
    if (!on_->updated()) {
      return true;
    }
    if (!on_->getNewVal()) {
      timer_.stop(millis());
      return true;
    }
    return timer_.start(millis());
  }

  void refresh(const tion4s::UartStatus& status) {
    publish(on_, timer_.tick(status, millis()));
  }

 private:
  BoostTimer timer_;
  Characteristic::On* on_;
};

}  // namespace tion_homekit

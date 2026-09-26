#pragma once
#include <esp_err.h>

// In-memory host double of the ESP-IDF NVS calls PairingBootstrap.h uses,
// with the documented return codes and one injectable failure.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using nvs_handle_t = std::uint32_t;
constexpr esp_err_t ESP_ERR_NVS_NOT_FOUND = 0x1102;
constexpr esp_err_t ESP_ERR_NVS_INVALID_LENGTH = 0x110c;
enum nvs_open_mode_t : std::uint8_t { NVS_READONLY, NVS_READWRITE };

namespace fake_nvs {

// 1-based index of the open, set or commit call that fails; 0 for none.
inline int fail_at = 0;
inline int operations = 0;
inline int open_handles = 0;
inline int blob_writes = 0;
inline std::map<std::string, std::vector<std::uint8_t>> values;
inline std::vector<std::string> handles;

inline esp_err_t operation() { return ++operations == fail_at ? ESP_FAIL : ESP_OK; }
inline std::string key(nvs_handle_t handle, const char* name) {
  return handles.at(handle) + "/" + name;
}
// Forgets injected failures and counters; `erase` also clears stored values.
inline void reset(bool erase) {
  fail_at = operations = open_handles = blob_writes = 0;
  handles.clear();
  if (erase) {
    values.clear();
  }
}

}  // namespace fake_nvs

inline esp_err_t nvs_open(const char* name, nvs_open_mode_t, nvs_handle_t* handle) {
  const auto status = fake_nvs::operation();
  if (status == ESP_OK) {
    *handle = static_cast<nvs_handle_t>(fake_nvs::handles.size());
    fake_nvs::handles.emplace_back(name);
    ++fake_nvs::open_handles;
  }
  return status;
}

inline void nvs_close(nvs_handle_t) { --fake_nvs::open_handles; }

inline esp_err_t nvs_get_u8(nvs_handle_t handle, const char* name, std::uint8_t* value) {
  const auto found = fake_nvs::values.find(fake_nvs::key(handle, name));
  if (found == fake_nvs::values.end() || found->second.size() != 1) {
    return ESP_ERR_NVS_NOT_FOUND;
  }
  *value = found->second[0];
  return ESP_OK;
}

inline esp_err_t nvs_get_blob(nvs_handle_t handle, const char* name, void* output,
                              std::size_t* size) {
  const auto found = fake_nvs::values.find(fake_nvs::key(handle, name));
  if (found == fake_nvs::values.end()) {
    return ESP_ERR_NVS_NOT_FOUND;
  }
  if (found->second.size() > *size) {
    return ESP_ERR_NVS_INVALID_LENGTH;
  }
  std::memcpy(output, found->second.data(), found->second.size());
  *size = found->second.size();
  return ESP_OK;
}

inline esp_err_t nvs_set_blob(nvs_handle_t handle, const char* name, const void* value,
                              std::size_t size) {
  ++fake_nvs::blob_writes;
  const auto status = fake_nvs::operation();
  if (status == ESP_OK) {
    const auto* bytes = static_cast<const std::uint8_t*>(value);
    fake_nvs::values[fake_nvs::key(handle, name)].assign(bytes, bytes + size);
  }
  return status;
}

inline esp_err_t nvs_set_u8(nvs_handle_t handle, const char* name, std::uint8_t value) {
  const auto status = fake_nvs::operation();
  if (status == ESP_OK) {
    fake_nvs::values[fake_nvs::key(handle, name)].assign(1, value);
  }
  return status;
}

inline esp_err_t nvs_commit(nvs_handle_t) { return fake_nvs::operation(); }

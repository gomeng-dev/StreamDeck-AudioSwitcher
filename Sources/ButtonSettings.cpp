/* Copyright (c) 2018-present, Fred Emmott
 *
 * This source code is licensed under the MIT-style license found in the
 * LICENSE file.
 */

#include "ButtonSettings.h"

#include <StreamDeckSDK/ESDLogger.h>

#include "audio_json.h"

#include <regex>

NLOHMANN_JSON_SERIALIZE_ENUM(
  DeviceMatchStrategy,
  {
    {DeviceMatchStrategy::ID, "ID"},
    {DeviceMatchStrategy::Fuzzy, "Fuzzy"},
  });

void from_json(const nlohmann::json& j, ButtonSettings& bs) {
  if (!j.contains("direction")) {
    return;
  }

  bs.direction = j.at("direction");

  if (j.contains("role")) {
    bs.role = j.at("role");
  }

  if (j.contains("primary")) {
    const auto& primary = j.at("primary");
    if (primary.is_string()) {
      bs.primaryDevice.id = primary;
    } else {
      bs.primaryDevice = primary;
    }
  }

  if (j.contains("secondary")) {
    const auto& secondary = j.at("secondary");
    if (secondary.is_string()) {
      bs.secondaryDevice.id = secondary;
    } else {
      bs.secondaryDevice = secondary;
    }
  }

  if (j.contains("matchStrategy")) {
    bs.matchStrategy = j.at("matchStrategy");
  }
  
  if (j.contains("primarySpatialAudioMode")) {
    bs.primarySpatialAudioMode = j.at("primarySpatialAudioMode").get<std::string>();
  }

  if (j.contains("secondarySpatialAudioMode")) {
    bs.secondarySpatialAudioMode = j.at("secondarySpatialAudioMode").get<std::string>();
  }
}

void to_json(nlohmann::json& j, const ButtonSettings& bs) {
  j = {
    {"direction", bs.direction},
    {"role", bs.role},
    {"primary", bs.primaryDevice},
    {"secondary", bs.secondaryDevice},
    {"matchStrategy", bs.matchStrategy},
    {"primarySpatialAudioMode", bs.primarySpatialAudioMode},
    {"secondarySpatialAudioMode", bs.secondarySpatialAudioMode},
  };
}

namespace {

std::string FuzzifyInterface(const std::string& name) {
  // Windows likes to replace "Foo" with "2- Foo"
  const std::regex pattern {"^([0-9]+- )?(.+)$"};
  std::smatch captures;
  if (!std::regex_match(name, captures, pattern)) {
    return name;
  } 
  return captures[2];
}

bool HasFuzzyMatchMetadata(const AudioDeviceInfo& device) {
  return !device.interfaceName.empty() && !device.endpointName.empty();
}

std::string GetVolatileID(
  const AudioDeviceInfo& device,
  DeviceMatchStrategy strategy,
  const ButtonSettings::AudioDeviceListSnapshot& devices) {
  if (device.id.empty()) {
    return {};
  }

  const auto exact = devices.find(device.id);
  if (
    exact != devices.end()
    && exact->second.state == AudioDeviceState::CONNECTED) {
    return device.id;
  }

  if (strategy == DeviceMatchStrategy::ID) {
    return device.id;
  }

  if (!HasFuzzyMatchMetadata(device)) {
    ESDDebug("Skipping fuzzy match for {}: missing metadata", device.id);
    return device.id;
  }

  const auto fuzzyInterface = FuzzifyInterface(device.interfaceName);
  ESDDebug("Looking for a fuzzy match: {} -> {}", device.interfaceName, fuzzyInterface);

  for (const auto& [otherID, other] : devices) {
    if (other.state != AudioDeviceState::CONNECTED) {
      continue;
    }
    const auto otherFuzzyInterface = FuzzifyInterface(other.interfaceName);
    ESDDebug("Trying {} -> {}", other.interfaceName, otherFuzzyInterface);
    if (
      fuzzyInterface == otherFuzzyInterface
      && device.endpointName == other.endpointName) {
      ESDDebug(
        "Fuzzy device match for {}/{}",
        device.interfaceName,
        device.endpointName);
      return otherID;
    }
  }
  ESDDebug(
    "Failed fuzzy match for {}/{}", device.interfaceName, device.endpointName);
  return device.id;
}
}// namespace

std::string ButtonSettings::VolatilePrimaryID() const {
  return VolatilePrimaryID(GetAudioDeviceList(direction));
}

std::string ButtonSettings::VolatileSecondaryID() const {
  return VolatileSecondaryID(GetAudioDeviceList(direction));
}

std::string ButtonSettings::VolatilePrimaryID(
  const AudioDeviceListSnapshot& devices) const {
  return GetVolatileID(primaryDevice, matchStrategy, devices);
}

std::string ButtonSettings::VolatileSecondaryID(
  const AudioDeviceListSnapshot& devices) const {
  return GetVolatileID(secondaryDevice, matchStrategy, devices);
}

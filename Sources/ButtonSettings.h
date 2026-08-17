/* Copyright (c) 2018-present, Fred Emmott
 *
 * This source code is licensed under the MIT-style license found in the
 * LICENSE file.
 */
#pragma once

#include <AudioDevices/AudioDevices.h>

#include <nlohmann/json.hpp>

using namespace FredEmmott::Audio;

enum class DeviceMatchStrategy {
  ID,
  Fuzzy,
};

struct ButtonSettings {
  using AudioDeviceListSnapshot
    = decltype(GetAudioDeviceList(AudioDeviceDirection::INPUT));

  AudioDeviceDirection direction = AudioDeviceDirection::INPUT;
  AudioDeviceRole role = AudioDeviceRole::DEFAULT;
  AudioDeviceInfo primaryDevice;
  AudioDeviceInfo secondaryDevice;
  DeviceMatchStrategy matchStrategy = DeviceMatchStrategy::ID;
  std::string primarySpatialAudioMode;
  std::string secondarySpatialAudioMode;

  // Changes if there's a fuzzy match
  std::string VolatilePrimaryID() const;
  std::string VolatileSecondaryID() const;
  std::string VolatilePrimaryID(const AudioDeviceListSnapshot& devices) const;
  std::string VolatileSecondaryID(const AudioDeviceListSnapshot& devices) const;
};

void from_json(const nlohmann::json&, ButtonSettings&);
void to_json(nlohmann::json&, const ButtonSettings&);

#include "ButtonSettings.h"

#include <cassert>

using namespace FredEmmott::Audio;

int main() {
  ButtonSettings settings;
  settings.direction = AudioDeviceDirection::OUTPUT;
  settings.matchStrategy = DeviceMatchStrategy::Fuzzy;
  settings.primaryDevice = {
    .id = "saved-id",
    .interfaceName = "3- USB Audio",
    .endpointName = "Speakers",
    .displayName = "Speakers (3- USB Audio)",
    .direction = AudioDeviceDirection::OUTPUT,
    .state = AudioDeviceState::DEVICE_NOT_PRESENT,
  };

  ButtonSettings::AudioDeviceListSnapshot devices {
    {"saved-id",
     {
       .id = "saved-id",
       .interfaceName = "3- USB Audio",
       .endpointName = "Speakers",
       .displayName = "Speakers (3- USB Audio)",
       .direction = AudioDeviceDirection::OUTPUT,
       .state = AudioDeviceState::DEVICE_NOT_PRESENT,
     }},
    {"current-id",
     {
       .id = "current-id",
       .interfaceName = "USB Audio",
       .endpointName = "Speakers",
       .displayName = "Speakers (USB Audio)",
       .direction = AudioDeviceDirection::OUTPUT,
       .state = AudioDeviceState::CONNECTED,
     }},
  };

  assert(settings.VolatilePrimaryID(devices) == "current-id");
  devices.at("saved-id").state = AudioDeviceState::CONNECTED;
  assert(settings.VolatilePrimaryID(devices) == "saved-id");
}

//==============================================================================
/**
@file       AudioSwitcherStreamDeckPlugin.cpp

@brief      CPU plugin

@copyright  (c) 2018, Corsair Memory, Inc.
@copyright  (c) 2018-present, Fred Emmott.
      This source code is licensed under the MIT-style license found in the
LICENSE file.

**/
//==============================================================================

#include "AudioSwitcherStreamDeckPlugin.h"

#include <AudioDevices/AudioDevices.h>
#include <StreamDeckSDK/EPLJSONUtils.h>
#include <StreamDeckSDK/ESDConnectionManager.h>
#include <StreamDeckSDK/ESDLogger.h>

#ifdef _MSC_VER
#include <windows.h>
#include <PathCch.h> // For PathCchRemoveFileSpec
#endif

#include <functional>
#include <mutex>
#include <vector>

#ifdef _MSC_VER
#include <objbase.h>
#endif

#include "audio_json.h"

using namespace FredEmmott::Audio;
using json = nlohmann::json;

namespace {
constexpr std::string_view SET_ACTION_ID{
  "com.fredemmott.audiooutputswitch.set"};

// Helper function to fill in display info if missing.
bool NeedsAudioDeviceInfoBackfill(const AudioDeviceInfo& di) {
  if (di.id.empty()) {
    return false;
  }
  return di.displayName.empty()
    || di.interfaceName.empty()
    || di.endpointName.empty();
}

const AudioDeviceInfo* FindDeviceByID(
  const ButtonSettings::AudioDeviceListSnapshot& devices,
  const std::string& deviceID) {
  const auto it = devices.find(deviceID);
  if (it == devices.end()) {
    return nullptr;
  }
  return &it->second;
}

bool FillAudioDeviceInfo(
  AudioDeviceInfo& di,
  const ButtonSettings::AudioDeviceListSnapshot& devices) {
  if (di.id.empty()) {
    return false;
  }
  if (!NeedsAudioDeviceInfoBackfill(di)) {
    return false;
  }

  const auto it = devices.find(di.id);
  if (it == devices.end()) {
    return false;
  }
  di = it->second;
  return true;
}

// Helper function to get the directory of the current executable
std::string GetExecutableDirectory() {
#ifdef _MSC_VER
  wchar_t widePath[MAX_PATH];
  if (GetModuleFileNameW(NULL, widePath, MAX_PATH) == 0) {
    ESDLog("GetModuleFileNameW failed ({})", GetLastError());
    return "";
  }
  // Remove the file name to get the directory
  if (PathCchRemoveFileSpec(widePath, MAX_PATH) != S_OK) {
    ESDLog("PathCchRemoveFileSpec failed");
    return "";
  }
  // Convert wide char string to narrow char string (UTF-8)
  int bufferSize = WideCharToMultiByte(CP_UTF8, 0, widePath, -1, NULL, 0, NULL, NULL);
  if (bufferSize == 0) {
    ESDLog("WideCharToMultiByte (size query) failed ({})", GetLastError());
    return "";
  }
  std::string narrowPath(bufferSize, 0);
  if (WideCharToMultiByte(CP_UTF8, 0, widePath, -1, narrowPath.data(), bufferSize, NULL, NULL) == 0) {
    ESDLog("WideCharToMultiByte (conversion) failed ({})", GetLastError());
    return "";
  }
  narrowPath.pop_back();
  return narrowPath;
#else
  return ""; // Not implemented for non-Windows
#endif
}

// Executes a command line and waits for it to finish
// Returns the process exit code, or -1 if CreateProcess fails
int ExecuteCommandLine(const std::string& command) {
#ifdef _MSC_VER
  STARTUPINFOA si;
  PROCESS_INFORMATION pi;

  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  ZeroMemory(&pi, sizeof(pi));

  // CreateProcess requires a mutable command string
  std::vector<char> cmd(command.begin(), command.end());
  cmd.push_back('\0');

  ESDLog("Executing command: {}", command);

  // Start the child process.
  if (!CreateProcessA(
        NULL,           // No module name (use command line)
        cmd.data(),     // Command line
        NULL,           // Process handle not inheritable
        NULL,           // Thread handle not inheritable
        FALSE,          // Set handle inheritance to FALSE
        CREATE_NO_WINDOW, // Don't create a console window
        NULL,           // Use parent's environment block
        NULL,           // Use parent's starting directory
        &si,            // Pointer to STARTUPINFO structure
        &pi)            // Pointer to PROCESS_INFORMATION structure
  ) {
    ESDLog("CreateProcess failed ({}) for command: {}", GetLastError(), command);
    return -1; // Indicate failure
  }

  // svcl normally exits immediately; do not block all button events forever.
  if (WaitForSingleObject(pi.hProcess, 10000) == WAIT_TIMEOUT) {
    ESDLog("Command timed out: {}", command);
    TerminateProcess(pi.hProcess, ERROR_TIMEOUT);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return -1;
  }

  // Get exit code.
  DWORD exitCode;
  if (!GetExitCodeProcess(pi.hProcess, &exitCode)) {
    ESDLog("GetExitCodeProcess failed ({})", GetLastError());
    exitCode = -1; // Indicate failure
  }

  // Close process and thread handles.
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);

  ESDLog("Command finished with exit code: {}", exitCode);
  return exitCode;
#else
  // Not implemented for non-Windows platforms
  ESDLog("ExecuteCommandLine is only implemented for Windows.");
  return -1;
#endif
}

// Maps PI spatial audio mode names to svcl.exe names
std::string MapSpatialAudioModeForSvcl(const std::string& piMode) {
  if (piMode == "Off") return ""; // AHK script and svcl readme indicate empty string for "Off"
  if (piMode == "WindowsSonic") return "Windows Sonic For Headphones";
  // Using partial name "Dolby Atmos" for Dolby Atmos for Headphones as per AHK.ahk and svcl readme
  if (piMode == "DolbyAtmosForHeadphones") return "Dolby Atmos";
  if (piMode == "DTSHeadphoneX") return "DTS"; // As per AHK.ahk (partial name "DTS")
  if (piMode == "DolbyAtmosForHomeTheater") return "Dolby Atmos for home theater"; // Placeholder - needs verification for svcl.exe
  // Add other mappings here if needed
  return piMode; // Default to using the PI name if no specific mapping
}

}// namespace

AudioSwitcherStreamDeckPlugin::AudioSwitcherStreamDeckPlugin() {
#ifdef _MSC_VER
  CoInitializeEx(
    NULL, COINIT_MULTITHREADED);// initialize COM for the main thread
#endif
  mCallbackHandle = AddDefaultAudioDeviceChangeCallback(std::bind_front(
    &AudioSwitcherStreamDeckPlugin::OnDefaultDeviceChanged, this));
  mPlugCallbackHandle = AddAudioDevicePlugEventCallback(
    [this](AudioDevicePlugEvent, const std::string&) {
      OnAudioDeviceListChanged();
    });
}

AudioSwitcherStreamDeckPlugin::~AudioSwitcherStreamDeckPlugin() {
  mPlugCallbackHandle = {};
  mCallbackHandle = {};
}

void AudioSwitcherStreamDeckPlugin::OnDefaultDeviceChanged(
  AudioDeviceDirection direction,
  AudioDeviceRole role,
  const std::string& device) {
  std::vector<std::string> contextsToUpdate;
  {
    std::scoped_lock lock(mVisibleContextsMutex);
    for (const auto& [context, button] : mButtons) {
      if (button.settings.direction != direction) {
        continue;
      }
      if (button.settings.role != role) {
        continue;
      }
      contextsToUpdate.push_back(context);
    }
  }

  for (const auto& context : contextsToUpdate) {
    UpdateState(context, device);
  }
}

void AudioSwitcherStreamDeckPlugin::OnAudioDeviceListChanged() {
  std::vector<std::pair<std::string, std::string>> buttons;
  {
    std::scoped_lock lock(mVisibleContextsMutex);
    for (const auto& [context, button] : mButtons) {
      buttons.emplace_back(button.action, context);
    }
  }

  for (const auto& [action, context] : buttons) {
    SendAudioDeviceList(action, context);
    UpdateState(context);
  }
}

void AudioSwitcherStreamDeckPlugin::KeyDownForAction(
  const std::string& inAction,
  const std::string& inContext,
  const json& inPayload,
  const std::string& inDeviceID) {
  (void)inAction;
  (void)inContext;
  (void)inPayload;
  (void)inDeviceID;
}

void AudioSwitcherStreamDeckPlugin::KeyUpForAction(
  const std::string& inAction,
  const std::string& inContext,
  const json& inPayload,
  const std::string& inDeviceID) {
  ESDDebug("{}: {}", __FUNCTION__, inPayload.dump());

  if (!inPayload.contains("settings")) {
    return;
  }

  ButtonSettings settings = inPayload.at("settings");
  auto devices = GetAudioDeviceList(settings.direction);
  {
    std::scoped_lock lock(mVisibleContextsMutex);
    const auto it = mButtons.find(inContext);
    if (it == mButtons.end()) {
      return;
    }
    it->second.settings = settings;
  }
  FillButtonDeviceInfo(inContext, devices);
  {
    std::scoped_lock lock(mVisibleContextsMutex);
    const auto it = mButtons.find(inContext);
    if (it == mButtons.end()) {
      return;
    }
    settings = it->second.settings;
  }

  const auto state = EPLJSONUtils::GetIntByName(inPayload, "state");
  const auto usePrimaryDevice = (state != 0 || inAction == SET_ACTION_ID);
  const auto resolveTargetID
    = [&](const ButtonSettings::AudioDeviceListSnapshot& snapshot) {
        return usePrimaryDevice
          ? settings.VolatilePrimaryID(snapshot)
          : settings.VolatileSecondaryID(snapshot);
      };

  // this looks inverted - but if state is 0, we want to move to state 1, so
  // we want the secondary devices. if state is 1, we want state 0, so we want
  // the primary device
  auto deviceID = resolveTargetID(devices);
  if (deviceID.empty()) {
    ESDDebug("Doing nothing, no device ID");
    return;
  }

  // Determine the spatial audio mode to apply based on the device being activated (from PI settings)
  const auto& spatialAudioModeToApply = (state != 0 || inAction == SET_ACTION_ID)
    ? settings.primarySpatialAudioMode
    : settings.secondarySpatialAudioMode;

  const auto refreshTarget = [&]() {
    const auto refreshedDevices = GetAudioDeviceList(settings.direction);
    const auto refreshedID = resolveTargetID(refreshedDevices);
    if (refreshedID.empty()) {
      return;
    }
    const auto refreshedDevice = FindDeviceByID(refreshedDevices, refreshedID);
    if (!refreshedDevice) {
      return;
    }

    deviceID = refreshedID;
    devices = refreshedDevices;
  };

  auto targetDevice = FindDeviceByID(devices, deviceID);
  if (!targetDevice || targetDevice->state != AudioDeviceState::CONNECTED) {
    refreshTarget();
    targetDevice = FindDeviceByID(devices, deviceID);
  }

  const auto deviceState = targetDevice
    ? targetDevice->state
    : AudioDeviceState::DEVICE_NOT_PRESENT;
  if (deviceState != AudioDeviceState::CONNECTED) {
    if (inAction == SET_ACTION_ID) {
      mConnectionManager->SetState(1, inContext);
    }
    mConnectionManager->ShowAlertForContext(inContext);
    return;
  }

  const auto alreadyDefault =
    inAction == SET_ACTION_ID
    && deviceID == GetDefaultAudioDeviceID(settings.direction, settings.role);
  if (alreadyDefault) {
    mConnectionManager->SetState(state, inContext);
    ESDDebug("Device is already the default");
  } else {
    ESDDebug("Setting device to {}", deviceID);
    SetDefaultAudioDeviceID(settings.direction, settings.role, deviceID);
    if (GetDefaultAudioDeviceID(settings.direction, settings.role) != deviceID) {
      ESDLog("Windows did not set the default audio device to {}", deviceID);
      mConnectionManager->ShowAlertForContext(inContext);
      return;
    }
  }

  // Apply spatial audio settings using svcl.exe if specified and not "Unchanged"
  if (!spatialAudioModeToApply.empty() && spatialAudioModeToApply != "Unchanged") {
    if (!targetDevice) {
      ESDLog("Could not find AudioDeviceInfo for device ID {}.", deviceID);
      mConnectionManager->ShowAlertForContext(inContext);
      return;
    }

    if (
      targetDevice->interfaceName.empty()
      || targetDevice->endpointName.empty()) {
      ESDLog(
        "Cannot apply spatial audio for {}: missing interface/endpoint info.",
        deviceID);
      mConnectionManager->ShowAlertForContext(inContext);
      return;
    }

    const std::string deviceIdentifierForSvcl
      = targetDevice->interfaceName + "\\Device\\" + targetDevice->endpointName;
    const std::string svclSpatialModeName = MapSpatialAudioModeForSvcl(spatialAudioModeToApply);

    std::string svclPath = GetExecutableDirectory();
    if (svclPath.empty()) {
        ESDLog("Failed to get executable directory. Cannot locate svcl.exe.");
        mConnectionManager->ShowAlertForContext(inContext);
        return;
    }
    // Assuming svcl.exe is in a 'svcl' subdirectory relative to the plugin executable
    // If svcl.exe is in the same directory, just append "\\svcl.exe"
    // Based on your context.md, svcl.exe is in a 'svcl' folder.
    // However, for deployment, it's often easier if it's in the same folder as the plugin exe.
    // Let's assume for now it's in the same folder as sdaudioswitch.exe for simplicity of deployment.
    // If it's in a subfolder like 'svcl', the path should be (svclPath + "\\svcl\\svcl.exe")
    svclPath += "\\svcl.exe"; // If svcl.exe is in the same directory as sdaudioswitch.exe

    std::string command = "\"" + svclPath + "\" /SetSpatial \"" + deviceIdentifierForSvcl + "\" \"" + svclSpatialModeName + "\"";

    int exitCode = ExecuteCommandLine(command);
    if (exitCode != 0) {
      ESDLog("Failed to set spatial audio mode via svcl.exe for device {}. Command: {}. Exit code: {}", deviceID, command, exitCode);
      mConnectionManager->ShowAlertForContext(inContext); // Show alert on failure
    } else {
      ESDLog("Successfully set spatial audio mode via svcl.exe.");
    }
  }
}

void AudioSwitcherStreamDeckPlugin::WillAppearForAction(
  const std::string& inAction,
  const std::string& inContext,
  const json& inPayload,
  const std::string& inDeviceID) {
  ButtonSettings settings;
  {
    std::scoped_lock lock(mVisibleContextsMutex);
    mVisibleContexts.insert(inContext);
    auto& button = mButtons[inContext];
    button = {inAction, inContext};

    if (!inPayload.contains("settings")) {
      return;
    }
    button.settings = inPayload.at("settings");
    settings = button.settings;
  }

  const auto devices = GetAudioDeviceList(settings.direction);
  FillButtonDeviceInfo(inContext, devices);
  UpdateState(inContext);
}

void AudioSwitcherStreamDeckPlugin::FillButtonDeviceInfo(
  const std::string& context,
  const ButtonSettings::AudioDeviceListSnapshot& devices) {
  std::scoped_lock lock(mVisibleContextsMutex);
  const auto it = mButtons.find(context);
  if (it == mButtons.end()) {
    return;
  }
  auto& settings = it->second.settings;

  const auto filledPrimary = FillAudioDeviceInfo(settings.primaryDevice, devices);
  const auto filledSecondary = FillAudioDeviceInfo(settings.secondaryDevice, devices);
  if (filledPrimary || filledSecondary) {
    ESDDebug("Backfilling settings to {}", json(settings).dump());
    mConnectionManager->SetSettings(settings, context);
  }
}

void AudioSwitcherStreamDeckPlugin::WillDisappearForAction(
  const std::string& inAction,
  const std::string& inContext,
  const json& inPayload,
  const std::string& inDeviceID) {
  // Remove the context
  std::scoped_lock lock(mVisibleContextsMutex);
  mVisibleContexts.erase(inContext);
  mButtons.erase(inContext);
}

void AudioSwitcherStreamDeckPlugin::SendToPlugin(
  const std::string& inAction,
  const std::string& inContext,
  const json& inPayload,
  const std::string& inDeviceID) {
  const auto event = EPLJSONUtils::GetStringByName(inPayload, "event");
  ESDDebug("Received event {}", event);

  if (event == "getDeviceList") {
    SendAudioDeviceList(inAction, inContext);
    return;
  }
}

void AudioSwitcherStreamDeckPlugin::SendAudioDeviceList(
  const std::string& action,
  const std::string& context) {
  mConnectionManager->SendToPropertyInspector(
    action,
    context,
    json({
      {"event", "getDeviceList"},
      {"outputDevices", GetAudioDeviceList(AudioDeviceDirection::OUTPUT)},
      {"inputDevices", GetAudioDeviceList(AudioDeviceDirection::INPUT)},
    }));
}

void AudioSwitcherStreamDeckPlugin::UpdateState(
  const std::string& context,
  const std::string& optionalDefaultDevice) {
  Button button;
  {
    std::scoped_lock lock(mVisibleContextsMutex);
    const auto it = mButtons.find(context);
    if (it == mButtons.end()) {
      return;
    }
    button = it->second;
  }

  const auto action = button.action;
  const auto settings = button.settings;
  const auto devices = GetAudioDeviceList(settings.direction);
  const auto activeDevice = optionalDefaultDevice.empty()
    ? GetDefaultAudioDeviceID(settings.direction, settings.role)
    : optionalDefaultDevice;

  const auto primaryID = settings.VolatilePrimaryID(devices);
  const auto secondaryID = settings.VolatileSecondaryID(devices);

  {
    std::scoped_lock lock(mVisibleContextsMutex);
    if (!mButtons.contains(context)) {
      return;
    }
  }

  if (action == SET_ACTION_ID) {
    mConnectionManager->SetState(activeDevice == primaryID ? 0 : 1, context);
    return;
  }

  if (activeDevice == primaryID) {
    mConnectionManager->SetState(0, context);
    return;
  }

  if (activeDevice == secondaryID) {
    mConnectionManager->SetState(1, context);
    return;
  }

  mConnectionManager->ShowAlertForContext(context);
}

void AudioSwitcherStreamDeckPlugin::DeviceDidConnect(
  const std::string& inDeviceID,
  const json& inDeviceInfo) {
  // Nothing to do
}

void AudioSwitcherStreamDeckPlugin::DeviceDidDisconnect(
  const std::string& inDeviceID) {
  // Nothing to do
}

void AudioSwitcherStreamDeckPlugin::DidReceiveGlobalSettings(
  const json& inPayload) {
}

void AudioSwitcherStreamDeckPlugin::DidReceiveSettings(
  const std::string& inAction,
  const std::string& inContext,
  const json& inPayload,
  const std::string& inDeviceID) {
  WillAppearForAction(inAction, inContext, inPayload, inDeviceID);
}

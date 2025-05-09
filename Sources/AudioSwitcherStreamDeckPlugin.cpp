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

#ifdef _MSC_VER
#include <mmdeviceapi.h>     // For IMMDeviceEnumerator, IMMDevice
#include <audioclient.h>     // For IAudioClient
#include <spatialaudioclient.h> // For ISpatialAudioClient
#include <wrl/client.h>      // For ComPtr
// #include <SpatialAudioMetadata.h> // Might be needed for specific format details
#endif

#include <atomic>
#include <functional>
#include <mutex>

#ifdef _MSC_VER
#include <objbase.h>
#endif

#include "audio_json.h"

using namespace FredEmmott::Audio;
using json = nlohmann::json;

namespace {
constexpr std::string_view SET_ACTION_ID{
  "com.fredemmott.audiooutputswitch.set"};
constexpr std::string_view TOGGLE_ACTION_ID{
  "com.fredemmott.audiooutputswitch.toggle"};

// Helper function to fill in display info if missing
// Assumes mVisibleContextsMutex is locked
bool FillAudioDeviceInfo(AudioDeviceInfo& di) {
  if (di.id.empty()) {
    return false;
  }
  if (!di.displayName.empty()) {
    return false;
  }

  const auto devices = GetAudioDeviceList(di.direction);
  if (!devices.contains(di.id)) {
    return false;
  }
  di = devices.at(di.id);
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
  std::string narrowPath(bufferSize -1, 0); // -1 for null terminator
  if (WideCharToMultiByte(CP_UTF8, 0, widePath, -1, &narrowPath[0], bufferSize, NULL, NULL) == 0) {
    ESDLog("WideCharToMultiByte (conversion) failed ({})", GetLastError());
    return "";
  }
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

  // Wait until child process exits.
  WaitForSingleObject(pi.hProcess, INFINITE);

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
  // Using GUID for Windows Sonic as per AHK.ahk, which might be more reliable
  if (piMode == "WindowsSonic") return "{b53d940c-b846-4831-9f76-d102b9b725a0}"; 
  // Using partial name "Dolby Atmos" for Dolby Atmos for Headphones as per AHK.ahk and svcl readme
  if (piMode == "DolbyAtmosForHeadphones") return "Dolby Atmos";
  if (piMode == "DTSHeadphoneX") return "DTS"; // As per AHK.ahk (partial name "DTS")
  if (piMode == "DolbyAtmosForHomeTheater") return "Dolby Atmos for home theater"; // Placeholder - needs verification for svcl.exe
  // Add other mappings here if needed
  return piMode; // Default to using the PI name if no specific mapping
}

#ifdef _MSC_VER
// Known Spatial Audio Format GUIDs
const GUID SPATIAL_AUDIO_FORMAT_GUID_WINDOWS_SONIC_FOR_HEADPHONES = { 0xb53d940c, 0xb846, 0x4831, { 0x9f, 0x76, 0xd1, 0x02, 0xb9, 0xb7, 0x25, 0xa0 } };
// Placeholder for Dolby Atmos for Headphones GUID - This needs to be verified or obtained.
// Nirsoft's SoundVolumeView or other tools might reveal this GUID when Dolby Atmos is active.
// For now, we can't reliably detect Dolby Atmos via its GUID without knowing it.
// const GUID SPATIAL_AUDIO_FORMAT_GUID_DOLBY_ATMOS_HP = { 0x????????, 0x????, 0x????, { 0x??, 0x??, 0x??, 0x??, 0x??, 0x??, 0x??, 0x?? } };

// Function to query the current spatial audio mode for a given device ID
// Returns: "WindowsSonic", "DolbyAtmosForHeadphones" (if GUID known), "Off", or "Unknown"
std::string QueryCurrentSpatialAudioMode(const std::string& deviceID_str) {
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> pEnumerator;
    Microsoft::WRL::ComPtr<IMMDevice> pDevice;
    Microsoft::WRL::ComPtr<ISpatialAudioClient> pSpatialAudioClient;

    std::wstring wideDeviceID;
    try {
        // Convert UTF-8 std::string to std::wstring
        int len = MultiByteToWideChar(CP_UTF8, 0, deviceID_str.c_str(), -1, NULL, 0);
        if (len == 0) throw std::runtime_error("MultiByteToWideChar failed to get length");
        wideDeviceID.resize(len - 1); // -1 for null terminator
        if (MultiByteToWideChar(CP_UTF8, 0, deviceID_str.c_str(), -1, &wideDeviceID[0], len) == 0) {
            throw std::runtime_error("MultiByteToWideChar failed to convert");
        }
    } catch (const std::exception& e) {
        ESDLog("QueryCurrentSpatialAudioMode: Error converting device ID to wide string: {}", e.what());
        return "Unknown";
    }

    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)pEnumerator.GetAddressOf());
    if (FAILED(hr)) {
        ESDLog("QueryCurrentSpatialAudioMode: CoCreateInstance(MMDeviceEnumerator) failed: {:#08x}", hr);
        return "Unknown";
    }

    hr = pEnumerator->GetDevice(wideDeviceID.c_str(), pDevice.GetAddressOf());
    if (FAILED(hr)) {
        // This can happen if the device is unplugged or the ID is no longer valid.
        // ESDLog("QueryCurrentSpatialAudioMode: GetDevice failed for ID {}: {:#08x}", deviceID_str, hr);
        return "Off"; // Assume off if device not found or error
    }

    hr = pDevice->Activate(__uuidof(ISpatialAudioClient), CLSCTX_INPROC_SERVER, nullptr, (void**)pSpatialAudioClient.GetAddressOf());
    if (FAILED(hr)) {
        // If ISpatialAudioClient cannot be activated, it's highly likely spatial audio is off or not supported.
        // ESDLog("QueryCurrentSpatialAudioMode: Activate(ISpatialAudioClient) failed for ID {}: {:#08x}", deviceID_str, hr);
        return "Off";
    }

    // At this point, ISpatialAudioClient is active. Now, try to determine the *type* of spatial audio.
    // This is the tricky part as there's no direct API to get the "current friendly name".
    // We can try to get the current render stream parameters and check the subtype.
    Microsoft::WRL::ComPtr<IAudioFormatEnumerator> pFormatEnumerator;
    UINT32 defaultFormatIndex;
    hr = pSpatialAudioClient->GetSupportedAudioObjectFormatEnumerator(pFormatEnumerator.GetAddressOf());
    if (SUCCEEDED(hr) && pFormatEnumerator) {
         // This enumerator lists formats the client *could* support, not necessarily the *active* one.
         // A more direct way to get the *active* spatial audio format GUID is not straightforward with ISpatialAudioClient alone.
         // One potential (but complex and less reliable) approach might involve checking properties of the active audio stream.
         // For now, if ISAC is active, we know *some* spatial audio is on.
         // We can try checking if Windows Sonic is the one by attempting to get specific stream params for it.
         // This is still more of a "can it do Windows Sonic?" rather than "is it *currently* Windows Sonic?".
         // A more robust solution might involve looking at registry keys (like svcl.exe might do) or undocumented APIs.
         ESDLog("QueryCurrentSpatialAudioMode: ISpatialAudioClient active for {}. Specific mode detection is complex.", deviceID_str);
         // Since svcl.exe handles setting, we might not need to query the exact type here if the goal is just to update UI based on "On/Off".
         // If we *must* know the type, this part needs significant research or alternative methods.
         // For now, let's assume if ISAC is active, we can't distinguish specific types easily via official API.
         // We could *assume* it's the one we last set, but that's not a true query.
         return "Unknown"; // Placeholder: Means "Spatial Audio is On, but specific type is hard to get here"
    }

    return "Off"; // Default to Off if we couldn't determine otherwise
}
#endif

}// namespace

AudioSwitcherStreamDeckPlugin::AudioSwitcherStreamDeckPlugin() {
#ifdef _MSC_VER
  CoInitializeEx(
    NULL, COINIT_MULTITHREADED);// initialize COM for the main thread
#endif
  mCallbackHandle = AddDefaultAudioDeviceChangeCallback(std::bind_front(
    &AudioSwitcherStreamDeckPlugin::OnDefaultDeviceChanged, this));
}

AudioSwitcherStreamDeckPlugin::~AudioSwitcherStreamDeckPlugin() {
  mCallbackHandle = {};
}

void AudioSwitcherStreamDeckPlugin::OnDefaultDeviceChanged(
  AudioDeviceDirection direction,
  AudioDeviceRole role,
  const std::string& device) {
  std::scoped_lock lock(mVisibleContextsMutex);
  for (const auto& [context, button] : mButtons) {
    if (button.settings.direction != direction) {
      continue;
    }
    if (button.settings.role != role) {
      continue;
    }
    UpdateState(context, device);
  }
}

void AudioSwitcherStreamDeckPlugin::KeyDownForAction(
  const std::string& inAction,
  const std::string& inContext,
  const json& inPayload,
  const std::string& inDeviceID) {
  const auto state = EPLJSONUtils::GetIntByName(inPayload, "state");
}

void AudioSwitcherStreamDeckPlugin::KeyUpForAction(
  const std::string& inAction,
  const std::string& inContext,
  const json& inPayload,
  const std::string& inDeviceID) {
  ESDDebug("{}: {}", __FUNCTION__, inPayload.dump());
  std::scoped_lock lock(mVisibleContextsMutex);

  if (!inPayload.contains("settings")) {
    return;
  }
  auto& settings = mButtons[inContext].settings;
  settings = inPayload.at("settings");
  FillButtonDeviceInfo(inContext);
  const auto state = EPLJSONUtils::GetIntByName(inPayload, "state");
  // this looks inverted - but if state is 0, we want to move to state 1, so
  // we want the secondary devices. if state is 1, we want state 0, so we want
  // the primary device
  const auto deviceID = (state != 0 || inAction == SET_ACTION_ID)
    ? settings.VolatilePrimaryID()
    : settings.VolatileSecondaryID();
  if (deviceID.empty()) {
    ESDDebug("Doing nothing, no device ID");
    return;
  }

  // Determine the spatial audio mode to apply based on the device being activated (from PI settings)
  const auto& spatialAudioModeToApply = (state != 0 || inAction == SET_ACTION_ID)
    ? settings.primarySpatialAudioMode
    : settings.secondarySpatialAudioMode; // Note: secondarySpatialAudioMode is only saved for TOGGLE_ACTION_ID

  const auto deviceState = GetAudioDeviceState(deviceID);
  if (deviceState != AudioDeviceState::CONNECTED) {
    if (inAction == SET_ACTION_ID) {
      mConnectionManager->SetState(1, inContext);
    }
    mConnectionManager->ShowAlertForContext(inContext);
    return;
  }

  if (
    inAction == SET_ACTION_ID
    && deviceID == GetDefaultAudioDeviceID(settings.direction, settings.role)) {
    // We already have the correct device, undo the state change
    mConnectionManager->SetState(state, inContext);
    ESDDebug("Already set, nothing to do");
    return;
  }

  ESDDebug("Setting device to {}", deviceID);
  SetDefaultAudioDeviceID(settings.direction, settings.role, deviceID);

  // Apply spatial audio settings using svcl.exe if specified and not "Unchanged"
  if (!spatialAudioModeToApply.empty() && spatialAudioModeToApply != "Unchanged" && deviceState == AudioDeviceState::CONNECTED) {
    // Find the AudioDeviceInfo for the target deviceID to get interfaceName and endpointName
    AudioDeviceInfo targetDeviceInfo;
    targetDeviceInfo.id = deviceID;
    targetDeviceInfo.direction = settings.direction; // Need direction to query list
    // Temporarily unlock mutex to call GetAudioDeviceList if needed, though FillButtonDeviceInfo might have already done this
    // A safer approach might be to get the device list once and store it, or ensure FillButtonDeviceInfo is always called before this.
    // For simplicity in this step, we'll re-query or assume info is available.
    // Let's use the info from settings.primaryDevice or settings.secondaryDevice if available and matches deviceID
    if (settings.primaryDevice.id == deviceID) {
        targetDeviceInfo = settings.primaryDevice;
    } else if (settings.secondaryDevice.id == deviceID) {
        targetDeviceInfo = settings.secondaryDevice;
    } else {
        // Fallback: Try to get info from the full device list
        const auto devices = GetAudioDeviceList(settings.direction);
        if (devices.count(deviceID)) {
            targetDeviceInfo = devices.at(deviceID);
        } else {
             ESDLog("Could not find AudioDeviceInfo for device ID {} to get interface/endpoint names.", deviceID);
             mConnectionManager->ShowAlertForContext(inContext);
             return;
        }
    }

    const std::string deviceIdentifierForSvcl = targetDeviceInfo.interfaceName + "\\Device\\" + targetDeviceInfo.endpointName;
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
  std::scoped_lock lock(mVisibleContextsMutex);
  // Remember the context
  mVisibleContexts.insert(inContext);
  auto& button = mButtons[inContext];
  button = {inAction, inContext};

  if (!inPayload.contains("settings")) {
    return;
  }
  button.settings = inPayload.at("settings");

  UpdateState(inContext);
  FillButtonDeviceInfo(inContext);
}

void AudioSwitcherStreamDeckPlugin::FillButtonDeviceInfo(
  const std::string& context) {
  auto& settings = mButtons.at(context).settings;

  const auto filledPrimary = FillAudioDeviceInfo(settings.primaryDevice);
  const auto filledSecondary = FillAudioDeviceInfo(settings.secondaryDevice);
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
  json outPayload;

  const auto event = EPLJSONUtils::GetStringByName(inPayload, "event");
  ESDDebug("Received event {}", event);

  if (event == "getDeviceList") {
    const auto outputList = GetAudioDeviceList(AudioDeviceDirection::OUTPUT);
    const auto inputList = GetAudioDeviceList(AudioDeviceDirection::INPUT);
    mConnectionManager->SendToPropertyInspector(
      inAction,
      inContext,
      json({
        {"event", event},
        {"outputDevices", outputList},
        {"inputDevices", inputList},
      }));
    return;
  }
}

// This function will be called when the PI requests an update of the current spatial audio state
void AudioSwitcherStreamDeckPlugin::RequestCurrentSpatialAudioState(const std::string& context, const std::string& deviceID) {
#ifdef _MSC_VER
    std::string currentMode = QueryCurrentSpatialAudioMode(deviceID);
    ESDLog("Queried spatial audio mode for device {}: {}", deviceID, currentMode);
    // TODO: Send this 'currentMode' back to the Property Inspector for the given context.
    // mConnectionManager->SendToPropertyInspector(mButtons[context].action, context, json({{"event", "currentSpatialAudioState"}, {"deviceID", deviceID}, {"mode", currentMode}}));
#endif
}

void AudioSwitcherStreamDeckPlugin::UpdateState(
  const std::string& context,
  const std::string& optionalDefaultDevice) {
  const auto button = mButtons[context];
  const auto action = button.action;
  const auto settings = button.settings;
  const auto activeDevice = optionalDefaultDevice.empty()
    ? GetDefaultAudioDeviceID(settings.direction, settings.role)
    : optionalDefaultDevice;

  const auto primaryID = settings.VolatilePrimaryID();
  const auto secondaryID = settings.VolatileSecondaryID();

  // Query and log current spatial audio state for the active device (if it's an output device)
  // This is for debugging/information for now. PI integration would be next.
  if (settings.direction == AudioDeviceDirection::OUTPUT && !activeDevice.empty()) {
    ESDLog("Attempting to query spatial audio state for active device: {}", activeDevice);
    RequestCurrentSpatialAudioState(context, activeDevice); // Call the query function
  }

  std::scoped_lock lock(mVisibleContextsMutex);
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

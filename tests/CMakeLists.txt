add_executable(
  button_settings_test
  ButtonSettingsTest.cpp
  ../Sources/audio_json.cpp
  ../Sources/ButtonSettings.cpp
)
target_include_directories(button_settings_test PRIVATE ../Sources)
target_link_libraries(button_settings_test AudioDeviceLib StreamDeckSDK)
add_test(NAME button_settings_test COMMAND button_settings_test)

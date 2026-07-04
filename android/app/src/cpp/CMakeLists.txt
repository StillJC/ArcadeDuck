set(CMAKE_CXX_FLAGS_RELEASE  "-O3")
set(SRCS
  android_host_interface.cpp
  android_host_interface.h
  android_progress_callback.cpp
  android_progress_callback.h
  android_settings_interface.cpp
  android_settings_interface.h
  trackball.cpp
)

add_library(arcade1up-native SHARED ${SRCS})
target_link_libraries(arcade1up-native PRIVATE android frontend-common core common glad imgui)

find_package(OpenSLES)
if(OPENSLES_FOUND)
  message("Enabling OpenSL ES audio stream")
  target_sources(arcade1up-native PRIVATE
    opensles_audio_stream.cpp
    opensles_audio_stream.h)
  target_link_libraries(arcade1up-native PRIVATE OpenSLES::OpenSLES)
  target_compile_definitions(arcade1up-native PRIVATE "-DUSE_OPENSLES=1")
endif()

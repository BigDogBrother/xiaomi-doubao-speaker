cmake_minimum_required(VERSION 3.16)
project(doubao_edge LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(USE_TLS ON CACHE BOOL "" FORCE)
set(USE_OPEN_SSL ON CACHE BOOL "" FORCE)
set(USE_ZLIB ON CACHE BOOL "" FORCE)
set(OPENSSL_INCLUDE_DIR "${EDGE_ROOT}/sysroot/usr/include" CACHE PATH "" FORCE)
set(OPENSSL_SSL_LIBRARY "${EDGE_ROOT}/sysroot/usr/lib/arm-linux-gnueabihf/libssl.a" CACHE FILEPATH "" FORCE)
set(OPENSSL_CRYPTO_LIBRARY "${EDGE_ROOT}/sysroot/usr/lib/arm-linux-gnueabihf/libcrypto.a" CACHE FILEPATH "" FORCE)
set(ZLIB_INCLUDE_DIR "${EDGE_ROOT}/sysroot/usr/include" CACHE PATH "" FORCE)
set(ZLIB_LIBRARY "${EDGE_ROOT}/sysroot/usr/lib/arm-linux-gnueabihf/libz.a" CACHE FILEPATH "" FORCE)
include_directories("${EDGE_ROOT}/sysroot/usr/include/arm-linux-gnueabihf")
add_compile_options(-Wno-psabi)
add_subdirectory("${EDGE_ROOT}/deps/json" json-build EXCLUDE_FROM_ALL)
add_subdirectory("${EDGE_ROOT}/deps/ixwebsocket" ix-build EXCLUDE_FROM_ALL)
add_subdirectory("${EDGE_ROOT}/deps/spdlog" spdlog-build EXCLUDE_FROM_ALL)
find_package(Threads REQUIRED)
set(SHERPA_LIB "${EDGE_ROOT}/sherpa-shared/sherpa-onnx-v1.12.25-linux-arm-gnueabihf-shared/lib")
add_executable(doubao-edge
  src/main.cpp src/app/app.cpp src/app/native_lease.cpp src/app/speech_activity.cpp
  src/audio/recorder_arecord.cpp src/audio/player_aplay.cpp src/audio/aec_webrtc.cpp
  src/config/config.cpp src/realtime/client.cpp src/wakeup/gate.cpp src/wakeup/trigger.cpp
  src/wakeup/local_listener.cpp src/wakeup/kws_zipformer.cpp)
target_include_directories(doubao-edge PRIVATE src
  "${EDGE_ROOT}/deps/sherpa-source"
  "${EDGE_ROOT}/sysroot/usr/include"
  "${EDGE_ROOT}/sysroot/usr/include/arm-linux-gnueabihf"
  "${EDGE_ROOT}/sysroot/usr/include/webrtc_audio_processing")
target_compile_definitions(doubao-edge PRIVATE WEBRTC_POSIX WEBRTC_AUDIO_PROCESSING_ONLY_BUILD)
target_link_libraries(doubao-edge PRIVATE nlohmann_json::nlohmann_json ixwebsocket::ixwebsocket
  spdlog::spdlog "${SHERPA_LIB}/libsherpa-onnx-cxx-api.so" "${SHERPA_LIB}/libsherpa-onnx-c-api.so"
  "${SHERPA_LIB}/libonnxruntime.so" "${EDGE_ROOT}/sysroot/usr/lib/arm-linux-gnueabihf/libwebrtc_audio_processing.a"
  "${ZLIB_LIBRARY}" "${OPENSSL_SSL_LIBRARY}" "${OPENSSL_CRYPTO_LIBRARY}" Threads::Threads ${CMAKE_DL_LIBS})

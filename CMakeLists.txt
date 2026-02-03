cmake_minimum_required(VERSION 3.16)
project(pc-control C)

set(CMAKE_C_STANDARD 11)

# Find Paho MQTT C library
list(APPEND CMAKE_PREFIX_PATH "${CMAKE_SOURCE_DIR}/../paho.mqtt.c/install")
find_package(eclipse-paho-mqtt-c REQUIRED)

# Common link libraries for static build
set(COMMON_LIBS
    eclipse-paho-mqtt-c::paho-mqtt3c-static
    powrprof
    user32
    ws2_32
    crypt32
    rpcrt4
)

# Console version (for debugging/interactive use)
add_executable(pc-control src/main.c)
target_link_libraries(pc-control ${COMMON_LIBS})

# Hidden version (GUI subsystem, no console window at all)
# Uses mainCRTStartup entry point to keep regular main() function
add_executable(pc-control-hidden src/main.c)
target_compile_definitions(pc-control-hidden PRIVATE HIDDEN_BUILD)
target_link_libraries(pc-control-hidden ${COMMON_LIBS})
if(MINGW)
    target_link_options(pc-control-hidden PRIVATE -mwindows -Wl,-e,mainCRTStartup)
endif()

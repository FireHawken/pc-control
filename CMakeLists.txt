cmake_minimum_required(VERSION 3.16)
project(pc-control C)

set(CMAKE_C_STANDARD 11)

# Find Paho MQTT C library
list(APPEND CMAKE_PREFIX_PATH "${CMAKE_SOURCE_DIR}/../paho.mqtt.c/install")
find_package(eclipse-paho-mqtt-c REQUIRED)

add_executable(pc-control src/main.c)
target_link_libraries(pc-control
    eclipse-paho-mqtt-c::paho-mqtt3c
    powrprof
    user32
    ws2_32
)

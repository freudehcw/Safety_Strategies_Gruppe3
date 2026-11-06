cmake_minimum_required(VERSION 3.10)
project(SafetyDemonstrator C)

# Konfigurierbare App-Namen
set(DEFAULT_PI1_NAME "pi1_app")
if(DEFINED ENV{PI1_APP_NAME})
    set(DEFAULT_PI1_NAME $ENV{PI1_APP_NAME})
endif()

set(DEFAULT_PI2_NAME "pi2_app")
if(DEFINED ENV{PI2_APP_NAME})
    set(DEFAULT_PI2_NAME $ENV{PI2_APP_NAME})
endif()

# Override mit cmake -D
set(PI1_APP_NAME "${DEFAULT_PI1_NAME}" CACHE STRING "Name der Pi 1 Executable")
set(PI2_APP_NAME "${DEFAULT_PI2_NAME}" CACHE STRING "Name der Pi 2 Executable")

# Gemeinsame Quellcode-Dateien
set(COMMON_SRC src/common/crypto_utils.c)

# Raspberry Pi 1 (Host/Bedienung)
add_executable(${PI1_APP_NAME}
    src/pi1_host/main_pi1.c
    src/pi1_host/ui_lcd.c
    src/pi1_host/logging.c
    src/pi1_host/bt_client.c
    ${COMMON_SRC}
)
target_link_libraries(${PI1_APP_NAME}
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    ${BLUETOOTH_LIBRARIES}
    ${PIGPIO_LIBRARIES}
)

# Raspberry Pi 2 (Safety/Steuerung)
add_executable(${PI2_APP_NAME}
    src/pi2_safety/main_pi2.c
    src/pi2_safety/safety_logic.c
    src/pi2_safety/sensors.c
    src/pi2_safety/motor.c
    src/pi2_safety/bt_server.c
    ${COMMON_SRC}
)
target_link_libraries(${PI2_APP_NAME}
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    ${BLUETOOTH_LIBRARIES}
    ${PIGPIO_LIBRARIES}
)

cmake_minimum_required(VERSION 3.10)
project(SafetyDemonstrator C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_definitions(-D_GNU_SOURCE)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Werror)
endif()

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

# ---------------------------------------------------------------
# Abhängigkeiten
# ---------------------------------------------------------------
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# BlueZ (RFCOMM). Ohne BlueZ steht nur der TCP-Entwicklungstransport zur Verfügung.
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(BLUEZ QUIET bluez)
endif()

# pigpio (nur auf dem Raspberry Pi vorhanden)
find_path(PIGPIO_INCLUDE_DIR pigpio.h)
find_library(PIGPIO_LIBRARY pigpio)

# Hardware-Abstraktion für Pi 2: pigpio (reale Hardware) oder sim (Simulation)
set(PI2_HAL "auto" CACHE STRING "HAL fuer Pi 2: auto, pigpio oder sim")
set_property(CACHE PI2_HAL PROPERTY STRINGS auto pigpio sim)
set(PI2_HAL_SELECTED "${PI2_HAL}")
if(PI2_HAL_SELECTED STREQUAL "auto")
    if(PIGPIO_INCLUDE_DIR AND PIGPIO_LIBRARY)
        set(PI2_HAL_SELECTED "pigpio")
    else()
        set(PI2_HAL_SELECTED "sim")
    endif()
endif()
if(PI2_HAL_SELECTED STREQUAL "pigpio" AND NOT (PIGPIO_INCLUDE_DIR AND PIGPIO_LIBRARY))
    message(FATAL_ERROR "PI2_HAL=pigpio, aber pigpio wurde nicht gefunden (libpigpio-dev installieren)")
endif()
message(STATUS "Pi 2 HAL: ${PI2_HAL_SELECTED}")
if(PI2_HAL_SELECTED STREQUAL "sim")
    message(WARNING "Pi 2 wird mit SIMULIERTER Hardware gebaut - nicht fuer den realen Aufbau geeignet.")
endif()

# ---------------------------------------------------------------
# Gemeinsame Quellcode-Dateien (Pi 1 und Pi 2)
# ---------------------------------------------------------------
add_library(common STATIC
    src/common/crypto_utils.c
    src/common/protocol.c
    src/common/time_utils.c
)
target_include_directories(common PUBLIC src/common)

# ---------------------------------------------------------------
# Raspberry Pi 2 (Safety/Steuerung)
# ---------------------------------------------------------------
# Hardwareunabhängiger Teil (wird auch von den Unit-Tests verwendet).
# Er wird zweimal gebaut: normal und mit SSM_TESTMODE (Fehlereinspeisung,
# Test-Log). Im normalen Build ist kein Testcode enthalten.
set(PI2_CORE_SRC
    src/pi2_safety/safety_logic.c
    src/pi2_safety/sensors.c
    src/pi2_safety/motor.c
    src/pi2_safety/bt_server.c
    src/pi2_safety/shared.c
    src/pi2_safety/watchdog.c
)
if(NOT BLUEZ_FOUND)
    message(WARNING "BlueZ nicht gefunden: Pi 2 kann nur mit --tcp gestartet werden.")
endif()

function(add_pi2_core name)
    add_library(${name} STATIC ${PI2_CORE_SRC})
    target_include_directories(${name} PUBLIC src/pi2_safety)
    target_link_libraries(${name} PUBLIC common Threads::Threads)
    if(BLUEZ_FOUND)
        target_compile_definitions(${name} PRIVATE HAVE_BLUEZ)
        target_include_directories(${name} PRIVATE ${BLUEZ_INCLUDE_DIRS})
        target_link_libraries(${name} PUBLIC ${BLUEZ_LIBRARIES})
    endif()
endfunction()

function(add_pi2_app name core)
    add_executable(${name} src/pi2_safety/main_pi2.c)
    if(PI2_HAL_SELECTED STREQUAL "pigpio")
        target_sources(${name} PRIVATE src/pi2_safety/hal_pigpio.c)
        target_include_directories(${name} PRIVATE ${PIGPIO_INCLUDE_DIR})
        target_link_libraries(${name} ${PIGPIO_LIBRARY} rt)
    else()
        target_sources(${name} PRIVATE src/pi2_safety/hal_sim.c)
    endif()
    target_link_libraries(${name} ${core})
endfunction()

# Normaler Build
add_pi2_core(pi2_core)
add_pi2_app(${PI2_APP_NAME} pi2_core)

# Testmodus (Designdokument Abschnitt 2.10, TC-01 bis TC-04, Nachlaufmessung)
add_pi2_core(pi2_core_test)
target_compile_definitions(pi2_core_test PUBLIC SSM_TESTMODE)
add_pi2_app(${PI2_APP_NAME}_test pi2_core_test)

# ---------------------------------------------------------------
# Raspberry Pi 1 (Host/Bedienung) - wird gebaut, sobald der Quellcode existiert
# ---------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/pi1_host/main_pi1.c")
    find_package(OpenSSL REQUIRED)
    add_executable(${PI1_APP_NAME}
        src/pi1_host/main_pi1.c
        src/pi1_host/ui_lcd.c
        src/pi1_host/logging.c
        src/pi1_host/bt_client.c
    )
    target_link_libraries(${PI1_APP_NAME}
        common
        Threads::Threads
        OpenSSL::SSL
        OpenSSL::Crypto
        ${BLUEZ_LIBRARIES}
        ${PIGPIO_LIBRARY}
    )
else()
    message(STATUS "src/pi1_host/ fehlt noch - ${PI1_APP_NAME} wird nicht gebaut.")
endif()

# ---------------------------------------------------------------
# Unit-Tests (hardwareunabhängig): ctest oder make test
# ---------------------------------------------------------------
enable_testing()
add_executable(test_pi2 tests/test_pi2.c src/pi2_safety/hal_sim.c)
target_link_libraries(test_pi2 pi2_core)
add_test(NAME pi2_unit_tests COMMAND test_pi2)

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
# OpenSSL (AES-256-GCM, EVP-Schnittstelle) fuer beide Pis
find_package(OpenSSL REQUIRED)

add_library(common STATIC
    src/common/crypto_utils.c
    src/common/names.c
    src/common/protocol.c
    src/common/proto_stream.c
    src/common/session.c
    src/common/time_utils.c
    src/common/transport.c
)
target_include_directories(common PUBLIC src/common)
target_link_libraries(common PUBLIC OpenSSL::Crypto)
if(BLUEZ_FOUND)
    target_compile_definitions(common PRIVATE HAVE_BLUEZ)
    target_include_directories(common PRIVATE ${BLUEZ_INCLUDE_DIRS})
    target_link_libraries(common PUBLIC ${BLUEZ_LIBRARIES})
endif()

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
    # BlueZ (RFCOMM) kommt ueber common/transport.c
    target_link_libraries(${name} PUBLIC common Threads::Threads)
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
# Raspberry Pi 1 (Host/Bedienung)
# ---------------------------------------------------------------
# Alles ausser main_pi1.c als Bibliothek, damit die Unit-Tests es nutzen koennen.
add_library(pi1_core STATIC
    src/pi1_host/bt_client.c
    src/pi1_host/input.c
    src/pi1_host/lcd_console.c
    src/pi1_host/lcd_i2c.c
    src/pi1_host/logging.c
    src/pi1_host/options.c
    src/pi1_host/shared.c
    src/pi1_host/ui_lcd.c
)
target_include_directories(pi1_core PUBLIC src/pi1_host)
target_link_libraries(pi1_core PUBLIC common Threads::Threads)

add_executable(${PI1_APP_NAME} src/pi1_host/main_pi1.c)
target_link_libraries(${PI1_APP_NAME} pi1_core)

# Simulierter Pi 2 (Testwerkzeug fuer Pi 1, TCP)
add_executable(mock_pi2 tools/mock_pi2/mock_pi2.c)
target_link_libraries(mock_pi2 common)

# ---------------------------------------------------------------
# Unit-Tests (hardwareunabhängig): ctest oder make test
# ---------------------------------------------------------------
enable_testing()
add_executable(test_pi2 tests/test_pi2.c src/pi2_safety/hal_sim.c)
target_link_libraries(test_pi2 pi2_core)
add_test(NAME pi2_unit_tests COMMAND test_pi2)

add_executable(test_common tests/test_common.c)
target_link_libraries(test_common common)
add_test(NAME common_unit_tests COMMAND test_common)

add_executable(test_pi1 tests/test_pi1.c)
target_link_libraries(test_pi1 pi1_core)
add_test(NAME pi1_unit_tests COMMAND test_pi1)

# ---------------------------------------------------------------
# Statische Analyse: make cppcheck (cppcheck mit MISRA-Addon)
# Geprueft werden src/common, src/pi1_host und tools/mock_pi2.
# ---------------------------------------------------------------
find_program(CPPCHECK_EXE cppcheck)
if(CPPCHECK_EXE)
    add_custom_target(cppcheck
        COMMAND ${CPPCHECK_EXE}
            --addon=misra
            --enable=warning,style,performance,portability
            --std=c11 -D_GNU_SOURCE -DHAVE_BLUEZ
            --inline-suppr --error-exitcode=0
            --suppress=missingIncludeSystem
            -I ${CMAKE_SOURCE_DIR}/src/common
            -I ${CMAKE_SOURCE_DIR}/src/pi1_host
            ${CMAKE_SOURCE_DIR}/src/common
            ${CMAKE_SOURCE_DIR}/src/pi1_host
            ${CMAKE_SOURCE_DIR}/tools/mock_pi2
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "cppcheck mit MISRA-Addon"
        VERBATIM)
else()
    message(STATUS "cppcheck nicht gefunden - Target 'cppcheck' fehlt")
endif()

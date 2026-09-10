cmake_minimum_required(VERSION 3.16) # same we use in idf
project(libjade C CXX)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Choose the type of build (Debug|Release|RelWithDebInfo|MinSizeRel|Sanitize)" FORCE)
  message(STATUS "Building in ${CMAKE_BUILD_TYPE} mode (default)")
else()
  message(STATUS "Building in ${CMAKE_BUILD_TYPE} mode (user provided or cached)")
endif()

if(DEFINED ENV{IDF_PATH})
    set(IDF_PATH $ENV{IDF_PATH})
    message(STATUS "Using IDF_PATH from environment: ${IDF_PATH}")
else()
    message(FATAL_ERROR "IDF_PATH is not defined. Please set the IDF_PATH environment variable.")
endif()

find_package(Threads REQUIRED)
set(CMAKE_POSITION_INDEPENDENT_CODE ON CACHE BOOL "Build position-independent code" FORCE)

set(mbedtls_targets mbedtls mbedcrypto mbedx509 everest p256m)
set(ENABLE_TESTING CACHE BOOL OFF)
set(ENABLE_PROGRAMS CACHE BOOL OFF)
set(GEN_FILES CACHE BOOL OFF)
add_subdirectory(${IDF_PATH}/components/mbedtls/mbedtls
                 ${CMAKE_CURRENT_BINARY_DIR}/mbedtls)

set(CBOR_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/espressif__cbor/tinycbor/src")
file(GLOB CBOR_SOURCES "${CBOR_SOURCE_DIR}/*.c")
add_library(cbor_target STATIC ${CBOR_SOURCES})
target_include_directories(cbor_target PUBLIC "${CBOR_SOURCE_DIR}")
set(CBOR_INCLUDE_DIR "${CBOR_SOURCE_DIR}")
set(ESP_MOCKED "${CMAKE_CURRENT_SOURCE_DIR}/include")

set(OTPAUTH_MIGRATE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../components/google-otpauth-migrate")
file(GLOB OTPAUTH_MIGRATE_SOURCES "${OTPAUTH_MIGRATE_SOURCE_DIR}/*.c")
add_library(otpauth_migrate_target STATIC "${OTPAUTH_MIGRATE_SOURCES}")
target_include_directories(otpauth_migrate_target PUBLIC "${OTPAUTH_MIGRATE_SOURCE_DIR}")
set(OTPAUTH_MIGRATE_INCLUDE_DIR "${OTPAUTH_MIGRATE_SOURCE_DIR}")

# Note -DVERIFY enables (expensive) libsecp256k1 verification
set(CMAKE_C_FLAGS_DEBUG "-O0 -ggdb3 -DVERIFY -D_FORTIFY_SOURCE=3 -D_GLIBCXX_ASSERTIONS -fstack-protector-strong -fstack-clash-protection")

# BBB-AIRGAP: the shipping build gets the same hardening the debug build already had.
# Measured on the image's own compiler (gcc 12.2.0, Raspbian) before being added here; nothing
# is listed that was not compiled.  _FORTIFY_SOURCE needs an optimising build, which Release is.
# _GLIBCXX_ASSERTIONS only affects C++ but is set on both so the two lists stay comparable.
# The linker options are set for this build type alone: relro plus BIND_NOW makes the GOT
# read-only once the dynamic loader is done, so an overwritten entry cannot redirect a later
# call.  Verified after building with nm -D (__stack_chk_fail present) and readelf -d (BIND_NOW).
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG -D_FORTIFY_SOURCE=3 -D_GLIBCXX_ASSERTIONS -fstack-protector-strong -fstack-clash-protection")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG -D_FORTIFY_SOURCE=3 -D_GLIBCXX_ASSERTIONS -fstack-protector-strong -fstack-clash-protection")
set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "-Wl,-z,relro,-z,now")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-Wl,-z,relro,-z,now")

if(CMAKE_BUILD_TYPE STREQUAL "Sanitize")
    add_compile_options(-O1 -ggdb -fsanitize=address -fsanitize=undefined -fsanitize=alignment -fsanitize-address-use-after-scope -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address -fsanitize=undefined -fsanitize=alignment -fsanitize-address-use-after-scope -fno-sanitize-recover=all)
endif()

option(LOG "Enable libjade logging" OFF)
if (NOT LOG)
    add_compile_options(-DCONFIG_LOG_DEFAULT_LEVEL_NONE)
endif()

option(COVERAGE "Enable libjade profile coverage" OFF)
if (COVERAGE)
    add_compile_options(-fprofile-arcs -ftest-coverage)
    add_link_options(-lgcov --coverage)
endif()

# CI mode defaults to ON since libjade is typically used for testing
option(CI "Enable CI (auto-click) mode" ON)
if (CI)
    add_compile_options(-DCONFIG_DEBUG_UNATTENDED_CI)
endif()

# BBB-AIRGAP: the panel is a 240x240 ST7789 on a Waveshare 1.3" HAT, not the 320x200 placeholder
# libjade compiles with. Upstream's own FIXME in sdkconfig.h asks for this to be configurable.
set(DISPLAY_WIDTH "320" CACHE STRING "libjade display width in pixels")
set(DISPLAY_HEIGHT "200" CACHE STRING "libjade display height in pixels")
add_compile_options(-DCONFIG_DISPLAY_WIDTH=${DISPLAY_WIDTH} -DCONFIG_DISPLAY_HEIGHT=${DISPLAY_HEIGHT})

# BBB-AIRGAP: debug mode gates Jade's own debug message handlers (debug_set_mnemonic writes a
# seed straight into the device) and libjade's RPC surface (reads the display, presses the
# buttons, exports nvs - see libjade.c). Neither may be present in a device that holds keys,
# so this is a build option. It defaults ON to keep the upstream test tooling working.
option(DEBUG_MODE "Enable Jade debug handlers and the libjade RPC surface" ON)
if (NOT DEBUG_MODE)
    add_compile_options(-DCONFIG_LIBJADE_NO_DEBUG_MODE)
endif()

option(CAMERA "Enable libjade camera support" OFF)
if (CAMERA)
    add_compile_options(-DCONFIG_LIBJADE_CAMERA)
endif()

set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN YES)

# for consistency with the Jade build - we can change if needed
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-exceptions")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")

set(COMMON_INCLUDES
  ${CBOR_INCLUDE_DIR}
  ${OTPAUTH_MIGRATE_INCLUDE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../main
  # BBB-AIRGAP: main/qrmode.c includes miner.h; miner.c itself is pulled in by libjade.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../components/miner
  ${CMAKE_CURRENT_SOURCE_DIR}/../components/libwally-core/upstream/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../components/assets
  ${CMAKE_CURRENT_SOURCE_DIR}/../components/esp32_bsdiff
  ${CMAKE_CURRENT_SOURCE_DIR}/../components/esp32-quirc
  ${CMAKE_CURRENT_SOURCE_DIR}/../components/esp32-quirc/lib
  ${CMAKE_CURRENT_SOURCE_DIR}/../components/libwally-core/upstream
  ${CMAKE_CURRENT_SOURCE_DIR}/../components/libwally-core/upstream/src
  ${CMAKE_CURRENT_SOURCE_DIR}/../components/libwally-core/upstream/src/ccan
  ${CMAKE_CURRENT_SOURCE_DIR}/../components/libwally-core
  ${CMAKE_CURRENT_SOURCE_DIR}/../components/libwally-core/upstream/src/secp256k1/include
  ${IDF_PATH}/components/http_parser
)

set(COMMON_SRC
  libjade.c
  # BBB-AIRGAP: installs a std::terminate handler so a C++ throw clears keys before dying.
  # See the file header for why the firmware's -Wl,--wrap=abort cannot do this job here.
  cxx_terminate.cpp
  ${IDF_PATH}/components/http_parser/http_parser.c
)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../components/esp32_bc-ur
                 ${CMAKE_CURRENT_BINARY_DIR}/bcur)
target_include_directories(bcur PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../components/libwally-core/upstream/include)
target_link_libraries(bcur PRIVATE mbedcrypto)

# libjade.so
add_library(jade SHARED ${COMMON_SRC})
target_link_libraries(jade PRIVATE m z bcur cbor_target otpauth_migrate_target mbedtls mbedcrypto mbedx509)
target_include_directories(jade PRIVATE ${COMMON_INCLUDES})

set_source_files_properties(libjade.c PROPERTIES COMPILE_OPTIONS
  "-Wall;-W;-Wno-format;-Wno-unused-function;-Wno-unused-parameter;-Wno-sign-compare;-Wno-missing-field-initializers;-Wno-narrowing")
target_compile_definitions(jade PRIVATE LIBJADE_BUILD)

# libjade selfcheck overrides, used for internal firmware unit tests.
# Build with -DSELFCHECK=<test> to link libjade/selfcheck/<test>.c.
set(SELFCHECK "" CACHE STRING "Link a given debug_selfcheck implementation for testing")

# libjade.a
add_library(jade_static STATIC ${COMMON_SRC})
target_link_libraries(jade_static PRIVATE m z mbedtls mbedcrypto mbedx509 bcur)
target_include_directories(jade_static PRIVATE ${COMMON_INCLUDES})
target_compile_definitions(jade_static PRIVATE LIBJADE_BUILD)

if(NOT "${SELFCHECK}" STREQUAL "")
    target_compile_definitions(jade PRIVATE SELFCHECK=${SELFCHECK})
    # BBB-AIRGAP: measured with a daemon build; selecting only jade silently ran the default
    # debug_selfcheck because libjade_daemon links jade_static.
    target_compile_definitions(jade_static PRIVATE SELFCHECK=${SELFCHECK})
endif()

# BBB-AIRGAP: settings_store.c is pijade-host's; linking it here lets the emulator persist
# settings through the same code the card runs, which is what makes storage failure paths testable.
add_executable(libjade_daemon daemon.c ${CMAKE_CURRENT_SOURCE_DIR}/../pijade/host/settings_store.c)
target_link_libraries(libjade_daemon PRIVATE jade_static bcur cbor_target otpauth_migrate_target m z mbedtls mbedcrypto mbedx509 Threads::Threads)
target_include_directories(libjade_daemon PRIVATE ${COMMON_INCLUDES})

# Assets
set(ASSETS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../components/assets)

add_custom_command(OUTPUT asset_data.inc
    COMMAND python3 ${ASSETS_DIR}/gen_assets.py ${ASSETS_DIR}/asset_data.json ${CMAKE_CURRENT_BINARY_DIR}/asset_data.inc
    DEPENDS ${ASSETS_DIR}/gen_assets.py ${ASSETS_DIR}/asset_data.json
    VERBATIM)

add_custom_command(OUTPUT asset_data_testnet.inc
    COMMAND python3 ${ASSETS_DIR}/gen_assets.py ${ASSETS_DIR}/asset_data_testnet.json ${CMAKE_CURRENT_BINARY_DIR}/asset_data_testnet.inc
    DEPENDS ${ASSETS_DIR}/gen_assets.py ${ASSETS_DIR}/asset_data_testnet.json
    VERBATIM)

add_custom_target(asset_data DEPENDS asset_data.inc asset_data_testnet.inc)
add_dependencies(jade asset_data)
add_dependencies(jade_static asset_data)

target_compile_options(jade PRIVATE -Werror=unused-result)
target_compile_options(jade_static PRIVATE -Werror=unused-result)
target_compile_options(libjade_daemon PRIVATE -Werror=unused-result)

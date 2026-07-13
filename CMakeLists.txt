cmake_minimum_required(VERSION 3.20)
project(openrsp VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
include(CheckLanguage)
check_language(CXX)
if(CMAKE_CXX_COMPILER)
    enable_language(CXX)
    set(CMAKE_CXX_STANDARD 11)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

# glibc hides POSIX/GNU declarations such as usleep, getopt, and sigaction
# under strict C11 unless a feature profile is selected.  Apple exposes these
# declarations without this definition.
if(UNIX AND NOT APPLE)
    add_compile_definitions(_DEFAULT_SOURCE)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
find_package(Threads REQUIRED)

add_library(openrsp src/device_db.c src/discovery.c src/session.c)
add_library(openrsp::openrsp ALIAS openrsp)
target_include_directories(openrsp PUBLIC include)
target_link_libraries(openrsp PUBLIC PkgConfig::LIBUSB)
target_compile_options(openrsp PRIVATE -Wall -Wextra -Wpedantic -Werror)

if(UNIX)
    add_library(openrsp_client src/client_posix.c)
    set_target_properties(openrsp_client PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(openrsp_client PUBLIC include)
    target_compile_options(openrsp_client PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_library(openrsp_daemon_backend STATIC compat/daemon_backend.c)
    set_target_properties(openrsp_daemon_backend PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(openrsp_daemon_backend PUBLIC compat include)
    target_link_libraries(openrsp_daemon_backend PUBLIC openrsp_client Threads::Threads)
    target_compile_options(openrsp_daemon_backend PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(openrsp-probe tools/openrsp_probe.c)
target_link_libraries(openrsp-probe PRIVATE openrsp::openrsp)
target_compile_options(openrsp-probe PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(openrsp-lifecycle tools/openrsp_lifecycle.c)
target_link_libraries(openrsp-lifecycle PRIVATE openrsp::openrsp)
target_compile_options(openrsp-lifecycle PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(openrsp-reset tools/openrsp_reset.c)
target_link_libraries(openrsp-reset PRIVATE openrsp::openrsp)
target_compile_options(openrsp-reset PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(openrsp-extract-firmware tools/openrsp_extract_firmware.c)
target_compile_options(openrsp-extract-firmware PRIVATE -Wall -Wextra -Wpedantic -Werror)

if(UNIX)
    add_executable(openrspd daemon/openrspd.c)
    target_include_directories(openrspd PRIVATE include)
    target_link_libraries(openrspd PRIVATE openrsp_miri Threads::Threads m)
    target_compile_options(openrspd PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

enable_testing()
add_executable(device-db-test tests/device_db_test.c)
target_link_libraries(device-db-test PRIVATE openrsp::openrsp)
target_compile_options(device-db-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_compile_options(device-db-test PRIVATE -UNDEBUG)
add_test(NAME device-db COMMAND device-db-test)

if(UNIX)
    add_executable(openrsp-client-test tests/client_test.c)
    target_link_libraries(openrsp-client-test PRIVATE openrsp_client)
    target_compile_options(openrsp-client-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(openrsp-client-test PRIVATE -UNDEBUG)
    add_executable(openrsp-client-timeout-test
        tests/client_timeout_test.c src/client_posix.c)
    target_include_directories(openrsp-client-timeout-test PRIVATE include)
    target_link_libraries(openrsp-client-timeout-test PRIVATE Threads::Threads)
    target_compile_definitions(openrsp-client-timeout-test PRIVATE
        OPENRSP_SOCKET_TIMEOUT_MILLISECONDS=100)
    target_compile_options(openrsp-client-timeout-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    add_test(NAME openrsp-client-timeout COMMAND openrsp-client-timeout-test)
    add_executable(openrspd-multi-client-test tests/openrspd_multi_client_test.c)
    target_link_libraries(openrspd-multi-client-test PRIVATE openrsp_client)
    target_compile_options(openrspd-multi-client-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(openrspd-multi-client-test PRIVATE -UNDEBUG)
    add_test(NAME openrspd-multi-client
        COMMAND openrspd-multi-client-test $<TARGET_FILE:openrspd>)
endif()

# GPL-2.0-or-later direct hardware backend derived from libmirisdr-5.
add_library(openrsp_miri STATIC third_party/libmirisdr/src/libmirisdr.c)
target_include_directories(openrsp_miri PUBLIC third_party/libmirisdr/include)
target_include_directories(openrsp_miri PRIVATE third_party/libmirisdr/src)
target_compile_definitions(openrsp_miri PUBLIC mirisdr_STATIC)
target_link_libraries(openrsp_miri PUBLIC PkgConfig::LIBUSB Threads::Threads m)

add_executable(rspduo-pll-test tests/rspduo_pll_test.c)
target_link_libraries(rspduo-pll-test PRIVATE openrsp_miri)
target_compile_options(rspduo-pll-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_compile_options(rspduo-pll-test PRIVATE -UNDEBUG)
add_test(NAME rspduo-pll COMMAND rspduo-pll-test)

add_executable(rspduo-async-state-test tests/rspduo_async_state_test.c)
target_include_directories(rspduo-async-state-test PRIVATE
    third_party/libmirisdr/include third_party/libmirisdr/src)
target_link_libraries(rspduo-async-state-test PRIVATE openrsp_miri Threads::Threads)
target_compile_options(rspduo-async-state-test PRIVATE
    -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
add_test(NAME rspduo-async-state COMMAND rspduo-async-state-test)

add_executable(openrsp-iq third_party/libmirisdr/src/miri_sdr.c)
target_link_libraries(openrsp-iq PRIVATE openrsp_miri)

add_executable(openrsp-miri-probe tools/openrsp_miri_probe.c)
target_link_libraries(openrsp-miri-probe PRIVATE openrsp_miri)

if(UNIX)
    add_library(sdrplay_api_compat SHARED compat/sdrplay_api_compat.c)
    target_compile_definitions(sdrplay_api_compat PRIVATE OPENRSP_SDRPLAY_API_BUILD=1)
    target_include_directories(sdrplay_api_compat PUBLIC compat)
    target_link_libraries(sdrplay_api_compat PRIVATE openrsp_daemon_backend Threads::Threads m)
    target_compile_options(sdrplay_api_compat PRIVATE -Wall -Wextra -Wpedantic -Werror)
    set_target_properties(sdrplay_api_compat PROPERTIES
        OUTPUT_NAME sdrplay_api
        PREFIX "lib"
        SUFFIX ".so.3.15"
    )

    add_executable(sdrplay-api-compat-test tests/sdrplay_api_compat_test.c)
    target_include_directories(sdrplay-api-compat-test PRIVATE include compat)
    target_link_libraries(sdrplay-api-compat-test PRIVATE sdrplay_api_compat Threads::Threads m)
    target_compile_options(sdrplay-api-compat-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(sdrplay-api-compat-test PRIVATE -UNDEBUG)
    add_test(NAME sdrplay-api-compat COMMAND sdrplay-api-compat-test)

    add_executable(sdrplay-abi-layout-test tools/sdrplay_abi_layout.c)
    target_include_directories(sdrplay-abi-layout-test PRIVATE compat)
    target_compile_options(sdrplay-abi-layout-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME sdrplay-abi-layout COMMAND sdrplay-abi-layout-test)

    add_executable(sdrplay-header-compat-test tests/sdrplay_header_compat_test.c)
    target_include_directories(sdrplay-header-compat-test PRIVATE compat)
    target_link_libraries(sdrplay-header-compat-test PRIVATE sdrplay_api_compat)
    target_compile_options(sdrplay-header-compat-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    add_test(NAME sdrplay-header-compat COMMAND sdrplay-header-compat-test)

    if(CMAKE_CXX_COMPILER)
        add_executable(sdrplay-header-cpp-test tests/sdrplay_header_cpp_test.cpp)
        target_include_directories(sdrplay-header-cpp-test PRIVATE compat)
        target_link_libraries(sdrplay-header-cpp-test PRIVATE sdrplay_api_compat)
        target_compile_options(sdrplay-header-cpp-test PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
        add_test(NAME sdrplay-header-cpp COMMAND sdrplay-header-cpp-test)
    endif()

    add_executable(sdrplay-compat-stream-test tools/sdrplay_compat_stream_test.c)
    target_link_libraries(sdrplay-compat-stream-test PRIVATE sdrplay_api_compat Threads::Threads m)
    target_compile_options(sdrplay-compat-stream-test PRIVATE -Wall -Wextra -Wpedantic -Werror)

    add_executable(sdrplay-gain-sweep-test tools/sdrplay_gain_sweep_test.c)
    target_link_libraries(sdrplay-gain-sweep-test PRIVATE sdrplay_api_compat Threads::Threads)
    target_compile_options(sdrplay-gain-sweep-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
else()
    message(STATUS "OpenRSP direct driver builds on this platform; the local API-compatibility daemon is currently Unix-only")
endif()

if(APPLE)
    add_library(openrsp_libusb_trace SHARED tools/libusb_trace_interpose.c)
    target_link_libraries(openrsp_libusb_trace PRIVATE PkgConfig::LIBUSB ${CMAKE_DL_LIBS})
    target_compile_options(openrsp_libusb_trace PRIVATE -Wall -Wextra -Wpedantic -Werror)
    set_target_properties(openrsp_libusb_trace PROPERTIES OUTPUT_NAME openrsp_libusb_trace)
endif()

cmake_minimum_required(VERSION 3.20)
project(openrsp VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
find_package(Threads REQUIRED)

add_library(openrsp src/device_db.c src/discovery.c src/session.c)
add_library(openrsp::openrsp ALIAS openrsp)
target_include_directories(openrsp PUBLIC include)
target_link_libraries(openrsp PUBLIC PkgConfig::LIBUSB)
target_compile_options(openrsp PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(openrsp-probe tools/openrsp_probe.c)
target_link_libraries(openrsp-probe PRIVATE openrsp::openrsp)
target_compile_options(openrsp-probe PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(openrsp-lifecycle tools/openrsp_lifecycle.c)
target_link_libraries(openrsp-lifecycle PRIVATE openrsp::openrsp)
target_compile_options(openrsp-lifecycle PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(openrsp-reset tools/openrsp_reset.c)
target_link_libraries(openrsp-reset PRIVATE openrsp::openrsp)
target_compile_options(openrsp-reset PRIVATE -Wall -Wextra -Wpedantic -Werror)

enable_testing()
add_executable(device-db-test tests/device_db_test.c)
target_link_libraries(device-db-test PRIVATE openrsp::openrsp)
target_compile_options(device-db-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME device-db COMMAND device-db-test)

# GPL-2.0-or-later direct hardware backend derived from libmirisdr-5.
add_library(openrsp_miri STATIC third_party/libmirisdr/src/libmirisdr.c)
target_include_directories(openrsp_miri PUBLIC third_party/libmirisdr/include)
target_include_directories(openrsp_miri PRIVATE third_party/libmirisdr/src)
target_compile_definitions(openrsp_miri PUBLIC mirisdr_STATIC)
target_link_libraries(openrsp_miri PUBLIC PkgConfig::LIBUSB Threads::Threads m)

add_executable(openrsp-iq third_party/libmirisdr/src/miri_sdr.c)
target_link_libraries(openrsp-iq PRIVATE openrsp_miri)

add_executable(openrsp-miri-probe tools/openrsp_miri_probe.c)
target_link_libraries(openrsp-miri-probe PRIVATE openrsp_miri)

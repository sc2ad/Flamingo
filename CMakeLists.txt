cmake_minimum_required(VERSION 3.22)

# c++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED 20)

# define that stores the actual source directory
set(SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)
set(EXTERN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/extern)
set(SHARED_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shared)

if (NOT DEFINED CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE "Debug")
endif()

add_compile_options(-fno-rtti -fPIE -fPIC -fno-exceptions -fcolor-diagnostics)
add_link_options(-lstdc++ -lm)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Flamingo static build always exists
add_library(flamingo-static ${SOURCE_DIR}/fixups.cpp ${SOURCE_DIR}/installer.cpp ${SOURCE_DIR}/page-allocator.cpp)
target_compile_options(flamingo-static PRIVATE -Wall -Wextra -Werror -Wpedantic -DFMT_HEADER_ONLY)

option(TEST_BUILD "Enable local executable test builds for sanity checks" ON)

# If we are making a test build, go into this block instead
if (TEST_BUILD)
    project(flamingo-test)

    # Fetch capstone (v5.0.1) via: pip install capstone==5.0.1 -U --user
    include(FetchContent)
    FetchContent_Declare(
        capstone
        GIT_REPOSITORY https://github.com/capstone-engine/capstone.git
        GIT_TAG 5.0.1
    )

    set(CAPSTONE_ARCHITECTURE_DEFAULT OFF)
    set(CAPSTONE_ARM64_SUPPORT ON)

    FetchContent_MakeAvailable(capstone)
    target_link_libraries(flamingo-static PUBLIC capstone)
    target_include_directories(flamingo-static PUBLIC ${SHARED_DIR} ${capstone_SOURCE_DIR}/include ${EXTERN_DIR}/includes/fmt/fmt/include)

    add_compile_definitions(TEST_BUILD)
    MESSAGE(STATUS "Compiling test build")

    add_executable(fixup-test ${CMAKE_CURRENT_SOURCE_DIR}/test/main.cpp)
    target_link_libraries(fixup-test PRIVATE flamingo-static)
else()
    MESSAGE(ERROR "No NDK support yet! Try using -DTEST_BUILD=ON")
endif()

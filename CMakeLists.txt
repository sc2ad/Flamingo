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

add_compile_options(-fno-rtti -fPIE -fPIC -fno-exceptions -fcolor-diagnostics -DFMT_HEADER_ONLY)
add_link_options(-lm)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# get git info
execute_process(COMMAND git config user.name OUTPUT_VARIABLE GIT_USER)
execute_process(COMMAND git branch --show-current OUTPUT_VARIABLE GIT_BRANCH)
execute_process(COMMAND git rev-parse --short HEAD OUTPUT_VARIABLE GIT_COMMIT)
execute_process(COMMAND git diff-index --quiet HEAD RESULT_VARIABLE GIT_MODIFIED)

string(STRIP "${GIT_USER}" GIT_USER)
string(STRIP "${GIT_BRANCH}" GIT_BRANCH)
string(STRIP "${GIT_COMMIT}" GIT_COMMIT)
string(STRIP "${GIT_MODIFIED}" GIT_MODIFIED)

message(STATUS "GIT_USER: ${GIT_USER}")
message(STATUS "GIT_BRANCH: ${GIT_BRANCH}")
message(STATUS "GIT_COMMIT: 0x${GIT_COMMIT}")
message(STATUS "GIT_MODIFIED: ${GIT_MODIFIED}")

# Check for file presence and read current contents
set(GIT_INFO_H_PATH "${CMAKE_CURRENT_SOURCE_DIR}/include/git_info.inc")
if(EXISTS "${GIT_INFO_H_PATH}")
    file(READ "${GIT_INFO_H_PATH}" GIT_INFO_H_CURRENT)
else()
    set(GIT_INFO_H_CURRENT "")
endif()

# Define new git info content
set(GIT_INFO_H "#pragma once
#define GIT_USER \"${GIT_USER}\"
#define GIT_BRANCH \"${GIT_BRANCH}\"
#define GIT_COMMIT 0x${GIT_COMMIT}
#define GIT_MODIFIED ${GIT_MODIFIED}
")

# Write git info to file if the contents have changed
if(NOT "${GIT_INFO_H}" STREQUAL "${GIT_INFO_H_CURRENT}")
    file(WRITE "${GIT_INFO_H_PATH}" "${GIT_INFO_H}")
endif()

option(TEST_BUILD "Enable local executable test builds for sanity checks" ON)

# If we are making a test build, go into this block instead
if (TEST_BUILD)
    project(flamingo-test)

    # Static library of flamingo to link against for tests.
    # TODO: We may want to test a dynamic version at some point too
    add_library(flamingo-static ${SOURCE_DIR}/fixups.cpp ${SOURCE_DIR}/installer.cpp ${SOURCE_DIR}/page-allocator.cpp ${SOURCE_DIR}/capi.cpp)
    target_compile_options(flamingo-static PRIVATE -Wall -Wextra -Werror -Wpedantic -fvisibility=hidden)
    target_include_directories(flamingo-static PUBLIC ${SHARED_DIR})

    # Fetch capstone (v5.0.1)
    include(FetchContent)
    FetchContent_Declare(
        capstone
        GIT_REPOSITORY https://github.com/capstone-engine/capstone.git
        GIT_TAG 5.0.1
    )

    set(CAPSTONE_ARCHITECTURE_DEFAULT OFF)
    set(CAPSTONE_ARM64_SUPPORT ON)

    FetchContent_MakeAvailable(capstone)
    target_link_libraries(flamingo-static PUBLIC capstone -lstdc++)
    target_include_directories(flamingo-static PUBLIC ${capstone_SOURCE_DIR}/include ${EXTERN_DIR}/includes/fmt/fmt/include)

    add_compile_definitions(TEST_BUILD)
    MESSAGE(STATUS "Compiling test build")

    add_executable(fixup-test ${CMAKE_CURRENT_SOURCE_DIR}/test/main.cpp)
    target_link_libraries(fixup-test PRIVATE flamingo-static)

    add_executable(api-test ${CMAKE_CURRENT_SOURCE_DIR}/test/api.cpp)
    target_link_libraries(api-test PRIVATE flamingo-static)
    include(CTest)

    add_test(fixups fixup-test)
    add_test(apis api-test)
else()
    include(qpm_defines.cmake)
    project(${COMPILE_ID})

    add_library(${COMPILE_ID} SHARED)

    target_link_libraries(${COMPILE_ID} PRIVATE capstone -llog)
    target_include_directories(${COMPILE_ID} PRIVATE ${EXTERN_DIR}/includes ${EXTERN_DIR}/includes/capstone/shared ${EXTERN_DIR}/includes/fmt/fmt/include)
    # add shared dir as include dir
    target_include_directories(${COMPILE_ID} PUBLIC ${SHARED_DIR})
    target_include_directories(${COMPILE_ID} PRIVATE ${SOURCE_DIR} ${INCLUDE_DIR})

    # TODO: Until paper lets us build without exceptions, we don't use paper.
    target_link_options(${COMPILE_ID} PRIVATE -Wl,--exclude-libs,ALL)
    target_compile_options(${COMPILE_ID} PRIVATE -DFLAMINGO_HEADER_ONLY -fvisibility=hidden -Wall -Wextra -Werror -Wpedantic)

    target_sources(${COMPILE_ID} PUBLIC ${SOURCE_DIR}/capi.cpp ${SOURCE_DIR}/flamingo-stamp.cpp ${SOURCE_DIR}/fixups.cpp ${SOURCE_DIR}/installer.cpp ${SOURCE_DIR}/page-allocator.cpp)

    include(extern.cmake)

    add_custom_command(TARGET ${COMPILE_ID} POST_BUILD
            COMMAND ${CMAKE_STRIP} -S -d --strip-all
            "lib${COMPILE_ID}.so" -o "stripped_lib${COMPILE_ID}.so"
            COMMENT "Strip debug symbols done on final binary.")

    add_custom_command(TARGET ${COMPILE_ID} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory debug
                COMMENT "Create the debug dir"
    )
    add_custom_command(TARGET ${COMPILE_ID} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E rename lib${COMPILE_ID}.so debug/lib${COMPILE_ID}.so
                COMMENT "Rename the lib to debug_ since it has debug symbols"
                )

    add_custom_command(TARGET ${COMPILE_ID} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E rename stripped_lib${COMPILE_ID}.so lib${COMPILE_ID}.so
                COMMENT "Rename the stripped lib to regular"
                )
endif()

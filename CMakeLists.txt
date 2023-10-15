# include some defines automatically made by qpm
include(qpm_defines.cmake)

cmake_minimum_required(VERSION 3.22)
project(${COMPILE_ID})
# Get GSL
# include(FetchContent)

# FetchContent_Declare(GSL
#     GIT_REPOSITORY "https://github.com/microsoft/GSL"
#     GIT_TAG "v4.0.0"
# )

# FetchContent_MakeAvailable(GSL)

# FetchContent_Declare(
#   googletest
#   URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
# )
# # For Windows: Prevent overriding the parent project's compiler/linker settings
# set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
# FetchContent_MakeAvailable(googletest)

# c++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED 20)

# define that stores the actual source directory
set(SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

# compile options used
add_compile_options(-fno-rtti -flto -fPIE -fPIC -fno-exceptions -fcolor-diagnostics)
add_compile_definitions(MOD_VERSION="${MOD_VERSION}")
add_compile_definitions(MOD_ID="${MOD_ID}")
# TODO: For now, this is the only safe way we can build a binary
add_compile_definitions(FLAMINGO_HEADER_ONLY)
add_compile_options(-Wall -Wextra -Werror -Wpedantic)
# compile definitions used

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (DEFINED TEST_BUILD)
    MESSAGE(STATUS "Compiling with test defines")
endif()

add_library(
    ${COMPILE_ID} 
    SHARED 
)

# recursively get all src files
# RECURSE_FILES(cpp_file_list_utils ${SOURCE_DIR}/*.cpp)
# RECURSE_FILES(c_file_list_utils ${SOURCE_DIR}/*.c)
target_sources(${COMPILE_ID} PRIVATE ${SOURCE_DIR}/trampoline.cpp ${SOURCE_DIR}/trampoline-allocator.cpp)
# target_sources(${COMPILE_ID} PRIVATE ${c_file_list_utils})

# if (DEFINED TEST_BUILD)
# RECURSE_FILES(cpp_file_list_tests ${SOURCE_DIR}/tests/*.cpp)
# RECURSE_FILES(c_file_list_tests ${SOURCE_DIR}/tests/*.c)
# target_sources(${COMPILE_ID} PRIVATE ${cpp_file_list_tests})
# target_sources(${COMPILE_ID} PRIVATE ${c_file_list_tests})
# endif()

# add root dir as include dir
target_include_directories(${COMPILE_ID} PRIVATE ${CMAKE_SOURCE_DIR})
# add src dir as include dir
target_include_directories(${COMPILE_ID} PRIVATE ${SOURCE_DIR})
# add include dir as include dir
target_include_directories(${COMPILE_ID} PRIVATE ${INCLUDE_DIR})
# add shared dir as include dir
target_include_directories(${COMPILE_ID} PUBLIC ${SHARED_DIR})
target_link_libraries(${COMPILE_ID} PRIVATE -llog)

include(extern.cmake)
MESSAGE(STATUS "extern added!")

add_custom_command(TARGET ${COMPILE_ID} POST_BUILD
            COMMAND ${CMAKE_STRIP} -g -S -d --strip-all
            "lib${COMPILE_ID}.so" -o "stripped_lib${COMPILE_ID}.so"
            COMMENT "Strip debug symbols done on final binary.")

add_custom_command(TARGET ${COMPILE_ID} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E rename lib${COMPILE_ID}.so debug_lib${COMPILE_ID}.so
            COMMENT "Rename the lib to debug_ since it has debug symbols"
            )

add_custom_command(TARGET ${COMPILE_ID} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E rename stripped_lib${COMPILE_ID}.so lib${COMPILE_ID}.so
            COMMENT "Rename the stripped lib to regular"
            )

enable_testing()

add_compile_options(-Wall -Wextra -Werror -Wpedantic)
# add_executable(
#     test
#     test/main.cpp
# )
# target_include_directories(test PUBLIC ${SHARED_DIR})
# target_link_libraries(
#     test
#     GTest::gtest_main
# )

# include(GoogleTest)
# gtest_discover_tests(test)

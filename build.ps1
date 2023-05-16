
function Clean-Build-Folder {
    if (Test-Path -Path "build") {
        remove-item build -R
        new-item -Path build -ItemType Directory
    }
    else {
        new-item -Path build -ItemType Directory
    }
}

$NDKPath = Get-Content $PSScriptRoot/ndkpath.txt

# Clean-Build-Folder
# build tests

& cmake -G "Ninja" -DCMAKE_BUILD_TYPE="RelWithDebInfo" -DTEST_BUILD=1 -B build
& cmake --build ./build

$ExitCode = $LastExitCode


if (-not ($ExitCode -eq 0)) {
    $msg = "ExitCode: " + $ExitCode
    Write-Output $msg
    exit $ExitCode
}

# clean folder
Clean-Build-Folder
# build mod

& cmake -G "Ninja" -DCMAKE_BUILD_TYPE="RelWithDebInfo" -B build
& cmake --build ./build

$ExitCode = $LastExitCode

# Post build, we actually want to transform the compile_commands.json file such that it has no \\ characters and ONLY has / characters
# (Get-Content -Path build/compile_commands.json) |
#     ForEach-Object {$_ -Replace '\\\\', '/'} | Set-Content -Path build/compile_commands.json

# To build tests, we just compile with our local clang++ into an executable
# Kind of wacky but will work on linux
# Requires libcapstone-dev installed, and GSL/gtest headers fetched from cmake
# clang++ test/main.cpp src/trampoline.cpp src/trampoline-allocator.cpp -o build/test -std=c++20 -I/usr/include/ -Ishared -Ibuild/_deps/googletest-src/googletest/include -Ibuild/_deps/gsl-src/include -lcapstone -Iextern/includes/fmt/fmt/include -L/usr/lib/x86_64-linux-gnu -DFMT_HEADER_ONLY -Wall -Wextra -Werror -g

exit $ExitCode
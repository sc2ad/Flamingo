
function Clean-Build-Folder {
    if (Test-Path -Path "build")
    {
        remove-item build -R
        new-item -Path build -ItemType Directory
    } else {
        new-item -Path build -ItemType Directory
    }
}

$NDKPath = Get-Content $PSScriptRoot/ndkpath.txt

Clean-Build-Folder
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

exit $ExitCode
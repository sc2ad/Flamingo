Param(
    [Parameter(Mandatory=$false)]
    [Switch] $clean,

    [Parameter(Mandatory=$false)]
    [Switch] $help,

    [Parameter(Mandatory=$false)]
    [Switch] $test
)

if ($help -eq $true) {
    Write-Output "`"Build`" - Copiles your mod into a `".so`" or a `".a`" library"
    Write-Output "`n-- Arguments --`n"

    Write-Output "-clean `t`t Deletes the `"build`" folder, so that the entire library is rebuilt"
    Write-Output "-test `t`t Creates a test build"
    Write-Output "-help `t`t Prints this message"
    exit
}

# if user specified clean, remove all build files
if ($clean.IsPresent)
{
    if (Test-Path -Path "build")
    {
        remove-item build -R
    }
}


if (($clean.IsPresent) -or (-not (Test-Path -Path "build")))
{
    $out = new-item -Path build -ItemType Directory
} 

if ($test.IsPresent) {
    & cmake -G "Ninja" -DCMAKE_BUILD_TYPE="RelWithDebInfo" -DTEST_BUILD=1 -B build_desktop --preset desktop
} else {
    & cmake -G "Ninja" -DCMAKE_BUILD_TYPE="RelWithDebInfo" -B build_desktop --preset desktop
}
& cmake --build ./build_desktop

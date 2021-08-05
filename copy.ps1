$NDKPath = Get-Content $PSScriptRoot/ndkpath.txt

$buildScript = "$NDKPath/build/ndk-build"
if (-not ($PSVersionTable.PSEdition -eq "Core")) {
    $buildScript += ".cmd"
}

& cd extern/beatsaber-hook

& $buildScript NDK_PROJECT_PATH=$PWD APP_BUILD_SCRIPT=$PWD/Android.mk NDK_APPLICATION_MK=$PWD/Application.mk
& cd ../..

& $buildScript NDK_PROJECT_PATH=$PSScriptRoot APP_BUILD_SCRIPT=$PSScriptRoot/Android.mk NDK_APPLICATION_MK=$PSScriptRoot/Application.mk

& adb push libs/arm64-v8a/libquesthitscorevisualizer.so /sdcard/Android/data/com.beatgames.beatsaber/files/mods/libquesthitscorevisualizer.so
& adb push libs/arm64-v8a/libbeatsaber-hook_1_2_3.so /sdcard/Android/data/com.beatgames.beatsaber/files/libs/libbeatsaber-hook_1_2_3.so
& adb shell am force-stop com.beatgames.beatsaber

#!/bin/sh
# Builds libprojectM for one Android ABI with the flags the app uses. Run from inside the checkout,
# on the android branch, whose commits are the patches. Needs ANDROID_NDK_HOME, cmake and ninja.
set -e
abi=${1:?usage: build-android.sh <armeabi-v7a|arm64-v8a>}
: "${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME to the NDK}"
cmake -S . -B "build-$abi" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$abi" \
    -DANDROID_PLATFORM=android-30 \
    -DCMAKE_BUILD_TYPE=Release \
    -DANDROID_STL=c++_shared \
    -DENABLE_GLES=ON \
    -DBUILD_SHARED_LIBS=ON \
    -DBUILD_TESTING=OFF \
    -DBUILD_DOCS=OFF \
    -DENABLE_SDL_UI=OFF \
    -DENABLE_SYSTEM_PROJECTM_EVAL=OFF \
    -DENABLE_SYSTEM_GLM=OFF \
    -DENABLE_DEBUG_POSTFIX=OFF \
    -DENABLE_PLAYLIST=OFF
cmake --build "build-$abi"

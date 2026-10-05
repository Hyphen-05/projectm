# libprojectM, as built into the Android app

An Android music visualiser links libprojectM, which is licensed under the GNU Lesser
General Public License version 2.1 (`LICENSE.txt`). **This branch, `android`, is that library's source exactly as
the app uses it.** The app's own code is not part of libprojectM and is not here.

## What this branch is

- Upstream: https://github.com/projectM-visualizer/projectm
- Base: tag `v4.1.7`, commit `e0b0a967f0ffd7d332106c366668ed271718472b`
- On top of it, six commits, one per patch the app applies before every native build, in this order:
  `0001` to `0006`. Each commit's diff is the patch.
- Submodule: `vendor/projectm-eval` at `da885dcdf33620ef26aa04cac9e215378b80252e`, unchanged from upstream.

## Rebuilding it

```
git clone --branch android https://github.com/Hyphen-05/projectm.git projectm
cd projectm
git submodule update --init --recursive
ANDROID_NDK_HOME=/path/to/ndk/29.0.14206865 android-app/build-android.sh arm64-v8a
```

Run it once per ABI the app ships: `armeabi-v7a` and `arm64-v8a`. It needs `cmake` and `ninja`. The result is
`build-<abi>/src/libprojectM/libprojectM-4.so`.

## Replacing it in the app

The app loads libprojectM as a separate shared library (`lib/<abi>/libprojectM-4.so` in the APK), so a rebuilt or
modified one can be put in its place: replace the file, zipalign with `-P 16`, and re-sign the APK with your own key.
A modified library must keep the C API this branch exports, including the functions the patches add.

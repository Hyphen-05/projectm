# libprojectM, as built into Cometfly

Cometfly, an Android music visualiser, links libprojectM, which is licensed under the GNU Lesser
General Public License version 2.1 ([LICENSE.txt](LICENSE.txt)). **This branch, `android`, is that library's source exactly as
the app uses it.** The app's own code is not part of libprojectM and is not here.

## What this branch is

- Upstream: https://github.com/projectM-visualizer/projectm
- Base: tag `v4.1.8`, commit `4967dc003e5206d7df3ff447287f7609c47d2343`
- On top of it, the patches the app applies before every native build, `0002` to `0014`, in that order. Until
  2026-10-13 the base was `v4.1.7` with patches `0001` to `0006`, one commit each; the move to `v4.1.8` is a merge
  of the upstream tag carrying `0002` to `0006`, with `0001` dropped because upstream now does the same thing, and
  `0007` follows as its own commit. Later fixes to a patch follow as commits of their own. The branch's latest tree is
  always exactly what the app builds.
- Submodule: `vendor/projectm-eval` at `e8c311e86f4d8c082e96e02b6a33525034eee6d4`, unchanged from upstream.

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

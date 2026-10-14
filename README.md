# libprojectM for Cometfly

This is a fork of [projectM](https://github.com/projectM-visualizer/projectm). Its `android` branch is libprojectM
exactly as the Android music visualiser **Cometfly** ships it: upstream's library with a few patches on top.

Cometfly links libprojectM as a separate shared library, which is licensed under the GNU Lesser General Public
License version 2.1 (`LICENSE.txt`). This branch is published so anyone can read, rebuild or replace the library the
app uses. Cometfly's own code is not part of libprojectM and is not here.

- **What the patches are, how to rebuild the library and how to swap it into the app:**
  [ANDROID-APP.md](ANDROID-APP.md).
- **projectM itself**, its documentation, issues and community: [projectM-visualizer/projectm](https://github.com/projectM-visualizer/projectm).
  Cometfly is not made or supported by the projectM developers, so please don't take questions about the app there.

The projectM authors are listed in `AUTHORS.txt`.

# Native dependency inventory

Generated for Resonance 0.1.0. Required by FRD section 15 ("the native
dependency review must include redistribution obligations") and DIST-002 ("no
application, helper or installer component depends on .NET or C++/CLI; end users
need no development SDK, Python, Node or separately downloaded helper").

## Verified on this host

Ubuntu resolute, x86_64, GCC 15.2.0, CMake 4.2.3, Ninja 1.13.2.

| Dependency | Version | Licence | Linkage | Used by | Obligation |
|---|---|---|---|---|---|
| TagLib | 2.2.1 | LGPL-2.1 / MPL-1.1 | dynamic | Infrastructure | Relinking must remain possible. Dynamic linking satisfies this. |
| SQLite | 3.46.1 | public domain | dynamic | Infrastructure | None. |
| libjpeg-turbo | 2.1.5 | BSD-3-Clause / IJG | dynamic | Infrastructure | Attribution. |
| libpng | 1.6.57 | PNG Reference Library | dynamic | Infrastructure | Attribution. |
| zlib | 1.3.1 | zlib | dynamic | transitive | Attribution. |
| libcurl | 8.18.0 | curl (MIT-like) | dynamic | Infrastructure | Attribution. |
| minimp3 | pinned `ea99364f61c14656440e8d77e9c233ccf3124633` | CC0-1.0 | **vendored, compiled in** | Infrastructure, Desktop | None. Public-domain equivalent. |
| Qt 6 Core / Gui / Widgets | 6.10.2 | LGPL-3.0 | dynamic | Desktop only | Relinking must remain possible. Dynamic linking satisfies this. |
| Qt 6 Multimedia | 6.10.2 | LGPL-3.0 | dynamic | Desktop only | See the note below. |

The CLI links **none** of the Qt modules and runs without a display server.

## No managed runtime

Nothing in the application, the helpers or the installer depends on .NET,
C++/CLI, Python, Node or a separately downloaded helper. There are no helper
executables at all: the image pipeline, the MP3 decoder and the tempo engine are
compiled into the application. `ldd` on the built binaries lists only the
libraries in the table above plus the C and C++ runtimes.

## Note: Qt Multimedia reintroduces FFmpeg

ADR 0001 chose minimp3 over FFmpeg partly to avoid FFmpeg's size and its
GPL/LGPL obligation. Adding audio playback through Qt Multimedia reintroduces
FFmpeg as a **transitive runtime dependency** of Qt's multimedia backend:

```
qt.multimedia.ffmpeg: Using Qt multimedia with FFmpeg version 8.0.1 GPL version 2 or later
```

This is worth stating plainly rather than leaving buried:

- Resonance does not bundle FFmpeg and does not call it. Decoding for both
  playback and analysis is done by the vendored minimp3.
- FFmpeg arrives as a dependency of the `qt6-multimedia` runtime package, which
  the Debian package declares through `CPACK_DEBIAN_PACKAGE_SHLIBDEPS` rather
  than shipping.
- The Ubuntu build of FFmpeg identifies as **GPL version 2 or later**. A GPL
  FFmpeg in the dependency chain is a distribution question, not merely a
  technical one. Resonance itself is GPL-3.0-or-later, so this is consistent for
  source distribution, but it must be checked before any binary redistribution,
  and a build of Qt Multimedia against an LGPL FFmpeg would be preferable.
- The consequence is recorded in `docs/open-questions.md` as an item requiring a
  decision before release, not as something already resolved.

If playback is later moved to a direct platform audio API (WASAPI, CoreAudio,
ALSA or PipeWire), the Qt Multimedia dependency and therefore this FFmpeg
dependency disappear.

## Reproducing this inventory

```bash
cmake --preset linux-release
cmake --build --preset linux-release
ldd build/linux-release/src/MusicLibrary.Cli/resonance
ldd build/linux-release/src/MusicLibrary.Desktop/resonance-desktop
```

## Pinning

- minimp3 is vendored at a fixed revision in `third_party/minimp3`.
- System libraries are resolved by the distribution. The Debian package declares
  the versions actually linked against; the Windows and macOS packages must
  stage their dependencies with `windeployqt` and `macdeployqt` plus an explicit
  collection step, which has not yet been executed on those platforms.

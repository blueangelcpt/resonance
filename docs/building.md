# Building Resonance on Windows and macOS

Two routes. The first needs no Windows or Mac machine at all.

**Nothing in this document has been executed.** Only the Linux x64 build has
actually been produced and run. The recipes below are written and the
dependency manifest is pinned, but the first person to run them should expect to
fix something. That is what `docs/open-questions.md` item 1 records.

## Route 1 — GitHub Actions (recommended)

The repository already has workflows that build all four artifacts on
GitHub-hosted runners: Windows x64, macOS ARM64, macOS x64 and Linux x64.

### Build without releasing

Go to **Actions → CI → Run workflow**, or:

```bash
gh workflow run ci.yml
gh run watch
```

Each platform job builds, runs the test suite, and uploads what it produced.
Download them from the run's Artifacts section.

### Build a release

Tag a version. The release workflow builds every target, runs its tests,
packages it, and opens a **draft** release with SHA-256 checksums and the
dependency notice attached:

```bash
git tag v0.1.0
git push origin v0.1.0
gh run watch
```

The release is created as a draft and is never published automatically, so
nothing becomes public until you choose to publish it.

Artifacts produced:

| Platform | Artifact |
|---|---|
| Windows x64 | `resonance-0.1.0-win64.exe` (NSIS installer) |
| macOS ARM64 | `resonance-0.1.0-macos-arm64.dmg` |
| macOS x64 | `resonance-0.1.0-macos-x86_64.dmg` |
| Linux x64 | `resonance_0.1.0_amd64.deb` |

Both releases will be **unsigned**. Windows SmartScreen and macOS Gatekeeper
will warn. Signing needs an Authenticode certificate and an Apple Developer ID
added as repository secrets; the release notes state the signing status plainly
rather than implying it.

## Route 2 — building natively

Every target uses the same CMake presets, so the commands differ only in which
preset you name.

```bash
cmake --list-presets
```

### Windows

Needs Visual Studio 2022 (or the Build Tools), CMake, Ninja and Git.

```powershell
# Qt 6.10 with the Multimedia module, from the online installer or aqtinstall
$env:CMAKE_PREFIX_PATH = "C:\Qt\6.10.2\msvc2022_64"

# vcpkg supplies TagLib, SQLite, libjpeg-turbo, libpng, zlib and curl.
git clone https://github.com/microsoft/vcpkg
.\vcpkg\bootstrap-vcpkg.bat
$env:VCPKG_ROOT = "$PWD\vcpkg"

cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
cmake --build build/windows-release --config Release --target package
```

`vcpkg.json` pins the dependency set and its baseline, so vcpkg installs exactly
the versions this project was built against. You do not install those packages
by hand.

The packaging step produces an NSIS installer. It bundles the Qt runtime via
`windeployqt`; verify on a clean machine, because a deployment tool staging
files is not the same as a working installation.

### macOS

Needs Xcode command line tools and Homebrew.

```bash
brew install ninja cmake taglib sqlite jpeg-turbo libpng curl qt@6
export CMAKE_PREFIX_PATH="$(brew --prefix qt@6):$(brew --prefix taglib)"

# Apple Silicon
cmake --preset macos-arm64
cmake --build --preset macos-arm64
ctest --preset macos-arm64
cmake --build build/macos-arm64 --target package

# Intel
cmake --preset macos-x64
cmake --build --preset macos-x64
ctest --preset macos-x64
cmake --build build/macos-x64 --target package
```

Separate Intel and Apple Silicon disk images are the target. A universal binary
would need universal copies of every bundled dependency, which Homebrew does not
provide; that is additional scope, not a flag to flip.

Distributing outside your own machine needs signing and notarisation:

```bash
codesign --deep --force --options runtime \
  --sign "Developer ID Application: YOUR NAME (TEAMID)" \
  build/macos-arm64/resonance-desktop.app
xcrun notarytool submit build/macos-arm64/*.dmg \
  --apple-id you@example.com --team-id TEAMID --wait
xcrun stapler staple build/macos-arm64/*.dmg
```

### Linux

```bash
sudo apt install -y build-essential ninja-build cmake pkg-config \
  qt6-base-dev qt6-base-dev-tools qt6-multimedia-dev qt6-svg-dev \
  libgl1-mesa-dev libtag1-dev libsqlite3-dev libjpeg-dev libpng-dev \
  zlib1g-dev libcurl4-openssl-dev

cmake --preset linux-release
cmake --build --preset linux-release
ctest --preset linux-release
cmake --build build/linux-release --target package   # .deb
```

Build release artifacts on the **oldest** distribution you intend to support.
Building on a newer one silently raises the glibc requirement and the package
will refuse to install on older systems.

## How dependencies are found on each platform

One piece of portability is worth knowing about, because it is where a
cross-platform CMake build usually breaks.

TagLib is located in two different ways:

1. `find_package(taglib CONFIG)` — what vcpkg and Homebrew install, and the only
   route available on Windows, where pkg-config generally is not present.
2. `pkg_check_modules(TAGLIB ... taglib)` — the fallback, for distributions that
   ship only a `.pc` file.

Whichever succeeds is exposed as one target, `Resonance::TagLib`, so nothing
downstream depends on which route was taken. The configure output says which was
used:

```
--   TagLib .............. 2.2.1 (via CMake config package)
```

Everything else (SQLite, zlib, libjpeg, libpng, curl, Qt) has a CMake config or
find module on all three platforms. minimp3 is vendored, so it is never
searched for.

## Other presets

| Preset | Purpose |
|---|---|
| `linux-strict` | Warnings are errors. What CI runs; run it before pushing. |
| `linux-sanitisers` | ASan and UBSan, CLI only. Catches memory and UB faults in the tag parser's fuzz pass. |
| `linux-debug` | Ordinary debug build. |

```bash
cmake --preset linux-strict && cmake --build --preset linux-strict && ctest --preset linux-strict
```

## What is verified, and what is not

| | |
|---|---|
| Linux x64 | Built, tested, packaged and **run**. |
| Windows x64 | Recipe written. **Never built.** |
| macOS ARM64 | Recipe written. **Never built.** |
| macOS x64 | Recipe written. **Never built.** |
| Installer lifecycle (install, upgrade, uninstall) | **Not tested on any platform.** |
| Code signing | **Not configured on any platform.** |

Per FRD section 16, an untested architecture must not be described as qualified.
These are recipes, not evidence.

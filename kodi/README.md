# mpvlib-kodi

Kodi's Android engine packaged as an Android library (AAR), for apps that embed Kodi next to mpv.
mpvRx's `kodi` variant uses it.

Kodi has no embeddable player API: `libkodi.so` is the whole application. It runs its own event loop
in a `NativeActivity`, registers its JNI natives on Kodi's own Java classes, and calls `exit(0)` when
the user quits. The AAR therefore keeps Kodi intact and runs it in a separate **`:kodi` process** of
the host application, with Kodi's own interface.

## What the AAR contains

| Part | Source |
|---|---|
| `jni/<abi>/*.so` | `libkodi.so`, binary add-ons and Python modules from the official Kodi APKs (`arm64-v8a`, `armeabi-v7a`, `x86`; Kodi ships no `x86_64`). `libc++_shared.so` is left out: the host app provides a newer, backward-compatible one. |
| `org.xbmc.kodi.*` classes | Kodi's Java glue, generated from `tools/android/packaging/xbmc/src/*.java.in` with Kodi's package name, which the native code expects. |
| `res/` | Kodi's Android resources, renamed with a `kodi_` prefix so they don't clash with the host's. |
| `assets/kodi/kodi-assets.zip` | The official APK's `assets/` tree (skins, add-ons, Python) as one archive, plus `LICENSE.md` and `SOURCES.txt` (provenance and checksums). |
| `AndroidManifest.xml` | `Splash`, `Main`, a receiver, providers and job services, all in `android:process=":kodi"`, not exported, in task affinity `${applicationId}.kodi`. Provider authorities are `${applicationId}.kodi.{media,file,ytdl}`. |

`kodi.properties` pins the Kodi tag and the official APK URLs with their SHA-256 checksums.
`scripts/prepare_kodi.py` downloads and verifies them, sparse-clones Kodi's packaging sources at the
tag, and generates `build/generated/kodi/`. The `prepareKodi` Gradle task runs it before every build.
Downloads are cached in `~/.cache/mpvlib-kodi` (`MPVLIB_KODI_CACHE` overrides this).

### Changes to Kodi's Java glue

`prepare_kodi.py` (`JAVA_PATCHES`) applies these changes, and fails if any of them no longer applies:

- Provider authorities come from the host package (`KodiHost.authorityRoot()`).
- `Splash` extracts `kodi/kodi-assets.zip` instead of every `assets/` entry of the host APK, and never
  deletes the APK on read errors. Kodi's home directory is still `<cacheDir>/apk/assets`.
- `Splash` checks for an already running Kodi by the host's component name, and requests
  `READ_EXTERNAL_STORAGE` instead of `WRITE_EXTERNAL_STORAGE` on Android TV API 30-32.
- Autostart and global search target the embedded components, and the optional
  `/sdcard/xbmc_env.properties` override is read from `/sdcard/<host package>.xbmc_env.properties`.

Native code and assets are unchanged. Binary add-on `<platform>` tags are set to `android` so one
asset archive serves every ABI.

## Build

```sh
./gradlew :kodi:assembleRelease   # kodi/build/outputs/aar/kodi-release.aar
```

Requires Python 3 and Git. Optional Gradle properties:

- `-PkodiAbis=x86,arm64-v8a`: package a subset of ABIs.
- `-PkodiApk.<abi>=/path/to/kodi.apk`: package a Kodi APK you built from source for that ABI.
- `-PkodiSourceDir=/path/to/xbmc`: generate the Java glue from an existing Kodi checkout.

The `kodi` workflow builds the AAR on the `kodi` branch. Pushing a `kodi-v<aar.version>` tag publishes
`mpvlib-kodi-<aar.version>.aar` as a prerelease (these tags don't start the `v*.*.*` release workflow).

## Using it in an app

1. Depend on the AAR and on `androidx.tvprovider:tvprovider` and `com.google.code.gson:gson`. AARs
   downloaded from releases have no dependency metadata.
2. Keep native libraries extracted (`packaging.jniLibs.useLegacyPackaging = true`). Kodi loads add-on
   libraries from the native library directory.
3. Skip the app's own initialization in the `:kodi` process (`Application.getProcessName()` ends with
   `:kodi`).
4. Provide `<applicationId>.XBMCInputDeviceListener` and `<applicationId>.XBMCBroadcastReceiver`, each
   an empty subclass of the `org.xbmc.kodi` class of the same name, in the package equal to the
   **application ID** of every variant. Kodi's native code creates these two classes from the runtime
   package name; without them the `:kodi` process aborts at startup. mpvRx generates them with a
   Gradle task (`GenerateKodiHostClasses` in `app/build.gradle.kts`); the AAR's keep rules cover them.
5. Start Kodi with an explicit intent to `org.xbmc.kodi.Splash`. To play something, use `ACTION_VIEW`
   with a `file://` URI, a MediaStore `content://` URI or a network URL; Kodi accepts request headers
   as `url|Header=value`.
6. The app needs `MANAGE_EXTERNAL_STORAGE`. On first start, Kodi's splash screen asks for storage
   access and extracts about 100 MB of assets into the app's cache.

## License

Kodi is licensed GPL-2.0-or-later, with some files under other licenses; see `assets/kodi/LICENSE.md`
in the AAR and <https://github.com/xbmc/xbmc/tree/21.3-Omega/LICENSES>. Its source is at
<https://github.com/xbmc/xbmc>. Kodi® is a registered trademark of the XBMC Foundation, which does not
endorse this package. Review both before you distribute an app that contains it.

# mpvlib-kodi

Kodi's playback engine as an Android library (AAR), with the API of mpv-android's `MPVLib`. mpvRx's
`kodi` distribution plays with it instead of libmpv, behind mpvRx's own player screen.

Kodi shows no interface of its own. It renders only video and subtitles, into Surfaces of the host
app, and the host controls it with mpv's commands, properties and events through
`org.xbmc.kodi.engine.KodiLib`.

## How it works

Kodi has no embeddable player API: `libkodi.so` is the whole application, a `NativeActivity` that
keeps global state and calls `exit(0)` when it quits. The AAR runs it headless:

- **Own process.** `KodiEngineService` runs in the host's `:kodi` process, and `KodiLib` (in the
  host's process) binds to it over AIDL (`IKodiEngine`). `KodiLib.destroy()` ends that process,
  because Kodi cannot start twice in one process; the next `create()` starts a new one.
- **No Activity.** `libkodiengine.so` (`src/main/cpp/kodi_engine.c`) builds the `ANativeActivity`
  that Kodi's `android_main()` expects, with the headless `org.xbmc.kodi.Main` as its activity.
  `Main`, `XBMCMainView`, `XBMCVideoView` and `XBMCMediaSession` replace Kodi's classes and keep
  their native methods, which `libkodi.so` registers.
- **No GUI.** Kodi starts with `skin.mpvlib.headless` (`headless/`), a skin that draws nothing but
  video and subtitles. `EngineSettings` writes the settings a headless engine needs on every start:
  that skin, no screensaver or pause dimmer, no refresh-rate switching, no network services. Neither
  an older `guisettings.xml` nor a runtime change can bring Kodi's GUI back.
- **Host Surfaces.** Kodi renders into Surfaces of the host app, passed over Binder: the GUI layer
  (`LAYER_GUI`) and, optionally, the video layer (`LAYER_VIDEO`, see below). Until the host
  attaches one, Kodi renders off-screen. Kodi positions the video inside the layer, and the engine
  reports that rectangle as `android-video-rect` and to `VideoRectObserver`s.
- **mpv dialect.** `PlayerBridge` translates mpv's commands, properties and events to Kodi's
  JSON-RPC API, which runs in-process (`EngineBridge`), and polls Kodi's player state. `fd://N`
  arguments are served to Kodi over loopback HTTP with range requests (`FdStreamServer`).
- **Assets.** On the first start, and after every app update, `AssetInstaller` extracts
  `assets/kodi/kodi-assets.zip` (about 60 MB: Kodi's add-ons, Python and the headless skin) into
  `<cacheDir>/apk/assets`, Kodi's home directory.

## API

`KodiLib` has `MPVLib`'s methods:

| Area | Methods |
|---|---|
| Lifecycle | `create(context)`, `init()`, `destroy()`, `isCreated()` |
| Surfaces | `attachSurface(surface)` (GUI layer), `attachSurface(layer, surface, width, height)`, `detachSurface([layer])`, `setSurfaceSize([layer,] width, height)` |
| Commands | `command(String...)`: `loadfile`, `stop`, `seek`, `set`, `cycle`, `add`, `multiply speed`, `sub-add`, `frame-step`, `playlist-next`, `quit` |
| Properties | `setOptionString`, `setPropertyString/Boolean/Int/Double`, `getPropertyString/Boolean/Int/Long/Double/Node`, `observeProperty(name, format)` |
| Observers | `addObserver` (`EventObserver`: property changes and events), `addLogObserver`, `addVideoRectObserver` |
| Kodi | `jsonRpc(request)`: any Kodi JSON-RPC request; returns Kodi's response |

Events: `EVENT_START_FILE`, `EVENT_FILE_LOADED`, `EVENT_PLAYBACK_RESTART`, `EVENT_SEEK`,
`EVENT_END_FILE` (with mpv's `reason`), `EVENT_IDLE`, `EVENT_VIDEO_RECONFIG`, `EVENT_AUDIO_RECONFIG`
and `EVENT_SHUTDOWN`.
Property reads return the latest values the engine reported and never block.

mpv properties and options the engine maps to Kodi:

- **Playback:** `pause`, `time-pos`/`playback-time`, `percent-pos`, `duration`, `seeking`,
  `paused-for-cache`, `eof-reached`, `idle-active`, `speed` (Kodi's tempo), `chapter`,
  `chapter-list`, `ab-loop-a`/`ab-loop-b`, `media-title`, `force-media-title`, and `keep-open`:
  Kodi stops at the end of a file, so the engine holds the last position and a seek re-opens the
  file.
- **Tracks:** `track-list` (also paths such as `track-list/0/lang`), `aid`, `sid`, `vid`,
  `sub-visibility`, `sub-delay`, `audio-delay`, `sub-add`.
- **Audio:** `volume` (mpv's cubic scale on Kodi's 60 dB range; above 100 through Kodi's volume
  amplification, up to `volume-max`), `mute`, `audio-spdif` (passthrough), `audio-channels`.
- **Video:** `hwdec` (`no` decodes with FFmpeg, anything else with MediaCodec; a playing file
  re-opens at its position), `video-zoom`, `video-aspect-override` and `panscan` (Kodi's view
  modes), `brightness`, `contrast`, `video-params` (with HDR transfer and primaries) and
  `vo-configured`.
- **Subtitle style:** `sub-font-size`, `sub-scale`, `sub-bold`, `sub-italic`, `sub-color`,
  `sub-border-color`, `sub-border-size`, `sub-back-color`, `sub-shadow-offset`, `sub-shadow-color`,
  `sub-border-style`, `sub-pos` and `sub-ass-override` set Kodi's global subtitle settings
  (`SettingsMapper`).
- **Network:** `user-agent`, `referrer` and `http-header-fields` become Kodi's `url|Header=value`
  options.
- **Kodi extensions:** `kodi-setting/<id>` (`setOptionString`, before or after the engine starts)
  sets any Kodi setting, for example `kodi-setting/audiooutput.passthrough`. `kodi-video/<field>`
  sets a field of `Player.SetVideoSettings` for the current video, for example
  `kodi-video/tonemapmethod`. `kodi-engine-state` is `starting`, `ready` or `failed`.

mpv features Kodi doesn't have (filters, shaders, scripts, OSD, key bindings) are ignored.

## Video output and HDR

- **GL output** (only `LAYER_GUI` attached): Kodi draws the video with OpenGL ES, from FFmpeg or
  MediaCodec frames. HDR video is tone mapped to SDR (`kodi-video/tonemapmethod`: Reinhard, ACES or
  Hable). Brightness and contrast work here only.
- **Direct output** (`LAYER_VIDEO` attached as well): MediaCodec decodes straight to the host's
  video Surface, so HDR10, HDR10+, HLG and Dolby Vision reach an HDR display unchanged. The
  `LAYER_GUI` Surface then only carries subtitles and must be translucent and above the video (mpvRx
  uses a `SurfaceView` with `setZOrderMediaOverlay(true)`). Kodi's settings
  `videoplayer.convertdovi` (Dolby Vision profile 7 conversion) and
  `videoplayer.allowedhdrformats` (Dolby Vision and HDR10+ metadata) apply here.
- **Vulkan:** Kodi 21 renders with OpenGL ES only on Android; it has no Vulkan renderer.

## Changes to Kodi

### Native code (`patches/`)

Applied by `scripts/build-kodi-source.sh`. They only change Kodi's behaviour when the engine sets
`KODI_ENGINE_MODE`, except 0004, whose JSON-RPC additions are also available in Kodi itself.

| Patch | Change |
|---|---|
| `0001-engine-gui-follows-surface-size` | The GUI resolution follows the size of the host's Surface (rotation, split screen, picture in picture). Adds `kodi_engine_capabilities()`, which the engine checks. |
| `0002-engine-host-owns-audio-focus` | Kodi doesn't request or abandon the audio focus; the host owns it and pauses the engine. |
| `0003-engine-display-capabilities` | Kodi's window is never attached, so it falls back to the default display for HDR capabilities, display modes and the refresh rate. Display mode and refresh-rate changes are left to the host's window. |
| `0004-engine-jsonrpc-player-extensions` | `Player.GetVideoSettings`, `Player.SetVideoSettings` (picture, view mode, deinterlacing, scaling, tone mapping, volume amplification) and `Player.GetChapters`. `Player.Open` checks that a local file exists without Kodi's directory cache. |
| `0005-engine-paused-video-redraw` | A paused video drawn in the GUI layer is redrawn twice a second, and at full rate for half a second after a seek or frame step, instead of on every frame. |

### Java glue

`scripts/prepare_kodi.py` generates Kodi's Java glue from `tools/android/packaging/xbmc/src/*.java.in`
with Kodi's package name, which the native code expects, and applies `JAVA_PATCHES`. It fails if any
of them no longer applies:

- Provider authorities come from the host package (`KodiHost.authorityRoot()`).
- The optional `/sdcard/xbmc_env.properties` override is read from
  `/sdcard/<host package>.xbmc_env.properties`, so it isn't shared with an installed Kodi.
- The remaining patches adapt Kodi's activity glue (`Splash`, autostart, global search), which the
  engine doesn't start; they keep those classes consistent with the embedded package.

## What the AAR contains

| Part | Source |
|---|---|
| `jni/<abi>/*.so` | `libkodi.so`, binary add-ons and Python modules from a Kodi APK (`arm64-v8a`, `armeabi-v7a`, `x86`; Kodi ships no `x86_64`), and the engine shim `libkodiengine.so`. `libc++_shared.so` is left out: the host app provides a newer, backward-compatible one. |
| `org.xbmc.kodi.*` | Kodi's generated Java glue, the headless replacements and the engine (`org.xbmc.kodi.engine`). |
| `res/` | Kodi's Android resources, renamed with a `kodi_` prefix so they don't clash with the host's. |
| `assets/kodi/` | `kodi-assets.zip` (the APK's `assets/` tree plus the headless skin), `LICENSE.md` and `SOURCES.txt` (Kodi version, APK checksums, patches). |
| `AndroidManifest.xml` | `KodiEngineService` in `android:process=":kodi"`, not exported. |

## Build

```sh
./gradlew :kodi:assembleRelease   # kodi/build/outputs/aar/kodi-release.aar
```

Requires Python 3, Git, CMake 3.22.1 and NDK 27.3.13750724. `kodi.properties` pins the Kodi tag and
the official Kodi APKs with their SHA-256 checksums. `prepareKodi` (`scripts/prepare_kodi.py`)
downloads and verifies them, sparse-clones Kodi's packaging sources at the tag, and generates
`build/generated/kodi/`. Downloads are cached in `~/.cache/mpvlib-kodi` (`MPVLIB_KODI_CACHE`
overrides this).

Official APKs lack the patches above, so they are only good for quick local builds: Kodi then keeps
its first GUI resolution, takes the audio focus, sees no HDR display and has no chapter titles or
`kodi-video/*`. Published AARs package Kodi built from source:

```sh
JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64 ANDROID_HOME=$HOME/Android/Sdk \
  kodi/scripts/build-kodi-source.sh arm64-v8a ~/kodi-android-build
./gradlew :kodi:assembleRelease -PkodiAbis=arm64-v8a \
  -PkodiApk.arm64-v8a=$HOME/kodi-android-build/arm64-v8a/xbmc/kodiapp-arm64-v8a-release.apk
```

`build-kodi-source.sh` follows Kodi's `docs/README.Android.md` for the pinned tag: NDK r21e, release
depends, the `peripheral.joystick` add-on, Kodi with `patches/*.patch`, then `make apk`. Each ABI
gets its own checkout under the work directory. Completed steps are skipped when the script runs
again, so it resumes after a failure. The depends tarballs come from several Kodi mirrors in
parallel (`prefetch_kodi_tarballs.py`). One ABI takes about an hour on 6 cores and about 10 GB of
disk.

Optional Gradle properties:

- `-PkodiAbis=x86,arm64-v8a`: package a subset of ABIs.
- `-PkodiApk.<abi>=/path/to/kodi.apk`: package a Kodi APK built from source for that ABI.
- `-PkodiSourceDir=/path/to/xbmc`: generate the Java glue from an existing Kodi checkout.

### CI and releases

`.github/workflows/kodi.yml` builds Kodi from source with the patches for every ABI (cached by Kodi
tag, patches and build scripts), packages the AAR and checks that `libkodi.so` contains the patches.
Pushing a `kodi-v<aar.version>` tag, or running the workflow manually with `publish`, publishes
`mpvlib-kodi-<aar.version>.aar`, its `.sha256` and `SOURCES.txt` as the prerelease
`kodi-v<aar.version>` (these tags don't start the `v*.*.*` release workflow). Bump `aar.version` in
`kodi.properties` for every published change.

## Using it in an app

1. Depend on the AAR and on `androidx.tvprovider:tvprovider` and `com.google.code.gson:gson`. AARs
   downloaded from releases have no dependency metadata.
2. Keep native libraries extracted (`packaging.jniLibs.useLegacyPackaging = true`). Kodi loads add-on
   libraries from the native library directory.
3. Skip the app's own initialization in the `:kodi` process (`Application.getProcessName()` ends with
   `:kodi`).
4. Call `KodiLib.create(context)`, attach a Surface and play with `command("loadfile", url)`. Paths,
   network URLs, `content://` URIs and `fd://N` work. The engine process has the app's permissions:
   to play files by path, the app needs storage access.

## License

Kodi is licensed GPL-2.0-or-later, with some files under other licenses; see `assets/kodi/LICENSE.md`
in the AAR and <https://github.com/xbmc/xbmc/tree/21.3-Omega/LICENSES>. Its source is at
<https://github.com/xbmc/xbmc>; the changes are in `patches/` and `scripts/prepare_kodi.py`. Kodi® is a
registered trademark of the XBMC Foundation, which does not endorse this package. Review both before
you distribute an app that contains it.

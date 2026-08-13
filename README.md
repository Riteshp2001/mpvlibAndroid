# mpvlib Android

**MpvRx / mpvlibAndroid** — An Android video player library built on mpv.

## What is This?

This library brings the full power of mpv to Android — play any video, generate thumbnails, and more. All through a simple Kotlin API.

## Features

### 🎥 Core Video Playback
- Full mpv engine on Android — play virtually any media file
- 200+ formats supported (mp4, mkv, avi, mov, webm, flac, mp3, gif, etc.)
- 15+ network protocols: http, https, rtmp, rtmps, rtp, rtsp, mms, tcp, udp, and more
- Android Surface rendering with hardware acceleration

### 🖼️ Thumbnail Generation
- Two engines: mpv-based and direct FFmpeg
- Hardware-accelerated with MediaCodec
- Sync, async, and batch modes
- ~50-100ms per thumbnail

### 📜 Scripting
- **Lua** 5.2.4 — mpv scripts work as-is
- **JavaScript** via MuJS 1.3.9

### 🔒 Security
- SSL/TLS via MbedTLS
- CA certificate bundle included
- Secure streaming by default

### 🎨 Video Output
- OpenGL ES rendering via libplacebo
- GPU shader cache support
- Subtitle rendering with libass + HarfBuzz + FriBidi
- AV1 decoding via dav1d

### ⚡ Modern Android API
- Kotlin-friendly with StateFlow / Flow support
- Observe any mpv property reactively
- Typed property delegates (Int, Long, Float, Double, Boolean, String, Node)

## Requirements

- Android 7.0+ (API 24+)
- Android Studio
- JDK 17+

## Quick Start

### Installation

1. Download the AAR from [releases](https://github.com/Riteshp2001/mpvlibAndroid/releases)
2. Place it in `app/libs/`
3. Add to your `build.gradle`:

```gradle
dependencies {
    implementation(files("libs/mpvlib.aar"))
}
```

### Basic Usage

```kotlin
import is.xyz.mpv.MPVLib

MPVLib.create(context)
MPVLib.init()

// Play anything — local file, stream
MPVLib.command("loadfile", "/sdcard/video.mp4")

// Controls
MPVLib.setPropertyBoolean("pause", true)
MPVLib.setPropertyBoolean("pause", false)

// Thumbnail
FastThumbnails.initialize(context)
FastThumbnails.generate("video.mp4", positionSec = 30.0, width = 320)
```

## Building from Source

### Prerequisites

```bash
sudo apt install openjdk-17-jdk ninja-build cmake autoconf automake \
    libtool-bin pkg-config meson wget
```

### Commands

```bash
./buildscripts/download.sh    # Download SDK, NDK, and dependencies
./buildscripts/buildall.sh    # Build everything (all 4 ABIs)
./buildscripts/docker-build.sh # Or build with Docker
```

Output: `app/build/outputs/aar/app-release.aar`

## Supported ABIs

- `arm64-v8a` (64-bit ARM)
- `armeabi-v7a` (32-bit ARM)
- `x86_64` (64-bit x86)
- `x86` (32-bit x86)

## Key Classes

| Class | Purpose |
|-------|---------|
| `MPVLib` | Main API — init, play, pause, seek, properties, events |
| `BaseMPVView` | Drop-in video surface for XML layouts |
| `FastThumbnails` | Generate thumbnails sync/async/batch |
| `Utils` | File helpers, metadata, storage, version info |
| `MPVNode` | Handle complex mpv data types |

All native dependency versions packaged into the AAR are available through
`Utils.VERSIONS`. Use the individual fields (for example,
`Utils.VERSIONS.mpv`, `Utils.VERSIONS.mpvClientApi`,
`Utils.VERSIONS.ffmpeg`, and `Utils.VERSIONS.libAvcodec`) or iterate over the
stable display map. Player/release versions are reported separately from
their library API/ABI versions:

```kotlin
Utils.VERSIONS.dependencies.forEach { (name, version) ->
    println("$name: $version")
}
```

## What's Inside

| Component | Version |
|-----------|---------|
| mpv | latest |
| FFmpeg | n8.1.2 |
| libplacebo | latest |
| shaderc | disabled |
| libass | latest |
| dav1d | latest |
| Lua | 5.2.4 |
| MuJS (JavaScript) | 1.3.9 |
| MbedTLS | 3.6.6 |
| HarfBuzz | 14.2.1 |
| FreeType | 2.14.3 |
| FriBidi | 1.0.16 |
| libunibreak | 7.0 |
| Android NDK | r29 |
| Min API | 24 (Android 7.0) |

## License

**MIT License** — See [LICENSE](LICENSE).

## Credits

- [mpv](https://mpv.io/)
- Original authors: Ilya Zhuravlev and sfan5

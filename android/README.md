# Android (ARM64)

The APK embeds the same `frontend/static` assets and `frontend/fates_web.py` server
used by the desktop WebUI. The Gradle build compiles `src/fates.cpp` for Android
ARM64 and packages it as `lib/arm64-v8a/libfates.so`. It is a PIE **executable**,
named `.so` only so Android extracts it to the executable native-library directory.
The Python server executes it as a subprocess. Searches do not leave the phone.

Requirements: Android SDK platform 35, build-tools 35, NDK 27.0.12077973,
JDK 21, Python 3.12 on the build machine, Gradle 8.11.1, and the Android SDK
licenses accepted by the developer. Gradle downloads Android Gradle Plugin
8.9.2 and Chaquopy 17.0.0 on the first build. The APK targets Android 7.0+
on 64-bit ARM devices; x86 emulators and 32-bit devices are not supported.

On Windows, set `ANDROID_HOME` to the installed SDK directory and use JDK 21
for `JAVA_HOME`. From the repository root, with `gradle` available on `PATH`:

```powershell
gradle -p android :app:assembleDebug
```

The installable debug build appears at
`android/app/build/outputs/apk/debug/app-debug.apk`. Install it with
`adb install -r android/app/build/outputs/apk/debug/app-debug.apk`. A debug APK
is signed by the local Android debug key, not suitable for store distribution.
GitHub Actions uses a fresh debug key for each build, so an APK downloaded
from a different run cannot update an existing installation: uninstall the
old app first (which removes its local app data). The GitHub release asset is
named `fates-android-arm64-debug.apk` to make this limitation explicit.
Build release artifacts only after configuring an owned signing key; do not
commit signing keys to this repository.

The app binds only to 127.0.0.1, chooses a new ephemeral port at launch and
stops active searches when closed. WebView blocks navigation off the local
server. The frontend remains responsive down to narrow phone viewports. This
project needs an on-device launch/search/cancel test before calling a release
APK verified; desktop tests do not establish Android execution behavior.

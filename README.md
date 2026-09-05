# Bully: Anniversary Edition Vita

<p align="center"><img src="./screenshots/game.png"></p>

This is a wrapper/port of *Bully: Anniversary Edition* for the *PS Vita*.

The port works by loading the official Android ARMv7 executable in memory, resolving its imports with native functions and patching it in order to properly run.
By doing so, it's basically as if we emulate a minimalist Android environment in which we run natively the executable as is.

## Setup Instructions (For End Users)

(If you have already installed the game and want to update to a newer release, you can simply install [Bully.vpk](https://github.com/TheOfficialFloW/bully_vita/releases/download/v1.0/Bully.vpk) on your *PS Vita*).

In order to properly install the game, you'll have to follow these steps precisely:

- Install [kubridge](https://github.com/TheOfficialFloW/kubridge/releases/) and [FdFix](https://github.com/TheOfficialFloW/FdFix/releases/) by copying `kubridge.skprx` and `fd_fix.skprx` to your taiHEN plugins folder (usually `ux0:tai`) and adding two entries to your `config.txt` under `*KERNEL`:
  
```
  *KERNEL
  ux0:tai/kubridge.skprx
  ux0:tai/fd_fix.skprx
```

**Note** Don't install fd_fix.skprx if you're using repatch plugin

- **Optional**: Install [PSVshell](https://github.com/Electry/PSVshell/releases) to overclock your device to 500Mhz.
- **Optional**: Install [CapUnlocker](https://github.com/GrapheneCt/CapUnlocker/releases) to use the 4th core.
- Obtain your copy of *Bully: Anniversary Edition* legally for Android in form of an `.apk` file and one or more `.obb` files (usually `main.11.com.rockstargames.bully.obb` and `patch.11.com.rockstargames.bully.obb` located inside the `/sdcard/android/obb/com.rockstargames.bully/`) folder. [You can get all the required files directly from your phone](https://stackoverflow.com/questions/11012976/how-do-i-get-the-apk-of-an-installed-app-without-root-access) or by using an apk extractor you can find in the play store. The apk can be extracted with whatever Zip extractor you prefer (eg: WinZip, WinRar, etc...) since apk is basically a zip file. You can rename `.apk` to `.zip` to open them with your default zip extractor.
- Open the apk with your zip explorer, extract the `assets` folder from your `.apk` file to `ux0:data` and rename it to `Bully`. The result would be `ux0:data/Bully/`
- Still in the apk, extract the file `libBully.so` from the `lib/armeabi-v7a` folder to `ux0:data/Bully`. 
- Create the folder `ux0:data/Bully/Android`, copy and paste (do not extract!) `main.11.com.rockstargames.bully.obb` to `ux0:data/Bully/Android/main.11.com.rockstargames.bully.obb` and `patch.11.com.rockstargames.bully.obb` to `ux0:data/Bully/Android/patch.11.com.rockstargames.bully.obb` and finally rename them to to `ux0:data/Bully/Android/main.obb` and `ux0:data/Bully/Android/patch.obb`.
- Download the [gamefiles.zip](https://github.com/TheOfficialFloW/bully_vita/releases/download/v1.0/gamefiles.zip) and extract the contents to `ux0:data/Bully`.
- Install [Bully.vpk](https://github.com/TheOfficialFloW/bully_vita/releases/download/v1.0/Bully.vpk) on your *PS Vita* and enjoy the game. Note that for the first boot, the game may take around 3min to generate `.idx` files for the `.obb` files. Make sure that your device does not go to sleep in that time. After the first boot, the game should take around 1min to start.
- Once in game, press START to open the pause menu, then go to Settings and change "Clarity" to "High" for native resolution.

## Notice

- The official game does not free unused textures (as modern smartphones have more RAM than the PS Vita), which used to make the game crash after a long gameplay session. The loader now tracks the textures the game uploads and, once they no longer fit in the *PS Vita*'s video memory, drops the ones it has stopped drawing with. Dropping one is reversible: a copy is kept in `ux0:data/Bully/texcache.bin` and the texture is uploaded again the moment the game draws with it, so revisiting an area you have already been to looks the same as the first time. That file is emptied on every boot and removed when you quit. It is sized from the free space on your memory card and never takes the last 512MB of it; with less than that spare the loader does without it entirely, which costs the reloading but still keeps the game inside the *PS Vita*'s memory.
- If the game crashes, and there are files available in `ux0:data/Bully/glsl`, please send them to us. If there are too many, then it is because you forgot to install `gamefiles.zip`, in which case do not send us the files.

## Build Instructions (For Developers)

Every object in the build has to use the soft-float calling convention, because
the loader calls back and forth into the game's Android `.so`. VitaSDK publishes
a toolchain and package set for exactly this, so this no longer means building
the SDK yourself:

```bash
curl -sSLO https://raw.githubusercontent.com/vitasdk/vdpm/master/bootstrap-vitasdk.sh
chmod +x bootstrap-vitasdk.sh
VITASDK_CHANNEL=nightly-softfp ./bootstrap-vitasdk.sh --install-dir "$HOME/vitasdk"
export VITASDK="$HOME/vitasdk"
export PATH="$VITASDK/bin:$PATH"
```

Most dependencies are then a one-liner:

```bash
vdpm install libmathneon mpg123 kubridge taihen SceShaccCgExt vitaShaRK
```

`openal-soft` has to be built from source, with one patch:

```bash
git clone -b vita-1.19.1 https://github.com/isage/openal-soft
git -C openal-soft checkout --detach c851a68f0939040a56bf0dd453367c1021264bb9
grep -rl 'alignas(16)' --include=*.h --include=*.c openal-soft | xargs sed -i 's/alignas(16)/alignas(8)/g'
cmake -S openal-soft -B openal-soft/build \
  -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS="-mfloat-abi=softfp -Wno-error=implicit-function-declaration -Wno-error=int-conversion -Wno-error=incompatible-pointer-types"
cmake --build openal-soft/build --target install
```

openal-soft declares `aluVector` and `aluMatrixf` `alignas(16)`, but reaches them
through struct-return slots derived from the stack pointer, and AAPCS only
guarantees SP is 8-byte aligned. The compiler is then entitled to emit a
16-byte-aligned NEON store to an 8-byte-aligned address, which faults. Whether it
does comes down to frame layout, so it is luck rather than a setting: the 2021
build survives it, a build with a current compiler does not, and it crashes in
`aluMixData` before the first frame. Asking for 8-byte alignment costs nothing on
this hardware and makes the store legal either way.

`vitaGL` has to be built from source, because the port depends on compile-time
options the published package does not set, and it has to be **this revision**:

```bash
git clone https://github.com/Rinnegatamante/vitaGL
git -C vitaGL checkout --detach 4e4f5b6ad0bf43754935250a1707fad67eb4e450
make -C vitaGL \
  CC="arm-vita-eabi-gcc -Wno-error=incompatible-pointer-types -Wno-error=implicit-function-declaration -Wno-error=int-conversion" \
  SOFTFP_ABI=1 UNPURE_TEXTURES=1 PHYCONT_ON_DEMAND=1 install
```

That is the revision this port was written and released against, and two later
changes break it outright:

The port now compiles the game's GLSL at runtime through vitaGL's shader
compiler and caches the result, which is what current ports of this family do
(see gtasa_vita). **This requires `libshacccg.suprx` to be installed on the
console.** The `gxp` folder in `gamefiles.zip` is no longer used.

- `glShaderBinary` now expects vitaGL's own serialized shader-cache format
  instead of a raw GXP. The `.gxp` files in `gamefiles.zip` are raw GXPs, so a
  newer vitaGL reads one as a cache blob and registers no shader at all. Nothing
  reports an error: the game boots, plays audio and renders a black screen.
  `CMakeLists.txt` checks the installed `libvitaGL.a` for `unserialize_shader`
  at configure time and refuses to build, so this shows up as a build error
  rather than as a black screen.
- `vglEnableRuntimeShaderCompiler` was removed, so `vglInit` always starts the
  runtime shader compiler, which loads `SceShaccCg` and patches it through
  taiHEN. This port supplies precompiled shaders and has no reason to.

Delete `libvitaGL.a` before building it, both in the vitaGL checkout and in
`$VITASDK/arm-vita-eabi/lib`. The Makefile finishes with `ar -rc`, which adds
to an existing archive instead of replacing it, so an archive left behind by a
different revision keeps objects that revision no longer builds. The file lists
differ enough between revisions that the link then contains two incompatible
versions of vitaGL at once: it links cleanly, `sceGxmInitialize`,
`sceGxmBeginScene` and `sceGxmEndScene` all return success, and nothing is ever
drawn.

vitaGL's own error reporting (`LOG_ERRORS=1`) goes through `vgl_log`, which it
defines as `sceClibPrintf` -- a debug console that is not attached on a retail
Vita, so nothing reaches the card. Patch `source/utils/debug_utils.h` to call
`vgl_file_log` instead and the loader supplies one that appends to its trace
file. Without that, an absent log means nothing was routed anywhere, not that
nothing went wrong.

Do not take a newer vitaGL without checking both. The overridden `CC` demotes
diagnostics GCC 14 turned into errors; this is 2021 code written under the older
default, so demoting them preserves its behaviour rather than changing it.

Do not turn on vitaGL's `HAVE_TEXTURE_CACHE` or `TEXTURE_UPLOADS_SPEEDHACK`.
The loader does its own texture eviction and both of those change the ownership
of texture memory underneath it.

Finally, install the SceLibc stubs and build:

```bash
make -C libc_bridge install
cmake -B build -S .
cmake --build build
```

The result is `build/Bully.vpk`.

[`.github/workflows/build.yml`](.github/workflows/build.yml) runs exactly these
steps on every push and pins the same vitaGL revision.

If the game misbehaves and you want to rule the texture cache out, create an
empty file at `ux0:data/Bully/no_texcache`. The loader then leaves the game's
texture handling exactly as it was, with no tracking, no eviction and no cache
file. Delete it to turn the cache back on; no rebuild either way.

## Credits

- Rinnegatamante for porting the renderer using vitaGL and making various improvements to the port.
- Freakler for providing LiveArea assets.
- frangarcj, fgsfds and Bythos for graphics-related stuff.
- isage for the native audio backend for OpenAL-Soft.
- VictorPines for PlayStation buttons and m1s3ry for shrinking and centering them.
- Samilop Iter for betatesting.

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

- The official game does not free unused textures (as modern smartphones have more RAM than the PS Vita), which used to make the game crash after a long gameplay session. The loader now tracks the textures the game uploads and, once they no longer fit in the *PS Vita*'s video memory, drops the ones it has stopped drawing with. Dropping one is reversible: a copy is kept in `ux0:data/Bully/texcache.bin` and the texture is uploaded again the moment the game draws with it, so revisiting an area you have already been to looks the same as the first time. That file is written as you play, is deleted when you quit, and is recreated empty on every boot, so make sure you have a few hundred MB free on your memory card.
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
vdpm install libmathneon mpg123 openal-soft kubridge taihen SceShaccCgExt
```

`vitaShaRK` and `vitaGL` have to be built from source, because the port depends
on vitaGL compile-time options the published package does not set:

```bash
git clone https://github.com/Rinnegatamante/vitaShaRK
make -C vitaShaRK SOFTFP_ABI=1 install

git clone https://github.com/Rinnegatamante/vitaGL
make -C vitaGL SOFTFP_ABI=1 UNPURE_TEXTURES=1 PHYCONT_ON_DEMAND=1 install
```

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
steps on every push, and pins the vitaGL and vitaShaRK revisions it was last
verified against. If you take a newer vitaGL, bump the pin there in the same
commit as whatever it breaks.

## Credits

- Rinnegatamante for porting the renderer using vitaGL and making various improvements to the port.
- Freakler for providing LiveArea assets.
- frangarcj, fgsfds and Bythos for graphics-related stuff.
- isage for the native audio backend for OpenAL-Soft.
- VictorPines for PlayStation buttons and m1s3ry for shrinking and centering them.
- Samilop Iter for betatesting.

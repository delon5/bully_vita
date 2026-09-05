cmake_minimum_required(VERSION 2.8)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(Bully C CXX)
include("${VITASDK}/share/vita.cmake" REQUIRED)
set(VITA_APP_NAME "Bully")
set(VITA_TITLEID  "BULLY0000")
set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -Wl,-q,--wrap,memcpy,--wrap,memmove,--wrap,memset -D_GNU_SOURCE -Wall -O3 -mfloat-abi=softfp")
set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} -std=c++11")

# The game hands raw .gxp blobs to glShaderBinary. vitaGL changed that entry
# point after the revision this port targets: newer builds route it through
# unserialize_shader, which expects vitaGL's own serialized cache format and
# quietly registers nothing when handed a bare .gxp. Nothing fails loudly --
# the game boots, plays audio and renders a black screen. Check for the newer
# symbol at configure time so a mismatched vitaGL is a build error instead of
# an evening of coredumps. See README.md for the revision to build.
find_library(VITAGL_LIBRARY NAMES vitaGL PATHS "${VITASDK}/arm-vita-eabi/lib" NO_DEFAULT_PATH)
if(VITAGL_LIBRARY)
  execute_process(
    COMMAND "${VITASDK}/bin/arm-vita-eabi-nm" "${VITAGL_LIBRARY}"
    OUTPUT_VARIABLE VITAGL_SYMBOLS
    ERROR_QUIET
  )
  if(VITAGL_SYMBOLS MATCHES "unserialize_shader")
    message(FATAL_ERROR
      "The installed vitaGL routes glShaderBinary through unserialize_shader, "
      "which this port's precompiled .gxp shaders are not in the format for. "
      "Rebuild vitaGL at the revision pinned in .github/workflows/build.yml "
      "(VITAGL_REV) -- see README.md.")
  endif()
endif()

add_executable(Bully
  loader/main.c
  loader/dialog.c
  loader/fios.c
  loader/so_util.c
  loader/jni_patch.c
  loader/movie_patch.c
  loader/openal_patch.c
  loader/sha1.c
  loader/texture_cache.c
)

target_link_libraries(Bully
  -Wl,--whole-archive pthread -Wl,--no-whole-archive
  # These depend on each other in both directions -- vitaGL calls into mathneon
  # and libstdc++, openal calls into libm -- and a linker only resolves an
  # archive's undefined symbols from archives listed after it. Group them so it
  # iterates until everything is resolved instead of relying on one true order.
  -Wl,--start-group
  mathneon
  mpg123
  openal
  vitaGL
  vitashark
  # vitaShaRK references these to patch the ShaccCg module through taiHEN. The
  # loader disables the runtime shader compiler so none of it ever runs, but the
  # symbols still have to resolve.
  SceShaccCgExt
  taihen_stub
  stdc++
  m
  -Wl,--end-group
  kubridge_stub
  SceAppMgr_stub
  SceAppUtil_stub
  SceAudio_stub
  SceAudioIn_stub
  SceAvPlayer_stub
  SceCtrl_stub
  SceCommonDialog_stub
  SceDisplay_stub
  SceKernelDmacMgr_stub
  SceFios2_stub
  SceGxm_stub
  SceLibcBridge_stub
  SceShaccCg_stub
  SceSysmodule_stub
  ScePower_stub
  SceTouch_stub
  SceVshBridge_stub
)

vita_create_self(eboot.bin Bully UNSAFE)
vita_create_vpk(Bully.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE ${CMAKE_SOURCE_DIR}/sce_sys/icon0.png sce_sys/icon0.png
       ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
       ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
       ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
)

add_custom_target(copy
  COMMAND cp eboot.bin E:/app/${VITA_TITLEID}/eboot.bin
  DEPENDS eboot.bin
)

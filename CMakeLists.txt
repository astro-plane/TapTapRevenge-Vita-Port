cmake_minimum_required(VERSION 3.16)

# PS Vita build (default when VITASDK is set):
#     cmake -S . -B build && cmake --build build          -> build/ttrvita.vpk
# Desktop build (mouse + keyboard):
#     cmake -S . -B build-pc -DDESKTOP=ON && cmake --build build-pc
option(DESKTOP "Build for the host PC instead of the PS Vita" OFF)
# Putting app_data/ inside the VPK makes it ~1 GB, which VitaShell needs well
# over an hour to install.  By default the VPK holds only the program, and the
# app_data folder is copied to ux0:data/ttrvita/app_data/ by hand (README.md).
option(EMBED_DATA "Pack app_data/ into the VPK (slow to install)" OFF)

if(NOT DESKTOP)
  if(NOT DEFINED ENV{VITASDK})
    message(FATAL_ERROR "VITASDK is not set. Run: export VITASDK=/usr/local/vitasdk  (or pass -DDESKTOP=ON)")
  endif()
  if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  endif()
endif()

project(ttrvita C)
set(CMAKE_C_STANDARD 99)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(ttrvita src/main.c src/game.c src/theme.c src/png_load.c src/plat_sdl.c)
target_compile_options(ttrvita PRIVATE -Wall)

if(DESKTOP)
  find_package(SDL2 REQUIRED)
  find_package(PNG REQUIRED)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(VORBISFILE REQUIRED IMPORTED_TARGET vorbisfile)
  target_include_directories(ttrvita PRIVATE ${SDL2_INCLUDE_DIRS})
  target_link_libraries(ttrvita PRIVATE ${SDL2_LIBRARIES} PNG::PNG PkgConfig::VORBISFILE m)
else()
  include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

  set(VITA_APP_NAME "Open Tap Avenged")   # the name on the Vita's home screen
  set(VITA_TITLEID  "TTRV00001")
  set(VITA_VERSION  "01.00")

  if(NOT EXISTS "${CMAKE_SOURCE_DIR}/app_data/songs.txt")
    if(EMBED_DATA)
      message(FATAL_ERROR "app_data/songs.txt not found - run tools/prep_assets.py first (see README.md).")
    endif()
    message(WARNING "app_data/ has not been built yet: the game needs app_data copied to "
                    "ux0:data/ttrvita/app_data/.")
  endif()

  # Home screen icon: this project's own (sce_sys/icon0.png: the three
  # tappers, red, green and blue, over a neon grid), 128x128 and 8-bit
  # indexed as LiveArea requires.
  set(VITA_ICON "${CMAKE_SOURCE_DIR}/sce_sys/icon0.png")

  target_include_directories(ttrvita PRIVATE "$ENV{VITASDK}/arm-vita-eabi/include/SDL2")
  # Static libraries: order matters (vorbisfile -> vorbis -> ogg).
  # Packages: vdpm install sdl2 libpng zlib libvorbis libogg
  target_link_libraries(ttrvita
    SDL2 vorbisfile vorbis ogg png z m
    SceDisplay_stub SceCtrl_stub SceAudio_stub SceAudioIn_stub SceSysmodule_stub
    SceGxm_stub SceCommonDialog_stub SceAppMgr_stub SceAppUtil_stub SceTouch_stub
    SceHid_stub SceMotion_stub ScePower_stub SceIme_stub SceProcessmgr_stub
  )

  set(VPK_FILES
    FILE ${VITA_ICON} sce_sys/icon0.png
    FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
    FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
    FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
  )
  if(EMBED_DATA)
    list(APPEND VPK_FILES FILE app_data app_data)
  elseif(EXISTS "${CMAKE_SOURCE_DIR}/app_data/gfx/art.png")
    # The song covers are small (about 2 MB): they ride along in the VPK, so
    # they need not be copied by hand. A copy in ux0:data/ttrvita/app_data/gfx/
    # still wins. Re-run cmake after making art.png for the first time.
    list(APPEND VPK_FILES FILE app_data/gfx/art.png app_data/gfx/art.png)
  endif()

  vita_create_self(ttrvita.self ttrvita)
  vita_create_vpk(ttrvita.vpk ${VITA_TITLEID} ttrvita.self
    VERSION ${VITA_VERSION}
    NAME "${VITA_APP_NAME}"
    ${VPK_FILES}
  )
endif()

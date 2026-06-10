cmake_minimum_required(VERSION 3.16)
project(goattrk2-qt LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
# Optional: self-voicing accessibility (Speech.cpp). Without the module the
# editor builds and runs unchanged, just silent — all speech code is behind
# #ifdef GT2_HAVE_TTS.
find_package(Qt6 QUIET OPTIONAL_COMPONENTS TextToSpeech)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL REQUIRED sdl)
pkg_check_modules(JACK REQUIRED jack)

# PortAudio replaces QtMultimedia for audio output — Qt 6.4's pull-mode
# QAudioSink forced a fat sink buffer to avoid PipeWire/Pulse underruns
# (callback start() only landed in Qt 6.11). PortAudio gives us a direct
# RT-priority callback at ~10 ms latency on all hosts we ship.
find_library(PORTAUDIO_LIB portaudio REQUIRED)
find_path(PORTAUDIO_INCLUDE portaudio.h REQUIRED)

# Some SDL 1.2 pkg-config files (notably macOS sdl12-compat) list -lSDLmain,
# which hijacks main() and may not even exist as a separate library. This app
# provides its own main() (via Qt), so drop SDLmain when present.
if(SDL_LIBRARIES)
    list(REMOVE_ITEM SDL_LIBRARIES SDLmain)
endif()

# SDL/JACK are linked by name (-lSDL, -ljack); make sure their pkg-config
# library directories are on the linker search path (needed on macOS where
# Homebrew lives outside the default search paths).
link_directories(${SDL_LIBRARY_DIRS} ${JACK_LIBRARY_DIRS})

# SDL 1.2 (and sdl12-compat) #define main SDL_main on macOS/Windows and expect
# an SDLmain library to supply the real entry point. This app owns its entry
# point via Qt, so tell SDL to keep its hands off main().
add_compile_definitions(SDL_MAIN_HANDLED)

set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../src)

# Core data + edit logic (no rendering calls verified by grep)
set(CORE_SRC
    ${SRC_DIR}/gsong.c
    ${SRC_DIR}/gorder.c
    ${SRC_DIR}/gpattern.c
    ${SRC_DIR}/ginstr.c
    ${SRC_DIR}/gtable.c
    ${SRC_DIR}/gplay.c
    ${SRC_DIR}/gsound.c
    ${SRC_DIR}/gsid.cpp
)

# libresidfp 2.x backend (vendored under src/residfp/)
set(RESIDFP_SRC
    ${SRC_DIR}/residfp/Dac.cpp
    ${SRC_DIR}/residfp/EnvelopeGenerator.cpp
    ${SRC_DIR}/residfp/ExternalFilter.cpp
    ${SRC_DIR}/residfp/Filter.cpp
    ${SRC_DIR}/residfp/Filter6581.cpp
    ${SRC_DIR}/residfp/Filter8580.cpp
    ${SRC_DIR}/residfp/FilterModelConfig.cpp
    ${SRC_DIR}/residfp/FilterModelConfig6581.cpp
    ${SRC_DIR}/residfp/FilterModelConfig8580.cpp
    ${SRC_DIR}/residfp/Integrator6581.cpp
    ${SRC_DIR}/residfp/Integrator8580.cpp
    ${SRC_DIR}/residfp/OpAmp.cpp
    ${SRC_DIR}/residfp/SID.cpp
    ${SRC_DIR}/residfp/Spline.cpp
    ${SRC_DIR}/residfp/version.cc
    ${SRC_DIR}/residfp/WaveformCalculator.cpp
    ${SRC_DIR}/residfp/WaveformGenerator.cpp
    ${SRC_DIR}/residfp/resample/SincResampler.cpp
    ${SRC_DIR}/residfp/residfp/residfp.cpp
)
file(GLOB ASM_SRC ${SRC_DIR}/asm/*.c)

set(BME_SRC
    ${SRC_DIR}/bme/bme.c
    ${SRC_DIR}/bme/bme_snd.c
    ${SRC_DIR}/bme/bme_io.c
    ${SRC_DIR}/bme/bme_end.c
)

set(QT_SRC
    main.cpp
    MainWindow.cpp
    CheatSheet.cpp
    PatternView.cpp
    OrderView.cpp
    InstrumentView.cpp
    TablesView.cpp
    SongNameView.cpp
    OrderMiniMap.cpp
    InstrumentQuickList.cpp
    StatusStrip.cpp
    UndoStack.cpp
    Speech.cpp
    RpcServer.cpp
    PaAudio.cpp
    CoreEvents.cpp
    InstrColors.cpp
    Log.cpp
    qt_globals.c
    qt_stubs.c
)

set(QT_HEADERS
    Theme.h
    SdlKeyMap.h
)

add_executable(goattrk2-qt
    ${QT_SRC}
    ${CORE_SRC}
    ${RESIDFP_SRC}
    ${ASM_SRC}
    ${BME_SRC}
)

target_include_directories(goattrk2-qt PRIVATE
    ${SRC_DIR}
    ${SRC_DIR}/bme
    ${SRC_DIR}/asm
    ${SRC_DIR}/residfp
    ${SRC_DIR}/residfp/residfp
    ${SDL_INCLUDE_DIRS}
    ${JACK_INCLUDE_DIRS}
    ${PORTAUDIO_INCLUDE}
)

target_compile_options(goattrk2-qt PRIVATE
    -fcommon
    -Wno-write-strings
    -Wno-narrowing
    ${SDL_CFLAGS_OTHER}
    ${JACK_CFLAGS_OTHER}
)

target_link_libraries(goattrk2-qt PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets
    ${SDL_LIBRARIES}
    ${JACK_LIBRARIES}
    ${PORTAUDIO_LIB}
    m pthread
)

# Wire up self-voicing only when the Qt TextToSpeech module is present.
if(Qt6TextToSpeech_FOUND)
    target_link_libraries(goattrk2-qt PRIVATE Qt6::TextToSpeech)
    target_compile_definitions(goattrk2-qt PRIVATE GT2_HAVE_TTS=1)
endif()

# Stereo support is now a runtime flag (stereo_mode global) inside the single
# goattrk2-qt binary above. The separate goattrk2-qt-stereo target is gone.

# Qt may build Windows executables for the GUI subsystem (entry point WinMain).
# This app keeps a plain main() (see main.cpp), so force the console subsystem
# on Windows; the linker then uses main() as the entry point.
if(WIN32)
    target_link_options(goattrk2-qt PRIVATE -Wl,-subsystem,console)
endif()

# ---------------------------------------------------------------------------
# gt2reloc — standalone commandline relocator/packer (backport from v2.73)
#
# Plain CLI tool, no Qt. Reuses the core tracker objects (gsong, greloc, ...)
# plus the bme and reSIDfp backends so the same loadsong() / relocator() code
# paths run headlessly. The GT2RELOC #define stubs out the interactive UI
# code paths inside greloc.c.
# ---------------------------------------------------------------------------

set(GT2RELOC_SRC
    ${SRC_DIR}/gt2reloc.c
    ${SRC_DIR}/gsong.c
    ${SRC_DIR}/gorder.c
    ${SRC_DIR}/gpattern.c
    ${SRC_DIR}/ginstr.c
    ${SRC_DIR}/gtable.c
    ${SRC_DIR}/gplay.c
    ${SRC_DIR}/gdisplay.c
    ${SRC_DIR}/gfile.c
    ${SRC_DIR}/ghelp.c
    ${SRC_DIR}/gsound.c
    ${SRC_DIR}/gconsole.c
    ${SRC_DIR}/gsid.cpp
)

set(BME_FULL_SRC
    ${SRC_DIR}/bme/bme.c
    ${SRC_DIR}/bme/bme_snd.c
    ${SRC_DIR}/bme/bme_io.c
    ${SRC_DIR}/bme/bme_end.c
    ${SRC_DIR}/bme/bme_gfx.c
    ${SRC_DIR}/bme/bme_win.c
    ${SRC_DIR}/bme/bme_mou.c
    ${SRC_DIR}/bme/bme_kbd.c
)

# gt2reloc needs only the empty datafile[] symbol; everything else
# (editstring, resettime, etc.) comes from the SDL files it already links.
add_executable(gt2reloc
    ${GT2RELOC_SRC}
    gt2reloc_stub.c
    ${RESIDFP_SRC}
    ${ASM_SRC}
    ${BME_FULL_SRC}
)

target_include_directories(gt2reloc PRIVATE
    ${SRC_DIR}
    ${SRC_DIR}/bme
    ${SRC_DIR}/asm
    ${SRC_DIR}/residfp
    ${SRC_DIR}/residfp/residfp
    ${SDL_INCLUDE_DIRS}
    ${JACK_INCLUDE_DIRS}
)

target_compile_options(gt2reloc PRIVATE
    -fcommon
    -fgnu89-inline
    -Wno-write-strings
    -Wno-narrowing
    ${SDL_CFLAGS_OTHER}
)

target_link_libraries(gt2reloc PRIVATE
    ${SDL_LIBRARIES}
    ${JACK_LIBRARIES}
    m pthread
)

# ---------------------------------------------------------------------------
# sid2sng — convert GoatTracker-generated .sid files back to .sng so the
# Qt frontend can open them. Vendored copy of github.com/sasq64/sid2sng
# under ext/sid2sng/. Standalone CLI; MainWindow runs it via QProcess
# when the user picks a .sid in the Open Song dialog.
# ---------------------------------------------------------------------------
set(SID2SNG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../ext/sid2sng)
add_executable(sid2sng
    ${SID2SNG_DIR}/main.cpp
    ${SID2SNG_DIR}/gsong.cpp
)
target_include_directories(sid2sng PRIVATE ${SID2SNG_DIR})
target_compile_features(sid2sng PRIVATE cxx_std_17)
target_link_libraries(sid2sng PRIVATE stdc++fs)

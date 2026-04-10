cmake_minimum_required(VERSION 3.22.1)

project(questretrodepth_native LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(QUESTRETRODEPTH_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../..")
set(SNES9X_DIR "${QUESTRETRODEPTH_ROOT}/third_party/snes9x")
set(SNES9X_LIBRETRO_COMMON_DIR "${SNES9X_DIR}/libretro/libretro-common")
set(PICODRIVE_DIR "${QUESTRETRODEPTH_ROOT}/third_party/picodrive")
set(PICODRIVE_LIBRETRO_COMMON_DIR "${PICODRIVE_DIR}/platform/libretro/libretro-common")
set(PICODRIVE_LIBCHDR_DIR "${PICODRIVE_DIR}/pico/cd/libchdr")
set(PICODRIVE_LZMA_DIR "${PICODRIVE_LIBCHDR_DIR}/deps/lzma-24.05")
set(PICODRIVE_ZSTD_DIR "${PICODRIVE_LIBCHDR_DIR}/deps/zstd-1.5.6/lib")

set(SNES9X_SOURCES
    ${SNES9X_DIR}/filter/snes_ntsc.c
    ${SNES9X_DIR}/apu/apu.cpp
    ${SNES9X_DIR}/apu/bapu/dsp/sdsp.cpp
    ${SNES9X_DIR}/apu/bapu/smp/smp.cpp
    ${SNES9X_DIR}/apu/bapu/smp/smp_state.cpp
    ${SNES9X_DIR}/bsx.cpp
    ${SNES9X_DIR}/c4.cpp
    ${SNES9X_DIR}/c4emu.cpp
    ${SNES9X_DIR}/cheats.cpp
    ${SNES9X_DIR}/cheats2.cpp
    ${SNES9X_DIR}/clip.cpp
    ${SNES9X_DIR}/conffile.cpp
    ${SNES9X_DIR}/controls.cpp
    ${SNES9X_DIR}/cpu.cpp
    ${SNES9X_DIR}/cpuexec.cpp
    ${SNES9X_DIR}/cpuops.cpp
    ${SNES9X_DIR}/crosshairs.cpp
    ${SNES9X_DIR}/dma.cpp
    ${SNES9X_DIR}/dsp.cpp
    ${SNES9X_DIR}/dsp1.cpp
    ${SNES9X_DIR}/dsp2.cpp
    ${SNES9X_DIR}/dsp3.cpp
    ${SNES9X_DIR}/dsp4.cpp
    ${SNES9X_DIR}/fxinst.cpp
    ${SNES9X_DIR}/fxemu.cpp
    ${SNES9X_DIR}/gfx.cpp
    ${SNES9X_DIR}/globals.cpp
    ${SNES9X_DIR}/memmap.cpp
    ${SNES9X_DIR}/movie.cpp
    ${SNES9X_DIR}/msu1.cpp
    ${SNES9X_DIR}/obc1.cpp
    ${SNES9X_DIR}/ppu.cpp
    ${SNES9X_DIR}/sa1.cpp
    ${SNES9X_DIR}/sa1cpu.cpp
    ${SNES9X_DIR}/screenshot.cpp
    ${SNES9X_DIR}/sdd1.cpp
    ${SNES9X_DIR}/sdd1emu.cpp
    ${SNES9X_DIR}/seta.cpp
    ${SNES9X_DIR}/seta010.cpp
    ${SNES9X_DIR}/seta011.cpp
    ${SNES9X_DIR}/seta018.cpp
    ${SNES9X_DIR}/sha256.cpp
    ${SNES9X_DIR}/snapshot.cpp
    ${SNES9X_DIR}/snes9x.cpp
    ${SNES9X_DIR}/spc7110.cpp
    ${SNES9X_DIR}/srtc.cpp
    ${SNES9X_DIR}/stream.cpp
    ${SNES9X_DIR}/tile.cpp
    ${SNES9X_DIR}/tileimpl-h2x1.cpp
    ${SNES9X_DIR}/tileimpl-n1x1.cpp
    ${SNES9X_DIR}/tileimpl-n2x1.cpp
    ${SNES9X_DIR}/bml.cpp
    ${SNES9X_DIR}/fscompat.cpp
    ${SNES9X_DIR}/libretro/libretro.cpp
    # Thin accessor so our frontend can read GFX.ZBuffer without pulling in snes9x.h
    ${CMAKE_CURRENT_SOURCE_DIR}/snes9x_zbuffer_accessor.cpp
    # Main-screen visible-source ownership accessor.
    ${CMAKE_CURRENT_SOURCE_DIR}/snes9x_visible_source_accessor.cpp
    # Per-hardware-layer capture buffers + accessor (needs snes9x.h / gfx.h)
    ${CMAKE_CURRENT_SOURCE_DIR}/snes9x_layer_capture.cpp
)

add_library(snes9x_libretro STATIC ${SNES9X_SOURCES})

set_target_properties(snes9x_libretro PROPERTIES
    CXX_STANDARD 14
    CXX_STANDARD_REQUIRED ON
    C_STANDARD 11
)

target_include_directories(snes9x_libretro PRIVATE
    ${SNES9X_DIR}
    ${SNES9X_DIR}/apu
    ${SNES9X_DIR}/apu/bapu
    ${SNES9X_DIR}/libretro
    ${SNES9X_LIBRETRO_COMMON_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}   # for snes9x_zbuffer_accessor.h
)

target_compile_definitions(snes9x_libretro PRIVATE
    RIGHTSHIFT_IS_SAR
    __LIBRETRO__
    ALLOW_CPU_OVERCLOCK
    HAVE_STDINT_H
    HAVE_STRINGS_H
    ARM
)

target_compile_options(snes9x_libretro PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
)

configure_file(
    ${PICODRIVE_DIR}/tools/offsets/generic-ilp32-offsets.h
    ${PICODRIVE_DIR}/pico/pico_int_offs.h
    COPYONLY
)

set(PICODRIVE_SOURCES
    ${PICODRIVE_DIR}/platform/libretro/libretro.c
    ${PICODRIVE_LIBRETRO_COMMON_DIR}/formats/png/rpng.c
    ${PICODRIVE_LIBRETRO_COMMON_DIR}/streams/trans_stream.c
    ${PICODRIVE_LIBRETRO_COMMON_DIR}/streams/trans_stream_pipe.c
    ${PICODRIVE_LIBRETRO_COMMON_DIR}/streams/trans_stream_zlib.c
    ${PICODRIVE_LIBRETRO_COMMON_DIR}/file/file_path_io.c
    ${PICODRIVE_LIBRETRO_COMMON_DIR}/file/file_path.c
    ${PICODRIVE_LIBRETRO_COMMON_DIR}/streams/file_stream.c
    ${PICODRIVE_LIBRETRO_COMMON_DIR}/streams/file_stream_transforms.c
    ${PICODRIVE_LIBRETRO_COMMON_DIR}/vfs/vfs_implementation.c
    ${PICODRIVE_LIBRETRO_COMMON_DIR}/time/rtime.c
    ${PICODRIVE_LIBRETRO_COMMON_DIR}/string/stdstring.c
    ${PICODRIVE_LIBRETRO_COMMON_DIR}/encodings/encoding_utf.c
    ${PICODRIVE_LIBRETRO_COMMON_DIR}/compat/compat_strcasestr.c
    ${PICODRIVE_LIBRETRO_COMMON_DIR}/compat/compat_strl.c
    ${PICODRIVE_DIR}/platform/common/mp3.c
    ${PICODRIVE_DIR}/platform/common/mp3_sync.c
    ${PICODRIVE_DIR}/platform/common/mp3_dummy.c
    ${PICODRIVE_DIR}/unzip/unzip.c
    ${PICODRIVE_LIBCHDR_DIR}/src/libchdr_bitstream.c
    ${PICODRIVE_LIBCHDR_DIR}/src/libchdr_cdrom.c
    ${PICODRIVE_LIBCHDR_DIR}/src/libchdr_chd.c
    ${PICODRIVE_LIBCHDR_DIR}/src/libchdr_flac.c
    ${PICODRIVE_LIBCHDR_DIR}/src/libchdr_huffman.c
    ${PICODRIVE_LZMA_DIR}/src/Alloc.c
    ${PICODRIVE_LZMA_DIR}/src/CpuArch.c
    ${PICODRIVE_LZMA_DIR}/src/Delta.c
    ${PICODRIVE_LZMA_DIR}/src/LzFind.c
    ${PICODRIVE_LZMA_DIR}/src/LzmaDec.c
    ${PICODRIVE_LZMA_DIR}/src/LzmaEnc.c
    ${PICODRIVE_LZMA_DIR}/src/Sort.c
    ${PICODRIVE_ZSTD_DIR}/common/entropy_common.c
    ${PICODRIVE_ZSTD_DIR}/common/error_private.c
    ${PICODRIVE_ZSTD_DIR}/common/fse_decompress.c
    ${PICODRIVE_ZSTD_DIR}/common/xxhash.c
    ${PICODRIVE_ZSTD_DIR}/common/zstd_common.c
    ${PICODRIVE_ZSTD_DIR}/decompress/huf_decompress.c
    ${PICODRIVE_ZSTD_DIR}/decompress/zstd_ddict.c
    ${PICODRIVE_ZSTD_DIR}/decompress/zstd_decompress_block.c
    ${PICODRIVE_ZSTD_DIR}/decompress/zstd_decompress.c
    ${PICODRIVE_DIR}/pico/pico.c
    ${PICODRIVE_DIR}/pico/cart.c
    ${PICODRIVE_DIR}/pico/memory.c
    ${PICODRIVE_DIR}/pico/state.c
    ${PICODRIVE_DIR}/pico/sek.c
    ${PICODRIVE_DIR}/pico/z80if.c
    ${PICODRIVE_DIR}/pico/videoport.c
    ${PICODRIVE_DIR}/pico/draw2.c
    ${PICODRIVE_DIR}/pico/draw.c
    ${PICODRIVE_DIR}/pico/mode4.c
    ${PICODRIVE_DIR}/pico/misc.c
    ${PICODRIVE_DIR}/pico/eeprom.c
    ${PICODRIVE_DIR}/pico/patch.c
    ${PICODRIVE_DIR}/pico/debug.c
    ${PICODRIVE_DIR}/pico/media.c
    ${PICODRIVE_DIR}/pico/sms.c
    ${PICODRIVE_DIR}/pico/cd/mcd.c
    ${PICODRIVE_DIR}/pico/cd/memory.c
    ${PICODRIVE_DIR}/pico/cd/sek.c
    ${PICODRIVE_DIR}/pico/cd/cdc.c
    ${PICODRIVE_DIR}/pico/cd/cdd.c
    ${PICODRIVE_DIR}/pico/cd/cd_image.c
    ${PICODRIVE_DIR}/pico/cd/cd_parse.c
    ${PICODRIVE_DIR}/pico/cd/gfx.c
    ${PICODRIVE_DIR}/pico/cd/gfx_dma.c
    ${PICODRIVE_DIR}/pico/cd/misc.c
    ${PICODRIVE_DIR}/pico/cd/pcm.c
    ${PICODRIVE_DIR}/pico/cd/megasd.c
    ${PICODRIVE_DIR}/pico/32x/32x.c
    ${PICODRIVE_DIR}/pico/32x/memory.c
    ${PICODRIVE_DIR}/pico/32x/draw.c
    ${PICODRIVE_DIR}/pico/32x/sh2soc.c
    ${PICODRIVE_DIR}/pico/32x/pwm.c
    ${PICODRIVE_DIR}/pico/pico/pico.c
    ${PICODRIVE_DIR}/pico/pico/memory.c
    ${PICODRIVE_DIR}/pico/pico/xpcm.c
    ${PICODRIVE_DIR}/pico/carthw/carthw.c
    ${PICODRIVE_DIR}/pico/carthw/eeprom_spi.c
    ${PICODRIVE_DIR}/pico/carthw/svp/svp.c
    ${PICODRIVE_DIR}/pico/carthw/svp/memory.c
    ${PICODRIVE_DIR}/pico/carthw/svp/ssp16.c
    ${PICODRIVE_DIR}/pico/sound/sound.c
    ${PICODRIVE_DIR}/pico/sound/resampler.c
    ${PICODRIVE_DIR}/pico/sound/sn76496.c
    ${PICODRIVE_DIR}/pico/sound/ym2612.c
    ${PICODRIVE_DIR}/pico/sound/ym2413.c
    ${PICODRIVE_DIR}/pico/sound/mix.c
    ${PICODRIVE_DIR}/cpu/fame/famec.c
    ${PICODRIVE_DIR}/cpu/cz80/cz80.c
    ${PICODRIVE_DIR}/cpu/drc/cmn.c
    ${PICODRIVE_DIR}/cpu/sh2/sh2.c
    ${PICODRIVE_DIR}/cpu/sh2/mame/sh2pico.c
)

add_library(picodrive_libretro STATIC ${PICODRIVE_SOURCES})

set_target_properties(picodrive_libretro PROPERTIES
    C_STANDARD 11
)

target_include_directories(picodrive_libretro PRIVATE
    ${PICODRIVE_DIR}
    ${PICODRIVE_DIR}/platform/libretro
    ${PICODRIVE_LIBRETRO_COMMON_DIR}/include
    ${PICODRIVE_LIBRETRO_COMMON_DIR}/include/compat
    ${PICODRIVE_LIBRETRO_COMMON_DIR}/include/encodings
    ${PICODRIVE_LIBRETRO_COMMON_DIR}/include/file
    ${PICODRIVE_LIBRETRO_COMMON_DIR}/include/formats
    ${PICODRIVE_LIBRETRO_COMMON_DIR}/include/streams
    ${PICODRIVE_LIBRETRO_COMMON_DIR}/include/string
    ${PICODRIVE_LIBRETRO_COMMON_DIR}/include/time
    ${PICODRIVE_LIBRETRO_COMMON_DIR}/include/vfs
    ${PICODRIVE_LIBCHDR_DIR}/include
    ${PICODRIVE_LZMA_DIR}/include
    ${PICODRIVE_ZSTD_DIR}
    ${PICODRIVE_DIR}/zlib
)

target_compile_definitions(picodrive_libretro PRIVATE
    __LIBRETRO__
    USE_LIBRETRO_VFS=1
    USE_LIBCHDR=1
    Z7_ST
    ZSTD_DISABLE_ASM
    _USE_CZ80
    EMU_F68K
    NO_THREADS
    REVISION=""
    retro_set_environment=picodrive_retro_set_environment
    retro_set_video_refresh=picodrive_retro_set_video_refresh
    retro_set_audio_sample=picodrive_retro_set_audio_sample
    retro_set_audio_sample_batch=picodrive_retro_set_audio_sample_batch
    retro_set_input_poll=picodrive_retro_set_input_poll
    retro_set_input_state=picodrive_retro_set_input_state
    retro_api_version=picodrive_retro_api_version
    option_defs_us=picodrive_option_defs_us
    option_defs_tr=picodrive_option_defs_tr
    option_defs_intl=picodrive_option_defs_intl
    retro_set_controller_port_device=picodrive_retro_set_controller_port_device
    retro_get_system_info=picodrive_retro_get_system_info
    retro_get_system_av_info=picodrive_retro_get_system_av_info
    retro_serialize_size=picodrive_retro_serialize_size
    retro_serialize=picodrive_retro_serialize
    retro_unserialize=picodrive_retro_unserialize
    retro_cheat_reset=picodrive_retro_cheat_reset
    retro_cheat_set=picodrive_retro_cheat_set
    retro_load_game=picodrive_retro_load_game
    retro_load_game_special=picodrive_retro_load_game_special
    retro_unload_game=picodrive_retro_unload_game
    retro_get_region=picodrive_retro_get_region
    retro_get_memory_data=picodrive_retro_get_memory_data
    retro_get_memory_size=picodrive_retro_get_memory_size
    retro_reset=picodrive_retro_reset
    retro_run=picodrive_retro_run
    retro_init=picodrive_retro_init
    retro_deinit=picodrive_retro_deinit
)

target_compile_options(picodrive_libretro PRIVATE
    -fno-strict-aliasing
)

add_library(questretrodepth_native SHARED
    emulator_backend.cpp
    game_config.cpp
    layer_processor.cpp
    gles_renderer.cpp
    openxr_shell.cpp
    picodrive_backend.cpp
    rom_browser.cpp
    snes_libretro_backend.cpp
    snes_backend_stub.cpp
    questretrodepth_main.cpp
)

find_package(OpenXR REQUIRED CONFIG)

find_library(log-lib log)
find_library(math-lib m)
find_library(egl-lib EGL)
find_library(gles-lib GLESv3)
find_library(aaudio-lib aaudio)
find_library(z-lib z)

target_include_directories(questretrodepth_native PRIVATE
    ${PICODRIVE_DIR}
    ${PICODRIVE_DIR}/platform/libretro
    ${PICODRIVE_LIBRETRO_COMMON_DIR}/include
    ${SNES9X_DIR}
    ${SNES9X_DIR}/libretro
    ${SNES9X_LIBRETRO_COMMON_DIR}/include
)

target_link_libraries(questretrodepth_native
    snes9x_libretro
    picodrive_libretro
    OpenXR::openxr_loader
    OpenXR::headers
    ${log-lib}
    ${math-lib}
    ${egl-lib}
    ${gles-lib}
    ${aaudio-lib}
    ${z-lib}
)

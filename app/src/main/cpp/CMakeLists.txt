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

# ============================================================
# mGBA (GBA + GB/GBC)
# ============================================================
set(MGBA_DIR "${QUESTRETRODEPTH_ROOT}/third_party/mgba")

set(MGBA_SOURCES
    # --- Platform / libretro ---
    ${MGBA_DIR}/src/platform/libretro/libretro.c
    ${MGBA_DIR}/src/platform/posix/memory.c

    # --- ARM CPU ---
    ${MGBA_DIR}/src/arm/arm.c
    ${MGBA_DIR}/src/arm/decoder.c
    ${MGBA_DIR}/src/arm/decoder-arm.c
    ${MGBA_DIR}/src/arm/decoder-thumb.c
    ${MGBA_DIR}/src/arm/isa-arm.c
    ${MGBA_DIR}/src/arm/isa-thumb.c

    # --- SM83 CPU (GB/GBC) ---
    ${MGBA_DIR}/src/sm83/sm83.c
    ${MGBA_DIR}/src/sm83/decoder.c
    ${MGBA_DIR}/src/sm83/isa-sm83.c

    # --- Core ---
    ${MGBA_DIR}/src/core/bitmap-cache.c
    ${MGBA_DIR}/src/core/cache-set.c
    ${MGBA_DIR}/src/core/cheats.c
    ${MGBA_DIR}/src/core/config.c
    ${MGBA_DIR}/src/core/core.c
    ${MGBA_DIR}/src/core/directories.c
    ${MGBA_DIR}/src/core/input.c
    ${MGBA_DIR}/src/core/interface.c
    ${MGBA_DIR}/src/core/lockstep.c
    ${MGBA_DIR}/src/core/log.c
    ${MGBA_DIR}/src/core/map-cache.c
    ${MGBA_DIR}/src/core/mem-search.c
    ${MGBA_DIR}/src/core/rewind.c
    ${MGBA_DIR}/src/core/serialize.c
    ${MGBA_DIR}/src/core/sync.c
    ${MGBA_DIR}/src/core/thread.c
    ${MGBA_DIR}/src/core/tile-cache.c
    ${MGBA_DIR}/src/core/timing.c

    # --- GBA ---
    ${MGBA_DIR}/src/gba/audio.c
    ${MGBA_DIR}/src/gba/bios.c
    ${MGBA_DIR}/src/gba/cart/ereader.c
    ${MGBA_DIR}/src/gba/cart/gpio.c
    ${MGBA_DIR}/src/gba/cart/matrix.c
    ${MGBA_DIR}/src/gba/cart/unlicensed.c
    ${MGBA_DIR}/src/gba/cart/vfame.c
    ${MGBA_DIR}/src/gba/cheats.c
    ${MGBA_DIR}/src/gba/cheats/codebreaker.c
    ${MGBA_DIR}/src/gba/cheats/gameshark.c
    ${MGBA_DIR}/src/gba/cheats/parv3.c
    ${MGBA_DIR}/src/gba/core.c
    ${MGBA_DIR}/src/gba/dma.c
    ${MGBA_DIR}/src/gba/gba.c
    ${MGBA_DIR}/src/gba/hle-bios.c
    ${MGBA_DIR}/src/gba/input.c
    ${MGBA_DIR}/src/gba/io.c
    ${MGBA_DIR}/src/gba/memory.c
    ${MGBA_DIR}/src/gba/overrides.c
    ${MGBA_DIR}/src/gba/renderers/cache-set.c
    ${MGBA_DIR}/src/gba/renderers/common.c
    ${MGBA_DIR}/src/gba/renderers/software-bg.c
    ${MGBA_DIR}/src/gba/renderers/software-mode0.c
    ${MGBA_DIR}/src/gba/renderers/software-obj.c
    ${MGBA_DIR}/src/gba/renderers/video-software.c
    ${MGBA_DIR}/src/gba/savedata.c
    ${MGBA_DIR}/src/gba/serialize.c
    ${MGBA_DIR}/src/gba/sharkport.c
    ${MGBA_DIR}/src/gba/sio.c
    ${MGBA_DIR}/src/gba/sio/gbp.c
    ${MGBA_DIR}/src/gba/timer.c
    ${MGBA_DIR}/src/gba/video.c

    # --- GB/GBC ---
    ${MGBA_DIR}/src/gb/audio.c
    ${MGBA_DIR}/src/gb/cheats.c
    ${MGBA_DIR}/src/gb/core.c
    ${MGBA_DIR}/src/gb/gb.c
    ${MGBA_DIR}/src/gb/input.c
    ${MGBA_DIR}/src/gb/io.c
    ${MGBA_DIR}/src/gb/mbc.c
    ${MGBA_DIR}/src/gb/mbc/huc-3.c
    ${MGBA_DIR}/src/gb/mbc/licensed.c
    ${MGBA_DIR}/src/gb/mbc/mbc.c
    ${MGBA_DIR}/src/gb/mbc/pocket-cam.c
    ${MGBA_DIR}/src/gb/mbc/tama5.c
    ${MGBA_DIR}/src/gb/mbc/unlicensed.c
    ${MGBA_DIR}/src/gb/memory.c
    ${MGBA_DIR}/src/gb/overrides.c
    ${MGBA_DIR}/src/gb/serialize.c
    ${MGBA_DIR}/src/gb/renderers/cache-set.c
    ${MGBA_DIR}/src/gb/renderers/software.c
    ${MGBA_DIR}/src/gb/sio.c
    ${MGBA_DIR}/src/gb/timer.c
    ${MGBA_DIR}/src/gb/video.c

    # --- Util ---
    ${MGBA_DIR}/src/util/circle-buffer.c
    ${MGBA_DIR}/src/util/configuration.c
    ${MGBA_DIR}/src/util/crc32.c
    ${MGBA_DIR}/src/util/formatting.c
    ${MGBA_DIR}/src/util/gbk-table.c
    ${MGBA_DIR}/src/util/hash.c
    ${MGBA_DIR}/src/util/md5.c
    ${MGBA_DIR}/src/util/sha1.c
    ${MGBA_DIR}/src/util/string.c
    ${MGBA_DIR}/src/util/table.c
    ${MGBA_DIR}/src/util/vector.c
    ${MGBA_DIR}/src/util/vfs.c
    ${MGBA_DIR}/src/util/audio-buffer.c
    ${MGBA_DIR}/src/util/audio-resampler.c
    ${MGBA_DIR}/src/util/geometry.c
    ${MGBA_DIR}/src/util/patch.c
    ${MGBA_DIR}/src/util/patch-fast.c
    ${MGBA_DIR}/src/util/patch-ips.c
    ${MGBA_DIR}/src/util/patch-ups.c
    ${MGBA_DIR}/src/util/ring-fifo.c
    # VFS backends: fd for file access, mem/fifo for in-memory operations
    ${MGBA_DIR}/src/util/vfs/vfs-fd.c
    ${MGBA_DIR}/src/util/vfs/vfs-mem.c
    ${MGBA_DIR}/src/util/vfs/vfs-fifo.c

    # --- Third party (inih) ---
    ${MGBA_DIR}/src/third-party/inih/ini.c

    # --- Generated version info ---
    ${MGBA_DIR}/version.c

    # --- QRD stubs: minimal implementations that avoid pulling in libpng ---
    ${CMAKE_CURRENT_SOURCE_DIR}/mgba_color_stub.c
)

add_library(mgba_libretro STATIC ${MGBA_SOURCES})

set_target_properties(mgba_libretro PROPERTIES
    C_STANDARD 11
)

target_include_directories(mgba_libretro PRIVATE
    ${MGBA_DIR}/include
    ${MGBA_DIR}/src
    ${MGBA_DIR}/src/platform/libretro
    ${MGBA_DIR}/src/third-party/inih
    ${CMAKE_CURRENT_SOURCE_DIR}   # for mgba_layer_capture.h
)

target_compile_definitions(mgba_libretro PRIVATE
    __LIBRETRO__
    COLOR_16_BIT
    COLOR_5_6_5
    DISABLE_THREADING
    MGBA_STANDALONE
    MINIMAL_CORE=2
    ENABLE_VFS
    ENABLE_VFS_FD
    M_CORE_GBA
    M_CORE_GB
    MGBA_QRD_LAYER_CAPTURE
    # Android NDK already provides locale_t via xlocale.h; suppress mGBA's stub typedef
    HAVE_LOCALE=1
    # Rename retro_* symbols to avoid collision with snes9x/picodrive
    retro_set_environment=mgba_retro_set_environment
    retro_set_video_refresh=mgba_retro_set_video_refresh
    retro_set_audio_sample=mgba_retro_set_audio_sample
    retro_set_audio_sample_batch=mgba_retro_set_audio_sample_batch
    retro_set_input_poll=mgba_retro_set_input_poll
    retro_set_input_state=mgba_retro_set_input_state
    retro_api_version=mgba_retro_api_version
    retro_set_controller_port_device=mgba_retro_set_controller_port_device
    retro_get_system_info=mgba_retro_get_system_info
    retro_get_system_av_info=mgba_retro_get_system_av_info
    retro_serialize_size=mgba_retro_serialize_size
    retro_serialize=mgba_retro_serialize
    retro_unserialize=mgba_retro_unserialize
    retro_cheat_reset=mgba_retro_cheat_reset
    retro_cheat_set=mgba_retro_cheat_set
    retro_load_game=mgba_retro_load_game
    retro_load_game_special=mgba_retro_load_game_special
    retro_unload_game=mgba_retro_unload_game
    retro_get_region=mgba_retro_get_region
    retro_get_memory_data=mgba_retro_get_memory_data
    retro_get_memory_size=mgba_retro_get_memory_size
    retro_reset=mgba_retro_reset
    retro_run=mgba_retro_run
    retro_init=mgba_retro_init
    retro_deinit=mgba_retro_deinit
    option_defs_us=mgba_option_defs_us
)

target_compile_options(mgba_libretro PRIVATE
    -fno-strict-aliasing
    -fvisibility=hidden
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
    ${CMAKE_CURRENT_SOURCE_DIR}   # for picodrive_sms_layer_capture.h
)

target_compile_definitions(picodrive_libretro PRIVATE
    __LIBRETRO__
    USE_LIBRETRO_VFS=1
    PICODRIVE_QRD_SMS_LAYER_CAPTURE
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

# ================================================================
# FCEUmm — NES libretro core
# ================================================================
set(FCEUMM_DIR "${QUESTRETRODEPTH_ROOT}/third_party/fceumm")
set(FCEUMM_LIBRETRO_COMMON_DIR "${FCEUMM_DIR}/src/drivers/libretro/libretro-common")

file(GLOB FCEUMM_BOARD_SOURCES "${FCEUMM_DIR}/src/boards/*.c")

set(FCEUMM_SOURCES
    # libretro interface
    ${FCEUMM_DIR}/src/drivers/libretro/libretro.c
    ${FCEUMM_DIR}/src/drivers/libretro/libretro_dipswitch.c
    # core
    ${FCEUMM_DIR}/src/cart.c
    ${FCEUMM_DIR}/src/cheat.c
    ${FCEUMM_DIR}/src/crc32.c
    ${FCEUMM_DIR}/src/debug.c
    ${FCEUMM_DIR}/src/fceu-endian.c
    ${FCEUMM_DIR}/src/fceu-memory.c
    ${FCEUMM_DIR}/src/fceu.c
    ${FCEUMM_DIR}/src/fds.c
    ${FCEUMM_DIR}/src/fds_apu.c
    ${FCEUMM_DIR}/src/file.c
    ${FCEUMM_DIR}/src/filter.c
    ${FCEUMM_DIR}/src/general.c
    ${FCEUMM_DIR}/src/ines.c
    ${FCEUMM_DIR}/src/input.c
    ${FCEUMM_DIR}/src/md5.c
    ${FCEUMM_DIR}/src/nsf.c
    ${FCEUMM_DIR}/src/palette.c
    ${FCEUMM_DIR}/src/ppu.c
    ${FCEUMM_DIR}/src/sound.c
    ${FCEUMM_DIR}/src/state.c
    ${FCEUMM_DIR}/src/unif.c
    ${FCEUMM_DIR}/src/video.c
    ${FCEUMM_DIR}/src/vsuni.c
    ${FCEUMM_DIR}/src/x6502.c
    # input devices
    ${FCEUMM_DIR}/src/input/arkanoid.c
    ${FCEUMM_DIR}/src/input/bbox.c
    ${FCEUMM_DIR}/src/input/bworld.c
    ${FCEUMM_DIR}/src/input/cursor.c
    ${FCEUMM_DIR}/src/input/fkb.c
    ${FCEUMM_DIR}/src/input/ftrainer.c
    ${FCEUMM_DIR}/src/input/hypershot.c
    ${FCEUMM_DIR}/src/input/mahjong.c
    ${FCEUMM_DIR}/src/input/mouse.c
    ${FCEUMM_DIR}/src/input/oekakids.c
    ${FCEUMM_DIR}/src/input/pec586kb.c
    ${FCEUMM_DIR}/src/input/powerpad.c
    ${FCEUMM_DIR}/src/input/quiz.c
    ${FCEUMM_DIR}/src/input/shadow.c
    ${FCEUMM_DIR}/src/input/suborkb.c
    ${FCEUMM_DIR}/src/input/toprider.c
    ${FCEUMM_DIR}/src/input/zapper.c
    # mapper boards (100+ files via glob)
    ${FCEUMM_BOARD_SOURCES}
    # libretro-common
    ${FCEUMM_LIBRETRO_COMMON_DIR}/compat/compat_posix_string.c
    ${FCEUMM_LIBRETRO_COMMON_DIR}/compat/compat_snprintf.c
    ${FCEUMM_LIBRETRO_COMMON_DIR}/compat/compat_strcasestr.c
    ${FCEUMM_LIBRETRO_COMMON_DIR}/compat/compat_strl.c
    ${FCEUMM_LIBRETRO_COMMON_DIR}/compat/fopen_utf8.c
    ${FCEUMM_LIBRETRO_COMMON_DIR}/encodings/encoding_utf.c
    ${FCEUMM_LIBRETRO_COMMON_DIR}/file/file_path.c
    ${FCEUMM_LIBRETRO_COMMON_DIR}/file/file_path_io.c
    ${FCEUMM_LIBRETRO_COMMON_DIR}/streams/file_stream.c
    ${FCEUMM_LIBRETRO_COMMON_DIR}/streams/file_stream_transforms.c
    ${FCEUMM_LIBRETRO_COMMON_DIR}/streams/memory_stream.c
    ${FCEUMM_LIBRETRO_COMMON_DIR}/string/stdstring.c
    ${FCEUMM_LIBRETRO_COMMON_DIR}/time/rtime.c
    ${FCEUMM_LIBRETRO_COMMON_DIR}/vfs/vfs_implementation.c
)

add_library(fceumm_libretro STATIC ${FCEUMM_SOURCES})

set_target_properties(fceumm_libretro PROPERTIES C_STANDARD 11)

target_include_directories(fceumm_libretro PRIVATE
    ${FCEUMM_DIR}/src
    ${FCEUMM_DIR}/src/drivers/libretro
    ${FCEUMM_LIBRETRO_COMMON_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}   # for fceux_layer_capture.h
)

target_compile_definitions(fceumm_libretro PRIVATE
    __LIBRETRO__
    FCEUMM_QRD_LAYER_CAPTURE
    FCEU_VERSION_NUMERIC=9813
    PATH_MAX=1024
    INLINE=inline
    HAVE_ASPRINTF
    # Rename globals that conflict with other cores in the same .so
    # Options arrays (collide with picodrive's libretro_core_options.h)
    option_cats_us=fceux_option_cats_us
    option_defs_us=fceux_option_defs_us
    options_us=fceux_options_us
    options_intl=fceux_options_intl
    option_cats_tr=fceux_option_cats_tr
    option_defs_tr=fceux_option_defs_tr
    options_tr=fceux_options_tr
    # PPU register array (collides with snes9x PPU object)
    PPU=fceux_PPU
    # OPLL YM2413 functions (collide with picodrive's emu2413)
    OPLL_new=fceux_OPLL_new
    OPLL_delete=fceux_OPLL_delete
    OPLL_reset=fceux_OPLL_reset
    OPLL_set_rate=fceux_OPLL_set_rate
    OPLL_set_quality=fceux_OPLL_set_quality
    OPLL_writeIO=fceux_OPLL_writeIO
    OPLL_writeReg=fceux_OPLL_writeReg
    OPLL_forceRefresh=fceux_OPLL_forceRefresh
    OPLL_setMask=fceux_OPLL_setMask
    OPLL_toggleMask=fceux_OPLL_toggleMask
    OPLL_calc=fceux_OPLL_calc
    OPLL_fillbuf=fceux_OPLL_fillbuf
    retro_set_environment=fceux_retro_set_environment
    retro_set_video_refresh=fceux_retro_set_video_refresh
    retro_set_audio_sample=fceux_retro_set_audio_sample
    retro_set_audio_sample_batch=fceux_retro_set_audio_sample_batch
    retro_set_input_poll=fceux_retro_set_input_poll
    retro_set_input_state=fceux_retro_set_input_state
    retro_api_version=fceux_retro_api_version
    retro_set_controller_port_device=fceux_retro_set_controller_port_device
    retro_get_system_info=fceux_retro_get_system_info
    retro_get_system_av_info=fceux_retro_get_system_av_info
    retro_serialize_size=fceux_retro_serialize_size
    retro_serialize=fceux_retro_serialize
    retro_unserialize=fceux_retro_unserialize
    retro_cheat_reset=fceux_retro_cheat_reset
    retro_cheat_set=fceux_retro_cheat_set
    retro_load_game=fceux_retro_load_game
    retro_load_game_special=fceux_retro_load_game_special
    retro_unload_game=fceux_retro_unload_game
    retro_get_region=fceux_retro_get_region
    retro_get_memory_data=fceux_retro_get_memory_data
    retro_get_memory_size=fceux_retro_get_memory_size
    retro_reset=fceux_retro_reset
    retro_run=fceux_retro_run
    retro_init=fceux_retro_init
    retro_deinit=fceux_retro_deinit
)

target_compile_options(fceumm_libretro PRIVATE -fno-strict-aliasing)

# ================================================================
# beetle-pce-fast — PC Engine / TurboGrafx-16 libretro core
# ================================================================
set(PCE_DIR "${QUESTRETRODEPTH_ROOT}/third_party/beetle-pce")
set(PCE_MEDNAFEN_DIR "${PCE_DIR}/mednafen")
set(PCE_LIBRETRO_COMMON_DIR "${PCE_DIR}/libretro-common")

file(GLOB PCE_TREMOR_SOURCES "${PCE_MEDNAFEN_DIR}/tremor/*.c")
list(FILTER PCE_TREMOR_SOURCES EXCLUDE REGEX "ivorbisfile_example\\.c$")

set(PCE_SOURCES
    # libretro interface
    ${PCE_DIR}/libretro.cpp
    # pce_fast core
    ${PCE_MEDNAFEN_DIR}/pce_fast/pcecd.cpp
    ${PCE_MEDNAFEN_DIR}/pce_fast/pcecd_drive.cpp
    ${PCE_MEDNAFEN_DIR}/pce_fast/psg.cpp
    ${PCE_MEDNAFEN_DIR}/pce_fast/huc6280.c
    ${PCE_MEDNAFEN_DIR}/pce_fast/input.c
    ${PCE_MEDNAFEN_DIR}/pce_fast/vdc.c
    # hardware misc
    ${PCE_MEDNAFEN_DIR}/hw_misc/arcade_card/arcade_card.cpp
    # CD-ROM subsystem
    ${PCE_MEDNAFEN_DIR}/cdrom/CDAccess.cpp
    ${PCE_MEDNAFEN_DIR}/cdrom/CDAccess_Image.cpp
    ${PCE_MEDNAFEN_DIR}/cdrom/CDAccess_CCD.cpp
    ${PCE_MEDNAFEN_DIR}/cdrom/CDAFReader.cpp
    ${PCE_MEDNAFEN_DIR}/cdrom/CDAFReader_Vorbis.cpp
    ${PCE_MEDNAFEN_DIR}/cdrom/cdromif.cpp
    ${PCE_MEDNAFEN_DIR}/cdrom/CDUtility.cpp
    ${PCE_MEDNAFEN_DIR}/cdrom/lec.cpp
    ${PCE_MEDNAFEN_DIR}/cdrom/galois.cpp
    ${PCE_MEDNAFEN_DIR}/cdrom/recover-raw.cpp
    ${PCE_MEDNAFEN_DIR}/cdrom/l-ec.cpp
    ${PCE_MEDNAFEN_DIR}/cdrom/edc_crc32.cpp
    # mednafen support
    ${PCE_MEDNAFEN_DIR}/general.cpp
    ${PCE_MEDNAFEN_DIR}/FileStream.cpp
    ${PCE_MEDNAFEN_DIR}/MemoryStream.cpp
    ${PCE_MEDNAFEN_DIR}/Stream.cpp
    ${PCE_MEDNAFEN_DIR}/mempatcher.cpp
    ${PCE_MEDNAFEN_DIR}/okiadpcm.cpp
    ${PCE_MEDNAFEN_DIR}/file.c
    ${PCE_MEDNAFEN_DIR}/settings.c
    ${PCE_MEDNAFEN_DIR}/state.c
    ${PCE_MEDNAFEN_DIR}/mednafen-endian.c
    # Blip_Buffer (sound resampler)
    ${PCE_MEDNAFEN_DIR}/sound/Blip_Buffer.c
    # Tremor — Ogg Vorbis decoder for CD audio tracks
    ${PCE_TREMOR_SOURCES}
    # libretro-common
    ${PCE_LIBRETRO_COMMON_DIR}/streams/file_stream.c
    ${PCE_LIBRETRO_COMMON_DIR}/streams/file_stream_transforms.c
    ${PCE_LIBRETRO_COMMON_DIR}/file/file_path.c
    ${PCE_LIBRETRO_COMMON_DIR}/file/retro_dirent.c
    ${PCE_LIBRETRO_COMMON_DIR}/lists/string_list.c
    ${PCE_LIBRETRO_COMMON_DIR}/lists/dir_list.c
    ${PCE_LIBRETRO_COMMON_DIR}/compat/compat_strl.c
    ${PCE_LIBRETRO_COMMON_DIR}/compat/compat_snprintf.c
    ${PCE_LIBRETRO_COMMON_DIR}/compat/compat_posix_string.c
    ${PCE_LIBRETRO_COMMON_DIR}/compat/compat_strcasestr.c
    ${PCE_LIBRETRO_COMMON_DIR}/compat/fopen_utf8.c
    ${PCE_LIBRETRO_COMMON_DIR}/encodings/encoding_utf.c
    ${PCE_LIBRETRO_COMMON_DIR}/encodings/encoding_crc32.c
    ${PCE_LIBRETRO_COMMON_DIR}/memmap/memalign.c
    ${PCE_LIBRETRO_COMMON_DIR}/string/stdstring.c
    ${PCE_LIBRETRO_COMMON_DIR}/time/rtime.c
    ${PCE_LIBRETRO_COMMON_DIR}/vfs/vfs_implementation.c
)

add_library(beetle_pce_libretro STATIC ${PCE_SOURCES})

set_target_properties(beetle_pce_libretro PROPERTIES
    C_STANDARD 11
    CXX_STANDARD 11
)

target_include_directories(beetle_pce_libretro PRIVATE
    ${PCE_DIR}
    ${PCE_MEDNAFEN_DIR}
    ${PCE_MEDNAFEN_DIR}/include
    ${PCE_MEDNAFEN_DIR}/hw_sound
    ${PCE_MEDNAFEN_DIR}/hw_cpu
    ${PCE_MEDNAFEN_DIR}/hw_misc
    ${PCE_LIBRETRO_COMMON_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}   # for pce_layer_capture.h
)

target_compile_definitions(beetle_pce_libretro PRIVATE
    __LIBRETRO__
    PCE_QRD_LAYER_CAPTURE
    NEED_CD
    NEED_TREMOR
    FRONTEND_SUPPORTS_RGB565
    MEDNAFEN_VERSION_NUMERIC=931
    STDC_HEADERS
    __STDC_LIMIT_MACROS
    _LOW_ACCURACY_
    INLINE=inline
    # Rename mednafen internal classes/functions that conflict with snes9x / fceumm
    # Stream class conflicts with snes9x's stream.cpp (both are mednafen-derived)
    Stream=PCEStream
    # FlipByteOrder conflicts with fceumm's fceu-endian.c
    FlipByteOrder=pce_FlipByteOrder
    # Rename log_cb to avoid conflict with snes9x's non-static log_cb
    log_cb=pce_log_cb
    # Rename options globals (libretro_core_options.h) — conflict with picodrive/snes9x
    option_cats_us=pce_option_cats_us
    option_defs_us=pce_option_defs_us
    options_us=pce_options_us
    options_intl=pce_options_intl
    # Rename all per-language option arrays (libretro_core_options_intl.h)
    # to avoid conflicts with fceumm's identical non-static global arrays
    option_cats_ar=pce_option_cats_ar
    option_defs_ar=pce_option_defs_ar
    options_ar=pce_options_ar
    option_cats_ast=pce_option_cats_ast
    option_defs_ast=pce_option_defs_ast
    options_ast=pce_options_ast
    option_cats_be=pce_option_cats_be
    option_defs_be=pce_option_defs_be
    options_be=pce_options_be
    option_cats_bg=pce_option_cats_bg
    option_defs_bg=pce_option_defs_bg
    options_bg=pce_options_bg
    option_cats_ca=pce_option_cats_ca
    option_defs_ca=pce_option_defs_ca
    options_ca=pce_options_ca
    option_cats_chs=pce_option_cats_chs
    option_defs_chs=pce_option_defs_chs
    options_chs=pce_options_chs
    option_cats_cht=pce_option_cats_cht
    option_defs_cht=pce_option_defs_cht
    options_cht=pce_options_cht
    option_cats_cs=pce_option_cats_cs
    option_defs_cs=pce_option_defs_cs
    options_cs=pce_options_cs
    option_cats_cy=pce_option_cats_cy
    option_defs_cy=pce_option_defs_cy
    options_cy=pce_options_cy
    option_cats_da=pce_option_cats_da
    option_defs_da=pce_option_defs_da
    options_da=pce_options_da
    option_cats_de=pce_option_cats_de
    option_defs_de=pce_option_defs_de
    options_de=pce_options_de
    option_cats_el=pce_option_cats_el
    option_defs_el=pce_option_defs_el
    options_el=pce_options_el
    option_cats_en=pce_option_cats_en
    option_defs_en=pce_option_defs_en
    options_en=pce_options_en
    option_cats_eo=pce_option_cats_eo
    option_defs_eo=pce_option_defs_eo
    options_eo=pce_options_eo
    option_cats_es=pce_option_cats_es
    option_defs_es=pce_option_defs_es
    options_es=pce_options_es
    option_cats_fa=pce_option_cats_fa
    option_defs_fa=pce_option_defs_fa
    options_fa=pce_options_fa
    option_cats_fi=pce_option_cats_fi
    option_defs_fi=pce_option_defs_fi
    options_fi=pce_options_fi
    option_cats_fr=pce_option_cats_fr
    option_defs_fr=pce_option_defs_fr
    options_fr=pce_options_fr
    option_cats_ga=pce_option_cats_ga
    option_defs_ga=pce_option_defs_ga
    options_ga=pce_options_ga
    option_cats_gl=pce_option_cats_gl
    option_defs_gl=pce_option_defs_gl
    options_gl=pce_options_gl
    option_cats_he=pce_option_cats_he
    option_defs_he=pce_option_defs_he
    options_he=pce_options_he
    option_cats_hr=pce_option_cats_hr
    option_defs_hr=pce_option_defs_hr
    options_hr=pce_options_hr
    option_cats_hu=pce_option_cats_hu
    option_defs_hu=pce_option_defs_hu
    options_hu=pce_options_hu
    option_cats_id=pce_option_cats_id
    option_defs_id=pce_option_defs_id
    options_id=pce_options_id
    option_cats_it=pce_option_cats_it
    option_defs_it=pce_option_defs_it
    options_it=pce_options_it
    option_cats_ja=pce_option_cats_ja
    option_defs_ja=pce_option_defs_ja
    options_ja=pce_options_ja
    option_cats_ko=pce_option_cats_ko
    option_defs_ko=pce_option_defs_ko
    options_ko=pce_options_ko
    option_cats_nl=pce_option_cats_nl
    option_defs_nl=pce_option_defs_nl
    options_nl=pce_options_nl
    option_cats_no=pce_option_cats_no
    option_defs_no=pce_option_defs_no
    options_no=pce_options_no
    option_cats_or=pce_option_cats_or
    option_defs_or=pce_option_defs_or
    options_or=pce_options_or
    option_cats_pl=pce_option_cats_pl
    option_defs_pl=pce_option_defs_pl
    options_pl=pce_options_pl
    option_cats_pt_br=pce_option_cats_pt_br
    option_defs_pt_br=pce_option_defs_pt_br
    options_pt_br=pce_options_pt_br
    option_cats_pt_pt=pce_option_cats_pt_pt
    option_defs_pt_pt=pce_option_defs_pt_pt
    options_pt_pt=pce_options_pt_pt
    option_cats_ru=pce_option_cats_ru
    option_defs_ru=pce_option_defs_ru
    options_ru=pce_options_ru
    option_cats_sk=pce_option_cats_sk
    option_defs_sk=pce_option_defs_sk
    options_sk=pce_options_sk
    option_cats_sr=pce_option_cats_sr
    option_defs_sr=pce_option_defs_sr
    options_sr=pce_options_sr
    option_cats_sv=pce_option_cats_sv
    option_defs_sv=pce_option_defs_sv
    options_sv=pce_options_sv
    option_cats_th=pce_option_cats_th
    option_defs_th=pce_option_defs_th
    options_th=pce_options_th
    option_cats_tr=pce_option_cats_tr
    option_defs_tr=pce_option_defs_tr
    options_tr=pce_options_tr
    option_cats_tt=pce_option_cats_tt
    option_defs_tt=pce_option_defs_tt
    options_tt=pce_options_tt
    option_cats_uk=pce_option_cats_uk
    option_defs_uk=pce_option_defs_uk
    options_uk=pce_options_uk
    option_cats_val=pce_option_cats_val
    option_defs_val=pce_option_defs_val
    options_val=pce_options_val
    option_cats_vn=pce_option_cats_vn
    option_defs_vn=pce_option_defs_vn
    options_vn=pce_options_vn
    # libretro API — rename all public entry points to avoid conflicts with other cores
    retro_set_environment=pce_retro_set_environment
    retro_set_video_refresh=pce_retro_set_video_refresh
    retro_set_audio_sample=pce_retro_set_audio_sample
    retro_set_audio_sample_batch=pce_retro_set_audio_sample_batch
    retro_set_input_poll=pce_retro_set_input_poll
    retro_set_input_state=pce_retro_set_input_state
    retro_api_version=pce_retro_api_version
    retro_set_controller_port_device=pce_retro_set_controller_port_device
    retro_get_system_info=pce_retro_get_system_info
    retro_get_system_av_info=pce_retro_get_system_av_info
    retro_serialize_size=pce_retro_serialize_size
    retro_serialize=pce_retro_serialize
    retro_unserialize=pce_retro_unserialize
    retro_cheat_reset=pce_retro_cheat_reset
    retro_cheat_set=pce_retro_cheat_set
    retro_load_game=pce_retro_load_game
    retro_load_game_special=pce_retro_load_game_special
    retro_unload_game=pce_retro_unload_game
    retro_get_region=pce_retro_get_region
    retro_get_memory_data=pce_retro_get_memory_data
    retro_get_memory_size=pce_retro_get_memory_size
    retro_reset=pce_retro_reset
    retro_run=pce_retro_run
    retro_init=pce_retro_init
    retro_deinit=pce_retro_deinit
)

target_compile_options(beetle_pce_libretro PRIVATE -fno-strict-aliasing)

add_library(questretrodepth_native SHARED
    emulator_backend.cpp
    experimental_rumble.cpp
    fceux_backend.cpp
    fceux_layer_capture.cpp
    game_config.cpp
    layer_processor.cpp
    panel_layout.cpp
    gles_renderer.cpp
    openxr_shell.cpp
    picodrive_backend.cpp
    picodrive_sms_layer_capture.cpp
    mgba_backend.cpp
    mgba_layer_capture.cpp
    pce_backend.cpp
    pce_layer_capture.cpp
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
find_library(android-lib android)
find_library(z-lib z)

target_include_directories(questretrodepth_native PRIVATE
    ${PICODRIVE_DIR}
    ${PICODRIVE_DIR}/platform/libretro
    ${PICODRIVE_LIBRETRO_COMMON_DIR}/include
    ${SNES9X_DIR}
    ${SNES9X_DIR}/libretro
    ${SNES9X_LIBRETRO_COMMON_DIR}/include
    ${MGBA_DIR}/include
    ${MGBA_DIR}/src
    ${MGBA_DIR}/src/platform/libretro
    ${FCEUMM_DIR}/src
    ${FCEUMM_DIR}/src/drivers/libretro
    ${FCEUMM_LIBRETRO_COMMON_DIR}/include
    ${PCE_DIR}
    ${PCE_MEDNAFEN_DIR}
    ${PCE_MEDNAFEN_DIR}/include
    ${PCE_LIBRETRO_COMMON_DIR}/include
)

target_link_libraries(questretrodepth_native
    snes9x_libretro
    picodrive_libretro
    mgba_libretro
    fceumm_libretro
    beetle_pce_libretro
    OpenXR::openxr_loader
    OpenXR::headers
    ${log-lib}
    ${math-lib}
    ${egl-lib}
    ${gles-lib}
    ${aaudio-lib}
    ${android-lib}
    ${z-lib}
)

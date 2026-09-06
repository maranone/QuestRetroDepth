cmake_minimum_required(VERSION 3.22)
project(mupen64plus-core C)

set(M64CORE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(M64MINI_SRC "${CMAKE_CURRENT_SOURCE_DIR}/subprojects/minizip")

# Explicit source list — mirrors the unix Makefile shared-library target.
# SDL-dependent files (eventloop, screenshot, sdl_key_converter, osd) are
# replaced by android_stubs.c; lirc/netplay are stubbed by -D guards or stubs.
# x86/arm32 dynarec excluded; arm64 new_dynarec included but NO_ASM disables JIT.
set(MUPEN64PLUS_CORE_SOURCES
    # stubs replacing SDL-heavy subsystems
    "${CMAKE_CURRENT_SOURCE_DIR}/android_stubs.c"

    # minizip
    "${M64MINI_SRC}/ioapi.c"
    "${M64MINI_SRC}/zip.c"
    "${M64MINI_SRC}/unzip.c"

    # md5 (used by rom.h via <md5.h>)
    "${CMAKE_CURRENT_SOURCE_DIR}/subprojects/md5/md5.c"

    # public API (vidext.c excluded — uses SDL video; stubs in android_stubs.c)
    "${M64CORE_SRC}/api/callbacks.c"
    "${M64CORE_SRC}/api/common.c"
    "${M64CORE_SRC}/api/config.c"
    "${M64CORE_SRC}/api/debugger.c"
    "${M64CORE_SRC}/api/frontend.c"

    # backends
    "${M64CORE_SRC}/backends/api/video_capture_backend.c"
    "${M64CORE_SRC}/backends/plugins_compat/audio_plugin_compat.c"
    "${M64CORE_SRC}/backends/plugins_compat/input_plugin_compat.c"
    "${M64CORE_SRC}/backends/clock_ctime_plus_delta.c"
    "${M64CORE_SRC}/backends/dummy_video_capture.c"
    "${M64CORE_SRC}/backends/file_storage.c"

    # debugger
    "${M64CORE_SRC}/debugger/dbg_breakpoints.c"
    "${M64CORE_SRC}/debugger/dbg_debugger.c"
    "${M64CORE_SRC}/debugger/dbg_decoder.c"
    "${M64CORE_SRC}/debugger/dbg_memory.c"

    # cart hardware
    "${M64CORE_SRC}/device/cart/af_rtc.c"
    "${M64CORE_SRC}/device/cart/cart.c"
    "${M64CORE_SRC}/device/cart/cart_rom.c"
    "${M64CORE_SRC}/device/cart/eeprom.c"
    "${M64CORE_SRC}/device/cart/flashram.c"
    "${M64CORE_SRC}/device/cart/is_viewer.c"
    "${M64CORE_SRC}/device/cart/sram.c"

    # controllers
    "${M64CORE_SRC}/device/controllers/game_controller.c"
    "${M64CORE_SRC}/device/controllers/vru_controller.c"
    "${M64CORE_SRC}/device/controllers/paks/biopak.c"
    "${M64CORE_SRC}/device/controllers/paks/mempak.c"
    "${M64CORE_SRC}/device/controllers/paks/rumblepak.c"
    "${M64CORE_SRC}/device/controllers/paks/transferpak.c"

    # disk drive
    "${M64CORE_SRC}/device/dd/dd_controller.c"
    "${M64CORE_SRC}/device/dd/disk.c"

    # device top-level
    "${M64CORE_SRC}/device/device.c"

    # GB cart
    "${M64CORE_SRC}/device/gb/gb_cart.c"
    "${M64CORE_SRC}/device/gb/m64282fp.c"
    "${M64CORE_SRC}/device/gb/mbc3_rtc.c"

    # memory
    "${M64CORE_SRC}/device/memory/memory.c"

    # PIF / boot ROM
    "${M64CORE_SRC}/device/pif/bootrom_hle.c"
    "${M64CORE_SRC}/device/pif/cic.c"
    "${M64CORE_SRC}/device/pif/n64_cic_nus_6105.c"
    "${M64CORE_SRC}/device/pif/pif.c"

    # R4300 CPU (pure interpreter; NO_ASM disables JIT)
    # new_dynarec.c and recomp_dbg.c excluded: require arch-specific headers
    # (arm64/assem_arm64.h etc.) and are only needed for the JIT recompiler path.
    "${M64CORE_SRC}/device/r4300/cached_interp.c"
    "${M64CORE_SRC}/device/r4300/cp0.c"
    "${M64CORE_SRC}/device/r4300/cp1.c"
    "${M64CORE_SRC}/device/r4300/cp2.c"
    "${M64CORE_SRC}/device/r4300/idec.c"
    "${M64CORE_SRC}/device/r4300/instr_counters.c"
    "${M64CORE_SRC}/device/r4300/interrupt.c"
    "${M64CORE_SRC}/device/r4300/pure_interp.c"
    "${M64CORE_SRC}/device/r4300/r4300_core.c"
    # recomp.c excluded: only needed when DYNAREC is defined; references hundreds
    # of gen_* JIT emitter functions from arch-specific backends we don't compile.
    "${M64CORE_SRC}/device/r4300/tlb.c"

    # RCP subsystems
    "${M64CORE_SRC}/device/rcp/ai/ai_controller.c"
    "${M64CORE_SRC}/device/rcp/mi/mi_controller.c"
    "${M64CORE_SRC}/device/rcp/pi/pi_controller.c"
    "${M64CORE_SRC}/device/rcp/rdp/fb.c"
    "${M64CORE_SRC}/device/rcp/rdp/rdp_core.c"
    "${M64CORE_SRC}/device/rcp/ri/ri_controller.c"
    "${M64CORE_SRC}/device/rcp/rsp/rsp_core.c"
    "${M64CORE_SRC}/device/rcp/si/si_controller.c"
    "${M64CORE_SRC}/device/rcp/vi/vi_controller.c"

    # RDRAM
    "${M64CORE_SRC}/device/rdram/rdram.c"

    # main — includes SDL (satisfied by android_sdl_shim)
    # workqueue.c excluded: M64P_PARALLEL not defined, header provides inline stubs
    "${M64CORE_SRC}/main/main.c"
    "${M64CORE_SRC}/main/cheat.c"
    "${M64CORE_SRC}/main/rom.c"
    "${M64CORE_SRC}/main/savestates.c"
    "${M64CORE_SRC}/main/util.c"

    # plugin interface
    "${M64CORE_SRC}/plugin/plugin.c"
    "${M64CORE_SRC}/plugin/dummy_audio.c"
    "${M64CORE_SRC}/plugin/dummy_input.c"
    "${M64CORE_SRC}/plugin/dummy_rsp.c"
    "${M64CORE_SRC}/plugin/dummy_video.c"

    # OSAL — unix variants only
    "${M64CORE_SRC}/osal/dynamiclib_unix.c"
    "${M64CORE_SRC}/osal/files_unix.c"
)

add_library(mupen64plus-core SHARED ${MUPEN64PLUS_CORE_SOURCES})

target_include_directories(mupen64plus-core PRIVATE
    # SDL shim must come BEFORE any system include paths so our SDL.h wins
    "${CMAKE_CURRENT_SOURCE_DIR}/android_sdl_shim"
    "${M64CORE_SRC}"
    # Both the direct path (for <unzip.h>) and parent (for <minizip/unzip.h>)
    "${M64MINI_SRC}"
    "${CMAKE_CURRENT_SOURCE_DIR}/subprojects"
    "${CMAKE_CURRENT_SOURCE_DIR}/subprojects/md5"
    "${CMAKE_CURRENT_SOURCE_DIR}/subprojects/xxhash"
)

target_compile_definitions(mupen64plus-core PRIVATE
    MUPEN64PLUS_CORE_AS_DLL   # export API symbols
    NO_ASM                    # disable JIT dynarec (pure interpreter)
    __LINUX__                 # OSAL platform guard
    HAVE_POSIX_MEMALIGN       # stdlib posix_memalign available
    ANDROID                   # Android-specific guards
    # Disable optional subsystems that pull in SDL_opengl / SDL_thread heavily
    # OSD stubs are static-inline in osd.h when M64P_OSD is absent
    # Netplay stubs are static-inline in netplay.h when M64P_NETPLAY is absent
)

target_link_libraries(mupen64plus-core PRIVATE
    log   # __android_log_print
    dl    # dlopen used by osal/dynamiclib_unix.c
    m     # math
    z     # zlib — used by minizip and savestates
)

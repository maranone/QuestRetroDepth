/* QuestRetroDepth: stub for the vendored lr-yabasanshiro PlayRecorder
 * feature (input record/playback for automated testing).
 *
 * PlayRecorder.cpp itself needs <experimental/filesystem>, unavailable in
 * the NDK's libc++, and this app never exposes a record/playback UI, so we
 * skip building it and just satisfy its two call sites in yabause.c with
 * no-ops.
 */
#include "peripheral.h"

void PlayRecorder_proc(u32 /*framecount*/) {}

void PlayRecorder_setPlayMode(const char* /*dir*/, yabauseinit_struct* /*init*/) {}

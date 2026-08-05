/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - dummy_rsp.c                                             *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2009 Richard Goedeken                                   *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <stdlib.h>
#include <android/log.h>

#include "api/m64p_types.h"
#include "dummy_rsp.h"
#include "plugin.h"

#define QRD_RSP_LOG(...) __android_log_print(ANDROID_LOG_INFO, "N64RSP", __VA_ARGS__)

static RSP_INFO g_rsp_info;
static int g_rsp_info_ready = 0;
static unsigned int g_rsp_cycle_calls = 0;

m64p_error dummyrsp_PluginGetVersion(m64p_plugin_type *PluginType, int *PluginVersion,
                                     int *APIVersion, const char **PluginNamePtr, int *Capabilities)
{
    if (PluginType != NULL)
        *PluginType = M64PLUGIN_RSP;

    if (PluginVersion != NULL)
        *PluginVersion = 0x00010000;

    if (APIVersion != NULL)
        *APIVersion = RSP_API_VERSION;

    if (PluginNamePtr != NULL)
        *PluginNamePtr = "Mupen64Plus-NoRSP";

    if (Capabilities != NULL)
        *Capabilities = 0;

    return M64ERR_SUCCESS;
}

unsigned int dummyrsp_DoRspCycles(unsigned int Cycles)
{
    unsigned int task_type;

    ++g_rsp_cycle_calls;
    if (!g_rsp_info_ready || g_rsp_info.DMEM == NULL) {
        if (g_rsp_cycle_calls <= 12 || (g_rsp_cycle_calls % 500) == 0)
            QRD_RSP_LOG("DoRspCycles call=%u but RSP_INFO not ready", g_rsp_cycle_calls);
        return Cycles;
    }

    task_type = *((unsigned int *) (g_rsp_info.DMEM + 0xfc0));
    if (g_rsp_cycle_calls <= 12 || (g_rsp_cycle_calls % 500) == 0) {
        QRD_RSP_LOG("DoRspCycles call=%u task_type=%u dlist=%p alist=%p rdp=%p",
                    g_rsp_cycle_calls, task_type,
                    (void *) g_rsp_info.ProcessDlistList,
                    (void *) g_rsp_info.ProcessAlistList,
                    (void *) g_rsp_info.ProcessRdpList);
    }

    if (task_type == 1 && g_rsp_info.ProcessDlistList != NULL)
        g_rsp_info.ProcessDlistList();
    else if (task_type == 2 && g_rsp_info.ProcessAlistList != NULL)
        g_rsp_info.ProcessAlistList();
    else if (g_rsp_info.ProcessRdpList != NULL)
        g_rsp_info.ProcessRdpList();

    return Cycles;
}

void dummyrsp_InitiateRSP(RSP_INFO Rsp_Info, unsigned int * CycleCount)
{
    (void) CycleCount;
    g_rsp_info = Rsp_Info;
    g_rsp_info_ready = 1;
    g_rsp_cycle_calls = 0;
    QRD_RSP_LOG("InitiateRSP DMEM=%p dlist=%p alist=%p rdp=%p",
                (void *) g_rsp_info.DMEM,
                (void *) g_rsp_info.ProcessDlistList,
                (void *) g_rsp_info.ProcessAlistList,
                (void *) g_rsp_info.ProcessRdpList);
}

void dummyrsp_RomClosed(void)
{
    g_rsp_info_ready = 0;
    QRD_RSP_LOG("RomClosed");
}


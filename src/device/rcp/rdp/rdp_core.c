/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - rdp_core.c                                              *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2014 Bobby Smiles                                       *
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

#include "rdp_core.h"

#include <string.h>

#include "device/memory/memory.h"
#include "device/rcp/mi/mi_controller.h"
#include "device/rcp/rsp/rsp_core.h"
#include "device/r4300/r4300_core.h"
#include "device/r4300/interrupt.h"
#include "plugin/plugin.h"

static void update_dpc_status(struct rdp_core* dp, uint32_t w)
{
    /* clear / set xbus_dmem_dma */
    if (w & DPC_CLR_XBUS_DMEM_DMA) dp->dpc_regs[DPC_STATUS_REG] &= ~DPC_STATUS_XBUS_DMEM_DMA;
    if (w & DPC_SET_XBUS_DMEM_DMA) dp->dpc_regs[DPC_STATUS_REG] |= DPC_STATUS_XBUS_DMEM_DMA;

    /* clear / set freeze */
    if (w & DPC_CLR_FREEZE)
    {
        dp->dpc_regs[DPC_STATUS_REG] &= ~DPC_STATUS_FREEZE;

        if (dp->do_on_unfreeze & DELAY_DP_INT) {
            if (gfx.fullSyncDelivered) gfx.fullSyncDelivered();
            signal_rcp_interrupt(dp->mi, MI_INTR_DP);
        }
        if (dp->do_on_unfreeze & DELAY_UPDATESCREEN)
            gfx.updateScreen();
        dp->do_on_unfreeze = 0;
    }
    if (w & DPC_SET_FREEZE) dp->dpc_regs[DPC_STATUS_REG] |= DPC_STATUS_FREEZE;

    /* clear / set flush */
    if (w & DPC_CLR_FLUSH) dp->dpc_regs[DPC_STATUS_REG] &= ~DPC_STATUS_FLUSH;
    if (w & DPC_SET_FLUSH) dp->dpc_regs[DPC_STATUS_REG] |= DPC_STATUS_FLUSH;

    /* clear clock counter */
    if (w & DPC_CLR_CLOCK_CTR) dp->dpc_regs[DPC_CLOCK_REG] = 0;
}


void init_rdp(struct rdp_core* dp,
              struct rsp_core* sp,
              struct mi_controller* mi,
              struct memory* mem,
              struct rdram* rdram,
              struct r4300_core* r4300)
{
    dp->sp = sp;
    dp->mi = mi;

    init_fb(&dp->fb, mem, rdram, r4300);
}

void poweron_rdp(struct rdp_core* dp)
{
    memset(dp->dpc_regs, 0, DPC_REGS_COUNT*sizeof(uint32_t));
    memset(dp->dps_regs, 0, DPS_REGS_COUNT*sizeof(uint32_t));
    dp->dpc_regs[DPC_STATUS_REG] |= DPC_STATUS_START_GCLK;

    dp->do_on_unfreeze = 0;
    dp->pending = 0;

    poweron_fb(&dp->fb);
}


void read_dpc_regs(void* opaque, uint32_t address, uint32_t* value)
{
    struct rdp_core* dp = (struct rdp_core*)opaque;
    uint32_t reg = dpc_reg(address);

    *value = dp->dpc_regs[reg];
}

void write_dpc_regs(void* opaque, uint32_t address, uint32_t value, uint32_t mask)
{
    struct rdp_core* dp = (struct rdp_core*)opaque;
    uint32_t reg = dpc_reg(address);

    switch(reg)
    {
    case DPC_STATUS_REG:
        update_dpc_status(dp, value & mask);
    case DPC_CURRENT_REG:
    case DPC_CLOCK_REG:
    case DPC_BUFBUSY_REG:
    case DPC_PIPEBUSY_REG:
    case DPC_TMEM_REG:
        return;
    }

    masked_write(&dp->dpc_regs[reg], value, mask);

    switch(reg)
    {
    case DPC_START_REG:
        dp->dpc_regs[DPC_CURRENT_REG] = dp->dpc_regs[DPC_START_REG];
        break;
    case DPC_END_REG:
        unprotect_framebuffers(&dp->fb);
        gfx.processRDPList();
        protect_framebuffers(&dp->fb);
        rdp_full_sync_discard(dp);
        signal_rcp_interrupt(dp->mi, MI_INTR_DP);
        break;
    }
}


void read_dps_regs(void* opaque, uint32_t address, uint32_t* value)
{
    struct rdp_core* dp = (struct rdp_core*)opaque;
    uint32_t reg = dps_reg(address);

    if (reg < DPS_REGS_COUNT)
    {
        *value = dp->dps_regs[reg];
    }
}

void write_dps_regs(void* opaque, uint32_t address, uint32_t value, uint32_t mask)
{
    struct rdp_core* dp = (struct rdp_core*)opaque;
    uint32_t reg = dps_reg(address);

    if (reg < DPS_REGS_COUNT)
    {
        masked_write(&dp->dps_regs[reg], value, mask);
    }
}

void rdp_interrupt_event(void* opaque)
{
    struct rdp_core* dp = (struct rdp_core*)opaque;

    if (gfx.fullSyncDelivered) gfx.fullSyncDelivered();     /* the frame must be drawn when the game learns it is */
    raise_rcp_interrupt(dp->mi, MI_INTR_DP);
    if (dp->pending != 0) {
        uint32_t d = dp->pending_delay[0];
        memmove(dp->pending_delay, dp->pending_delay + 1, --dp->pending * sizeof(dp->pending_delay[0]));
        cp0_update_count(dp->mi->r4300);
        add_interrupt_event(&dp->mi->r4300->cp0, DP_INT, d);
    }
}

void set_rdp_full_sync_delay(struct rdp_core* dp, uint32_t cycles)
{
    dp->full_sync_delay = cycles;
}

void set_rdp_clock_scale(struct rdp_core* dp, uint32_t permille)
{
    dp->clock_scale = permille;
}

/* the delay of the DP interrupt of the full sync(s) that just completed: the plugin's estimate of the RDP's time, or the fixed delay; at most 4 VIs */
static uint32_t full_sync_delay_now(struct rdp_core* dp)
{
    if (dp->clock_scale && gfx.rdpSyncClocks) {
        uint64_t d = (uint64_t)gfx.rdpSyncClocks() * 3 * dp->clock_scale / 4000;
        return d < 4000 ? 4000 : d > 3200000 ? 3200000 : (uint32_t)d;
    }
    return dp->full_sync_delay ? dp->full_sync_delay : 4000;
}

void rdp_full_sync_done(struct rdp_core* dp)
{
    struct cp0* cp0 = &dp->mi->r4300->cp0;
    uint32_t d = full_sync_delay_now(dp);
    if (get_event(&cp0->q, DP_INT)) {
        if (dp->pending < sizeof(dp->pending_delay) / sizeof(dp->pending_delay[0]))
            dp->pending_delay[dp->pending++] = d;
        else
            dp->pending_delay[dp->pending - 1] += d;
        return;
    }
    cp0_update_count(dp->mi->r4300);
    add_interrupt_event(cp0, DP_INT, d);
}

void rdp_full_sync_discard(struct rdp_core* dp)
{
    (void)dp;
    if (gfx.rdpSyncClocks)
        gfx.rdpSyncClocks();
}


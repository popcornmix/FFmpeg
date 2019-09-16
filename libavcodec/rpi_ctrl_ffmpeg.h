/*
 * FFMPEG HEVC decoder hardware accelerator
 * Copyright (C) 2019 Raspberry Pi Ltd
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef __CTRL_FFMPEG_H__
#define __CTRL_FFMPEG_H__

#include <stdint.h>

const char *rpi_ctrl_ffmpeg_init  (const char *hwaccel_device, void **id);
void      rpi_apb_write_addr    (void *id, uint16_t addr, uint32_t data);
void      rpi_apb_write         (void *id, uint16_t addr, uint32_t data);
uint32_t  rpi_apb_read          (void *id, uint16_t addr);
void      rpi_apb_read_drop     (void *id, uint16_t addr);
void      rpi_axi_write         (void *id, uint64_t addr, uint32_t size, void *buf);
void      rpi_axi_read          (void *id, uint64_t addr, uint32_t size, void *buf);
void      rpi_axi_read_alloc    (void *id, uint32_t size);
void      rpi_axi_read_tx       (void *id, uint64_t addr, uint32_t size);
void      rpi_axi_read_rx       (void *id, uint32_t size, void *buf);
void      rpi_wait_interrupt    (void *id, int phase);
void      rpi_ctrl_ffmpeg_free  (void *id);
uint64_t  rpi_axi_get_addr      (void *id);
void rpi_apb_dump_regs(void *id, uint16_t addr, int num);
void rpi_axi_dump(void *id, uint64_t addr, uint32_t size);
void rpi_axi_flush(void *id, int mode);

#endif // __CTRL_FILES_H__

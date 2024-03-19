/*
 * SPDX-License-Identifier:     GPL-2.0+
 *
 * Copyright (c) 2023 Namtso Technology Co., Ltd
 */

#ifndef __CONFIGS_NAMTSO_A10_RK3588_H
#define __CONFIGS_NAMTSO_A10_RK3588_H

#include <configs/rk3588_common.h>

#ifndef CONFIG_SPL_BUILD

#undef ROCKCHIP_DEVICE_SETTINGS
#define ROCKCHIP_DEVICE_SETTINGS \
		"logo_addr_c=0x06A00000\0" \
		"logo_addr_r=0x07000000\0" \
		"stdout=serial,vidconsole\0" \
		"stderr=serial,vidconsole\0"

#define CONFIG_SYS_MMC_ENV_DEV		0

#undef CONFIG_BOOTCOMMAND
#define CONFIG_BOOTCOMMAND RKIMG_BOOTCOMMAND

#endif
#endif
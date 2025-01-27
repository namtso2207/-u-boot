/*
 * (C) Copyright 2008-2017 Fuzhou Rockchip Electronics Co., Ltd
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <drm/drm_mipi_dsi.h>

#include <config.h>
#include <common.h>
#include <errno.h>
#include <malloc.h>
#include <video.h>
#include <backlight.h>
#include <spi.h>
#include <asm/gpio.h>
#include <dm/device.h>
#include <dm/read.h>
#include <dm/uclass.h>
#include <dm/uclass-id.h>
#include <linux/media-bus-format.h>
#include <power/regulator.h>

#include "rockchip_display.h"
#include "rockchip_crtc.h"
#include "rockchip_connector.h"
#include "rockchip_panel.h"

int is_mipi_lcd_exit = 0x0;
int vpx_id = 0;

struct rockchip_cmd_header {
	u8 data_type;
	u8 delay_ms;
	u8 payload_length;
} __packed;

struct rockchip_cmd_desc {
	struct rockchip_cmd_header header;
	const u8 *payload;
};

struct rockchip_panel_cmds {
	struct rockchip_cmd_desc *cmds;
	int cmd_cnt;
};

struct rockchip_panel_plat {
	bool power_invert;
	u32 bus_format;
	unsigned int bpc;

	struct {
		unsigned int prepare;
		unsigned int unprepare;
		unsigned int enable;
		unsigned int disable;
		//unsigned int reset;
		unsigned int init;
	} delay;

	struct rockchip_panel_cmds *on_cmds;
	struct rockchip_panel_cmds *off_cmds;
};

struct rockchip_panel_priv {
	bool prepared;
	bool enabled;
	struct udevice *power_supply;
	struct udevice *backlight;
	struct spi_slave *spi_slave;
	struct gpio_desc enable_gpio;
	//struct gpio_desc reset_gpio;

	int cmd_type;
	struct gpio_desc spi_sdi_gpio;
	struct gpio_desc spi_scl_gpio;
	struct gpio_desc spi_cs_gpio;
};

static inline int get_panel_cmd_type(const char *s)
{
	if (!s)
		return -EINVAL;

	if (strncmp(s, "spi", 3) == 0)
		return CMD_TYPE_SPI;
	else if (strncmp(s, "mcu", 3) == 0)
		return CMD_TYPE_MCU;

	return CMD_TYPE_DEFAULT;
}

static int rockchip_panel_parse_cmds(const u8 *data, int length,
				     struct rockchip_panel_cmds *pcmds)
{
	int len;
	const u8 *buf;
	const struct rockchip_cmd_header *header;
	int i, cnt = 0;

	/* scan commands */
	cnt = 0;
	buf = data;
	len = length;
	while (len > sizeof(*header)) {
		header = (const struct rockchip_cmd_header *)buf;
		buf += sizeof(*header) + header->payload_length;
		len -= sizeof(*header) + header->payload_length;
		cnt++;
	}

	pcmds->cmds = calloc(cnt, sizeof(struct rockchip_cmd_desc));
	if (!pcmds->cmds)
		return -ENOMEM;

	pcmds->cmd_cnt = cnt;

	buf = data;
	len = length;
	for (i = 0; i < cnt; i++) {
		struct rockchip_cmd_desc *desc = &pcmds->cmds[i];

		header = (const struct rockchip_cmd_header *)buf;
		length -= sizeof(*header);
		buf += sizeof(*header);
		desc->header.data_type = header->data_type;
		desc->header.delay_ms = header->delay_ms;
		desc->header.payload_length = header->payload_length;
		desc->payload = buf;
		buf += header->payload_length;
		length -= header->payload_length;
	}

	return 0;
}

static void rockchip_panel_write_spi_cmds(struct rockchip_panel_priv *priv,
					  u8 type, int value)
{
	int i;

	dm_gpio_set_value(&priv->spi_cs_gpio, 0);

	if (type == 0)
		value &= (~(1 << 8));
	else
		value |= (1 << 8);

	for (i = 0; i < 9; i++) {
		if (value & 0x100)
			dm_gpio_set_value(&priv->spi_sdi_gpio, 1);
		else
			dm_gpio_set_value(&priv->spi_sdi_gpio, 0);

		dm_gpio_set_value(&priv->spi_scl_gpio, 0);
		udelay(10);
		dm_gpio_set_value(&priv->spi_scl_gpio, 1);
		value <<= 1;
		udelay(10);
	}

	dm_gpio_set_value(&priv->spi_cs_gpio, 1);
}

static int rockchip_panel_send_mcu_cmds(struct rockchip_panel *panel, struct display_state *state,
					struct rockchip_panel_cmds *cmds)
{
	int i;

	if (!cmds)
		return -EINVAL;

	display_send_mcu_cmd(state, MCU_SETBYPASS, 1);
	for (i = 0; i < cmds->cmd_cnt; i++) {
		struct rockchip_cmd_desc *desc = &cmds->cmds[i];
		int value = 0;

		value = desc->payload[0];
		display_send_mcu_cmd(state, desc->header.data_type, value);

		if (desc->header.delay_ms)
			mdelay(desc->header.delay_ms);
	}
	display_send_mcu_cmd(state, MCU_SETBYPASS, 0);

	return 0;
}

static int rockchip_panel_send_spi_cmds(struct rockchip_panel *panel, struct display_state *state,
					struct rockchip_panel_cmds *cmds)
{
	struct rockchip_panel_priv *priv = dev_get_priv(panel->dev);
	int i;
	int ret;

	if (!cmds)
		return -EINVAL;

	if (priv->spi_slave) {
		ret = spi_claim_bus(priv->spi_slave);
		if (ret) {
			printf("%s: Failed to claim spi bus: %d\n", __func__, ret);
			return -EINVAL;
		}
	}

	for (i = 0; i < cmds->cmd_cnt; i++) {
		struct rockchip_cmd_desc *desc = &cmds->cmds[i];
		int value = 0;
		u16 mask = 0;
		u16 data = 0;

		if (priv->spi_slave) {
			mask = desc->header.data_type ? 0x100 : 0;
			data = (mask | desc->payload[0]) << 7;;
			data = ((data & 0xff) << 8) | (data >> 8);
			value = mask | desc->payload[0];
			ret = spi_xfer(priv->spi_slave, 9, &data, NULL, SPI_XFER_ONCE);
			if (ret)
				printf("%s: Failed to xfer spi cmd 0x%x: %d\n",
				       __func__, desc->payload[0], ret);
		} else {
			if (desc->header.payload_length == 2)
				value = (desc->payload[0] << 8) | desc->payload[1];
			else
				value = desc->payload[0];
			rockchip_panel_write_spi_cmds(priv, desc->header.data_type, value);
		}

		if (desc->header.delay_ms)
			mdelay(desc->header.delay_ms);
	}

	if (priv->spi_slave)
		spi_release_bus(priv->spi_slave);

	return 0;
}

static int rockchip_panel_send_dsi_cmds(struct mipi_dsi_device *dsi,
					struct rockchip_panel_cmds *cmds)
{
	int i, ret;
	struct drm_dsc_picture_parameter_set *pps = NULL;

	if (!cmds)
		return -EINVAL;

	for (i = 0; i < cmds->cmd_cnt; i++) {
		struct rockchip_cmd_desc *desc = &cmds->cmds[i];
		const struct rockchip_cmd_header *header = &desc->header;

		switch (header->data_type) {
		case MIPI_DSI_COMPRESSION_MODE:
			ret = mipi_dsi_compression_mode(dsi, desc->payload[0]);
			break;
		case MIPI_DSI_GENERIC_SHORT_WRITE_0_PARAM:
		case MIPI_DSI_GENERIC_SHORT_WRITE_1_PARAM:
		case MIPI_DSI_GENERIC_SHORT_WRITE_2_PARAM:
		case MIPI_DSI_GENERIC_LONG_WRITE:
			ret = mipi_dsi_generic_write(dsi, desc->payload,
						     header->payload_length);
			break;
		case MIPI_DSI_DCS_SHORT_WRITE:
		case MIPI_DSI_DCS_SHORT_WRITE_PARAM:
		case MIPI_DSI_DCS_LONG_WRITE:
			ret = mipi_dsi_dcs_write_buffer(dsi, desc->payload,
							header->payload_length);
			break;
		case MIPI_DSI_PICTURE_PARAMETER_SET:
			pps = kzalloc(sizeof(*pps), GFP_KERNEL);
			if (!pps)
				return -ENOMEM;

			memcpy(pps, desc->payload, header->payload_length);
			ret = mipi_dsi_picture_parameter_set(dsi, pps);
			kfree(pps);
			break;
		default:
			printf("unsupport command data type: %d\n",
			       header->data_type);
			return -EINVAL;
		}

		if (ret < 0) {
			printf("failed to write cmd%d: %d\n", i, ret);
			return ret;
		}

		if (header->delay_ms)
			mdelay(header->delay_ms);
	}

	return 0;
}

extern int namtso_mipi_id;
extern int namtso_mipi_id2;
static void panel_simple_prepare(struct rockchip_panel *panel)
{
	struct rockchip_panel_plat *plat = dev_get_platdata(panel->dev);
	struct rockchip_panel_priv *priv = dev_get_priv(panel->dev);
	struct mipi_dsi_device *dsi = dev_get_parent_platdata(panel->dev);
	int ret;
	u8 mode;

	if (priv->prepared)
		return;

	if (priv->power_supply)
		regulator_set_enable(priv->power_supply, !plat->power_invert);

	if (dm_gpio_is_valid(&priv->enable_gpio))
		dm_gpio_set_value(&priv->enable_gpio, 1);

	if (plat->delay.prepare)
		mdelay(plat->delay.prepare);

	if (plat->delay.init)
		mdelay(plat->delay.init);

	//namtso_mipi_id = 4;
	if(namtso_mipi_id !=4){
		mipi_dsi_dcs_get_power_mode(dsi, &mode);
		if(0x8 == mode){
			is_mipi_lcd_exit = is_mipi_lcd_exit | (0x1 << vpx_id);
		}
		else{
			if(2 == vpx_id && 2!=namtso_mipi_id){
				is_mipi_lcd_exit = is_mipi_lcd_exit & 0xb;
				run_command("fdt set /dsi@fde20000 status disable", 0);
				run_command("fdt set /dsi@fde20000/panel@0 status disable", 0);
				run_command("fdt set /dsi@fde20000/ports/port@0/endpoint@0 status disable", 0);
				run_command("fdt set /display-subsystem/route/route-dsi0 status disable", 0);
				printf("disable dsi0\n");
			}
			else if(3 == vpx_id && 2!=namtso_mipi_id2){
				is_mipi_lcd_exit = is_mipi_lcd_exit & 0x7;
				run_command("fdt set /dsi@fde30000 status disable", 0);
				run_command("fdt set /dsi@fde30000/panel@0 status disable", 0);
				run_command("fdt set /dsi@fde30000/ports/port@0/endpoint@1 status disable", 0);
				run_command("fdt set /display-subsystem/route/route-dsi1 status disable", 0);
				printf("disable dsi1\n");
			}
			printf("(vpx_id=%x)==(is_mipi_lcd_exit=%x)=vp2 and vp3 status disable\n", vpx_id,is_mipi_lcd_exit);
		}
		printf("0x8===>mode: 0x%d is_mipi_lcd_exit=%d\n", mode,is_mipi_lcd_exit);
		   /*ret = mipi_dsi_dcs_read(dsi, 0xDA, &namtso_mipi_id, sizeof(namtso_mipi_id));
		   if (ret <= 0) {
				   printf("mipi_dsi_dcs_read ID ,error=%d!!\n", ret);
		   }
		   printf("hlm panel_simple_prepare() namtso_mipi_id=%d\n", namtso_mipi_id);*/
	}

	if (plat->on_cmds) {
		if (priv->cmd_type == CMD_TYPE_SPI)
			ret = rockchip_panel_send_spi_cmds(panel, panel->state,
							   plat->on_cmds);
		else if (priv->cmd_type == CMD_TYPE_MCU)
			ret = rockchip_panel_send_mcu_cmds(panel, panel->state,
							   plat->on_cmds);
		else
			ret = rockchip_panel_send_dsi_cmds(dsi, plat->on_cmds);
		if (ret)
			printf("failed to send on cmds: %d\n", ret);
	}
	//mipi_dsi_dcs_get_power_mode(dsi, &mode);
	//printf("0x9c===>mode: 0x%x\n", mode);
	priv->prepared = true;
}

static void panel_simple_unprepare(struct rockchip_panel *panel)
{
	struct rockchip_panel_plat *plat = dev_get_platdata(panel->dev);
	struct rockchip_panel_priv *priv = dev_get_priv(panel->dev);
	struct mipi_dsi_device *dsi = dev_get_parent_platdata(panel->dev);
	int ret;

	if (!priv->prepared)
		return;

	if (plat->off_cmds) {
		if (priv->cmd_type == CMD_TYPE_SPI)
			ret = rockchip_panel_send_spi_cmds(panel, panel->state,
							   plat->off_cmds);
		else if (priv->cmd_type == CMD_TYPE_MCU)
			ret = rockchip_panel_send_mcu_cmds(panel, panel->state,
							   plat->off_cmds);
		else
			ret = rockchip_panel_send_dsi_cmds(dsi, plat->off_cmds);
		if (ret)
			printf("failed to send off cmds: %d\n", ret);
	}

	//if (dm_gpio_is_valid(&priv->reset_gpio))
	//	dm_gpio_set_value(&priv->reset_gpio, 1);

	if (dm_gpio_is_valid(&priv->enable_gpio))
		dm_gpio_set_value(&priv->enable_gpio, 0);

	if (priv->power_supply)
		regulator_set_enable(priv->power_supply, plat->power_invert);

	if (plat->delay.unprepare)
		mdelay(plat->delay.unprepare);

	priv->prepared = false;
}

static void panel_simple_enable(struct rockchip_panel *panel)
{
	struct rockchip_panel_plat *plat = dev_get_platdata(panel->dev);
	struct rockchip_panel_priv *priv = dev_get_priv(panel->dev);

	if (priv->enabled)
		return;

	if (plat->delay.enable)
		mdelay(plat->delay.enable);

	if (priv->backlight)
		backlight_enable(priv->backlight);

	priv->enabled = true;
}

static void panel_simple_disable(struct rockchip_panel *panel)
{
	struct rockchip_panel_plat *plat = dev_get_platdata(panel->dev);
	struct rockchip_panel_priv *priv = dev_get_priv(panel->dev);

	if (!priv->enabled)
		return;

	if (priv->backlight)
		backlight_disable(priv->backlight);

	if (plat->delay.disable)
		mdelay(plat->delay.disable);

	priv->enabled = false;
}

static const struct rockchip_panel_funcs rockchip_panel_funcs = {
	.prepare = panel_simple_prepare,
	.unprepare = panel_simple_unprepare,
	.enable = panel_simple_enable,
	.disable = panel_simple_disable,
};

#ifdef CONFIG_DM_I2C
#define TP_I2C_BUS_NUM 0
#define TP2_I2C_BUS_NUM 6
#define TP05_CHIP_ADDR "0x38"
#define TP10_CHIP_ADDR "0x14"
static struct udevice *i2c_cur_bus;

static int cmd_i2c_set_bus_num(unsigned int busnum)
{
    struct udevice *bus;
    int ret;

    ret = uclass_get_device_by_seq(UCLASS_I2C, busnum, &bus);
    if (ret) {
        printf("%s: No bus %d\n", __func__, busnum);
        return ret;
    }
    i2c_cur_bus = bus;

    return 0;
}

static int i2c_get_cur_bus(struct udevice **busp, unsigned int busnum)
{
	//if (!i2c_cur_bus) {
		if (cmd_i2c_set_bus_num(busnum)) {
		    printf("Default I2C bus %d not found\n",
		           busnum);
		    return -ENODEV;
		}
	//}

    if (!i2c_cur_bus) {
        puts("No I2C bus selected\n");
        return -ENODEV;
    }
    *busp = i2c_cur_bus;

    return 0;
}

static int i2c_get_cur_bus_chip(uint chip_addr, struct udevice **devp, unsigned int busnum)
{
    struct udevice *bus;
    int ret;

    ret = i2c_get_cur_bus(&bus, busnum);
    if (ret)
        return ret;

    return i2c_get_chip(bus, chip_addr, 1, devp);
}
#endif

static int kbi_i2c_read(unsigned int busnum, uint reg, const char *cp)
{
	int ret;
	char val[64];
	uchar   linebuf[1];
	uchar chip;
#ifdef CONFIG_DM_I2C
	struct udevice *dev;
#endif


	chip = simple_strtoul(cp, NULL, 16);

#ifdef CONFIG_DM_I2C
	ret = i2c_get_cur_bus_chip(chip, &dev, busnum);
	if (!ret)
		ret = dm_i2c_read(dev, reg, (uint8_t *)linebuf, 1);
#else
	ret = i2c_read(chip, reg, 1, linebuf, 1);
#endif

	if (ret)
		printf("Error reading the chip: %d\n",ret);
	else {
		sprintf(val, "%d", linebuf[0]);
		ret = simple_strtoul(val, NULL, 10);

	}
	return ret;
}

static int rockchip_panel_ofdata_to_platdata(struct udevice *dev)
{
	struct rockchip_panel_plat *plat = dev_get_platdata(dev);
	const void *data;
	int len = 0;
	int ret;
	static bool first_flag = 0;
	int tp_id;

	plat->power_invert = dev_read_bool(dev, "power-invert");

	plat->delay.prepare = dev_read_u32_default(dev, "prepare-delay-ms", 0);
	plat->delay.unprepare = dev_read_u32_default(dev, "unprepare-delay-ms", 0);
	plat->delay.enable = dev_read_u32_default(dev, "enable-delay-ms", 0);
	plat->delay.disable = dev_read_u32_default(dev, "disable-delay-ms", 0);
	plat->delay.init = dev_read_u32_default(dev, "init-delay-ms", 0);
	//plat->delay.reset = dev_read_u32_default(dev, "reset-delay-ms", 0);

	plat->bus_format = dev_read_u32_default(dev, "bus-format",
						MEDIA_BUS_FMT_RBG888_1X24);
	plat->bpc = dev_read_u32_default(dev, "bpc", 8);

	printf("hlm first_flag=%d namtso_mipi_id=%d\n", first_flag, namtso_mipi_id);

	//namtso_mipi_id = 4;
	if(namtso_mipi_id !=4){
		if(first_flag){
			tp_id = kbi_i2c_read(6,0xA8,TP05_CHIP_ADDR);
		}
		else
			tp_id = kbi_i2c_read(0,0xA8,TP05_CHIP_ADDR);

		printf("TP05 id=0x%x\n",tp_id);
		if(tp_id == 0x51){//old TS050
			if(!first_flag)
				namtso_mipi_id = 1;
			else
				namtso_mipi_id2 = 1;
			printf("old TS050 to parse panel init sequence\n");
			data = dev_read_prop(dev, "panel-init-sequence", &len);
		}else if(tp_id == 0x79){//new TS050
			if(!first_flag)
				namtso_mipi_id = 3;
			else
				namtso_mipi_id2 = 3;
			printf("new TS050 to parse panel init sequence2\n");
			data = dev_read_prop(dev, "panel-init-sequence2", &len);
		}else{
			if(first_flag)
				tp_id = kbi_i2c_read(6,0x9e,TP10_CHIP_ADDR);
			else{
				tp_id = kbi_i2c_read(0,0x9e,TP10_CHIP_ADDR);
			}

			printf("TP10 id=0x%x\n",tp_id);
			if(tp_id == 0x00){//TS101
				if(!first_flag)
					namtso_mipi_id = 2;
				else
					namtso_mipi_id2 = 2;
			}else {
				if(!first_flag)
					namtso_mipi_id = 0;
				else
					namtso_mipi_id2 = 0;
			}
			printf("old TS050 to parse panel init sequence\n");
			data = dev_read_prop(dev, "panel-init-sequence", &len);
		}
		printf("hlm namtso_mipi_id=%d namtso_mipi_id2=%d\n",namtso_mipi_id, namtso_mipi_id2);
	}
	else{
		data = dev_read_prop(dev, "panel-init-sequence", &len);
	}
	first_flag = !first_flag;

	if (data) {
		plat->on_cmds = calloc(1, sizeof(*plat->on_cmds));
		if (!plat->on_cmds)
			return -ENOMEM;

		ret = rockchip_panel_parse_cmds(data, len, plat->on_cmds);
		if (ret) {
			printf("failed to parse panel init sequence\n");
			goto free_on_cmds;
		}
	}

	data = dev_read_prop(dev, "panel-exit-sequence", &len);
	if (data) {
		plat->off_cmds = calloc(1, sizeof(*plat->off_cmds));
		if (!plat->off_cmds) {
			ret = -ENOMEM;
			goto free_on_cmds;
		}

		ret = rockchip_panel_parse_cmds(data, len, plat->off_cmds);
		if (ret) {
			printf("failed to parse panel exit sequence\n");
			goto free_cmds;
		}
	}

	return 0;

free_cmds:
	free(plat->off_cmds);
free_on_cmds:
	free(plat->on_cmds);
	return ret;
}

static int rockchip_panel_probe(struct udevice *dev)
{
	struct rockchip_panel_priv *priv = dev_get_priv(dev);
	struct rockchip_panel_plat *plat = dev_get_platdata(dev);
	struct rockchip_panel *panel;
	int ret;
	const char *cmd_type;

	ret = gpio_request_by_name(dev, "enable-gpios", 0,
				   &priv->enable_gpio, GPIOD_IS_OUT);
	if (ret && ret != -ENOENT) {
		printf("%s: Cannot get enable GPIO: %d\n", __func__, ret);
		return ret;
	}

	//ret = gpio_request_by_name(dev, "reset-gpios", 0,
	//			   &priv->reset_gpio, GPIOD_IS_OUT);
	//if (ret && ret != -ENOENT) {
	//	printf("%s: Cannot get reset GPIO: %d\n", __func__, ret);
	//	return ret;
	//}

	ret = uclass_get_device_by_phandle(UCLASS_PANEL_BACKLIGHT, dev,
					   "backlight", &priv->backlight);
	if (ret && ret != -ENOENT) {
		printf("%s: Cannot get backlight: %d\n", __func__, ret);
		return ret;
	}

	ret = uclass_get_device_by_phandle(UCLASS_REGULATOR, dev,
					   "power-supply", &priv->power_supply);
	if (ret && ret != -ENOENT) {
		printf("%s: Cannot get power supply: %d\n", __func__, ret);
		return ret;
	}

	ret = dev_read_string_index(dev, "rockchip,cmd-type", 0, &cmd_type);
	if (ret)
		priv->cmd_type = CMD_TYPE_DEFAULT;
	else
		priv->cmd_type = get_panel_cmd_type(cmd_type);

	if (priv->cmd_type == CMD_TYPE_SPI) {
		ofnode parent = ofnode_get_parent(dev->node);

		if (ofnode_valid(parent)) {
			struct dm_spi_slave_platdata *plat = dev_get_parent_platdata(dev);
			struct udevice *spi = dev_get_parent(dev);

			if (spi->seq < 0) {
				printf("%s: Failed to get spi bus num\n", __func__);
				return -EINVAL;
			}

			priv->spi_slave = spi_setup_slave(spi->seq, plat->cs, plat->max_hz,
							  plat->mode);
			if (!priv->spi_slave) {
				printf("%s: Failed to setup spi slave: %d\n", __func__, ret);
				return -EINVAL;
			}
		} else {
			ret = gpio_request_by_name(dev, "spi-sdi-gpios", 0,
						   &priv->spi_sdi_gpio, GPIOD_IS_OUT);
			if (ret && ret != -ENOENT) {
				printf("%s: Cannot get spi sdi GPIO: %d\n", __func__, ret);
				return ret;
			}
			ret = gpio_request_by_name(dev, "spi-scl-gpios", 0,
						   &priv->spi_scl_gpio, GPIOD_IS_OUT);
			if (ret && ret != -ENOENT) {
				printf("%s: Cannot get spi scl GPIO: %d\n", __func__, ret);
				return ret;
			}
			ret = gpio_request_by_name(dev, "spi-cs-gpios", 0,
						   &priv->spi_cs_gpio, GPIOD_IS_OUT);
			if (ret && ret != -ENOENT) {
				printf("%s: Cannot get spi cs GPIO: %d\n", __func__, ret);
				return ret;
			}
			dm_gpio_set_value(&priv->spi_sdi_gpio, 1);
			dm_gpio_set_value(&priv->spi_scl_gpio, 1);
			dm_gpio_set_value(&priv->spi_cs_gpio, 1);
			//dm_gpio_set_value(&priv->reset_gpio, 0);
		}
	}

	panel = calloc(1, sizeof(*panel));
	if (!panel)
		return -ENOMEM;

	dev->driver_data = (ulong)panel;
	panel->dev = dev;
	panel->bus_format = plat->bus_format;
	panel->bpc = plat->bpc;
	panel->funcs = &rockchip_panel_funcs;

	return 0;
}

static const struct udevice_id rockchip_panel_ids[] = {
	{ .compatible = "simple-panel", },
	{ .compatible = "simple-panel-dsi", },
	{ .compatible = "simple-panel-spi", },
	{}
};

U_BOOT_DRIVER(rockchip_panel) = {
	.name = "rockchip_panel",
	.id = UCLASS_PANEL,
	.of_match = rockchip_panel_ids,
	.ofdata_to_platdata = rockchip_panel_ofdata_to_platdata,
	.probe = rockchip_panel_probe,
	.priv_auto_alloc_size = sizeof(struct rockchip_panel_priv),
	.platdata_auto_alloc_size = sizeof(struct rockchip_panel_plat),
};

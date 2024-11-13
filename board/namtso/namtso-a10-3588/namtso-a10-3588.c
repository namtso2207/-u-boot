/*
 * SPDX-License-Identifier:     GPL-2.0+
 *
 * (C) Copyright 2023 Namtso Technology Co., Ltd
 */

#include <common.h>
#include <dwc3-uboot.h>
#include <usb.h>
#include <linux/usb/phy-rockchip-usbdp.h>
#include <asm/io.h>
#include <rockusb.h>
#include <i2c.h>
#include <dm.h>
#include <dt-bindings/gpio/gpio.h>
#include <asm/gpio.h>

#define TP_I2C_BUS_NUM 		(0)
#define TP2_I2C_BUS_NUM		(6)

DECLARE_GLOBAL_DATA_PTR;

#ifdef CONFIG_USB_DWC3
#define CRU_BASE		0xfd7c0000
#define CRU_SOFTRST_CON42	0x0aa8

static struct dwc3_device dwc3_device_data = {
	.maximum_speed = USB_SPEED_SUPER,
	.base = 0xfc000000,
	.dr_mode = USB_DR_MODE_PERIPHERAL,
	.index = 0,
	.dis_u2_susphy_quirk = 1,
	.dis_u1u2_quirk = 1,
	.usb2_phyif_utmi_width = 16,
};

int usb_gadget_handle_interrupts(int index)
{
	dwc3_uboot_handle_interrupt(0);
	return 0;
}

bool rkusb_usb3_capable(void)
{
	return true;
}

static void usb_reset_otg_controller(void)
{
	writel(0x00100010, CRU_BASE + CRU_SOFTRST_CON42);
	mdelay(1);
	writel(0x00100000, CRU_BASE + CRU_SOFTRST_CON42);
	mdelay(1);
}

int board_usb_init(int index, enum usb_init_type init)
{
	u32 ret = 0;

	usb_reset_otg_controller();

#if defined(CONFIG_SUPPORT_USBPLUG)
	dwc3_device_data.maximum_speed = USB_SPEED_HIGH;

	if (rkusb_switch_usb3_enabled()) {
		dwc3_device_data.maximum_speed = USB_SPEED_SUPER;
		ret = rockchip_u3phy_uboot_init();
		if (ret) {
			rkusb_force_to_usb2(true);
			dwc3_device_data.maximum_speed = USB_SPEED_HIGH;
		}
	}
#else
	ret = rockchip_u3phy_uboot_init();
	if (ret) {
		rkusb_force_to_usb2(true);
		dwc3_device_data.maximum_speed = USB_SPEED_HIGH;
	}
#endif

	return dwc3_uboot_init(&dwc3_device_data);
}

#if defined(CONFIG_SUPPORT_USBPLUG)
int board_usb_cleanup(int index, enum usb_init_type init)
{
	dwc3_uboot_exit(index);
	return 0;
}
#endif

#endif

int rk_board_init_ethernet(void)
{
	run_command("gpio set 150", 0);		//GPIO4_C6
	return 0;
}

int board_set_lcd_enable(void)
{
	char * lcd_panel = env_get("lcd_panel");
	if (NULL != lcd_panel) {
		if (!strcmp("null", lcd_panel)) {
			printf("disable dsi0 mipi panel.\n");
			run_command("fdt set /dsi@fde20000 status disable", 0);
			run_command("fdt set /dsi@fde20000/panel@0 status disable", 0);
			run_command("fdt set /dsi@fde20000/ports/port@0/endpoint@1 status disable", 0);
			run_command("fdt set /display-subsystem/route/route-dsi0 status disable", 0);
		}
	}

	lcd_panel = env_get("lcd_sec_panel");
	if (NULL != lcd_panel) {
		if (!strcmp("null", lcd_panel)) {
			printf("disable dsi1 mipi panel.\n");
			run_command("fdt set /dsi@fde30000 status disable", 0);
			run_command("fdt set /dsi@fde30000/panel@0 status disable", 0);
			run_command("fdt set /dsi@fde30000/ports/port@0/endpoint@0 status disable", 0);
			run_command("fdt set /display-subsystem/route/route-dsi1 status disable", 0);
		}
	}

	return 0;
}

#define EDP_HPD_GPIO	(37)
int board_set_edp_lcd_enable(void)
{
	int ret = -1;
	int value = -1;

	ret = gpio_request(EDP_HPD_GPIO, "edp_hpd_gpio");
	if (ret) {
		printf("gpio_request failed\n");
	}

	gpio_direction_input(EDP_HPD_GPIO);
	value = gpio_get_value(EDP_HPD_GPIO);
	printf("edp hdp gpio get value: [%d]\n", value);

	if (!value) {
		printf("disable edp panel.\n");
		run_command("fdt set /edp@fdec0000 status disable", 0);
		run_command("fdt set /edp@fdec0000/ports/port@0/endpoint@1 status disable", 0);
		run_command("fdt set /display-subsystem/route/route-edp0 status disable", 0);
		run_command("fdt set /phy@fed60000 status disable", 0);
	}

	return 0;
}

int board_init_lcd(void)
{
	unsigned long default_fdt_addr = 0x08300000;
	char * fdt_addr = env_get("fdt_addr_r");
	unsigned long load_fdt_addr = 0;
	char cmd_buf[64] = {'\0'};

	if (strict_strtoul(fdt_addr, 16, &load_fdt_addr) < 0) {
		printf("Get fdt_addr failed, set default.\n");
		load_fdt_addr = default_fdt_addr;
	}
	snprintf(cmd_buf, sizeof(cmd_buf), "fdt addr 0x%lx", load_fdt_addr);
	run_command(cmd_buf, 0);
	board_set_lcd_enable();

	return 0;
}

int board_init_wifi(void)
{
    char * wifi_status = env_get("wifi");
    if (NULL != wifi_status) {
        if (!strncmp("on", wifi_status, sizeof("on"))) {
            run_command("gpio set 77", 0);      //GPIO2_B5  set wifi/bt
        } else {
            run_command("gpio clear 77", 0);    //GPIO2_B5  set link
        }
    } else {
        run_command("gpio clear 77", 0);        //GPIO2_B5  set link
    }
	return 0;
}

int rk_board_init(void)
{
	int ret = 0;
	int res = 0;
	struct udevice *bus;
	struct udevice *dev;
	uchar linebuf[1];

	run_command("gpio set 75", 0);		//GPIO2_B3
	run_command("gpio set 78", 0);		//GPIO2_B6
	run_command("gpio set 76", 0);		//GPIO2_B4
	run_command("gpio set 85", 0);		//GPIO2_C5
	//run_command("gpio set 140", 0);	//GPIO4_B4
	run_command("gpio clear 140", 0);
	run_command("gpio set 137", 0);		//GPIO4_B1
	run_command("gpio set 128", 0);		//GPIO4_A0
	run_command("gpio clear 146", 0);	//GPIO4_C2
	run_command("gpio set 111", 0);		//GPIO3_B7  edp panel power

	run_command("nbi get_pcie_wol", 0);
	char * pcie_wol_en = env_get("pcie_wol_status");
	printf("get pcie eth wol status:%s\n", pcie_wol_en);
	if (NULL != pcie_wol_en) {
		if (!strncmp("1", pcie_wol_en, 1)) {
			pci_init();
		}
	}

	ret = uclass_get_device_by_seq(UCLASS_I2C, TP_I2C_BUS_NUM, &bus);
	if (ret) {
		printf("%s: No bus %d\n", __func__, TP_I2C_BUS_NUM);
		return 0;
	}
	ret = i2c_get_chip(bus, 0x38, 1, &dev);
	if (!ret) {
		res = dm_i2c_read(dev, 0xA8, linebuf, 1);
		if (!res) {
			printf("bus:0x%x TP05 id=0x%x\n", TP_I2C_BUS_NUM, linebuf[0]);
			if (linebuf[0] == 0x51){//old ts050
				env_set("lcd_panel","ts050");
			} else if (linebuf[0] == 0x79) {//new ts050
				env_set("lcd_panel","newts050");
			}
		}
	}
	if (ret || res) {
		ret = i2c_get_chip(bus, 0x14, 1, &dev);
		if (!ret) {
			res = dm_i2c_read(dev, 0x9e, linebuf, 1);
			if (!res) {
				printf("bus:0x%x TP10 id=0x%x\n", TP_I2C_BUS_NUM, linebuf[0]);
				if (linebuf[0] == 0x00) {//TS101
					env_set("lcd_panel","ts101");
				}
			} else {
				env_set("lcd_panel","null");
			}
		}
	}

	ret = uclass_get_device_by_seq(UCLASS_I2C, TP2_I2C_BUS_NUM, &bus);
	if (ret) {
		printf("%s: No bus %d\n", __func__, TP2_I2C_BUS_NUM);
		return 0;
	}
	ret = i2c_get_chip(bus, 0x38, 1, &dev);
	if (!ret) {
		res = dm_i2c_read(dev, 0xA8, linebuf, 1);
		if (!res) {
			printf("bus:0x%x TP05 id=0x%x\n", TP2_I2C_BUS_NUM, linebuf[0]);
			if (linebuf[0] == 0x51){//old ts050
				env_set("lcd_sec_panel","ts050");
			} else if (linebuf[0] == 0x79) {//new ts050
				env_set("lcd_sec_panel","newts050");
			}
		}
	}
	if (ret || res) {
		ret = i2c_get_chip(bus, 0x14, 1, &dev);
		if (!ret) {
			res = dm_i2c_read(dev, 0x9e, linebuf, 1);
			if (!res) {
				printf("bus:0x%x TP10 id=0x%x\n", TP2_I2C_BUS_NUM, linebuf[0]);
				if (linebuf[0] == 0x00) {//TS101
					env_set("lcd_sec_panel","ts101");
				}
			} else {
				env_set("lcd_sec_panel","null");
			}
		}
	}

	board_init_lcd();
	board_init_wifi();

	return 0;

}


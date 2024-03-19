#include <common.h>
#include <bootretry.h>
#include <cli.h>
#include <command.h>
#include <dm.h>
#include <edid.h>
#include <environment.h>
#include <errno.h>
#include <i2c.h>
#include <adc.h>
#include <malloc.h>
#include <stdlib.h>
#include <asm/byteorder.h>
#include <linux/compiler.h>
#include <asm/u-boot.h>
#include "asm/arch-rockchip/vendor.h"

#define CHIP_ADDR              0x18
#define CHIP_ADDR_CHAR         "0x18"
#define I2C_SPEED              100000
#define MCU_I2C_BUS_NUM        1

#define REG_MAC					0x0
#define REG_USID				0x12
#define REG_VERSION				0x19
#define REG_BOOT_MODE			0x20
#define REG_BOOT_EN_WOL			0x21
#define REG_BOOT_EN_RTC			0x22
#define REG_BOOT_EN_DCIN		0x23
#define REG_BOOT_EN_LPWR		0x24
#define REG_BOOT_EN_UPWR		0x25

#define REG_LED_ON_SYS			0x28
#define REG_LED_OFF_SYS			0x29
#define REG_LED_USER			0x2A
#define REG_MCU_SLEEP_EN		0x2B
#define REG_MAC_SWITCH			0x2C

#define REG_REST_CONF			0x80
#define REG_SYS_RST				0x81
#define REG_FAN_LEVEL			0x82
#define REG_FAN_TEST			0x83
#define REG_EN_WDT				0x84
#define REG_INIT_WOL			0x85
#define REG_SYS_OOPS			0x86
//#define REG_BOOT_FLAG			0x87


#define BOOT_EN_WOL				0
#define BOOT_EN_RTC				1
#define BOOT_EN_DCIN			2
#define BOOT_EN_LPWR			3
#define BOOT_EN_UPWR			4
#define BOOT_EN_MCU_SLEEP		5
#define BOOT_EN_WDT				6


#define LED_OFF_MODE			0
#define LED_ON_MODE				1
#define LED_BREATHE_MODE		2
#define LED_HEARTBEAT_MODE		3

#define LED_SYSTEM_OFF			0
#define LED_SYSTEM_ON			1

#define BOOT_MODE_SPI			0
#define BOOT_MODE_EMMC			1
#define BOOT_MODE_SD			2

#define FORCERESET_WOL			0
#define FORCERESET_GPIO			1

#define VERSION_LENGHT			2
#define USID_LENGHT				6
#define MAC_LENGHT				6
#define ADC_LENGHT				2
#define PASSWD_CUSTOM_LENGHT	6
#define PASSWD_VENDOR_LENGHT	6

static char* LED_MODE_STR[] = { "off", "on", "breathe", "heartbeat"};
		
#define VENDOR_LAN_MAC_ID  0
int vendor_storage_init(void);

#ifdef CONFIG_DM_I2C
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

static int i2c_get_cur_bus(struct udevice **busp)
{
	if (!i2c_cur_bus) {
		if (cmd_i2c_set_bus_num(MCU_I2C_BUS_NUM)) {
			printf("Default I2C bus %d not found\n",
					MCU_I2C_BUS_NUM);
			return -ENODEV;
		}
	}

	if (!i2c_cur_bus) {
		puts("No I2C bus selected\n");
		return -ENODEV;
	}
	*busp = i2c_cur_bus;

    return 0;
}

static int i2c_get_cur_bus_chip(uint chip_addr, struct udevice **devp)
{
    struct udevice *bus;
    int ret;

    ret = i2c_get_cur_bus(&bus);
    if (ret)
        return ret;

    return i2c_get_chip(bus, chip_addr, 1, devp);
}
#endif

static int nbi_i2c_read(uint reg)
{
	int ret;
	char val[64];
	uchar   linebuf[1];
	uchar chip;
#ifdef CONFIG_DM_I2C
	struct udevice *dev;
#endif


	chip = simple_strtoul(CHIP_ADDR_CHAR, NULL, 16);

#ifdef CONFIG_DM_I2C
	ret = i2c_get_cur_bus_chip(chip, &dev);
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

static void  nbi_i2c_read_block(uint start_reg, int count, char val[])
{
	uint addr;
	int nbytes;
	int ret;
	uchar chip;
#ifdef CONFIG_DM_I2C
	struct udevice *dev;
#endif

	chip = simple_strtoul(CHIP_ADDR_CHAR, NULL, 16);

	nbytes = count;
	addr = start_reg;
	do {
		unsigned char   linebuf[1];
#ifdef CONFIG_DM_I2C
		ret = i2c_get_cur_bus_chip(chip, &dev);
		if (!ret)
			ret = dm_i2c_read(dev, addr, (uint8_t *)linebuf, 1);
#else
		ret = i2c_read(chip, addr, 1, linebuf, 1);
#endif
		if (ret)
			printf("Error reading the chip: %d\n",ret);
		else
			val[count-nbytes] =  linebuf[0];

		addr++;
		nbytes--;

	} while (nbytes > 0);
}

static int get_wol(bool is_print)
{
	int enable;
	enable = nbi_i2c_read(REG_BOOT_EN_WOL);
	if (is_print) {
		printf("boot wol: %s\n", enable&0x01 ? "enable":"disable");
	}
	env_set("wol_enable", enable&0x01 ?"1" : "0");
	return enable;
}

static void set_wol(bool is_shutdown, int enable)
{
	char cmd[64];
	int mode;

	if ((enable&0x01) != 0) {
		char mac_addr[MAC_LENGHT] = {0};
		if (is_shutdown)
			run_command("mdio write ethernet@fe1c0000 0 0", 0);
		else
			run_command("mdio write ethernet@fe1c0000 0 0x1040", 0);

		run_command("mdio write ethernet@fe1c0000 0x1f 0xd40", 0);
		run_command("mdio write ethernet@fe1c0000 0x16 0x20", 0);
		run_command("mdio write ethernet@fe1c0000 0x1f 0", 0);

		mode = nbi_i2c_read(REG_MAC_SWITCH);
		if (mode == 1) {
			nbi_i2c_read_block(REG_MAC, MAC_LENGHT, mac_addr);
		} else {
//			run_command("efuse mac", 0);
//			char *s = getenv("eth_mac");
//			if ((s != NULL) && (strcmp(s, "00:00:00:00:00:00") != 0)) {
//				printf("getmac = %s\n", s);
//				int i = 0;
//				for (i = 0; i < 6 && s[0] != '\0' && s[1] != '\0'; i++) {
//				mac_addr[i] = chartonum(s[0]) << 4 | chartonum(s[1]);
//				s +=3;
//				}
//		} else {
//			nbi_i2c_read_block(REG_MAC, MAC_LENGHT, mac_addr);
//		}

			int ret;
			ret = vendor_storage_init();
			if (ret) {
				printf("nbi: vendor_storage_init failed %d\n", ret);
				return;
			}

			ret = vendor_storage_read(VENDOR_LAN_MAC_ID, mac_addr, MAC_LENGHT);
			if (MAC_LENGHT == ret && !is_zero_ethaddr((const u8 *)mac_addr)) {
				debug("read mac from vendor successfully!\n");
			} else {
				nbi_i2c_read_block(REG_MAC, MAC_LENGHT, mac_addr);
			}
		}
		run_command("mdio write ethernet@fe1c0000 0x1f 0xd8c", 0);
		sprintf(cmd, "mdio write ethernet@fe1c0000 0x10 0x%x%x", mac_addr[1], mac_addr[0]);
		run_command(cmd, 0);
		sprintf(cmd, "mdio write ethernet@fe1c0000 0x11 0x%x%x", mac_addr[3], mac_addr[2]);
		run_command(cmd, 0);
		sprintf(cmd, "mdio write ethernet@fe1c0000 0x12 0x%x%x", mac_addr[5], mac_addr[4]);
		run_command(cmd, 0);
		run_command("mdio write ethernet@fe1c0000 0x1f 0", 0);

		run_command("mdio write ethernet@fe1c0000 0x1f 0xd8a", 0);
		run_command("mdio write ethernet@fe1c0000 0x11 0x9fff", 0);
		run_command("mdio write ethernet@fe1c0000 0x1f 0", 0);

		run_command("mdio write ethernet@fe1c0000 0x1f 0xd8a", 0);
		run_command("mdio write ethernet@fe1c0000 0x10 0x1000", 0);
		run_command("mdio write ethernet@fe1c0000 0x1f 0", 0);

		run_command("mdio write ethernet@fe1c0000 0x1f 0xd80", 0);
		run_command("mdio write ethernet@fe1c0000 0x10 0x3000", 0);
		run_command("mdio write ethernet@fe1c0000 0x11 0x0020", 0);
		run_command("mdio write ethernet@fe1c0000 0x12 0x03c0", 0);
		run_command("mdio write ethernet@fe1c0000 0x13 0x0000", 0);
		run_command("mdio write ethernet@fe1c0000 0x14 0x0000", 0);
		run_command("mdio write ethernet@fe1c0000 0x15 0x0000", 0);
		run_command("mdio write ethernet@fe1c0000 0x16  0x0000", 0);
		run_command("mdio write ethernet@fe1c0000 0x17 0x0000", 0);
		run_command("mdio write ethernet@fe1c0000 0x1f 0", 0);

		run_command("mdio write ethernet@fe1c0000 0x1f 0xd8a", 0);
		run_command("mdio write ethernet@fe1c0000 0x13 0x1002", 0);
		run_command("mdio write ethernet@fe1c0000 0x1f 0", 0);

	} else {
		run_command("mdio write ethernet@fe1c0000 0x1f 0xd8a", 0);
		run_command("mdio write ethernet@fe1c0000 0x10 0", 0);
		run_command("mdio write ethernet@fe1c0000 0x11 0x7fff", 0);
		run_command("mdio write ethernet@fe1c0000 0x1f 0", 0);
	}

	run_command("i2c dev 1", 0);
	sprintf(cmd, "i2c mw %x %x %d 1", CHIP_ADDR, REG_BOOT_EN_WOL, enable);
	run_command(cmd, 0);
	printf("%s: %d\n", __func__, enable);
}

static void get_rtc(void)
{
	int enable;
	enable = nbi_i2c_read(REG_BOOT_EN_RTC);
	printf("boot rtc: %s\n", enable==1 ? "enable" : "disable" );
}

static void set_rtc(int enable)
{
	char cmd[64];
	sprintf(cmd, "i2c mw %x %x %d 1",CHIP_ADDR, REG_BOOT_EN_RTC, enable);
	run_command(cmd, 0);
}

static void get_dcin(void)
{
	int enable;
	enable = nbi_i2c_read(REG_BOOT_EN_DCIN);
	printf("boot dcin: %s\n", enable==1 ? "enable" : "disable" );
}

static void set_dcin(int enable)
{
	char cmd[64];
	sprintf(cmd, "i2c mw %x %x %d 1",CHIP_ADDR, REG_BOOT_EN_DCIN, enable);
	run_command(cmd, 0);
}

static void get_lpwr(void)
{
	int enable;
	enable = nbi_i2c_read(REG_BOOT_EN_LPWR);
	printf("en lpwr: %s\n", enable==1 ? "enable" : "disable" );
}

static void set_lpwr(int enable)
{
	char cmd[64];
	sprintf(cmd, "i2c mw %x %x %d 1",CHIP_ADDR, REG_BOOT_EN_LPWR, enable);
	run_command(cmd, 0);
}

static void get_upwr(void)
{
	int enable;
	enable = nbi_i2c_read(REG_BOOT_EN_UPWR);
	printf("en upwr: %s\n", enable==1 ? "enable" : "disable" );
}

static void set_upwr(int enable)
{
	char cmd[64];
	sprintf(cmd, "i2c mw %x %x %d 1",CHIP_ADDR, REG_BOOT_EN_UPWR, enable);
	run_command(cmd, 0);
}

static void get_switch_mac(void)
{
	int mode;
	mode = nbi_i2c_read(REG_MAC_SWITCH);
	printf("switch mac from %d\n", mode);
	env_set("switch_mac", mode==1 ? "1" : "0");
}

static void set_switch_mac(int mode)
{
	char cmd[64];
	sprintf(cmd, "i2c mw %x %x %d 1",CHIP_ADDR, REG_MAC_SWITCH, mode);
	printf("set_switch_mac :%d\n", mode);
	run_command(cmd, 0);
	env_set("switch_mac", mode==1 ? "1" : "0");
}

static void get_mcu_sleep_enable(void)
{
	int enable;
	enable = nbi_i2c_read(REG_MCU_SLEEP_EN);
	printf("en upwr: %s\n", enable==1 ? "enable" : "disable" );
}

static void set_mcu_sleep(int enable)
{
	char cmd[64];
	sprintf(cmd, "i2c mw %x %x %d 1",CHIP_ADDR, REG_MCU_SLEEP_EN, enable);
	run_command(cmd, 0);
}

static void get_wdt_enable(void)
{
	int enable;
	enable = nbi_i2c_read(REG_EN_WDT);
	printf("en wdt: %s\n", enable==1 ? "enable" : "disable" );
}

static void set_wdt_enable(int enable)
{
	char cmd[64];
	sprintf(cmd, "i2c mw %x %x %d 1",CHIP_ADDR, REG_EN_WDT, enable);
	run_command(cmd, 0);
}

static int do_nbi_switchmac(cmd_tbl_t * cmdtp, int flag, int argc, char * const argv[])
{

	if (argc < 2)
		return CMD_RET_USAGE;

	if (strcmp(argv[1], "w") == 0) {
		if (argc < 3)
			return CMD_RET_USAGE;

		if (strcmp(argv[2], "0") == 0) {
			set_switch_mac(0);
		} else if (strcmp(argv[2], "1") == 0) {
			set_switch_mac(1);
		} else {
			return CMD_RET_USAGE;
		}
	} else if (strcmp(argv[1], "r") == 0) {
		get_switch_mac();
	} else {
		return CMD_RET_USAGE;
	}
	return 0;
}

static void get_boot_enable(int type)
{
	if (type == BOOT_EN_WOL)
		get_wol(true);
	else if (type == BOOT_EN_RTC)
		get_rtc();
	else if (type == BOOT_EN_DCIN)
		get_dcin();
	else if (type == BOOT_EN_LPWR)
		get_lpwr();
	else if (type == BOOT_EN_UPWR)
		get_upwr();
	else if (type == BOOT_EN_MCU_SLEEP)
		get_mcu_sleep_enable();
	else if (type == BOOT_EN_WDT)
		get_wdt_enable();
}

static void set_boot_enable(int type, int enable)
{
	int state = 0;
	if (type == BOOT_EN_WOL) {
		state = get_wol(false);
		set_wol(false, enable|(state&0x02));
	}
	else if (type == BOOT_EN_RTC)
		set_rtc(enable);
	else if (type == BOOT_EN_DCIN)
		set_dcin(enable);
	else if (type == BOOT_EN_LPWR)
		set_lpwr(enable);
	else if (type == BOOT_EN_UPWR)
		set_upwr(enable);
	else if (type == BOOT_EN_MCU_SLEEP)
		set_mcu_sleep(enable);
	else if (type == BOOT_EN_WDT)
		set_wdt_enable(enable);
}

static int do_nbi_trigger(cmd_tbl_t * cmdtp, int flag, int argc, char * const argv[])
{
	if (argc < 3)
		return CMD_RET_USAGE;

	if (strcmp(argv[2], "r") == 0) {

		if (strcmp(argv[1], "wol") == 0)
			get_boot_enable(BOOT_EN_WOL);
		else if (strcmp(argv[1], "rtc") == 0)
			get_boot_enable(BOOT_EN_RTC);
		else if (strcmp(argv[1], "dcin") == 0)
			get_boot_enable(BOOT_EN_DCIN);
		else if (strcmp(argv[1], "lpwr") == 0)
			get_boot_enable(BOOT_EN_LPWR);
		else if (strcmp(argv[1], "upwr") == 0)
			get_boot_enable(BOOT_EN_UPWR);
		else if (strcmp(argv[1], "mcu_en_sleep") == 0)
			get_boot_enable(BOOT_EN_MCU_SLEEP);
		else if (strcmp(argv[1], "wdt") == 0)
			get_boot_enable(BOOT_EN_WDT);
		else
			return CMD_RET_USAGE;
	} else if (strcmp(argv[2], "w") == 0) {
		if (argc < 4)
			return CMD_RET_USAGE;
		if ((strcmp(argv[3], "1") != 0) && (strcmp(argv[3], "0") != 0))
			return CMD_RET_USAGE;

		if (strcmp(argv[1], "wol") == 0) {

			if (strcmp(argv[3], "1") == 0)
				set_boot_enable(BOOT_EN_WOL, 1);
			else
				set_boot_enable(BOOT_EN_WOL, 0);

	    } else if (strcmp(argv[1], "rtc") == 0) {

			if (strcmp(argv[3], "1") == 0)
				set_boot_enable(BOOT_EN_RTC, 1);
			else
				set_boot_enable(BOOT_EN_RTC, 0);

		} else if (strcmp(argv[1], "dcin") == 0) {

			if (strcmp(argv[3], "1") == 0)
				set_boot_enable(BOOT_EN_DCIN, 1);
			else
				set_boot_enable(BOOT_EN_DCIN, 0);

		} else if (strcmp(argv[1], "lpwr") == 0) {
			if (strcmp(argv[3], "1") == 0)
				set_boot_enable(BOOT_EN_LPWR, 1);
			else
				set_boot_enable(BOOT_EN_LPWR, 0);
		} else if (strcmp(argv[1], "upwr") == 0) {
			if (strcmp(argv[3], "1") == 0)
				set_boot_enable(BOOT_EN_UPWR, 1);
			else
				set_boot_enable(BOOT_EN_UPWR, 0);
		} else if (strcmp(argv[1], "mcu_en_sleep") == 0) {
			if (strcmp(argv[3], "1") == 0)
				set_boot_enable(BOOT_EN_MCU_SLEEP, 1);
			else
				set_boot_enable(BOOT_EN_MCU_SLEEP, 0);
		} else if (strcmp(argv[1], "wdt") == 0) {
			if (strcmp(argv[3], "1") == 0)
				set_boot_enable(BOOT_EN_WDT, 1);
			else
				set_boot_enable(BOOT_EN_WDT, 0);
		}else {
			return CMD_RET_USAGE;
		}
	} else {

		return CMD_RET_USAGE;
	}

	return 0;
}

static void get_version(void)
{
	char version[VERSION_LENGHT] = {};
	int i;

	nbi_i2c_read_block(REG_VERSION, VERSION_LENGHT, version);
	printf("version: ");
	for (i=0; i< VERSION_LENGHT; i++) {
		printf("%02x ",version[i]);
	}
	printf("\n");
}

static void get_usid(void)
{
	char serial[64]={0};
	char usid[USID_LENGHT] = {0};

	nbi_i2c_read_block(REG_USID, USID_LENGHT, usid);
	sprintf(serial, "%02X%02X%02X%02X%02X%02X",usid[0],usid[1],usid[2],usid[3],usid[4],usid[5]);
	printf("usid:%s\r\n",serial);
	env_set("usid", serial);
}

static int do_nbi_init(cmd_tbl_t * cmdtp, int flag, int argc, char * const argv[])
{
	// switch to i2c1
	run_command("i2c dev 1", 0);

	return 0;
}

static int do_nbi_version(cmd_tbl_t * cmdtp, int flag, int argc, char * const argv[])
{
	get_version();
	return 0;
}

static int do_nbi_usid(cmd_tbl_t * cmdtp, int flag, int argc, char * const argv[])
{
	get_usid();
	return 0;
}

static void set_bootmode(int mode)
{
	char cmd[64];
	sprintf(cmd, "i2c mw %x %x %d 1",CHIP_ADDR, REG_BOOT_MODE, mode);
	run_command(cmd, 0);
}

static void get_bootmode(void)
{
	int mode;
	mode = nbi_i2c_read(REG_BOOT_MODE);

	if (mode == BOOT_MODE_EMMC) {
		printf("bootmode: emmc\n");
	} else if (mode == BOOT_MODE_SPI) {
		printf("bootmode: spi\n");
	} else if (mode == BOOT_MODE_SD) {
		printf("bootmode: sd\n");
	}else {
		printf("bootmode err: %d\n",mode);
	}
}

static int do_nbi_bootmode(cmd_tbl_t * cmdtp, int flag, int argc, char * const argv[])
{
	if (argc < 2)
		return CMD_RET_USAGE;
	if (strcmp(argv[1], "w") == 0) {
		if (argc < 3)
			return CMD_RET_USAGE;
		if (strcmp(argv[2], "emmc") == 0) {
			set_bootmode(BOOT_MODE_EMMC);
		} else if (strcmp(argv[2], "spi") == 0) {
			set_bootmode(BOOT_MODE_SPI);
		} else if (strcmp(argv[2], "sd") == 0) {
			set_bootmode(BOOT_MODE_SD);
		} else {
			return CMD_RET_USAGE;
		}
	} else if (strcmp(argv[1], "r") == 0) {
		get_bootmode();
	} else {
		return CMD_RET_USAGE;
	}

	return 0;
}

static void get_sys_led_mode(int type)
{
	int mode;
	if (type == LED_SYSTEM_OFF) {
		mode = nbi_i2c_read(REG_LED_OFF_SYS);
		if ((mode >= 0) && (mode <=3) )
		printf("led mode: %s  [systemoff]\n",LED_MODE_STR[mode]);
		else
		printf("read led mode err\n");
	}
	else {
		mode = nbi_i2c_read(REG_LED_ON_SYS);
		if ((mode >= LED_OFF_MODE) && (mode <= LED_HEARTBEAT_MODE))
		printf("led mode: %s  [systemon]\n",LED_MODE_STR[mode]);
		else
		printf("read led mode err\n");
	}
}

static void get_user_led_mode(void)
{
	int mode;
	mode = nbi_i2c_read(REG_LED_USER);
	if ((mode >= 0) && (mode <=3) )
		printf("led mode: %s  [user_led]\n",LED_MODE_STR[mode]);
	else
		printf("read led mode err\n");
}

static int set_user_led_mode(int mode)
{
	char cmd[64];
	sprintf(cmd, "i2c mw %x %x %d 1",CHIP_ADDR, REG_LED_USER, mode);
	run_command(cmd, 0);
	return 0;
}

static int set_sys_led_mode(int type, int mode)
{
	char cmd[64];
	if (type == LED_SYSTEM_OFF) {
		sprintf(cmd, "i2c mw %x %x %d 1",CHIP_ADDR, REG_LED_OFF_SYS, mode);
	} else if (type == LED_SYSTEM_ON) {
		sprintf(cmd, "i2c mw %x %x %d 1",CHIP_ADDR, REG_LED_ON_SYS, mode);
	} else {
		return CMD_RET_USAGE;
	}

	run_command(cmd, 0);
	return 0;
}

static int do_nbi_led(cmd_tbl_t * cmdtp, int flag, int argc, char * const argv[])
{
	int ret = 0;
	if (argc < 3)
		return CMD_RET_USAGE;

	if (strcmp(argv[1], "systemoff") ==0) {
		if (strcmp(argv[2], "r") == 0) {
			get_sys_led_mode(LED_SYSTEM_OFF);
		} else if (strcmp(argv[2], "w") == 0) {
			if (argc < 4)
				return CMD_RET_USAGE;
			if (strcmp(argv[3], "breathe") == 0) {
				ret = set_sys_led_mode(LED_SYSTEM_OFF, LED_BREATHE_MODE);
			} else if (strcmp(argv[3], "heartbeat") == 0) {
				ret = set_sys_led_mode(LED_SYSTEM_OFF, LED_HEARTBEAT_MODE);
			} else if (strcmp(argv[3], "on") == 0) {
				ret = set_sys_led_mode(LED_SYSTEM_OFF, LED_ON_MODE);
			} else if (strcmp(argv[3], "off") == 0) {
				ret = set_sys_led_mode(LED_SYSTEM_OFF, LED_OFF_MODE);
			} else {
				ret =  CMD_RET_USAGE;
			}
		}
	} else if (strcmp(argv[1], "systemon") ==0) {

		if (strcmp(argv[2], "r") == 0) {
			get_sys_led_mode(LED_SYSTEM_ON);
		} else if (strcmp(argv[2], "w") == 0) {
			if (argc <4)
				return CMD_RET_USAGE;
			if (strcmp(argv[3], "breathe") == 0) {
				ret = set_sys_led_mode(LED_SYSTEM_ON, LED_BREATHE_MODE);
			} else if (strcmp(argv[3], "heartbeat") == 0) {
				ret = set_sys_led_mode(LED_SYSTEM_ON, LED_HEARTBEAT_MODE);
			} else if (strcmp(argv[3], "on") == 0) {
				ret = set_sys_led_mode(LED_SYSTEM_ON, LED_ON_MODE);
			} else if (strcmp(argv[3], "off") == 0) {
				ret = set_sys_led_mode(LED_SYSTEM_ON, LED_OFF_MODE);
			} else {
				ret =  CMD_RET_USAGE;
			}
		}
	} else if (strcmp(argv[1], "user") ==0) {
		if (strcmp(argv[2], "r") == 0) {
			get_user_led_mode();
		} else if (strcmp(argv[2], "w") == 0) {
			if (argc <4)
				return CMD_RET_USAGE;
			if (strcmp(argv[3], "breathe") == 0) {
				ret = set_user_led_mode(LED_BREATHE_MODE);
			} else if (strcmp(argv[3], "heartbeat") == 0) {
				ret = set_user_led_mode(LED_HEARTBEAT_MODE);
			} else if (strcmp(argv[3], "on") == 0) {
				ret = set_user_led_mode(LED_ON_MODE);
			} else if (strcmp(argv[3], "off") == 0) {
				ret = set_user_led_mode(LED_OFF_MODE);
			} else {
				ret =  CMD_RET_USAGE;
			}
		}
	} else {
		return CMD_RET_USAGE;
	}
	return ret;
}

static int do_nbi_reset_conf(cmd_tbl_t * cmdtp, int flag, int argc, char * const argv[])
{
	char cmd[64];
	if (argc < 1)
		return CMD_RET_USAGE;
	if (strcmp(argv[2], "0") == 0)
		sprintf(cmd, "i2c mw %x %x %d 1",CHIP_ADDR, REG_REST_CONF, 0);
	else if (strcmp(argv[2], "f0") == 0)
		sprintf(cmd, "i2c mw %x %x %d 1",CHIP_ADDR, REG_REST_CONF, 0xf0);
	else
		return CMD_RET_USAGE;

	run_command(cmd, 0);
	return 0;
}

static int do_nbi_reset_sys(cmd_tbl_t * cmdtp, int flag, int argc, char * const argv[])
{
	char cmd[64];
	if (argc < 1)
		return CMD_RET_USAGE;
	if (strcmp(argv[2], "0") == 0)
		sprintf(cmd, "i2c mw %x %x %d 1",CHIP_ADDR, REG_SYS_RST, 0);
	else if (strcmp(argv[2], "1") == 0)
		sprintf(cmd, "i2c mw %x %x %d 1",CHIP_ADDR, REG_SYS_RST, 1);
	else if (strcmp(argv[2], "2") == 0)
		sprintf(cmd, "i2c mw %x %x %d 1",CHIP_ADDR, REG_SYS_RST, 2);
	else if (strcmp(argv[2], "3") == 0)
		sprintf(cmd, "i2c mw %x %x %d 1",CHIP_ADDR, REG_SYS_RST, 3);
	else
		return CMD_RET_USAGE;

	run_command(cmd, 0);
	return 0;
}

static int do_nbi_fan_level_set(cmd_tbl_t * cmdtp, int flag, int argc, char * const argv[])
{
	char cmd[64];
	int level = 0;
	if (argc < 1)
		return CMD_RET_USAGE;
	level = atoi(argv[2]);
	if (level >=0 && level <= 9)
		sprintf(cmd, "i2c mw %x %x %d 1",CHIP_ADDR, REG_FAN_LEVEL, level);
	else
		return CMD_RET_USAGE;

	run_command(cmd, 0);
	return 0;
}

static int do_nbi_fan_auto_test(cmd_tbl_t * cmdtp, int flag, int argc, char * const argv[])
{
	char cmd[64];
	sprintf(cmd, "i2c mw %x %x %d 1",CHIP_ADDR, REG_FAN_TEST, 1);
	run_command(cmd, 0);
	return 0;
}

static int do_nbi_wol_init(cmd_tbl_t * cmdtp, int flag, int argc, char * const argv[])
{
	char cmd[64];
	sprintf(cmd, "i2c mw %x %x %d 1",CHIP_ADDR, REG_INIT_WOL, 0);
	run_command(cmd, 0);
	return 0;
}

static int do_nbi_sys_status(cmd_tbl_t * cmdtp, int flag, int argc, char * const argv[])
{
	int status = 0;
	status = nbi_i2c_read(REG_LED_USER);
	if (0 == status)
		printf("sys status: [%d], normal power off\n", status);
	else if (1 == status)
		printf("sys status: [%d], abnormal power off\n", status);
	else
		printf("read sys status err\n");
	return 0;
}

static cmd_tbl_t cmd_nbi_sub[] = {
	U_BOOT_CMD_MKENT(init, 1, 1, do_nbi_init, "", ""),
	U_BOOT_CMD_MKENT(usid, 1, 1, do_nbi_usid, "", ""),
	U_BOOT_CMD_MKENT(version, 1, 1, do_nbi_version, "", ""),
	U_BOOT_CMD_MKENT(bootmode, 3, 1, do_nbi_bootmode, "", ""),
	U_BOOT_CMD_MKENT(switchmac, 3, 1, do_nbi_switchmac, "", ""),
	U_BOOT_CMD_MKENT(led, 4, 1, do_nbi_led, "", ""),
	U_BOOT_CMD_MKENT(reset_conf, 2, 1, do_nbi_reset_conf, "", ""),
	U_BOOT_CMD_MKENT(reset_sys, 2, 1, do_nbi_reset_sys, "", ""),
	U_BOOT_CMD_MKENT(fan_level_set, 2, 1, do_nbi_fan_level_set, "", ""),
	U_BOOT_CMD_MKENT(fan_auto_test, 1, 1, do_nbi_fan_auto_test, "", ""),
	U_BOOT_CMD_MKENT(wol_init, 1, 1, do_nbi_wol_init, "", ""),
	U_BOOT_CMD_MKENT(sys_status, 1, 1, do_nbi_sys_status, "", ""),
	U_BOOT_CMD_MKENT(trigger, 4, 1, do_nbi_trigger, "", ""),
};

static int do_nbi(cmd_tbl_t * cmdtp, int flag, int argc, char * const argv[])
{
	cmd_tbl_t *c;

	if (argc < 2)
		return CMD_RET_USAGE;

	/* Strip off leading 'nbi' command argument */
	argc--;
	argv++;

	c = find_cmd_tbl(argv[0], &cmd_nbi_sub[0], ARRAY_SIZE(cmd_nbi_sub));

	if (c)
		return c->cmd(cmdtp, flag, argc, argv);
	else
		return CMD_RET_USAGE;

}
static char nbi_help_text[] =
		"[function] [mode] [write|read] <value>\n"
		"\n"
		"nbi version - read version information\n"
		"nbi usid - read usid information\n"
		"\n"
		"nbi led [systemoff|systemon] w <off|on|breathe|heartbeat> - set blue led mode\n"
		"nbi led [systemoff|systemon] r - read blue led mode\n"
		"nbi led user w <off|on|breathe|heartbeat>"
		"nbi led user r read blue led mode"
		"\n"
		"nbi bootmode w <emmc|spi> - set bootmode to emmc or spi\n"
		"nbi bootmode r - read current bootmode\n"
		"\n"
		"nbi trigger [wol|rtc|ir|dcin] w <0|1> - disable/enable boot trigger\n"
		"nbi trigger [wol|rtc|ir|dcin] r - read mode of a boot trigger";


U_BOOT_CMD(
		nbi, 6, 1, do_nbi,
		"Namtso Bootloader Instructions sub-system",
		nbi_help_text
);

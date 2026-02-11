#include <linux/module.h>
#define INCLUDE_VERMAGIC
#include <linux/build-salt.h>
#include <linux/elfnote-lto.h>
#include <linux/export-internal.h>
#include <linux/vermagic.h>
#include <linux/compiler.h>

BUILD_SALT;
BUILD_LTO_INFO;

MODULE_INFO(vermagic, VERMAGIC_STRING);
MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};

#ifdef CONFIG_RETPOLINE
MODULE_INFO(retpoline, "Y");
#endif


static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0x801168f2, "misc_deregister" },
	{ 0x13172f12, "devm_kmalloc" },
	{ 0x27802d33, "gpiod_set_value_cansleep" },
	{ 0x4829a47e, "memcpy" },
	{ 0xa589b6f5, "spi_write_then_read" },
	{ 0xc3055d20, "usleep_range_state" },
	{ 0x6b755970, "devm_gpiod_get_optional" },
	{ 0x8da6585d, "__stack_chk_fail" },
	{ 0x6cbbfc54, "__arch_copy_to_user" },
	{ 0xa916b694, "strnlen" },
	{ 0x4dfa8d4b, "mutex_lock" },
	{ 0xf16f400, "driver_unregister" },
	{ 0x9d2a154d, "misc_register" },
	{ 0x3213f038, "mutex_unlock" },
	{ 0x600d6645, "__spi_register_driver" },
	{ 0x12a4e128, "__arch_copy_from_user" },
	{ 0xdeedc9b0, "gpiod_direction_output" },
	{ 0xf9a482f9, "msleep" },
	{ 0x8f80e6e5, "module_layout" },
};

MODULE_INFO(depends, "");

MODULE_ALIAS("spi:rc522");
MODULE_ALIAS("spi:nxp,rc522");
MODULE_ALIAS("of:N*T*Cnxp,rc522");
MODULE_ALIAS("of:N*T*Cnxp,rc522C*");

MODULE_INFO(srcversion, "E45D999D39FF6E85C2A70CA");

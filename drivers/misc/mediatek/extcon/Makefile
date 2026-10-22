# SPDX-License-Identifier: GPL-2.0

ccflags-y += -I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/typec/tcpc/inc
ifneq ($(CONFIG_CHARGER_NT),)
ccflags-y += -I$(DEVICE_MODULES_PATH)/drivers/power/nt_supply
else
ccflags-y += -I$(DEVICE_MODULES_PATH)/drivers/power/supply
endif

obj-$(CONFIG_EXTCON_MTK_USB) += extcon-mtk-usb.o

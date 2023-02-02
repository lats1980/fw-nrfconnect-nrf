MODULE_NAME = nrf700x

#TODO: only include spi/usb related files
ccflags-y += \
		-DCONFIG_WPA_SUPP \
		-DCONFIG_NRF700X_ON_USB_ADAPTER \
		-D CONFIG_NRF700X_MAX_TX_TOKENS=10 \
		-D CONFIG_NRF700X_TX_MAX_DATA_SIZE=1600 \
		-D CONFIG_NRF700X_RX_MAX_DATA_SIZE=1600 \
		-I$(PWD)/inc \
		-I$(PWD)/../osal/os_if/inc \
		-I$(PWD)/../osal/bus_if/bus/qspi/inc \
		-I$(PWD)/../osal/utils/inc \
		-I$(PWD)/../osal/bus_if/bal/inc \
		-I$(PWD)/../osal/fw_if/umac_if/inc \
		-I$(PWD)/../osal/hw_if/hal/inc \
		-I$(PWD)/../osal/hw_if/hal/inc/fw \
		-I$(PWD)/../osal/fw_if/umac_if/inc/fw \
		-I$(PWD)/src/usb/inc

obj-m += $(MODULE_NAME).o

OS_IF_SRC := ../osal/os_if/src
UTILS_SRC := ../osal/utils/src
HW_IF_SRC := ../osal/hw_if/hal/src
BUF_IF_SRC := ../osal/bus_if/bal/src
FW_IF_SRC := ../osal/fw_if/umac_if/src

module_objects	+= \
		$(OS_IF_SRC)/osal.o \
		$(UTILS_SRC)/list.o \
		$(UTILS_SRC)/queue.o \
		$(UTILS_SRC)/util.o \
		$(HW_IF_SRC)/hal_api.o \
		$(HW_IF_SRC)/hal_fw_patch_loader.o \
		$(HW_IF_SRC)/hal_interrupt.o \
		$(HW_IF_SRC)/hal_mem.o \
		$(HW_IF_SRC)/hal_reg.o \
		$(HW_IF_SRC)/hpqm.o \
		$(HW_IF_SRC)/pal.o \
		../osal/bus_if/bus/qspi/src/qspi.o \
		$(BUF_IF_SRC)/bal.o \
		$(FW_IF_SRC)/cmd.o \
		$(FW_IF_SRC)/event.o \
		$(FW_IF_SRC)/fmac_api.o \
		$(FW_IF_SRC)/rx.o \
		$(FW_IF_SRC)/fmac_vif.o \
		$(FW_IF_SRC)/fmac_util.o \
		$(FW_IF_SRC)/fmac_peer.o

$(MODULE_NAME)-y := main.o src/shim.o $(module_objects) src/nrf700x_usb.o src/cfg80211.o

all: clean
#	make ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KERNEL) M=$(PWD) modules
	make -C /lib/modules/`uname -r`/build M=`pwd` modules

clean:
	rm -rf .*.cmd *.o *.ko
	rm -rf $(OS_IF_SRC)/.*.cmd $(OS_IF_SRC)/*.o
	rm -rf $(UTILS_SRC)/.*.cmd $(UTILS_SRC)/*.o
	rm -rf $(HW_IF_SRC)/.*.cmd $(HW_IF_SRC)/*.o
	rm -rf $(BUF_IF_SRC)/.*.cmd $(BUF_IF_SRC)/*.o
	rm -rf $(FW_IF_SRC)/.*.cmd $(FW_IF_SRC)/*.o
	make -C /lib/modules/`uname -r`/build M=`pwd` clean

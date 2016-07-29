# $Id: Makefile.k26,v 1.7 2004/01/30 16:24:00 ajfa Exp $

ccflags-y += -DDEBUG -DSIP_DEBUG -DFAST_TX_STATUS \
    -DKERNEL_IV_WAR -DRX_SENDUP_SYNC -DDEBUG_FS \
    -DSIF_DSR_WAR -DHAS_INIT_DATA -DHAS_FW \
    -DP2P_CONCURRENT -DESP_USE_SDIO

esp8089-objs := esp_debug.o sdio_sif_esp.o spi_sif_esp.o \
    esp_file.o esp_ext.o \
    esp_mac80211.o esp_debug.o esp_utils.o esp_pm.o testmode.o esp_io.o esp_main.o esp_sip.o esp_ctrl.o  

obj-$(CONFIG_ESP8089) += esp8089.o


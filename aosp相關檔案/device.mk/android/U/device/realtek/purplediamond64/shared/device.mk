ZYGOTE_FORCE_64 := true

TARGET_BOARD_PLATFORM := rtd2819a


# Enable updating of APEXes
$(call inherit-product, $(SRC_TARGET_DIR)/product/updatable_apex.mk)
#======================== device.mk common part =======================

$(call inherit-product, $(SRC_TARGET_DIR)/product/core_64_bit.mk)

ifeq ($(BUILD_AS_TABLET), true)

$(warning tablet launcher)
# Setup tablet build
$(call inherit-product, frameworks/native/build/tablet-10in-xhdpi-2048-dalvik-heap.mk)

# PRODUCT_PACKAGES += Launcher3QuickStep

$(call inherit-product, device/realtek/common/product/edla/tablet_atv_base.mk)
# mark this and include atv_base to build as tv
# $(call inherit-product, $(SRC_TARGET_DIR)/product/full_base.mk)
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.software.app_widgets.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.software.app_widgets.xml

PRODUCT_PACKAGE_OVERLAYS += device/realtek/purplediamond64/purplediamond64/overlay

# USE_OEM_TV_APP := true
#===================== purplediamond_base.mk common part =====================
# GTVS mainline modules are common to both the Watson version and Amati version
# $(call inherit-product, device/google/atv/products/atv_base.mk)

else
# Setup TV Build
USE_OEM_TV_APP := true
#===================== purplediamond_base.mk common part =====================
# GTVS mainline modules are common to both the Watson version and Amati version
$(call inherit-product, device/google/atv/products/atv_base.mk)
endif
RTK_TV_SYSTEM := all

# Prebuilt binary path
RTK_PREBUILTS_PATH     := vendor/realtek/common/$(TARGET_BOARD_PLATFORM)/prebuilts
CONFIG_NEW_STRUCTURE   := true

PRODUCT_SHIPPING_API_LEVEL := 34
# self-defined to determine fstab/rc settings
TARGET_SHIPPING_API_LEVEL := $(PRODUCT_SHIPPING_API_LEVEL)

# 3rd party modules are moved under vendor/realtek/<device>
VENDOR_DEVICE_PATH     := vendor/realtek/purplediamond64
RTK_DEVICE_THIRD_PARTY_PATH := $(VENDOR_DEVICE_PATH)/third_party/current

# Include RTK configs
include device/realtek/common/product/pre-config.mk
RTK_TV_CONFIG_HW       := $(RTK_FRAMEWORKS_PATH)/$(RTK_FRAMEWORKS_CONFIG_SUB_PATH)/configs/defconfig/hw/hw_2819a
RTK_TV_CONFIG_SYSTEM   := $(RTK_FRAMEWORKS_PATH)/$(RTK_FRAMEWORKS_CONFIG_SUB_PATH)/configs/defconfig/system/system_pie_std
RTK_TV_CONFIG_TVSYSTEM := $(RTK_FRAMEWORKS_PATH)/$(RTK_FRAMEWORKS_CONFIG_SUB_PATH)/configs/defconfig/tvsystem/tvsystem_64bit
RTK_TV_CONFIG_MEDIA    := $(RTK_FRAMEWORKS_PATH)/$(RTK_FRAMEWORKS_CONFIG_SUB_PATH)/configs/defconfig/media/media_std
include $(RTK_FRAMEWORKS_CONFIG_PATH)/version.mk
include $(RTK_FRAMEWORKS_CONFIG_PATH)/config.mk
include $(RTK_FRAMEWORKS_CONFIG_PATH)/media.mk

NEED_RTK_SLICE_APK := false

PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.opengles.aep.xml:vendor/etc/permissions/android.hardware.opengles.aep.xml \

# vulkan 1.1 support
PRODUCT_COPY_FILES += \
    device/realtek/common/etc/gpu/powervr.ini:system/etc/powervr.ini \
    frameworks/native/data/etc/android.hardware.vulkan.version-1_3.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.vulkan.version.xml \
    frameworks/native/data/etc/android.software.vulkan.deqp.level-2023-03-01.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.software.vulkan.deqp.level.xml \
    frameworks/native/data/etc/android.hardware.vulkan.level-0.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.vulkan.level.xml \
    frameworks/native/data/etc/android.hardware.vulkan.compute-0.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.vulkan.compute.xml \
    frameworks/native/data/etc/android.software.opengles.deqp.level-2023-03-01.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.software.opengles.deqp.level.xml

PRODUCT_VENDOR_PROPERTIES += \
    graphics.gpu.profiler.support=true

PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.software.freeform_window_management.xml:system/etc/permissions/android.software.freeform_window_management.xml

PRODUCT_OEM_PROPERTIES := \
    ro.product.name \
    ro.product.brand \
    ro.product.device \
    ro.product.manufacturer \
    ro.product.model \
    ro.vendor.nrdp.modelgroup \
    ro.vendor.nrdp.validation \
    ro.boot.product.vendor.sku \
    ro.com.google.clientidbase

# This device is xhdpi.  However the platform doesn't
# currently contain all of the bitmaps at xhdpi density so
# we do this little trick to fall back to the hdpi version
# if the xhdpi doesn't exist.
PRODUCT_AAPT_CONFIG := normal large hdpi xhdpi xxhdpi xxxhdpi
PRODUCT_AAPT_PREF_CONFIG := xxxhdpi

# In userdebug, add minidebug info the the boot image and the system server to support
# diagnosing native crashes.
ifneq (,$(filter userdebug, $(TARGET_BUILD_VARIANT)))
    # Boot image.
    PRODUCT_DEX_PREOPT_BOOT_FLAGS += --generate-mini-debug-info
    # System server and some of its services. This is just here for completeness and consistency,
    # as we currently only compile the boot image.
    # Note: we cannot use PRODUCT_SYSTEM_SERVER_JARS, as it has not been expanded at this point.
    $(call add-product-dex-preopt-module-config,services,--generate-mini-debug-info)
    $(call add-product-dex-preopt-module-config,wifi-service,--generate-mini-debug-info)
endif

# Media Configuration
PRODUCT_PROPERTY_OVERRIDES += \
    ro.vendor.rtk.variant.codecs=c2_4k_2 \
    ro.vendor.rtk.variant.codecs.backup=c2_4k_3 \
    ro.vendor.rtk.media.zerocopy=true \

# Audio xmls
PRODUCT_COPY_FILES += \
    device/realtek/common/audio/audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_policy_configuration.xml \
    device/realtek/common/audio/surround_sound_configuration_5_0.xml:$(TARGET_COPY_OUT_VENDOR)/etc/surround_sound_configuration_5_0.xml \
    frameworks/av/services/audiopolicy/config/a2dp_in_audio_policy_configuration_7_0.xml:$(TARGET_COPY_OUT_VENDOR)/etc/a2dp_in_audio_policy_configuration_7_0.xml \
    frameworks/av/services/audiopolicy/config/r_submix_audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/r_submix_audio_policy_configuration.xml \
    device/realtek/common/audio/usb_audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/usb_audio_policy_configuration.xml \
    frameworks/av/services/audiopolicy/config/default_volume_tables.xml:$(TARGET_COPY_OUT_VENDOR)/etc/default_volume_tables.xml \
    device/realtek/common/audio/audio_policy_volumes.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_policy_volumes.xml \
    frameworks/av/media/libeffects/data/audio_effects.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_effects.xml \
    device/realtek/common/audio/vendor_volume_tables.xml:$(TARGET_COPY_OUT_VENDOR)/etc/vendor_volume_tables.xml \

# 3rd party license
PRODUCT_COPY_FILES += \
    $(RTK_PREBUILTS_PATH)/third-party_license.txt:$(TARGET_COPY_OUT_VENDOR)/etc/third-party_license.txt \
    $(RTK_FRAMEWORKS_HAL_PATH)/rhal_tvfe/src/LICENSE:$(TARGET_COPY_OUT_VENDOR)/etc/third-party_tuner_license.txt

# factory executive prebuilt
PRODUCT_PACKAGES += \
    factory \
    RtkLeanbackLauncherCustomizer \
    RtkMenu

# Audio Processing (AOSP WebRTC)
PRODUCT_PACKAGES += \
    libaudiopreprocessing \
    libwebrtc_audioprocessing

# Hardware
PRODUCT_PACKAGES += \
    power.$(TARGET_BOARD_PLATFORM) \
    thermal.$(TARGET_BOARD_PLATFORM) \
    light.$(TARGET_BOARD_PLATFORM) \
    gatekeeper.$(TARGET_BOARD_PLATFORM) \
    9dc6502f-3c8e-4356-bd0d5fb1662c1e26.ta \
    keystore.$(TARGET_BOARD_PLATFORM) \
    hdmi_cec.$(TARGET_BOARD_PLATFORM) \
    vr.$(TARGET_BOARD_PLATFORM)

# keymint 2.0
PRODUCT_PACKAGES += \
    android.hardware.security.keymint-rtk-service

# RKP
PRODUCT_PACKAGES += \
    rkp_factory_extraction_tool \
    hwtrust

PRODUCT_PRODUCT_PROPERTIES += \
    remote_provisioning.hostname=remoteprovisioning.googleapis.com

# memtrack
PRODUCT_PACKAGES += \
    memtrack.$(TARGET_BOARD_PLATFORM)

PRODUCT_PROPERTY_OVERRIDES += \
    ro.control_privapp_permissions=enforce

PRODUCT_SYSTEM_DEFAULT_PROPERTIES += \
    ro.com.google.clientidbase=android-realtek-tv

# tv input hal
PRODUCT_PACKAGES += \
    android.hardware.tv.input-V1-ndk \
    android.hardware.tv.input-service.rtk

#leanback
ifeq ("$(wildcard vendor/google_gtvs)","")
PRODUCT_PACKAGES += \
    LeanbackLauncher

PRODUCT_COPY_FILES += \
    device/realtek/common/etc/permissions/privapp-permissions-atv.xml:system/etc/permissions/privapp-permissions-atv.xml
endif

# overlay
# PRODUCT_PACKAGES += \
#     RtkFrameworkOverlayAOSP

# llkd Configuration
PRODUCT_SYSTEM_DEFAULT_PROPERTIES += \
    ro.llk.ignorelist.process=;android.hardware.power-service.realtek


IS_LOW_RAM_DEVICE := false
# for lmkd
ifeq ($(IS_LOW_RAM_DEVICE), true)
$(call inherit-product, device/realtek/common/product/rtk_lowram.mk)
else
$(call inherit-product, device/realtek/common/product/rtk_nonlowram.mk)
endif

# for gles 3.2
PRODUCT_DEFAULT_PROPERTY_OVERRIDES += \
    ro.opengles.version=196610

# for HDMI resolution
# Tvservice 2k/4k judgement
# JIRA: R2841TV051-967
PRODUCT_SYSTEM_DEFAULT_PROPERTIES += \
    ro.hdmi.resolution=4K

# Enable it to add ODM properties
# PRODUCT_ODM_PROPERTIES += \

# for CTS adoptable test, default was aes-256-heh but not supported.
PRODUCT_DEFAULT_PROPERTY_OVERRIDES += \
    ro.crypto.volume.filenames_mode=aes-256-cts

# Enable Incremental on the device
PRODUCT_PROPERTY_OVERRIDES += \
    ro.incremental.enable=1

# Use the A/B updater.
AB_OTA_UPDATER := true
AB_OTA_PARTITIONS := \
    boot \
    vendor_boot \
    init_boot \
    dtbo \
    vbmeta \
    vbmeta_system \
    system \
    system_ext \
    system_dlkm \
    vendor \
    vendor_dlkm \
    product \
    odm \
    tvconfigs \
    factory_ro \
    oem \
    bootloader \
    FW_AKERNEL \
    FW_VKERNEL \
    FW_VKERNEL2 \
    FW_TZFW \
    FW_QSHOW

PRODUCT_USE_DYNAMIC_PARTITIONS := true

# Enforce native interfaces of product partition as VNDK
PRODUCT_PRODUCT_VNDK_VERSION := current

PRODUCT_PROPERTY_OVERRIDES += \
    ro.apk_verity.mode=2 \

# Arm64, this must be before other product mks
$(call inherit-product, device/realtek/common/product/rtk_base_arm64.mk)
$(call inherit-product, device/realtek/common/product/rtk_edla.mk)

ifeq ($(BUILD_AS_TABLET),true)
NEED_RTK_MODULES := false
$(call inherit-product, device/realtek/common/product/edla/rtk_tablet_dias_base.mk)
$(call inherit-product, device/realtek/common/product/edla/rtk_tablet_modules.mk)
else
# Dias TV Base
$(call inherit-product, device/realtek/common/product/rtk_dias_base.mk)
endif


$(call inherit-product, $(SRC_TARGET_DIR)/product/userspace_reboot.mk)
$(call inherit-product, frameworks/native/build/tablet-7in-xhdpi-2048-dalvik-heap.mk)

# Configuring SDCard replacement functionality
$(call inherit-product, $(SRC_TARGET_DIR)/product/emulated_storage.mk)

# Enforce generic ramdisk allow list
$(call inherit-product, $(SRC_TARGET_DIR)/product/generic_ramdisk.mk)

$(call inherit-product, device/realtek/common/root/root.mk)
$(call inherit-product, device/realtek/common/product/rtk_bt.mk)
$(call inherit-product-if-exists, vendor/realtek/common/ATV/hardware/current/mtkwlan/mtk_fw.mk)

$(call inherit-product, device/realtek/common/product/rtk_wifi.mk)
$(call inherit-product, device/realtek/common/product/rtk_common.mk)
$(call inherit-product, device/realtek/common/product/rtk_base.mk)

ifeq ($(BUILD_AS_TABLET), true)
PRODUCT_PACKAGES += DhcpClientd
else
$(call inherit-product, device/realtek/common/product/rtk_demeter_base.mk)
endif


$(call inherit-product, device/realtek/common/product/rtk_media.mk)
#$(call inherit-product, device/realtek/common/product/rtk_farfield.mk)
$(call inherit-product, device/realtek/common/product/display/rtk_display_xhdpi.mk)
$(call inherit-product-if-exists, $(VENDOR_DEVICE_PATH)/third_party/hbbtv/hbbtv.mk)
$(call inherit-product-if-exists, vendor/realtek/8k_codec/rtk_8kcodec.mk)
$(call inherit-product, device/realtek/common/product/rtk_ai_pq.mk)
$(call inherit-product, device/realtek/common/product/rtk_karaoke.mk)
$(call inherit-product, device/realtek/common/product/rtk_ci.mk)
$(call inherit-product-if-exists, $(RTK_DEVICE_THIRD_PARTY_PATH)/CommonInterface/ci.mk)

#AI Camera
$(call inherit-product-if-exists, $(RTK_FRAMEWORKS_HAL_PATH)/hal_src/hal/src/ai/rtk_ai_camera_svp.mk)

MDNS_OFFLOAD_SUPPORT := true
ifeq ($(MDNS_OFFLOAD_SUPPORT),true)
    $(call inherit-product, device/realtek/common/product/rtk_mdns_offload.mk)
endif

# Graphic
$(call inherit-product, vendor/realtek/common/graphic/products/24.2_RTM2_6643903_rtd2819a_arm64.mk)

OPTEE_PLATFORM_FLAVOR := $(TARGET_BOARD_PLATFORM)
RTK_TEE_TOOLCHAIN_NAME := asdk-6.4.1-a55-EL-4.4-g2.26-a32nut-170810
$(call inherit-product, device/realtek/common/product/rtk_optee.mk)

# DRM & prebuilt
$(call inherit-product-if-exists, $(RTK_TEE_PREBUILT_PATH)/oemcrypto/product/oemcrypto.mk)
$(call inherit-product-if-exists, $(RTK_TEE_PREBUILT_PATH)/playready/product/playready.mk)
$(call inherit-product-if-exists, $(RTK_TEE_PREBUILT_PATH)/lcf/product/device.mk)
$(call inherit-product-if-exists, vendor/realtek/common/ATV/modules/airplay/product/device.mk)

#Miracast
$(call inherit-product-if-exists, vendor/realtek/common/$(TARGET_BOARD_PLATFORM)/prebuilts/drm/hdcp2/product/hdcp2.mk)
PRODUCT_PACKAGES += Miracast

#ScreenRecorder
PRODUCT_PACKAGES += ScreenRecorder

# Netflix Ready Device
$(call inherit-product-if-exists, vendor/realtek/common/ATV/modules/netflix/product/device.mk)
$(call inherit-product-if-exists, $(RTK_TEE_PREBUILT_PATH)/mgkid/product/mgkid.mk)

# Include GSI keys for Dynamic System Updates( DSU)
$(call inherit-product, $(SRC_TARGET_DIR)/product/developer_gsi_keys.mk)

PRODUCT_TV_RESOLUTION := 4K

# Enable codec notification
PRODUCT_PROPERTY_OVERRIDES += \
    ro.vendor.rtk.hwc.suppresshdrnotification=0

PRODUCT_ENFORCE_PRODUCT_PARTITION_INTERFACE := true

# Set system properties identifying the chipset
PRODUCT_VENDOR_PROPERTIES += ro.soc.manufacturer=Realtek
PRODUCT_VENDOR_PROPERTIES += ro.soc.model=RTD2895P

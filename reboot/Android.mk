# Copyright 2013 The Android Open Source Project

LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:= reboot.c

LOCAL_SHARED_LIBRARIES:= libcutils

LOCAL_MODULE:= reboot

include $(BUILD_EXECUTABLE)

# /sbin/poweroff
include $(CLEAR_VARS)
LOCAL_MODULE := poweroff
LOCAL_SRC_FILES := poweroff.c
LOCAL_STATIC_LIBRARIES := libc libcutils
LOCAL_FORCE_STATIC_EXECUTABLE := true
LOCAL_CFLAGS += $(targetSmpFlag)
LOCAL_C_INCLUDES := $(libcutils_c_includes)
LOCAL_MODULE_PATH := $(TARGET_ROOT_OUT)/sbin
LOCAL_UNSTRIPPED_PATH := $(TARGET_ROOT_OUT_UNSTRIPPED)/sbin
include $(BUILD_EXECUTABLE)

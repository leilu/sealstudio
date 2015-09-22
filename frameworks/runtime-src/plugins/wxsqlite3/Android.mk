 LOCAL_PATH := $(call my-dir)

 include $(CLEAR_VARS)

 LOCAL_MODULE := wxsqlite3_static

 LOCAL_MODULE_FILENAME := libwxsqlite3

 LOCAL_CFLAGS := \
 -DSQLITE_HAS_CODEC \
 -DCODEC_TYPE=CODEC_TYPE_AES128

 LOCAL_SRC_FILES := \
 src/sqlite3secure.c

 LOCAL_EXPORT_C_INCLUDES := $(LOCAL_PATH)/src

 LOCAL_C_INCLUDES := $(LOCAL_PATH)/src

 include $(BUILD_STATIC_LIBRARY)
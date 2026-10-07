#pragma once

#include <jni.h>
#include <stdint.h>
#include <string>

#define jni_func_name(name) Java_is_xyz_mpv_MPVLib_##name
#define jni_func(return_type, name, ...) JNIEXPORT return_type JNICALL jni_func_name(name) (JNIEnv *env, jobject obj, ##__VA_ARGS__)

bool acquire_jni_env(JavaVM *vm, JNIEnv **env);
bool init_methods_cache(JNIEnv *env);
bool jstring_to_utf8(JNIEnv *env, jstring value, std::string *utf8);
jstring utf8_to_jstring(JNIEnv *env, const char *value);
void send_command_reply_to_java(JNIEnv *env, uint64_t request_id, int error,
                               int64_t result);

#ifndef UTIL_EXTERN
#define UTIL_EXTERN extern
#endif

UTIL_EXTERN jclass java_Integer, java_Double, java_Boolean;
UTIL_EXTERN jmethodID java_Integer_init, java_Double_init, java_Boolean_init;

UTIL_EXTERN jclass android_graphics_Bitmap, android_graphics_Bitmap_Config;
UTIL_EXTERN jmethodID android_graphics_Bitmap_createBitmap,
	android_graphics_Bitmap_createBitmapWH;
UTIL_EXTERN jfieldID android_graphics_Bitmap_Config_ARGB_8888;

UTIL_EXTERN jclass mpv_MPVLib;
UTIL_EXTERN jmethodID mpv_MPVLib_eventProperty_S,
	mpv_MPVLib_eventProperty_Sb,
	mpv_MPVLib_eventProperty_Sl,
	mpv_MPVLib_eventProperty_Sd,
	mpv_MPVLib_eventProperty_SS,
	mpv_MPVLib_eventProperty_SN,
	mpv_MPVLib_event,
	mpv_MPVLib_eventCommandReply_JiJ,
	mpv_MPVLib_eventEndFile_iiS,
	mpv_MPVLib_logMessage_SiS;

UTIL_EXTERN jclass mpv_MPVNode_None, mpv_MPVNode_StringNode, mpv_MPVNode_BooleanNode,
	mpv_MPVNode_IntNode, mpv_MPVNode_DoubleNode, mpv_MPVNode_ArrayNode, mpv_MPVNode_MapNode, mpv_MPVNode;
UTIL_EXTERN jfieldID mpv_MPVNode_None_INSTANCE;
UTIL_EXTERN jmethodID mpv_MPVNode_StringNode_init, mpv_MPVNode_BooleanNode_init,
	mpv_MPVNode_IntNode_init, mpv_MPVNode_DoubleNode_init,
	mpv_MPVNode_ArrayNode_init, mpv_MPVNode_MapNode_init;

UTIL_EXTERN jclass java_util_ArrayList, java_util_HashMap;
UTIL_EXTERN jmethodID java_util_ArrayList_init, java_util_ArrayList_add,
	java_util_HashMap_init, java_util_HashMap_put;

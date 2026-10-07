#pragma once

#include <jni.h>
#include <mpv/client.h>

int register_iso_stream(JNIEnv *env, mpv_handle *context);

#pragma once

#include <jni.h>
#include <stdint.h>
#include <string>
#include <vector>

struct mpv_event;

enum class SurfaceTarget {
    VIDEO,
    OSD,
};

// Command enqueue and release operations run while the owner holds g_mpv_mutex.
int enqueue_command(JNIEnv *env, uint64_t request_id,
                    std::vector<std::string> command);
int enqueue_command_long_result(JNIEnv *env, uint64_t request_id,
                               std::string result_key,
                               std::vector<std::string> command);
// Surface updates acquire the context lock internally. Blocking updates must not
// run on the native event thread, which delivers their completion.
int enqueue_surface(JNIEnv *env, SurfaceTarget target, jobject surface,
                    bool wait_for_completion);
int enqueue_shutdown(JNIEnv *env);
void handle_request_reply(JNIEnv *env, mpv_event *event);
void release_requests(JNIEnv *env);

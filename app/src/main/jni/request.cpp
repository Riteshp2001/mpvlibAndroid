#include <jni.h>

#include <condition_variable>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <mpv/client.h>

#include "globals.h"
#include "jni_utils.h"
#include "log.h"
#include "request.h"

enum class RequestType {
    VIDEO_SURFACE,
    OSD_SURFACE,
    COMMAND,
    COMMAND_LONG_RESULT,
    SHUTDOWN,
};

static constexpr uint64_t INTERNAL_REQUEST_ID =
    std::numeric_limits<uint64_t>::max();

struct SurfaceCompletion {
    std::condition_variable condition;
    bool done = false;
    int error = MPV_ERROR_UNINITIALIZED;
};

struct MpvRequest {
    RequestType type;
    uint64_t request_id;
    std::vector<std::string> command;
    std::string result_key;
    jobject surface;
    std::shared_ptr<SurfaceCompletion> completion;

    MpvRequest(RequestType type_, jobject surface_)
        : type(type_), request_id(INTERNAL_REQUEST_ID), surface(surface_) {}

    MpvRequest(uint64_t request_id_, std::vector<std::string> command_)
        : type(RequestType::COMMAND), request_id(request_id_),
          command(std::move(command_)), surface(NULL) {}

    MpvRequest(uint64_t request_id_, std::string result_key_,
               std::vector<std::string> command_)
        : type(RequestType::COMMAND_LONG_RESULT), request_id(request_id_),
          command(std::move(command_)), result_key(std::move(result_key_)),
          surface(NULL) {}

    MpvRequest()
        : type(RequestType::SHUTDOWN), request_id(INTERNAL_REQUEST_ID),
          command(1, "quit"), surface(NULL) {}
};

struct RequestFailure {
    uint64_t request_id;
    int error;
};

// libmpv may complete asynchronous mutations out of order. Keep one mutation
// in flight so commands and Surface changes are applied in submission order.
static std::mutex request_mutex;
static std::deque<std::unique_ptr<MpvRequest>> pending_requests;
static std::unique_ptr<MpvRequest> request_in_flight;
static std::vector<std::shared_ptr<SurfaceCompletion>> shutdown_surface_waiters;
static jobject video_surface;
static jobject osd_surface;

static void clear_surface(JNIEnv *env, jobject *target) {
    if (!*target)
        return;
    env->DeleteGlobalRef(*target);
    *target = NULL;
}

static void clear_request_surface(JNIEnv *env, MpvRequest *request) {
    if (request)
        clear_surface(env, &request->surface);
}

static void complete_surface_update(
        const std::shared_ptr<SurfaceCompletion> &completion, int error) {
    if (!completion)
        return;
    completion->error = error;
    completion->done = true;
    completion->condition.notify_all();
}

static jobject *get_applied_surface(RequestType type) {
    if (type == RequestType::VIDEO_SURFACE)
        return &video_surface;
    if (type == RequestType::OSD_SURFACE)
        return &osd_surface;
    return NULL;
}

static const char *get_surface_property(RequestType type) {
    if (type == RequestType::VIDEO_SURFACE)
        return "wid";
    if (type == RequestType::OSD_SURFACE)
        return "android-osd-wid";
    return NULL;
}

static bool is_command_request(RequestType type) {
    return type == RequestType::COMMAND ||
        type == RequestType::COMMAND_LONG_RESULT ||
        type == RequestType::SHUTDOWN;
}

static int run_command_request(mpv_handle *context, const MpvRequest *request) {
    std::vector<mpv_node> values(request->command.size());
    for (size_t i = 0; i < request->command.size(); ++i) {
        values[i].format = MPV_FORMAT_STRING;
        values[i].u.string = const_cast<char *>(request->command[i].c_str());
    }
    mpv_node_list arguments = {};
    arguments.num = static_cast<int>(values.size());
    arguments.values = values.data();
    mpv_node command = {};
    command.format = MPV_FORMAT_NODE_ARRAY;
    command.u.list = &arguments;
    return mpv_command_node_async(context, request->request_id, &command);
}

static bool read_long_result(const mpv_node *node, const std::string &key,
                             int64_t *result) {
    if (!node || node->format != MPV_FORMAT_NODE_MAP || !node->u.list ||
            !node->u.list->keys)
        return false;
    mpv_node_list *map = node->u.list;
    for (int i = 0; i < map->num; ++i) {
        if (map->keys[i] && key == map->keys[i] &&
                map->values[i].format == MPV_FORMAT_INT64) {
            *result = map->values[i].u.int64;
            return true;
        }
    }
    return false;
}

static int get_reply_event_id(const MpvRequest *request) {
    return is_command_request(request->type)
        ? MPV_EVENT_COMMAND_REPLY
        : MPV_EVENT_SET_PROPERTY_REPLY;
}

static int start_next_request_locked(JNIEnv *env,
                                    std::vector<RequestFailure> *failures) {
    if (request_in_flight || pending_requests.empty())
        return MPV_ERROR_SUCCESS;

    mpv_handle *context = g_mpv.load();
    if (!context || !g_event_thread_started)
        return MPV_ERROR_UNINITIALIZED;

    std::unique_ptr<MpvRequest> request = std::move(pending_requests.front());
    pending_requests.pop_front();

    int result;
    if (request->type == RequestType::COMMAND_LONG_RESULT) {
        result = run_command_request(context, request.get());
    } else if (is_command_request(request->type)) {
        std::vector<const char *> arguments(request->command.size() + 1, NULL);
        for (size_t i = 0; i < request->command.size(); ++i)
            arguments[i] = request->command[i].c_str();
        result = mpv_command_async(context, request->request_id,
                                  arguments.data());
    } else {
        const char *property = get_surface_property(request->type);
        int64_t wid = reinterpret_cast<intptr_t>(request->surface);
        result = mpv_set_property_async(context, request->request_id,
            property, MPV_FORMAT_INT64, &wid);
    }
    if (result < 0) {
        const char *action = is_command_request(request->type)
            ? "mpv_command_async" : "mpv_set_property_async";
        const char *target = is_command_request(request->type)
            ? (request->command.empty() ? "<empty>" : request->command[0].c_str())
            : get_surface_property(request->type);
        ALOGE("%s(%s) returned error %s", action, target,
              mpv_error_string(result));
        if (request->type == RequestType::SHUTDOWN) {
            g_force_shutdown = true;
            mpv_wakeup(context);
            return MPV_ERROR_SUCCESS;
        }
        if (failures && request->request_id != INTERNAL_REQUEST_ID)
            failures->push_back({request->request_id, result});
        clear_request_surface(env, request.get());
        complete_surface_update(request->completion, result);
        return result;
    }

    request_in_flight = std::move(request);
    return MPV_ERROR_SUCCESS;
}

static int enqueue_request(JNIEnv *env, std::unique_ptr<MpvRequest> request) {
    std::lock_guard<std::mutex> lock(request_mutex);
    if (!g_mpv || !g_event_thread_started) {
        clear_request_surface(env, request.get());
        return MPV_ERROR_UNINITIALIZED;
    }
    if (g_shutdown_requested) {
        clear_request_surface(env, request.get());
        // A Surface destruction callback must still await native destruction.
        if (request->completion) {
            shutdown_surface_waiters.push_back(request->completion);
            return MPV_ERROR_SUCCESS;
        }
        return MPV_ERROR_UNINITIALIZED;
    }
    pending_requests.push_back(std::move(request));
    return start_next_request_locked(env, NULL);
}

int enqueue_command(JNIEnv *env, uint64_t request_id,
                    std::vector<std::string> command) {
    std::unique_ptr<MpvRequest> request(
        new MpvRequest(request_id, std::move(command)));
    return enqueue_request(env, std::move(request));
}

int enqueue_command_long_result(JNIEnv *env, uint64_t request_id,
                               std::string result_key,
                               std::vector<std::string> command) {
    std::unique_ptr<MpvRequest> request(new MpvRequest(
        request_id, std::move(result_key), std::move(command)));
    return enqueue_request(env, std::move(request));
}

int enqueue_surface(JNIEnv *env, SurfaceTarget target, jobject surface,
                    bool wait_for_completion) {
    std::shared_ptr<SurfaceCompletion> completion;
    if (wait_for_completion)
        completion = std::make_shared<SurfaceCompletion>();
    {
        std::lock_guard<std::mutex> context_lock(g_mpv_mutex);
        jobject surface_ref = surface ? env->NewGlobalRef(surface) : NULL;
        if (surface && !surface_ref)
            return MPV_ERROR_NOMEM;
        RequestType type = target == SurfaceTarget::VIDEO
            ? RequestType::VIDEO_SURFACE : RequestType::OSD_SURFACE;
        std::unique_ptr<MpvRequest> request(new MpvRequest(type, surface_ref));
        request->completion = completion;
        int result = enqueue_request(env, std::move(request));
        if (result < 0)
            return result;
    }
    if (!completion)
        return MPV_ERROR_SUCCESS;
    // Surface destruction may proceed only after native output stops using it.
    // The event dispatcher needs the context lock to acknowledge this request.
    std::unique_lock<std::mutex> lock(request_mutex);
    completion->condition.wait(lock, [&] { return completion->done; });
    return completion->error;
}

int enqueue_shutdown(JNIEnv *env) {
    std::lock_guard<std::mutex> lock(request_mutex);
    if (!g_mpv || !g_event_thread_started)
        return MPV_ERROR_UNINITIALIZED;
    if (g_shutdown_requested.exchange(true))
        return MPV_ERROR_SUCCESS;
    pending_requests.push_back(std::unique_ptr<MpvRequest>(new MpvRequest()));
    const int result = start_next_request_locked(env, NULL);
    if (result < 0) {
        mpv_handle *context = g_mpv.load();
        if (context) {
            g_force_shutdown = true;
            mpv_wakeup(context);
        }
    }
    return MPV_ERROR_SUCCESS;
}

void handle_request_reply(JNIEnv *env, mpv_event *event) {
    if (event->event_id != MPV_EVENT_SET_PROPERTY_REPLY &&
            event->event_id != MPV_EVENT_COMMAND_REPLY)
        return;

    bool notify_current;
    int reply_error = event->error;
    int64_t reply_result = 0;
    std::vector<RequestFailure> failures;
    {
        std::lock_guard<std::mutex> context_lock(g_mpv_mutex);
        std::lock_guard<std::mutex> lock(request_mutex);
        if (!request_in_flight ||
                event->reply_userdata != request_in_flight->request_id ||
                event->event_id != get_reply_event_id(request_in_flight.get()))
            return;

        MpvRequest *request = request_in_flight.get();
        notify_current = request->request_id != INTERNAL_REQUEST_ID;
        if (reply_error >= 0 && request->type == RequestType::COMMAND_LONG_RESULT) {
            mpv_event_command *reply = static_cast<mpv_event_command *>(event->data);
            if (!reply || !read_long_result(&reply->result, request->result_key,
                                           &reply_result)) {
                ALOGE("asynchronous mpv command returned no int64 result for %s",
                      request->result_key.c_str());
                reply_error = MPV_ERROR_COMMAND;
            }
        }
        jobject *applied_surface = get_applied_surface(request->type);
        if (reply_error < 0) {
            ALOGE("asynchronous mpv request failed: %s",
                  mpv_error_string(reply_error));
            clear_request_surface(env, request);
        } else if (applied_surface) {
            clear_surface(env, applied_surface);
            *applied_surface = request->surface;
            request->surface = NULL;
        }
        complete_surface_update(request->completion, reply_error);
        request_in_flight.reset();

        while (!pending_requests.empty()) {
            int result = start_next_request_locked(env, &failures);
            if (result >= 0 || result == MPV_ERROR_UNINITIALIZED)
                break;
        }
    }

    if (notify_current)
        send_command_reply_to_java(env, event->reply_userdata, reply_error,
                                   reply_result);
    for (const RequestFailure &failure : failures)
        send_command_reply_to_java(env, failure.request_id, failure.error, 0);
}

void release_requests(JNIEnv *env) {
    std::lock_guard<std::mutex> lock(request_mutex);
    clear_surface(env, &video_surface);
    clear_surface(env, &osd_surface);
    clear_request_surface(env, request_in_flight.get());
    if (request_in_flight)
        complete_surface_update(request_in_flight->completion,
                                MPV_ERROR_UNINITIALIZED);
    request_in_flight.reset();
    for (const std::unique_ptr<MpvRequest> &request : pending_requests) {
        clear_request_surface(env, request.get());
        complete_surface_update(request->completion, MPV_ERROR_UNINITIALIZED);
    }
    pending_requests.clear();
    for (const auto &completion : shutdown_surface_waiters)
        complete_surface_update(completion, MPV_ERROR_UNINITIALIZED);
    shutdown_surface_waiters.clear();
}

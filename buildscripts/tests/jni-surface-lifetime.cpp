// Exercise the production JNI Surface queue and its native completion boundary.
#undef NDEBUG
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <thread>
#include <mpv/client.h>

#include "app/src/main/jni/jni_utils.h"
#include "app/src/main/jni/globals.h"

JavaVM *g_vm;
std::atomic<mpv_handle *> g_mpv(nullptr);
std::atomic<bool> g_event_thread_started(false);
std::atomic<bool> g_shutdown_requested(false);
std::atomic<bool> g_force_shutdown(false);
std::mutex g_mpv_mutex;

#include "app/src/main/jni/request.cpp"
#include "app/src/main/jni/render.cpp"

static int context_storage, surface_storage[3];
static mpv_handle *const context = reinterpret_cast<mpv_handle *>(&context_storage);
static std::mutex test_mutex;
static std::condition_variable test_condition;
static int references[3], submissions, next_submit_error, exceptions;
static bool returned;
static thread_local bool exception_pending;

extern "C" int __android_log_print(int, const char *, const char *, ...) { return 0; }
extern "C" const char *mpv_error_string(int) { return "test error"; }
extern "C" void mpv_wakeup(mpv_handle *) {}
void send_command_reply_to_java(JNIEnv *, uint64_t, int, int64_t) {}
bool require_mpv_initialized(JNIEnv *) { return g_mpv && !g_shutdown_requested; }
void throw_java_exception(JNIEnv *, const char *) {
    std::lock_guard<std::mutex> lock(test_mutex);
    exceptions++;
    exception_pending = true;
}
static jboolean JNICALL exception_check(JNIEnv *) { return exception_pending; }
static int surface_index(jobject surface) {
    for (int i = 0; i < 3; i++) {
        if (surface == reinterpret_cast<jobject>(&surface_storage[i]))
            return i;
    }
    assert(false);
    return -1;
}
static jobject JNICALL new_global_ref(JNIEnv *, jobject surface) {
    std::lock_guard<std::mutex> lock(test_mutex);
    references[surface_index(surface)]++;
    return surface;
}
static void JNICALL delete_global_ref(JNIEnv *, jobject surface) {
    std::lock_guard<std::mutex> lock(test_mutex);
    assert(references[surface_index(surface)] > 0);
    references[surface_index(surface)]--;
}
static int submit(mpv_handle *handle) {
    assert(handle == context);
    std::lock_guard<std::mutex> lock(test_mutex);
    submissions++;
    int error = next_submit_error;
    next_submit_error = 0;
    test_condition.notify_all();
    return error;
}
extern "C" int mpv_set_property_async(mpv_handle *handle, uint64_t,
                                      const char *, mpv_format, void *) {
    return submit(handle);
}
extern "C" int mpv_command_async(mpv_handle *handle, uint64_t, const char **) {
    return submit(handle);
}
extern "C" int mpv_command_node_async(mpv_handle *handle, uint64_t, mpv_node *) {
    return submit(handle);
}
static void reply(JNIEnv *env, int error = 0) {
    mpv_event event = {};
    {
        std::lock_guard<std::mutex> lock(request_mutex);
        assert(request_in_flight);
        event.event_id = static_cast<mpv_event_id>(get_reply_event_id(request_in_flight.get()));
        event.reply_userdata = request_in_flight->request_id;
    }
    event.error = error;
    handle_request_reply(env, &event);
}
static void await_queued(size_t depth, size_t shutdown_waiters = 0) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(request_mutex);
            if (pending_requests.size() + !!request_in_flight == depth &&
                    shutdown_surface_waiters.size() == shutdown_waiters)
                return;
        }
        std::unique_lock<std::mutex> lock(test_mutex);
        assert(!returned && std::chrono::steady_clock::now() < deadline);
        test_condition.wait_for(lock, std::chrono::milliseconds(1));
    }
}
static void assert_waiting(size_t depth = 1, size_t shutdown_waiters = 0) {
    await_queued(depth, shutdown_waiters);
    std::unique_lock<std::mutex> lock(test_mutex);
    // This is a test deadlock/early-return guard, not a production delay.
    assert(!test_condition.wait_for(lock, std::chrono::milliseconds(100), [] { return returned; }));
}
static std::thread detach(JNIEnv *env, bool osd = false) {
    returned = false;
    return std::thread([env, osd] {
        if (osd)
            jni_func_name(detachOsdSurface)(env, nullptr);
        else
            jni_func_name(detachSurface)(env, nullptr);
        {
            std::lock_guard<std::mutex> lock(test_mutex);
            returned = true;
        }
        test_condition.notify_all();
    });
}
static void destroy(JNIEnv *env) {
    std::lock_guard<std::mutex> lock(g_mpv_mutex);
    g_mpv = nullptr; // The context is destroyed before its retained references.
    g_event_thread_started = false;
    release_requests(env);
}
static int reference_count(int index) {
    std::lock_guard<std::mutex> lock(test_mutex);
    return references[index];
}
static void start() {
    g_mpv = context;
    g_event_thread_started = true;
    g_shutdown_requested = false;
    exception_pending = false;
}
int main() {
    JNINativeInterface_ functions = {};
    functions.ExceptionCheck = exception_check;
    functions.NewGlobalRef = new_global_ref;
    functions.DeleteGlobalRef = delete_global_ref;
    JNIEnv env = {&functions};
    jobject first = reinterpret_cast<jobject>(&surface_storage[0]);
    jobject second = reinterpret_cast<jobject>(&surface_storage[1]);
    start();

    // Attach remains asynchronous, but its Surface is retained through reply.
    jni_func_name(attachSurface)(&env, nullptr, first);
    assert(reference_count(0) == 1 && request_in_flight);
    reply(&env);
    assert(video_surface == first);

    // Destruction cannot return while native still owns the old Surface.
    auto worker = detach(&env);
    assert_waiting();
    assert(reference_count(0) == 1);
    reply(&env); // Also proves neither native owner lock is held by the waiter.
    worker.join();
    assert(returned && reference_count(0) == 0 && !video_surface);

    jni_func_name(attachOsdSurface)(&env, nullptr, first);
    reply(&env);
    returned = false;
    worker = std::thread([&] {
        jni_func_name(replaceOsdSurface)(&env, nullptr, second);
        std::lock_guard<std::mutex> lock(test_mutex);
        returned = true;
        test_condition.notify_all();
    });
    assert_waiting();
    assert(reference_count(0) == 1 && reference_count(1) == 1);
    reply(&env);
    worker.join();
    assert(returned && reference_count(0) == 0 && reference_count(1) == 1);
    worker = detach(&env, true);
    assert_waiting();
    reply(&env, MPV_ERROR_PROPERTY_ERROR);
    worker.join();
    assert(exceptions == 1 && reference_count(1) == 1); // Failure retains applied Surface.
    destroy(&env);
    assert(reference_count(1) == 0);

    // A command ahead of detach can progress; failed queued submission wakes it.
    start();
    {
        std::lock_guard<std::mutex> lock(g_mpv_mutex);
        assert(enqueue_command(&env, 1, {"stop"}) == 0);
    }
    worker = detach(&env);
    assert_waiting(2);
    next_submit_error = MPV_ERROR_PROPERTY_ERROR;
    reply(&env);
    worker.join();
    assert(returned && exceptions == 2 && !request_in_flight);
    destroy(&env);

    // Both an in-flight waiter and a queued waiter are released after destruction.
    start();
    worker = detach(&env);
    assert_waiting();
    std::thread queued = std::thread([&] {
        jni_func_name(replaceOsdSurface)(&env, nullptr, second);
    });
    await_queued(2);
    destroy(&env);
    worker.join();
    queued.join();
    assert(returned && exceptions == 4 && reference_count(1) == 0);

    // Detach arriving after quit was requested must await actual destruction too.
    start();
    jni_func_name(attachSurface)(&env, nullptr, first);
    reply(&env);
    {
        std::lock_guard<std::mutex> lock(g_mpv_mutex);
        assert(enqueue_shutdown(&env) == 0);
    }
    worker = detach(&env);
    assert_waiting(1, 1);
    assert(reference_count(0) == 1);
    destroy(&env);
    worker.join();
    assert(returned && reference_count(0) == 0 && exceptions == 5);
    std::puts("JNI Surface completion, failure, FIFO and shutdown contracts passed");
}

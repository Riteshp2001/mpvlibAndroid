package `is`.xyz.mpv

import android.content.Context
import android.graphics.Bitmap
import android.view.Surface
import kotlinx.coroutines.channels.BufferOverflow
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.filter
import kotlinx.coroutines.flow.map
import java.nio.ByteBuffer
import java.util.concurrent.ConcurrentHashMap

@Suppress("unused")
object MPVLib {
    init {
        val libs = arrayOf("mpv", "player")
        for (lib in libs) {
            System.loadLibrary(lib)
        }
    }

    interface Stream {
        fun read(buffer: ByteBuffer): Int
        fun seek(position: Long): Long
        fun size(): Long
        fun cancel()
        fun close()
    }

    private val streams = ConcurrentHashMap<String, () -> Stream>()

    fun registerStream(uri: String, factory: () -> Stream) { streams[uri] = factory }
    fun unregisterStream(uri: String) { streams.remove(uri) }

    @JvmStatic
    fun openStream(uri: String): Stream? = streams[uri]?.invoke()

    external fun create(appctx: Context)
    external fun init()
    private external fun destroyNative(): Int
    external fun attachSurface(surface: Surface)
    external fun replaceSurface(surface: Surface)
    external fun detachSurface()
    external fun attachOsdSurface(surface: Surface)
    external fun replaceOsdSurface(surface: Surface)
    external fun detachOsdSurface()

    fun destroy() {
        val result = destroyNative()
        check(result >= 0) { "mpv shutdown failed: $result" }
        if (result == 0) {
            clearManagedState()
        }
    }

    external fun command(vararg cmd: String)
    external fun commandResult(cmd: Array<out String>): Int
    external fun commandNode(vararg cmd: String): MPVNode?
    external fun enqueueCommand(requestId: Long, cmd: Array<out String>): Int
    external fun enqueueCommandLongResult(requestId: Long, resultKey: String, cmd: Array<out String>): Int

    external fun setOptionString(name: String, value: String): Int

    external fun grabThumbnail(dimension: Int): Bitmap?
    external fun grabThumbnailFast(path: String, position: Double = 0.0, dimension: Int, useHwDec: Boolean = true): Bitmap?
    external fun setThumbnailJavaVM(appctx: Context)
    external fun clearThumbnailCache()

    external fun getPropertyInt(property: String): Int?
    external fun setPropertyInt(property: String, value: Int): Int
    external fun getPropertyDouble(property: String): Double?
    external fun setPropertyDouble(property: String, value: Double): Int
    external fun getPropertyBoolean(property: String): Boolean?
    external fun setPropertyBoolean(property: String, value: Boolean): Int
    external fun getPropertyString(property: String): String?
    external fun setPropertyString(property: String, value: String)
    external fun getPropertyByteArray(property: String): ByteArray?
    external fun getPropertyNode(property: String): MPVNode?
    external fun setPropertyNode(property: String, node: MPVNode)

    @JvmStatic
    fun getPropertyFloat(property: String) = getPropertyDouble(property)?.toFloat()
    @JvmStatic
    fun setPropertyFloat(property: String, value: Float) = setPropertyDouble(property, value.toDouble())
    @JvmStatic
    fun getPropertyLong(property: String) = getPropertyInt(property)?.toLong()
    @JvmStatic
    fun setPropertyLong(property: String, value: Long) = setPropertyInt(property, value.toInt())

    external fun observeProperty(property: String, format: Int): Int

    private val observers: MutableList<EventObserver> = ArrayList()

    private val eventFlow =
        MutableSharedFlow<Int>(extraBufferCapacity = 64, onBufferOverflow = BufferOverflow.DROP_OLDEST)
    private val eventPropertyFlow =
        MutableSharedFlow<String>(extraBufferCapacity = 64, onBufferOverflow = BufferOverflow.DROP_OLDEST)

    data class Property<T>(
        val type: Int,
        val getProperty: (String) -> T?,
        val flow: MutableSharedFlow<Pair<String, T>> =
            MutableSharedFlow(extraBufferCapacity = 64, onBufferOverflow = BufferOverflow.DROP_OLDEST),
        val map: MutableMap<String, StateFlow<T?>> = ConcurrentHashMap(),
    ) {
        operator fun get(property: String): StateFlow<T?> {
            return map.getOrPut(property) {
                observeProperty(property, type)
                MutableStateFlow(getProperty(property))
            }
        }

        operator fun set(property: String, value: T) {
            when (type) {
                MpvFormat.MPV_FORMAT_INT64 -> setPropertyInt(property, value as Int)
                MpvFormat.MPV_FORMAT_FLAG -> setPropertyBoolean(property, value as Boolean)
                MpvFormat.MPV_FORMAT_STRING -> setPropertyString(property, value as String)
                MpvFormat.MPV_FORMAT_DOUBLE -> setPropertyDouble(property, value as Double)
                MpvFormat.MPV_FORMAT_NODE,
                MpvFormat.MPV_FORMAT_NODE_ARRAY,
                MpvFormat.MPV_FORMAT_NODE_MAP -> setPropertyNode(property, value as MPVNode)
                else -> throw IllegalArgumentException("Unsupported property type")
            }
        }

        fun emit(property: String, value: T) {
            @Suppress("UNCHECKED_CAST")
            (map[property] as? MutableStateFlow<T?>)?.value = value
            flow.tryEmit(Pair(property, value))
        }
    }

    val propInt = Property(MpvFormat.MPV_FORMAT_INT64, ::getPropertyInt)
    val propBoolean = Property(MpvFormat.MPV_FORMAT_FLAG, ::getPropertyBoolean)
    val propString = Property(MpvFormat.MPV_FORMAT_STRING, ::getPropertyString)
    val propDouble = Property(MpvFormat.MPV_FORMAT_DOUBLE, ::getPropertyDouble)
    val propNode = Property(MpvFormat.MPV_FORMAT_NODE, ::getPropertyNode)

    val propLong = Property(MpvFormat.MPV_FORMAT_INT64, { getPropertyInt(it)?.toLong() })
    val propFloat = Property(MpvFormat.MPV_FORMAT_DOUBLE, { getPropertyDouble(it)?.toFloat() })

    fun eventFlow(property: String): Flow<Unit> {
        observeProperty(property, MpvFormat.MPV_FORMAT_NONE)
        return eventPropertyFlow.filter { it == property }.map { }
    }

    fun eventFlow(eventId: Int): Flow<Unit> {
        return eventFlow.filter { it == eventId }.map { }
    }

    @JvmStatic
    fun addObserver(o: EventObserver) {
        synchronized(observers) { observers.add(o) }
    }

    @JvmStatic
    fun removeObserver(o: EventObserver) {
        synchronized(observers) { observers.remove(o) }
    }

    private fun eventObserverSnapshot(): List<EventObserver> =
        synchronized(observers) { observers.toList() }

    private fun logObserverSnapshot(): List<LogObserver> =
        synchronized(log_observers) { log_observers.toList() }

    @JvmStatic
    fun eventProperty(property: String, value: Long) {
        for (o in eventObserverSnapshot()) o.eventProperty(property, value)
        propLong.emit(property, value)
        propInt.emit(property, value.toInt())
    }

    @JvmStatic
    fun eventProperty(property: String, value: Boolean) {
        for (o in eventObserverSnapshot()) o.eventProperty(property, value)
        propBoolean.emit(property, value)
    }

    @JvmStatic
    fun eventProperty(property: String, value: Double) {
        for (o in eventObserverSnapshot()) o.eventProperty(property, value)
        propDouble.emit(property, value)
        propFloat.emit(property, value.toFloat())
    }

    @JvmStatic
    fun eventProperty(property: String, value: String) {
        for (o in eventObserverSnapshot()) o.eventProperty(property, value)
        propString.emit(property, value)
    }

    @JvmStatic
    fun eventProperty(property: String, value: MPVNode) {
        for (o in eventObserverSnapshot()) o.eventProperty(property, value)
        propNode.emit(property, value)
    }

    @JvmStatic
    fun eventProperty(property: String) {
        for (o in eventObserverSnapshot()) o.eventProperty(property)
        eventPropertyFlow.tryEmit(property)
    }

    @JvmStatic
    fun event(eventId: Int, data: MPVNode) {
        try {
            for (o in eventObserverSnapshot()) o.event(eventId, data)
            eventFlow.tryEmit(eventId)
        } finally {
            if (eventId == MpvEvent.MPV_EVENT_SHUTDOWN) clearManagedState()
        }
    }

    @JvmStatic
    fun eventCommandReply(requestId: Long, error: Int) {
        eventCommandReply(requestId, error, 0)
    }

    @JvmStatic
    fun eventCommandReply(requestId: Long, error: Int, result: Long) {
        for (o in eventObserverSnapshot()) o.eventCommandReply(requestId, error, result)
    }

    @JvmStatic
    fun eventEndFile(reason: Int, error: Int, errorString: String?) {
        for (o in eventObserverSnapshot()) o.eventEndFile(reason, error, errorString)
    }

    private val log_observers: MutableList<LogObserver> = ArrayList()
    val logFlow =
        MutableSharedFlow<Triple<String, Int, String>>(
            extraBufferCapacity = 64,
            onBufferOverflow = BufferOverflow.DROP_OLDEST
        )

    @JvmStatic
    fun addLogObserver(o: LogObserver) {
        synchronized(log_observers) { log_observers.add(o) }
    }

    @JvmStatic
    fun removeLogObserver(o: LogObserver) {
        synchronized(log_observers) { log_observers.remove(o) }
    }

    private fun clearManagedState() {
        listOf(propInt, propBoolean, propDouble, propString, propFloat, propLong, propNode).forEach {
            it.map.clear()
        }
        synchronized(observers) { observers.clear() }
        synchronized(log_observers) { log_observers.clear() }
    }

    @JvmStatic
    fun logMessage(prefix: String, level: Int, text: String) {
        for (o in logObserverSnapshot()) o.logMessage(prefix, level, text)
        logFlow.tryEmit(Triple(prefix, level, text))
    }

    interface EventObserver {
        fun eventProperty(property: String)
        fun eventProperty(property: String, value: Long)
        fun eventProperty(property: String, value: Boolean)
        fun eventProperty(property: String, value: String)
        fun eventProperty(property: String, value: Double)
        fun eventProperty(property: String, value: MPVNode)
        fun event(eventId: Int, data: MPVNode)
        fun eventCommandReply(requestId: Long, error: Int) {}
        fun eventCommandReply(requestId: Long, error: Int, result: Long) {
            eventCommandReply(requestId, error)
        }
        fun eventEndFile(reason: Int, error: Int, errorString: String?) {}
    }

    interface LogObserver {
        fun logMessage(prefix: String, level: Int, text: String)
    }

    object MpvFormat {
        const val MPV_FORMAT_NONE: Int = 0
        const val MPV_FORMAT_STRING: Int = 1
        const val MPV_FORMAT_OSD_STRING: Int = 2
        const val MPV_FORMAT_FLAG: Int = 3
        const val MPV_FORMAT_INT64: Int = 4
        const val MPV_FORMAT_DOUBLE: Int = 5
        const val MPV_FORMAT_NODE: Int = 6
        const val MPV_FORMAT_NODE_ARRAY: Int = 7
        const val MPV_FORMAT_NODE_MAP: Int = 8
        const val MPV_FORMAT_BYTE_ARRAY: Int = 9
    }

    object MpvEvent {
        const val MPV_EVENT_NONE: Int = 0
        const val MPV_EVENT_SHUTDOWN: Int = 1
        const val MPV_EVENT_LOG_MESSAGE: Int = 2
        const val MPV_EVENT_GET_PROPERTY_REPLY: Int = 3
        const val MPV_EVENT_SET_PROPERTY_REPLY: Int = 4
        const val MPV_EVENT_COMMAND_REPLY: Int = 5
        const val MPV_EVENT_START_FILE: Int = 6
        const val MPV_EVENT_END_FILE: Int = 7
        const val MPV_EVENT_FILE_LOADED: Int = 8

        @Deprecated("")
        const val MPV_EVENT_IDLE: Int = 11

        @Deprecated("")
        const val MPV_EVENT_TICK: Int = 14
        const val MPV_EVENT_CLIENT_MESSAGE: Int = 16
        const val MPV_EVENT_VIDEO_RECONFIG: Int = 17
        const val MPV_EVENT_AUDIO_RECONFIG: Int = 18
        const val MPV_EVENT_SEEK: Int = 20
        const val MPV_EVENT_PLAYBACK_RESTART: Int = 21
        const val MPV_EVENT_PROPERTY_CHANGE: Int = 22
        const val MPV_EVENT_QUEUE_OVERFLOW: Int = 24
        const val MPV_EVENT_HOOK: Int = 25
    }

    object MpvEndFileReason {
        const val MPV_END_FILE_REASON_EOF: Int = 0
        const val MPV_END_FILE_REASON_STOP: Int = 2
        const val MPV_END_FILE_REASON_QUIT: Int = 3
        const val MPV_END_FILE_REASON_ERROR: Int = 4
        const val MPV_END_FILE_REASON_REDIRECT: Int = 5
    }

    object MpvError {
        const val MPV_ERROR_SUCCESS: Int = 0
        const val MPV_ERROR_EVENT_QUEUE_FULL: Int = -1
        const val MPV_ERROR_NOMEM: Int = -2
        const val MPV_ERROR_UNINITIALIZED: Int = -3
        const val MPV_ERROR_INVALID_PARAMETER: Int = -4
        const val MPV_ERROR_OPTION_NOT_FOUND: Int = -5
        const val MPV_ERROR_OPTION_FORMAT: Int = -6
        const val MPV_ERROR_OPTION_ERROR: Int = -7
        const val MPV_ERROR_PROPERTY_NOT_FOUND: Int = -8
        const val MPV_ERROR_PROPERTY_FORMAT: Int = -9
        const val MPV_ERROR_PROPERTY_UNAVAILABLE: Int = -10
        const val MPV_ERROR_PROPERTY_ERROR: Int = -11
        const val MPV_ERROR_COMMAND: Int = -12
        const val MPV_ERROR_LOADING_FAILED: Int = -13
        const val MPV_ERROR_AO_INIT_FAILED: Int = -14
        const val MPV_ERROR_VO_INIT_FAILED: Int = -15
        const val MPV_ERROR_NOTHING_TO_PLAY: Int = -16
        const val MPV_ERROR_UNKNOWN_FORMAT: Int = -17
        const val MPV_ERROR_UNSUPPORTED: Int = -18
        const val MPV_ERROR_NOT_IMPLEMENTED: Int = -19
        const val MPV_ERROR_GENERIC: Int = -20
    }

    object MpvLogLevel {
        const val MPV_LOG_LEVEL_NONE: Int = 0
        const val MPV_LOG_LEVEL_FATAL: Int = 10
        const val MPV_LOG_LEVEL_ERROR: Int = 20
        const val MPV_LOG_LEVEL_WARN: Int = 30
        const val MPV_LOG_LEVEL_INFO: Int = 40
        const val MPV_LOG_LEVEL_V: Int = 50
        const val MPV_LOG_LEVEL_DEBUG: Int = 60
        const val MPV_LOG_LEVEL_TRACE: Int = 70
    }
}

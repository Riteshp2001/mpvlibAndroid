package `is`.xyz.mpv

import android.content.Context
import android.util.AttributeSet
import android.util.Log
import android.view.SurfaceHolder
import android.view.SurfaceView
import `is`.xyz.mpv.MPVLib.MpvFormat
import `is`.xyz.mpv.MPVLib.observeProperty
import `is`.xyz.mpv.MPVLib.propBoolean
import `is`.xyz.mpv.MPVLib.propDouble
import `is`.xyz.mpv.MPVLib.propFloat
import `is`.xyz.mpv.MPVLib.propInt
import `is`.xyz.mpv.MPVLib.propLong
import `is`.xyz.mpv.MPVLib.propNode
import `is`.xyz.mpv.MPVLib.propString

// Contains only the essential code needed to get a picture on the screen

abstract class BaseMPVView(context: Context, attrs: AttributeSet) : SurfaceView(context, attrs), SurfaceHolder.Callback {
    private val lifecycleLock = Any()

    @Volatile
    private var initialized = false
    private var surfaceAttached = false

    /** True after libmpv has initialized and until [destroy] begins. */
    val isInitialized: Boolean
        get() = initialized

    /**
     * Initialize libmpv.
     *
     * Call this once before the view is shown. If any step fails, all native
     * resources created by this attempt are rolled back before the exception escapes.
     */
    fun initialize(configDir: String, cacheDir: String) {
        var failure: Throwable? = null
        var destroyAfterFailure = false

        synchronized(lifecycleLock) {
            check(!initialized && !MPVLib.isCreated()) { "MPV view is already initialized" }

            var callbackAdded = false
            try {
                MPVLib.create(context.applicationContext ?: context)

                MPVLib.setOptionString("config", "yes")
                MPVLib.setOptionString("config-dir", configDir)
                for (opt in arrayOf("gpu-shader-cache-dir", "icc-cache-dir")) {
                    MPVLib.setOptionString(opt, cacheDir)
                }
                initOptions()

                MPVLib.init()

                postInitOptions()
                MPVLib.setOptionString("force-window", "no")
                MPVLib.setOptionString("idle", "once")

                // Set this before registering the callback: SurfaceView may dispatch
                // an already-created surface immediately from addCallback().
                initialized = true
                holder.addCallback(this)
                callbackAdded = true
                observeProperties()
                reobserveAllProperties()
            } catch (t: Throwable) {
                if (callbackAdded)
                    holder.removeCallback(this)

                if (surfaceAttached && MPVLib.isCreated())
                    runCatching { detachSurfaceLocked() }
                surfaceAttached = false
                clearAllProperties()
                initialized = false

                // Do the potentially blocking native event-thread join only after
                // releasing this view monitor, so callbacks can finish cleanly.
                destroyAfterFailure = MPVLib.isCreated()
                failure = t
            }
        }

        failure?.let { cause ->
            if (destroyAfterFailure)
                runCatching { MPVLib.destroy() }
            throw cause
        }
    }

    /**
     * Deinitialize libmpv.
     *
     * Safe to call repeatedly. Surface ownership is released here as a fallback
     * even if Android never delivered surfaceDestroyed() before the owner died.
     */
    fun destroy() {
        val destroyNative = synchronized(lifecycleLock) {
            if (!initialized && !MPVLib.isCreated())
                return

            initialized = false
            holder.removeCallback(this)

            if (surfaceAttached && MPVLib.isCreated())
                detachSurfaceLocked()
            surfaceAttached = false

            clearAllProperties()
            MPVLib.isCreated()
        }

        // MPVLib.destroy() joins the native event thread. Never hold the view
        // monitor while doing that: an in-flight observer may need this monitor.
        if (destroyNative)
            MPVLib.destroy()
    }

    protected abstract fun initOptions()
    protected abstract fun postInitOptions()

    protected abstract fun observeProperties()

    private var filePath: String? = null

    /**
     * Set the first file to be played once the player is ready.
     */
    fun playFile(filePath: String) {
        synchronized(lifecycleLock) {
            this.filePath = filePath
        }
    }

    private var voInUse: String = "gpu"

    /**
     * Sets the VO to use.
     * It is automatically disabled/enabled when the surface dis-/appears.
     */
    fun setVo(vo: String) {
        synchronized(lifecycleLock) {
            voInUse = vo
            if (MPVLib.isCreated())
                MPVLib.setOptionString("vo", vo)
        }
    }

    // Surface callbacks

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        synchronized(lifecycleLock) {
            if (!initialized || !surfaceAttached)
                return
            MPVLib.setPropertyString("android-surface-size", "${width}x$height")
        }
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        synchronized(lifecycleLock) {
            if (!initialized || surfaceAttached)
                return

            Log.w(TAG, "attaching surface")
            MPVLib.attachSurface(holder.surface)
            surfaceAttached = true
            MPVLib.setOptionString("force-window", "yes")

            val pendingFile = filePath
            if (pendingFile != null) {
                MPVLib.command("loadfile", pendingFile)
                filePath = null
            } else {
                MPVLib.setPropertyString("vo", voInUse)
            }
        }
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        synchronized(lifecycleLock) {
            if (!surfaceAttached || !MPVLib.isCreated()) {
                surfaceAttached = false
                return
            }

            Log.w(TAG, "detaching surface")
            detachSurfaceLocked()
        }
    }

    private fun detachSurfaceLocked() {
        MPVLib.setPropertyString("vo", "null")
        MPVLib.setPropertyString("force-window", "no")
        MPVLib.detachSurface()
        surfaceAttached = false
    }

    private fun reobserveAllProperties() {
        propBoolean.map.keys.forEach { observeProperty(it, MpvFormat.MPV_FORMAT_FLAG) }
        propString.map.keys.forEach { observeProperty(it, MpvFormat.MPV_FORMAT_STRING) }
        propDouble.map.keys.forEach { observeProperty(it, MpvFormat.MPV_FORMAT_DOUBLE) }
        propFloat.map.keys.forEach { observeProperty(it, MpvFormat.MPV_FORMAT_DOUBLE) }
        propLong.map.keys.forEach { observeProperty(it, MpvFormat.MPV_FORMAT_INT64) }
        propInt.map.keys.forEach { observeProperty(it, MpvFormat.MPV_FORMAT_INT64) }
        propNode.map.keys.forEach { observeProperty(it, MpvFormat.MPV_FORMAT_NODE) }
    }

    private fun clearAllProperties() {
        listOf(propInt, propBoolean, propDouble, propString, propFloat, propLong, propNode).forEach {
            it.map.clear()
        }
    }

    companion object {
        private const val TAG = "mpv"
    }
}

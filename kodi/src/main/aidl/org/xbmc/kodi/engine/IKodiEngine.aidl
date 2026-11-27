package org.xbmc.kodi.engine;

import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.view.Surface;
import org.xbmc.kodi.engine.IKodiEngineCallback;

/** Kodi engine running in the host application's :kodi process. See KodiLib. */
interface IKodiEngine {
    /** Registers the client (replacing the previous one) and starts Kodi if needed. */
    void start(in Bundle options, IKodiEngineCallback callback);

    /**
     * Sets the Surface of a layer (KodiLib.LAYER_*), or removes it when surface is null.
     * Returns once Kodi no longer renders to the previous Surface.
     */
    void setSurface(int layer, in Surface surface, int width, int height);

    oneway void resizeSurface(int layer, int width, int height);

    /** mpv-style command, for example {"loadfile", url} or {"seek", "10", "relative"}. */
    oneway void command(in String[] args);

    /** mpv-style property; value uses mpv's string form ("yes"/"no", numbers, JSON for nodes). */
    oneway void setProperty(String name, String value);

    /** Raw Kodi JSON-RPC request; returns the JSON response, or null when Kodi is not ready. */
    String jsonRpc(String request);

    /**
     * Takes a descriptor of the client's (mpv's fd://N) and returns a URL that Kodi can read it
     * from. name is the file name, which Kodi uses to detect the format.
     */
    String registerFd(in ParcelFileDescriptor fd, String name);

    /** Stops playback and ends the engine process. */
    oneway void quit();
}

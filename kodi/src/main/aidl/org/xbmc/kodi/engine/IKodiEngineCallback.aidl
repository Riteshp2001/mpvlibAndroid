package org.xbmc.kodi.engine;

/** Engine events delivered to the client, in order. */
oneway interface IKodiEngineCallback {
    /** value is mpv's string form, JSON for node properties, or null when unavailable. */
    void onProperty(String name, String value);

    /** eventId is an mpv event id (KodiLib.EVENT_*); data is a JSON object or null. */
    void onEvent(int eventId, String data);

    void onLog(String prefix, int level, String text);

    /** Where Kodi places the video layer, in pixels of the GUI layer's Surface. */
    void onVideoRect(int left, int top, int right, int bottom);
}

package org.xbmc.kodi.engine;

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.media.MediaMetadata;
import android.media.session.PlaybackState;
import android.net.Uri;
import android.os.Handler;
import android.os.ParcelFileDescriptor;
import android.os.SystemClock;
import android.provider.MediaStore;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;
import org.xbmc.kodi.EngineBridge;
import org.xbmc.kodi.XBMCMediaSession;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * Speaks mpv's command/property/event dialect on top of Kodi's JSON-RPC API.
 *
 * <p>All methods run on the engine thread. Kodi's state is polled (JSON-RPC runs in-process, so
 * a poll costs well under a millisecond) and Kodi's media-session updates trigger extra polls, so
 * state changes surface quickly. Property values are published in mpv's string form; node
 * properties (track-list, video-params, ...) as JSON.
 */
final class PlayerBridge implements XBMCMediaSession.Listener
{
  /** Receives property changes and events for the client. */
  interface Sink
  {
    void property(String name, String value);

    void event(int eventId, JSONObject data);

    void log(int level, String text);
  }

  static final int EVENT_SHUTDOWN = 1;
  static final int EVENT_START_FILE = 6;
  static final int EVENT_END_FILE = 7;
  static final int EVENT_FILE_LOADED = 8;
  static final int EVENT_IDLE = 11;
  static final int EVENT_VIDEO_RECONFIG = 17;
  static final int EVENT_AUDIO_RECONFIG = 18;
  static final int EVENT_SEEK = 20;
  static final int EVENT_PLAYBACK_RESTART = 21;

  static final int LOG_ERROR = 20;
  static final int LOG_WARN = 30;
  static final int LOG_INFO = 40;

  private static final String TAG = "KodiEngine";
  private static final long POLL_ACTIVE_MS = 200;
  private static final long POLL_IDLE_MS = 1000;
  private static final long DETAILS_INTERVAL_MS = 2000;
  private static final long LOAD_TIMEOUT_MS = 90_000;

  private enum Phase
  {
    IDLE,
    LOADING,
    PLAYING,
    /** mpv's keep-open: the file ended but stays loaded, paused at its end; Kodi has stopped. */
    EOF
  }

  private final Context mContext;
  private final Handler mHandler;
  private final Sink mSink;
  private final FdStreamServer mFdServer;
  private final SettingsMapper mSettings = new SettingsMapper(this);
  private final Map<String, String> mPublished = new HashMap<>();
  private final Map<String, String> mExternalSubs = new HashMap<>();
  private final Runnable mPoll = this::poll;
  private int mRequestId;
  private boolean mReady;

  // Current file
  private Phase mPhase = Phase.IDLE;
  private long mEntryId;
  private String mPath;
  private String mKodiPath;
  private boolean mReopening;
  private String mForcedTitle;
  private long mLoadStarted;
  private long mOpenRequested;
  private boolean mStopRequested;
  private boolean mSawPlayer;
  private boolean mSessionStopped;
  private boolean mRestartPending;
  private int mPlayerId = -1;
  private double mTimePos;
  private double mDuration;
  private int mVideoWidth;
  private int mVideoHeight;
  private long mLastDetails;
  private ParcelFileDescriptor mOpenFd;

  // Player state that survives files, as in mpv
  private boolean mPause;
  private boolean mKeepOpen;
  private double mSpeed = 1.0;
  // What Kodi plays at; it takes tempo changes only while playing, so mSpeed is applied lazily.
  private double mKodiSpeed = 1.0;
  private int mSpeedFailures;
  private long mSpeedRetryAt;
  private double mVolume = 100;
  private boolean mMute;
  private double mAudioDelay;
  private double mSubDelay;
  private boolean mSubVisible = true;
  private String mVid = "auto";
  private String mUserAgent = "";
  private String mReferrer = "";
  private String mHeaderFields = "";
  private boolean mHwdec = true;
  // Picture: mpv's video-zoom (log2), aspect override (<0: none, 0: square pixels) and the
  // equalizer (-100..100); Kodi resets them per file, so they are applied to every file.
  private double mZoom;
  private double mAspectOverride = -1;
  private double mPanscan;
  private int mWindowWidth;
  private int mWindowHeight;
  private double mSourceAspect;
  private final Map<String, Double> mPicture = new LinkedHashMap<>();
  // mpv's A-B loop, done here: Kodi has none.
  private double mLoopA = -1;
  private double mLoopB = -1;
  private double mVolumeMax = 100;
  private double mAmplification;
  // Kodi's per-file video/audio settings set as "kodi-video/<field>" (Player.SetVideoSettings:
  // tonemapmethod, interlacemethod, scalingmethod, sharpness, ...), kept for every file.
  private final Map<String, Object> mKodiVideo = new LinkedHashMap<>();
  // Chapters of the current file, as mpv's chapter-list.
  private JSONArray mChapterList = new JSONArray();
  private boolean mChaptersFetched;

  // Seeking
  private boolean mSeeking;
  private long mSeekStarted;

  PlayerBridge(Context context, Handler handler, Sink sink, FdStreamServer fdServer)
  {
    mContext = context;
    mHandler = handler;
    mSink = sink;
    mFdServer = fdServer;
  }

  // ---- Engine ---------------------------------------------------------------------------------

  void onEngineReady()
  {
    mReady = true;
    JSONObject app = rpc("Application.GetProperties",
                         obj("properties", arr("volume", "muted")));
    if (app != null)
    {
      mVolume = mpvVolume(app.optDouble("volume", 100));
      mMute = app.optBoolean("muted", mMute);
    }
    // Kodi has no mpv audio/video filters; the lists stay empty.
    publish("af", "[]");
    publish("vf", "[]");
    publishIdle();
    schedulePoll(0);
  }

  /** Sends every known property again, for a client that (re)connected. */
  void republish()
  {
    Map<String, String> copy = new LinkedHashMap<>(mPublished);
    for (Map.Entry<String, String> e : copy.entrySet())
      mSink.property(e.getKey(), e.getValue());
  }

  boolean isPlaying()
  {
    return mPhase != Phase.IDLE;
  }

  // ---- Kodi media session (called on Kodi's threads) ------------------------------------------

  @Override
  public void onSessionActive(boolean active)
  {
    final long at = SystemClock.elapsedRealtime();
    mHandler.post(() -> {
      if (!active)
        onKodiStopped(at);
      schedulePoll(0);
    });
  }

  @Override
  public void onPlaybackState(PlaybackState state)
  {
    final int s = state != null ? state.getState() : PlaybackState.STATE_NONE;
    final long at = SystemClock.elapsedRealtime();
    mHandler.post(() -> {
      if (s == PlaybackState.STATE_STOPPED)
        onKodiStopped(at);
      schedulePoll(0);
    });
  }

  /** Kodi stopped playing; only stops after the current file was requested count. */
  private void onKodiStopped(long at)
  {
    if (mOpenRequested > 0 && at >= mOpenRequested)
      mSessionStopped = true;
  }

  @Override
  public void onMetadata(MediaMetadata metadata)
  {
    mHandler.post(() -> schedulePoll(0));
  }

  // ---- Commands -------------------------------------------------------------------------------

  void command(String[] args)
  {
    if (args == null || args.length == 0)
      return;
    String name = args[0];
    try
    {
      switch (name)
      {
        case "loadfile":
          loadfile(args);
          break;
        case "stop":
          stop();
          break;
        case "seek":
          seek(arg(args, 1), arg(args, 2));
          break;
        case "set":
          if (args.length >= 3)
            setProperty(args[1], args[2]);
          break;
        case "cycle":
          cycle(arg(args, 1), arg(args, 2));
          break;
        case "add":
          add(arg(args, 1), parseDouble(arg(args, 2), 1));
          break;
        case "multiply":
          if ("speed".equals(arg(args, 1)))
            setSpeed(mSpeed * parseDouble(arg(args, 2), 1));
          break;
        case "sub-add":
          subAdd(arg(args, 1), arg(args, 3), arg(args, 4));
          break;
        case "sub-remove":
        case "sub-reload":
          break;
        case "frame-step":
          action("frameadvance");
          break;
        case "playlist-next":
          stop();
          break;
        case "quit":
        case "quit-watch-later":
          stop();
          break;
        default:
          // Commands for mpv features Kodi does not have (filters, scripts, OSD, key bindings).
          Log.v(TAG, "ignoring command " + name);
          break;
      }
    }
    catch (RuntimeException e)
    {
      Log.e(TAG, "command " + name + " failed", e);
      mSink.log(LOG_ERROR, "command " + name + " failed: " + e);
    }
  }

  void setProperty(String name, String value)
  {
    if (name == null || value == null)
      return;
    switch (name)
    {
      case "pause":
        setPause(parseFlag(value, mPause));
        break;
      case "time-pos":
      case "playback-time":
        seekAbsolute(parseDouble(value, mTimePos));
        break;
      case "percent-pos":
        seekPercent(parseDouble(value, 0));
        break;
      case "volume":
        setVolume(parseDouble(value, mVolume));
        break;
      case "volume-max":
        mVolumeMax = Math.max(100, Math.min(1000, parseDouble(value, mVolumeMax)));
        publish("volume-max", num(mVolumeMax));
        if (mVolume > mVolumeMax)
          setVolume(mVolumeMax);
        break;
      case "chapter":
        seekChapter(parseInt(value));
        break;
      case "mute":
        setMute(parseFlag(value, mMute));
        break;
      case "speed":
        setSpeed(parseDouble(value, mSpeed));
        break;
      case "aid":
        selectAudio(value);
        break;
      case "sid":
        selectSubtitle(value);
        break;
      case "vid":
        selectVideo(value);
        break;
      case "sub-visibility":
        setSubVisible(parseFlag(value, mSubVisible));
        break;
      case "audio-delay":
        setAudioDelay(parseDouble(value, mAudioDelay));
        break;
      case "sub-delay":
        setSubDelay(parseDouble(value, mSubDelay));
        break;
      case "force-media-title":
        mForcedTitle = value.isEmpty() ? null : value;
        if (mPhase != Phase.IDLE)
          publish("media-title", mediaTitle(null));
        break;
      case "user-agent":
        mUserAgent = value;
        break;
      case "referrer":
        mReferrer = value;
        break;
      case "http-header-fields":
        mHeaderFields = value;
        break;
      case "keep-open":
        mKeepOpen = !"no".equals(value);
        break;
      case "hwdec":
        setHwdec(value);
        break;
      case "video-zoom":
        mZoom = parseDouble(value, 0);
        publish("video-zoom", num(mZoom));
        applyViewMode();
        break;
      case "video-aspect-override":
        mAspectOverride = parseAspect(value);
        publish("video-aspect-override", num(mAspectOverride));
        applyViewMode();
        break;
      case "panscan":
        mPanscan = Math.max(0, Math.min(1, parseDouble(value, 0)));
        publish("panscan", num(mPanscan));
        applyViewMode();
        break;
      case "brightness":
      case "contrast":
        mPicture.put(name, Math.max(-100, Math.min(100, parseDouble(value, 0))));
        publish(name, num(mPicture.get(name)));
        applyPicture();
        break;
      case "ab-loop-a":
        mLoopA = loopPoint(value);
        publish("ab-loop-a", mLoopA >= 0 ? num(mLoopA) : "no");
        break;
      case "ab-loop-b":
        mLoopB = loopPoint(value);
        publish("ab-loop-b", mLoopB >= 0 ? num(mLoopB) : "no");
        break;
      default:
        if (name.startsWith("kodi-video/"))
        {
          mKodiVideo.put(name.substring("kodi-video/".length()), jsonValue(value));
          publish(name, value);
          applyPicture();
          break;
        }
        // Subtitle style and audio output are Kodi settings; other mpv options and properties
        // (vo, shaders, filters, scripts, ...) have no Kodi equivalent.
        mSettings.set(name, value);
        break;
    }
    // The client shows a written value at once; send the resulting one even when it is unchanged
    // (Kodi clamped or refused the value), so the client never keeps a value Kodi does not have.
    if (mPublished.containsKey(name))
      mSink.property(name, mPublished.get(name));
  }

  // ---- loadfile / stop ------------------------------------------------------------------------

  private void loadfile(String[] args)
  {
    String url = arg(args, 1);
    if (url == null || url.isEmpty())
      return;
    String flags = arg(args, 2);
    if (flags != null && flags.startsWith("append"))
    {
      Log.w(TAG, "loadfile " + flags + " is not supported; the host owns the playlist");
      return;
    }
    // mpv >= 0.38: loadfile <url> <flags> <index> <options>; older: loadfile <url> <flags> <options>
    String options = null;
    if (args.length >= 5)
      options = args[4];
    else if (args.length == 4 && !isInteger(args[3]))
      options = args[3];
    Map<String, String> opts = parseOptionList(options);

    if (mPhase != Phase.IDLE)
      endFile("stop", null, false);
    // Kodi opens files asynchronously; with nothing else playing, any player that becomes
    // active afterwards plays this file.
    stopKodiPlayback();
    // Descriptors served for earlier files; this file's was registered just before.
    mFdServer.releaseExcept(url);

    mEntryId++;
    mPath = url;
    mKodiPath = null;
    mReopening = false;
    mForcedTitle = opts.containsKey("force-media-title") ? opts.get("force-media-title") : mForcedTitle;
    mLoadStarted = SystemClock.elapsedRealtime();
    mOpenRequested = 0;
    mStopRequested = false;
    mSawPlayer = false;
    mSessionStopped = false;
    mRestartPending = false;
    mSeeking = false;
    mTimePos = 0;
    mDuration = 0;
    mVideoWidth = 0;
    mVideoHeight = 0;
    mLastDetails = 0;
    mKodiSpeed = 1.0;
    mSpeedFailures = 0;
    mSpeedRetryAt = 0;
    mChapterList = new JSONArray();
    mChaptersFetched = false;
    mAmplification = 0;
    mExternalSubs.clear();
    if (opts.containsKey("pause"))
      mPause = parseFlag(opts.get("pause"), mPause);
    mPhase = Phase.LOADING;

    publish("idle-active", "no");
    publish("core-idle", "yes");
    publish("eof-reached", "no");
    publish("path", url);
    publish("filename", fileName(url));
    publish("media-title", mediaTitle(null));
    publish("demuxer-via-network", isNetwork(url) ? "yes" : "no");
    publish("playlist-count", "1");
    publish("playlist-pos", "0");
    publish("pause", flag(mPause));
    mSink.event(EVENT_START_FILE, obj("playlist_entry_id", mEntryId));

    String kodiPath;
    try
    {
      kodiPath = kodiPath(url, opts);
    }
    catch (IOException | RuntimeException e)
    {
      endFile("error", "Cannot open " + url + ": " + e.getMessage(), true);
      return;
    }

    JSONObject options2 = new JSONObject();
    put(options2, "resume", startPosition(opts.get("start")));
    mKodiPath = kodiPath;
    JSONObject item = obj("file", kodiPath);
    mOpenRequested = SystemClock.elapsedRealtime();
    if (rpc("Player.Open", obj("item", item, "options", options2)) == null)
    {
      endFile("error", "Kodi refused to open " + url, true);
      return;
    }
    schedulePoll(100);
  }

  private void stopKodiPlayback()
  {
    int playerId = activePlayer();
    if (playerId < 0)
      return;
    rpc("Player.Stop", obj("playerid", playerId));
    long deadline = SystemClock.elapsedRealtime() + 5000;
    while (activePlayer() >= 0 && SystemClock.elapsedRealtime() < deadline)
      SystemClock.sleep(50);
    // Let Kodi's stop announcements for the previous file arrive before the next open.
    SystemClock.sleep(100);
  }

  private Object startPosition(String start)
  {
    if (start == null || start.isEmpty() || "none".equals(start))
      return false;
    try
    {
      if (start.endsWith("%"))
        return Double.parseDouble(start.substring(0, start.length() - 1));
      double seconds = Double.parseDouble(start.startsWith("+") ? start.substring(1) : start);
      return seconds > 0 ? timeObject(seconds) : false;
    }
    catch (NumberFormatException e)
    {
      return false;
    }
  }

  private void stop()
  {
    if (mPhase == Phase.IDLE)
      return;
    if (mPhase == Phase.EOF)
    {
      mFdServer.release(mPath);
      endFile("stop", null, true);
      return;
    }
    mStopRequested = true;
    if (mPlayerId >= 0)
      rpc("Player.Stop", obj("playerid", mPlayerId));
    else
      rpc("Player.Stop", obj("playerid", 1));
    schedulePoll(50);
  }

  /** Ends the current file as mpv does: END_FILE with a reason, then IDLE. */
  private void endFile(String reason, String error, boolean idle)
  {
    if (mPhase == Phase.IDLE)
      return;
    Log.i(TAG, "end of file: " + reason + (error != null ? " (" + error + ")" : ""));
    if (error != null)
      mSink.log(LOG_ERROR, error);
    mPhase = Phase.IDLE;
    mSeeking = false;
    mReopening = false;
    mPlayerId = -1;
    mOpenRequested = 0;
    closeFd();
    JSONObject data = obj("reason", reason, "playlist_entry_id", mEntryId);
    if (error != null)
    {
      put(data, "file_error", error);
      put(data, "error", -13);
    }
    publish("eof-reached", "eof".equals(reason) ? "yes" : "no");
    mSink.event(EVENT_END_FILE, data);
    if (idle)
    {
      publishIdle();
      mSink.event(EVENT_IDLE, null);
    }
  }

  /**
   * mpv's keep-open: the ended file stays loaded, paused at its end, so the client can seek back
   * into it. Kodi itself has stopped; {@link #reopenAt} opens the file again when needed.
   */
  private void holdAtEof()
  {
    Log.i(TAG, "end of file: eof, kept open");
    mPhase = Phase.EOF;
    mPlayerId = -1;
    mSeeking = false;
    mRestartPending = false;
    mPause = true;
    if (mDuration > 0)
      mTimePos = mDuration;
    publish("seeking", "no");
    publish("paused-for-cache", "no");
    publish("time-pos", num(mTimePos));
    publish("playback-time", num(mTimePos));
    if (mDuration > 0)
    {
      publish("percent-pos", "100");
      publish("time-remaining", "0");
      publish("playtime-remaining", "0");
    }
    publish("pause", "yes");
    publish("core-idle", "yes");
    publish("eof-reached", "yes");
  }

  /** A seek in a file kept open at its end: Kodi plays the file again from there. */
  private void reopenAt(double seconds)
  {
    double target = Math.max(0, seconds);
    if (mKodiPath == null || (mDuration > 0 && target >= mDuration - 0.5))
    {
      // Still at the end, as mpv would be.
      mSink.event(EVENT_SEEK, null);
      mSink.event(EVENT_PLAYBACK_RESTART, null);
      return;
    }
    Log.i(TAG, "reopening " + mPath + " at " + num(target));
    mPhase = Phase.LOADING;
    mReopening = true;
    mLoadStarted = SystemClock.elapsedRealtime();
    mStopRequested = false;
    mSawPlayer = false;
    mSessionStopped = false;
    mRestartPending = false;
    mKodiSpeed = 1.0;
    mSpeedFailures = 0;
    mTimePos = target;
    publish("eof-reached", "no");
    publish("seeking", "yes");
    publish("time-pos", num(target));
    publish("playback-time", num(target));
    if (mDuration > 0)
      publish("percent-pos", num(target * 100 / mDuration));
    mSink.event(EVENT_SEEK, null);
    JSONObject options = obj("resume", target > 0 ? timeObject(target) : false);
    mOpenRequested = SystemClock.elapsedRealtime();
    if (rpc("Player.Open", obj("item", obj("file", mKodiPath), "options", options)) == null)
    {
      mFdServer.release(mPath);
      endFile("error", "Kodi could not open " + mPath + " again", true);
      return;
    }
    schedulePoll(100);
  }

  private void publishIdle()
  {
    publish("idle-active", "yes");
    publish("core-idle", "yes");
    publish("pause", flag(mPause));
    publish("seeking", "no");
    publish("paused-for-cache", "no");
    publish("time-pos", null);
    publish("playback-time", null);
    publish("percent-pos", null);
    publish("duration", null);
    publish("time-remaining", null);
    publish("playtime-remaining", null);
    publish("demuxer-cache-time", null);
    publish("demuxer-cache-duration", null);
    publish("track-list", "[]");
    publish("current-tracks", "{}");
    publish("chapter-list", "[]");
    publish("chapters", "0");
    publish("chapter", null);
    publish("aid", "no");
    publish("sid", "no");
    publish("vid", "no");
    publish("secondary-sid", "no");
    publish("video-params", null);
    publish("video-out-params", null);
    publish("vo-configured", "no");
    publish("audio-params", null);
    publish("playlist-count", "0");
    publish("playlist-pos", "-1");
    publish("volume", num(mVolume));
    publish("volume-max", num(mVolumeMax));
    publish("mute", flag(mMute));
    publish("speed", num(mSpeed));
    publish("audio-delay", num(mAudioDelay));
    publish("sub-delay", num(mSubDelay));
    publish("sub-visibility", flag(mSubVisible));
    publish("hwdec-current", "no");
    publish("seekable", "no");
  }

  // ---- Polling ----------------------------------------------------------------------------------

  private void schedulePoll(long delayMs)
  {
    mHandler.removeCallbacks(mPoll);
    mHandler.postDelayed(mPoll, delayMs);
  }

  private void poll()
  {
    if (!mReady)
      return;
    try
    {
      pollOnce();
    }
    catch (RuntimeException e)
    {
      Log.e(TAG, "poll failed", e);
    }
    schedulePoll(mPhase == Phase.IDLE || mPhase == Phase.EOF ? POLL_IDLE_MS : POLL_ACTIVE_MS);
  }

  private void pollOnce()
  {
    if (mPhase == Phase.IDLE || mPhase == Phase.EOF)
    {
      pollVolume();
      return;
    }

    int playerId = activePlayer();
    long now = SystemClock.elapsedRealtime();
    if (playerId < 0)
    {
      if (mPhase == Phase.LOADING)
      {
        if (mStopRequested)
          endFile("stop", null, true);
        else if (mSawPlayer || mSessionStopped)
          endFile("error", "Kodi could not play " + mPath, true);
        else if (now - mLoadStarted > LOAD_TIMEOUT_MS)
          endFile("error", "Timed out opening " + mPath, true);
        return;
      }
      String reason;
      if (mStopRequested)
        reason = "stop";
      else if (mDuration > 0 && mTimePos < mDuration - 10)
        reason = "error";
      else
        reason = "eof";
      if ("eof".equals(reason) && mKeepOpen)
      {
        holdAtEof();
        return;
      }
      // Only this file's descriptor: the client may already have sent the next one.
      mFdServer.release(mPath);
      endFile(reason, "error".equals(reason) ? "Playback stopped unexpectedly" : null, true);
      return;
    }
    mSawPlayer = true;
    mPlayerId = playerId;

    JSONObject props = rpc("Player.GetProperties", obj("playerid", playerId, "properties",
        arr("time", "totaltime", "percentage", "speed", "canseek", "live", "cachepercentage")));
    JSONObject bools = rpc("XBMC.GetInfoBooleans", obj("booleans",
        arr("Player.Playing", "Player.Paused", "Player.Caching", "Player.Seeking",
            "Player.HasVideo", "Player.HasAudio")));
    if (props == null || bools == null)
      return;

    mTimePos = seconds(props.optJSONObject("time"));
    mDuration = seconds(props.optJSONObject("totaltime"));
    boolean caching = bools.optBoolean("Player.Caching");
    boolean kodiSeeking = bools.optBoolean("Player.Seeking");
    boolean playing = bools.optBoolean("Player.Playing");
    boolean paused = bools.optBoolean("Player.Paused");
    boolean hasMedia = bools.optBoolean("Player.HasVideo") || bools.optBoolean("Player.HasAudio");

    if (mPhase == Phase.LOADING)
    {
      if (!hasMedia || (!playing && !paused))
        return;
      paused = fileLoaded(playerId, props) || paused;
    }

    publish("seekable", flag(props.optBoolean("canseek", true)));
    publishTimeline(props);
    publish("paused-for-cache", flag(caching));
    if (!mSeeking)
    {
      // Kodi may pause on its own (audio focus loss); mpv's pause reflects the actual state.
      mPause = paused;
      publish("pause", flag(paused));
    }
    publish("core-idle", flag(mPause || caching));
    if (playing && !paused)
      applySpeed();
    if (mLoopA >= 0 && mLoopB > mLoopA && !mSeeking && mTimePos >= mLoopB)
      seekAbsolute(mLoopA);

    if (mSeeking && !kodiSeeking && now - mSeekStarted > 150)
    {
      mSeeking = false;
      publish("seeking", "no");
      mSink.event(EVENT_PLAYBACK_RESTART, null);
    }
    if (mRestartPending && !caching)
    {
      mRestartPending = false;
      mSink.event(EVENT_PLAYBACK_RESTART, null);
    }
    if (now - mLastDetails > DETAILS_INTERVAL_MS)
      pollDetails(playerId);
  }

  /** Returns true when the file was paused as requested by the client. */
  private boolean fileLoaded(int playerId, JSONObject props)
  {
    mPhase = Phase.PLAYING;
    boolean reopened = mReopening;
    mReopening = false;
    Log.i(TAG, (reopened ? "file reopened: " : "file loaded: ") + mPath);
    mDuration = seconds(props.optJSONObject("totaltime"));
    publish("duration", mDuration > 0 ? num(mDuration) : null);
    pollDetails(playerId);
    // To the client, a file reopened after keep-open's end was loaded all along.
    if (!reopened)
    {
      mSink.event(EVENT_FILE_LOADED, null);
      mSink.event(EVENT_VIDEO_RECONFIG, null);
      mSink.event(EVENT_AUDIO_RECONFIG, null);
    }
    if (mPause)
      rpc("Player.PlayPause", obj("playerid", playerId, "play", false));
    applyPlaybackSettings(playerId);
    if (reopened)
      publish("seeking", "no");
    mRestartPending = true;
    return mPause;
  }

  /** Re-applies mpv properties that Kodi resets per file. */
  private void applyPlaybackSettings(int playerId)
  {
    mKodiSpeed = 1.0;
    applySpeed();
    applyViewMode();
    applyPicture();
    mAmplification = 0;
    applyAmplification();
    if (Math.abs(mAudioDelay) > 0.001)
      rpc("Player.SetAudioDelay", obj("playerid", playerId, "offset", mAudioDelay));
    if (!mSubVisible)
      rpc("Player.SetSubtitle", obj("playerid", playerId, "subtitle", "off"));
  }

  private void publishTimeline(JSONObject props)
  {
    if (mSeeking)
      return;
    publish("time-pos", num(mTimePos));
    publish("playback-time", num(mTimePos));
    if (mDuration > 0)
    {
      publish("duration", num(mDuration));
      publish("percent-pos", num(props.optDouble("percentage", mTimePos * 100 / mDuration)));
      double remaining = Math.max(0, mDuration - mTimePos);
      publish("time-remaining", num(remaining));
      publish("playtime-remaining", num(remaining / Math.max(mSpeed, 0.01)));
      double cached = mDuration * props.optDouble("cachepercentage", 0) / 100;
      if (cached > 0)
      {
        publish("demuxer-cache-time", num(cached));
        publish("demuxer-cache-duration", num(Math.max(0, cached - mTimePos)));
      }
    }
    else
    {
      publish("duration", null);
      publish("percent-pos", null);
    }
  }

  private void pollDetails(int playerId)
  {
    mLastDetails = SystemClock.elapsedRealtime();
    JSONObject streams = rpc("Player.GetProperties", obj("playerid", playerId, "properties",
        arr("audiostreams", "currentaudiostream", "subtitles", "currentsubtitle",
            "subtitleenabled", "videostreams", "currentvideostream")));
    JSONObject labels = rpc("XBMC.GetInfoLabels", obj("labels", arr(
        "Player.Process(videowidth)", "Player.Process(videoheight)", "Player.Process(videodar)",
        "Player.Process(videofps)", "Player.Process(videodecoder)", "Player.Process(pixformat)",
        "Player.Process(videohwdecoder)", "Player.Process(audiodecoder)",
        "Player.Process(audiochannels)", "Player.Process(audiosamplerate)",
        "Player.ChapterCount", "Player.Chapter", "Player.Chapters", "Player.Title",
        "VideoPlayer.Title", "VideoPlayer.HdrType")));
    if (streams != null)
      publishTracks(streams);
    if (labels != null)
      publishMediaDetails(labels);
    pollVolume();
  }

  private void publishTracks(JSONObject s)
  {
    JSONArray tracks = new JSONArray();
    JSONObject current = new JSONObject();
    int currentVideo = index(s.optJSONObject("currentvideostream"));
    int currentAudio = index(s.optJSONObject("currentaudiostream"));
    int currentSub = s.optBoolean("subtitleenabled") ? index(s.optJSONObject("currentsubtitle")) : -1;

    JSONArray video = s.optJSONArray("videostreams");
    for (int i = 0; video != null && i < video.length(); i++)
    {
      JSONObject v = video.optJSONObject(i);
      if (v == null)
        continue;
      int idx = v.optInt("index", i);
      JSONObject t = track(idx, "video", v.optString("name"), v.optString("language"),
                           v.optString("codec"), idx == currentVideo);
      if (idx == currentVideo)
      {
        mVideoWidth = v.optInt("width");
        mVideoHeight = v.optInt("height");
      }
      put(t, "demux-w", v.optInt("width"));
      put(t, "demux-h", v.optInt("height"));
      put(t, "image", false);
      put(t, "albumart", false);
      tracks.put(t);
      if (idx == currentVideo)
        put(current, "video", t);
    }
    JSONArray audio = s.optJSONArray("audiostreams");
    for (int i = 0; audio != null && i < audio.length(); i++)
    {
      JSONObject a = audio.optJSONObject(i);
      if (a == null)
        continue;
      int idx = a.optInt("index", i);
      JSONObject t = track(idx, "audio", a.optString("name"), a.optString("language"),
                           a.optString("codec"), idx == currentAudio);
      put(t, "default", a.optBoolean("isdefault"));
      put(t, "demux-channel-count", a.optInt("channels"));
      put(t, "audio-channels", a.optInt("channels"));
      put(t, "demux-samplerate", a.optInt("samplerate"));
      put(t, "demux-bitrate", a.optInt("bitrate"));
      put(t, "hearing-impaired", a.optBoolean("isimpaired"));
      tracks.put(t);
      if (idx == currentAudio)
        put(current, "audio", t);
    }
    JSONArray subs = s.optJSONArray("subtitles");
    for (int i = 0; subs != null && i < subs.length(); i++)
    {
      JSONObject u = subs.optJSONObject(i);
      if (u == null)
        continue;
      int idx = u.optInt("index", i);
      String name = u.optString("name");
      JSONObject t = track(idx, "sub", name, u.optString("language"), "", idx == currentSub);
      put(t, "default", u.optBoolean("isdefault"));
      put(t, "forced", u.optBoolean("isforced"));
      put(t, "hearing-impaired", u.optBoolean("isimpaired"));
      String external = externalSubtitle(name);
      put(t, "external", external != null);
      if (external != null)
        put(t, "external-filename", external);
      tracks.put(t);
      if (idx == currentSub)
        put(current, "sub", t);
    }

    publish("track-list", tracks.toString());
    publish("current-tracks", current.toString());
    publish("vid", "no".equals(mVid) ? "no" : currentVideo >= 0 ? String.valueOf(currentVideo + 1) : "no");
    publish("aid", currentAudio >= 0 ? String.valueOf(currentAudio + 1) : "no");
    publish("sid", currentSub >= 0 ? String.valueOf(currentSub + 1) : "no");
    publish("secondary-sid", "no");
    publish("sub-visibility", flag(mSubVisible));
  }

  private static JSONObject track(int index, String type, String title, String lang, String codec,
                                  boolean selected)
  {
    JSONObject t = obj("id", index + 1, "type", type, "src-id", index, "selected", selected,
                       "external", false, "default", false, "forced", false);
    if (title != null && !title.isEmpty())
      put(t, "title", title);
    if (lang != null && !lang.isEmpty())
      put(t, "lang", lang);
    if (codec != null && !codec.isEmpty())
      put(t, "codec", codec);
    return t;
  }

  private void publishMediaDetails(JSONObject l)
  {
    // The stream's size; Kodi's "videowidth" is the decoder's (aligned) buffer width.
    int w = mVideoWidth > 0 ? mVideoWidth : parseInt(l.optString("Player.Process(videowidth)"));
    int h = mVideoHeight > 0 ? mVideoHeight : parseInt(l.optString("Player.Process(videoheight)"));
    double dar = parseDouble(l.optString("Player.Process(videodar)"), 0);
    if (w > 0 && h > 0)
    {
      double aspect = dar > 0 ? dar : (double) w / h;
      boolean aspectChanged = Math.abs(aspect - mSourceAspect) > 0.001;
      mSourceAspect = aspect;
      // The custom view modes depend on the source aspect, which Kodi reports after the file
      // started.
      if (aspectChanged && (mPanscan > 0 || mAspectOverride >= 0))
        applyViewMode();
      int dw = (int) Math.round(h * aspect);
      JSONObject params = obj("w", w, "h", h, "dw", dw, "dh", h, "aspect", aspect,
                              "par", aspect * h / w, "rotate", 0);
      String pixfmt = l.optString("Player.Process(pixformat)");
      if (!pixfmt.isEmpty())
        put(params, "pixelformat", pixfmt.toLowerCase(Locale.ROOT));
      putHdr(params, l.optString("VideoPlayer.HdrType"));
      publish("video-params", params.toString());
      // Kodi applies no video filters, so what is displayed is the stream itself.
      publish("video-out-params", params.toString());
      publish("vo-configured", "yes");
      publish("width", String.valueOf(w));
      publish("height", String.valueOf(h));
      publish("dwidth", String.valueOf(dw));
      publish("dheight", String.valueOf(h));
    }
    else
    {
      publish("video-params", null);
      publish("video-out-params", null);
      publish("vo-configured", "no");
    }
    double fps = parseDouble(l.optString("Player.Process(videofps)"), 0);
    publish("container-fps", fps > 0 ? num(fps) : null);
    publish("estimated-vf-fps", fps > 0 ? num(fps) : null);
    String decoder = l.optString("Player.Process(videodecoder)");
    publish("video-codec", decoder.isEmpty() ? null : decoder);
    publish("hwdec-current", "true".equalsIgnoreCase(l.optString("Player.Process(videohwdecoder)"))
                             ? "mediacodec" : "no");
    String audioDecoder = l.optString("Player.Process(audiodecoder)");
    publish("audio-codec-name", audioDecoder.isEmpty() ? null : audioDecoder);
    int channels = parseInt(l.optString("Player.Process(audiochannels)").replaceAll("[^0-9].*", ""));
    int rate = parseInt(l.optString("Player.Process(audiosamplerate)"));
    if (rate > 0)
      publish("audio-params", obj("samplerate", rate, "channel-count", Math.max(channels, 0)).toString());
    int chapters = parseInt(l.optString("Player.ChapterCount"));
    publish("chapters", String.valueOf(Math.max(chapters, 0)));
    int chapter = parseInt(l.optString("Player.Chapter"));
    publish("chapter", chapters > 0 && chapter > 0 ? String.valueOf(chapter - 1) : null);
    if (chapters > 0 && !mChaptersFetched && mDuration > 0)
      fetchChapters(chapters, l.optString("Player.Chapters"));
    publish("chapter-list", mChapterList.toString());
    String title = l.optString("VideoPlayer.Title");
    if (title.isEmpty())
      title = l.optString("Player.Title");
    publish("media-title", mediaTitle(title));
  }

  /** Kodi's VideoPlayer.HdrType as mpv's colour parameters (what mpv clients show HDR badges by). */
  private static void putHdr(JSONObject params, String hdrType)
  {
    switch (hdrType == null ? "" : hdrType)
    {
      case "hdr10":
        put(params, "gamma", "pq");
        put(params, "primaries", "bt.2020");
        put(params, "colormatrix", "bt.2020-ncl");
        break;
      case "hlg":
        put(params, "gamma", "hlg");
        put(params, "primaries", "bt.2020");
        put(params, "colormatrix", "bt.2020-ncl");
        break;
      case "dolbyvision":
        put(params, "gamma", "pq");
        put(params, "primaries", "bt.2020");
        put(params, "colormatrix", "dolbyvision");
        break;
      default:
        break;
    }
  }

  /**
   * mpv's chapter-list ([{title, time}]). Player.GetChapters (engine patch) has names and times;
   * otherwise the chapter starts come from Player.Chapters' percentages.
   */
  private void fetchChapters(int count, String ranges)
  {
    mChaptersFetched = true;
    JSONArray list = new JSONArray();
    Object result = call("Player.GetChapters", obj("playerid", mPlayerId));
    JSONArray chapters = result instanceof JSONArray ? (JSONArray) result
        : result instanceof JSONObject ? ((JSONObject) result).optJSONArray("chapters") : null;
    for (int i = 0; chapters != null && i < chapters.length(); i++)
    {
      JSONObject c = chapters.optJSONObject(i);
      if (c == null)
        continue;
      JSONObject entry = obj("time", c.optDouble("time", 0));
      String title = c.optString("title", c.optString("name"));
      if (!title.isEmpty())
        put(entry, "title", title);
      list.put(entry);
    }
    if (list.length() == 0 && ranges != null && !ranges.isEmpty())
    {
      // "start,end,start,end,..." in percent; a chapter at 0 has no range of its own.
      List<Double> starts = new ArrayList<>();
      String[] values = ranges.split(",");
      for (int i = 0; i + 1 < values.length; i += 2)
      {
        starts.add(parseDouble(values[i], 0));
        if (i + 2 >= values.length - 1)
          starts.add(parseDouble(values[i + 1], 0));
      }
      for (int i = 0; i < starts.size() && i < count; i++)
        list.put(obj("time", Math.round(mDuration * starts.get(i) * 10) / 1000.0));
    }
    mChapterList = list;
  }

  private void seekChapter(int index)
  {
    if (index >= 0 && index < mChapterList.length())
    {
      JSONObject c = mChapterList.optJSONObject(index);
      if (c != null)
      {
        seekAbsolute(c.optDouble("time", 0));
        return;
      }
    }
    int current = parseInt(mPublished.get("chapter"));
    if (index != current)
      action(index > current ? "chapterorbigstepforward" : "chapterorbigstepback");
  }

  private void pollVolume()
  {
    JSONObject app = rpc("Application.GetProperties", obj("properties", arr("volume", "muted")));
    if (app == null)
      return;
    double kodi = app.optDouble("volume", kodiVolume(mVolume));
    // Above 100, mpv's volume is Kodi's full volume plus amplification.
    if (!(mVolume > 100 && kodi >= 99.5) && Math.abs(kodi - kodiVolume(mVolume)) > 0.5)
      mVolume = mpvVolume(kodi);
    mMute = app.optBoolean("muted", mMute);
    publish("volume", num(mVolume));
    publish("mute", flag(mMute));
  }

  /**
   * mpv's volume is cubic (gain = (v/100)^3); Kodi's percentage spans 60 dB. Both give the same
   * loudness with Kodi% = 100 * (1 + log10(v/100)).
   */
  private static double kodiVolume(double mpv)
  {
    if (mpv <= 10)
      return 0;
    return Math.min(100, 100 * (1 + Math.log10(mpv / 100)));
  }

  private static double mpvVolume(double kodi)
  {
    if (kodi <= 0)
      return 0;
    return Math.round(100 * Math.pow(10, kodi / 100 - 1) * 10) / 10.0;
  }

  // ---- Playback control ---------------------------------------------------------------------

  private void setPause(boolean pause)
  {
    // Kept open at its end, mpv pauses again at once: there is nothing left to play.
    mPause = pause || mPhase == Phase.EOF;
    publish("pause", flag(mPause));
    if (mPhase == Phase.PLAYING && mPlayerId >= 0)
    {
      rpc("Player.PlayPause", obj("playerid", mPlayerId, "play", !pause));
      if (!pause)
        applySpeed();
    }
    schedulePoll(50);
  }

  private void seek(String value, String flags)
  {
    double amount = parseDouble(value, Double.NaN);
    if (Double.isNaN(amount) || (mPhase != Phase.PLAYING && mPhase != Phase.EOF))
      return;
    String f = flags == null ? "relative" : flags;
    if (f.contains("absolute-percent"))
      seekPercent(amount);
    else if (f.contains("relative-percent"))
      seekPercent((mDuration > 0 ? mTimePos * 100 / mDuration : 0) + amount);
    else if (f.contains("absolute"))
      seekAbsolute(amount);
    else
      seekAbsolute(mTimePos + amount);
  }

  private void seekAbsolute(double seconds)
  {
    if (mPhase == Phase.EOF)
    {
      reopenAt(mDuration > 0 ? Math.min(seconds, mDuration) : seconds);
      return;
    }
    if (mPhase != Phase.PLAYING || mPlayerId < 0)
      return;
    double target = Math.max(0, mDuration > 0 ? Math.min(seconds, mDuration) : seconds);
    beginSeek(target);
    rpc("Player.Seek", obj("playerid", mPlayerId, "value", obj("time", timeObject(target))));
  }

  private void seekPercent(double percent)
  {
    double p = Math.max(0, Math.min(100, percent));
    if (mPhase == Phase.EOF)
    {
      if (mDuration > 0)
        reopenAt(mDuration * p / 100);
      return;
    }
    if (mPhase != Phase.PLAYING || mPlayerId < 0)
      return;
    beginSeek(mDuration > 0 ? mDuration * p / 100 : mTimePos);
    rpc("Player.Seek", obj("playerid", mPlayerId, "value", obj("percentage", p)));
  }

  private void beginSeek(double target)
  {
    mSeeking = true;
    mSeekStarted = SystemClock.elapsedRealtime();
    mTimePos = target;
    publish("seeking", "yes");
    publish("time-pos", num(target));
    publish("playback-time", num(target));
    if (mDuration > 0)
      publish("percent-pos", num(target * 100 / mDuration));
    mSink.event(EVENT_SEEK, null);
    schedulePoll(POLL_ACTIVE_MS);
  }

  private void setVolume(double volume)
  {
    mVolume = Math.max(0, Math.min(mVolumeMax, volume));
    rpc("Application.SetVolume", obj("volume", (int) Math.round(kodiVolume(mVolume))));
    applyAmplification();
    publish("volume", num(mVolume));
  }

  /** mpv's volume above 100 (volume boost) as Kodi's per-file volume amplification, in dB. */
  private void applyAmplification()
  {
    double db = mVolume > 100 ? Math.min(60, 60 * Math.log10(mVolume / 100)) : 0;
    if (mPhase != Phase.PLAYING || mPlayerId < 0 || Math.abs(db - mAmplification) < 0.05)
      return;
    if (rpc("Player.SetVideoSettings", obj("playerid", mPlayerId, "settings",
                                           obj("volumeamplification", db))) != null)
      mAmplification = db;
  }

  private void setMute(boolean mute)
  {
    mMute = mute;
    rpc("Application.SetMute", obj("mute", mute));
    publish("mute", flag(mute));
  }

  /**
   * Kodi plays at other speeds with audio ("tempo") between 0.75 and 2.1 exclusive (the engine
   * raises Kodi's default maximum of 1.55); faster speeds use its silent fast-forward. As in mpv,
   * the speed can be set at any time; Kodi gets it once it plays.
   */
  private void setSpeed(double speed)
  {
    if (!(speed > 0))
      return;
    mSpeed = speed >= 3 ? (speed >= 6 ? 8 : 4) : Math.max(0.76, Math.min(2.09, speed));
    mSpeedFailures = 0;
    mSpeedRetryAt = 0;
    publish("speed", num(mSpeed));
    applySpeed();
  }

  /** Kodi refuses tempo changes while paused or before the player is set up; retries later. */
  private void applySpeed()
  {
    if (mPhase != Phase.PLAYING || mPlayerId < 0 || mPause || Math.abs(mKodiSpeed - mSpeed) < 0.001)
      return;
    long now = SystemClock.elapsedRealtime();
    if (now < mSpeedRetryAt)
      return;
    boolean ok;
    if (mSpeed >= 3)
    {
      ok = rpc("Player.SetSpeed", obj("playerid", mPlayerId, "speed", (int) mSpeed)) != null;
    }
    else
    {
      if (mKodiSpeed >= 3)
      {
        rpc("Player.SetSpeed", obj("playerid", mPlayerId, "speed", 1));
        mKodiSpeed = 1.0;
      }
      ok = Math.abs(mSpeed - mKodiSpeed) < 0.001
           || rpc("Player.SetTempo", obj("playerid", mPlayerId, "tempo", mSpeed)) != null;
    }
    if (ok)
    {
      mKodiSpeed = mSpeed;
      mSpeedFailures = 0;
      return;
    }
    // This stream cannot change speed (live, passthrough audio): report Kodi's actual speed.
    if (++mSpeedFailures >= 5)
    {
      Log.w(TAG, "Kodi cannot play " + mPath + " at speed " + num(mSpeed));
      mSpeed = mKodiSpeed;
      publish("speed", num(mSpeed));
      return;
    }
    mSpeedRetryAt = now + 500;
  }

  private void selectAudio(String value)
  {
    int id = parseInt(value);
    if (id > 0 && mPlayerId >= 0)
      rpc("Player.SetAudioStream", obj("playerid", mPlayerId, "stream", id - 1));
    refreshDetailsSoon();
  }

  private void selectSubtitle(String value)
  {
    int id = parseInt(value);
    if (mPlayerId < 0)
      return;
    if (id > 0)
    {
      mSubVisible = true;
      rpc("Player.SetSubtitle", obj("playerid", mPlayerId, "subtitle", id - 1, "enable", true));
    }
    else if ("no".equals(value) || "0".equals(value))
    {
      rpc("Player.SetSubtitle", obj("playerid", mPlayerId, "subtitle", "off"));
    }
    refreshDetailsSoon();
  }

  private void selectVideo(String value)
  {
    // Kodi cannot run without its video stream; "no" is remembered so mpv clients that detach
    // video while backgrounded read their own value back.
    mVid = value;
    publish("vid", value);
    int id = parseInt(value);
    if (id > 0 && mPlayerId >= 0)
      rpc("Player.SetVideoStream", obj("playerid", mPlayerId, "stream", id - 1));
    refreshDetailsSoon();
  }

  private void setSubVisible(boolean visible)
  {
    mSubVisible = visible;
    if (mPlayerId >= 0)
      rpc("Player.SetSubtitle", obj("playerid", mPlayerId, "subtitle", visible ? "on" : "off"));
    publish("sub-visibility", flag(visible));
    refreshDetailsSoon();
  }

  private void setAudioDelay(double delay)
  {
    mAudioDelay = delay;
    if (mPlayerId >= 0)
      rpc("Player.SetAudioDelay", obj("playerid", mPlayerId, "offset", delay));
    publish("audio-delay", num(delay));
  }

  /** JSON-RPC has no subtitle delay; Kodi's delay actions step by 0.1 s. */
  private void setSubDelay(double delay)
  {
    int steps = (int) Math.round((delay - mSubDelay) / 0.1);
    for (int i = 0; i < Math.abs(steps) && mPlayerId >= 0; i++)
      action(steps > 0 ? "subtitledelayplus" : "subtitledelayminus");
    mSubDelay += steps * 0.1;
    publish("sub-delay", num(mSubDelay));
  }

  /** Kodi picks its decoder when a file opens: the playing file is opened again at its position. */
  private void setHwdec(String value)
  {
    boolean hw = !"no".equals(value);
    publish("hwdec", value);
    if (hw == mHwdec)
      return;
    mHwdec = hw;
    if (rpc("Settings.SetSettingValue", obj("setting", "videoplayer.usemediacodec", "value", hw)) == null)
      return;
    if (mPhase == Phase.PLAYING && mPlayerId >= 0)
    {
      double at = mTimePos;
      stopKodiPlayback();
      mPlayerId = -1;
      mPhase = Phase.EOF;
      reopenAt(at);
    }
  }

  /** mpv's video-zoom and video-aspect-override as Kodi's custom view mode. */
  private void applyViewMode()
  {
    if (mPhase != Phase.PLAYING || mPlayerId < 0)
      return;
    boolean zoomed = Math.abs(mZoom) >= 0.001;
    if (!zoomed && mAspectOverride < 0 && mPanscan <= 0)
    {
      rpc("Player.SetViewMode", obj("viewmode", "normal"));
      return;
    }
    if (!zoomed && mAspectOverride < 0 && mPanscan >= 1)
    {
      // Kodi's own crop to fill, which has no zoom limit.
      rpc("Player.SetViewMode", obj("viewmode", "zoom"));
      return;
    }
    double ratio = 1;
    double target = mAspectOverride == 0 && mVideoHeight > 0
                    ? (double) mVideoWidth / mVideoHeight : mAspectOverride;
    // Kodi shows the source aspect times the pixel ratio.
    if (target > 0 && mSourceAspect > 0)
      ratio = Math.max(0.5, Math.min(2, target / mSourceAspect));
    double zoom = Math.pow(2, mZoom) * panscanZoom(mSourceAspect * ratio);
    rpc("Player.SetViewMode",
        obj("viewmode", obj("zoom", Math.max(0.5, Math.min(2, zoom)), "pixelratio", ratio)));
  }

  /** mpv's panscan: the share of the black bars that the enlarged video covers. */
  private double panscanZoom(double displayAspect)
  {
    if (mPanscan <= 0 || displayAspect <= 0 || mWindowWidth <= 0 || mWindowHeight <= 0)
      return 1;
    double window = (double) mWindowWidth / mWindowHeight;
    double fill = displayAspect > window ? displayAspect / window : window / displayAspect;
    return 1 + mPanscan * (fill - 1);
  }

  /** Size of the GUI layer, which Kodi fits the video in. */
  void setWindowSize(int width, int height)
  {
    if (width <= 0 || height <= 0 || (width == mWindowWidth && height == mWindowHeight))
      return;
    mWindowWidth = width;
    mWindowHeight = height;
    if (mPanscan > 0)
      applyViewMode();
  }

  /**
   * mpv's equalizer (-100..100, 0 = unchanged) as Kodi's per-video settings (0..100, 50), with
   * the client's kodi-video/* settings.
   */
  private void applyPicture()
  {
    if (mPhase != Phase.PLAYING || mPlayerId < 0 || (mPicture.isEmpty() && mKodiVideo.isEmpty()))
      return;
    JSONObject settings = new JSONObject();
    for (Map.Entry<String, Double> e : mPicture.entrySet())
      put(settings, e.getKey(), 50 + e.getValue() / 2);
    for (Map.Entry<String, Object> e : mKodiVideo.entrySet())
      put(settings, e.getKey(), e.getValue());
    rpc("Player.SetVideoSettings", obj("playerid", mPlayerId, "settings", settings));
  }

  /** A property value as JSON: integers stay integers (Kodi's schema tells them apart). */
  private static Object jsonValue(String value)
  {
    if ("true".equals(value) || "yes".equals(value))
      return true;
    if ("false".equals(value) || "no".equals(value))
      return false;
    try
    {
      if (value.matches("-?\\d+"))
        return Long.parseLong(value);
      return Double.parseDouble(value);
    }
    catch (NumberFormatException e)
    {
      return value;
    }
  }

  private static double parseAspect(String value)
  {
    if (value == null || "no".equals(value) || value.startsWith("-"))
      return -1;
    int colon = value.indexOf(':');
    if (colon > 0)
    {
      double w = parseDouble(value.substring(0, colon), 0);
      double h = parseDouble(value.substring(colon + 1), 0);
      return w > 0 && h > 0 ? w / h : -1;
    }
    double ratio = parseDouble(value, -1);
    return ratio >= 0 ? ratio : -1;
  }

  private static double loopPoint(String value)
  {
    if (value == null || "no".equals(value))
      return -1;
    double seconds = parseDouble(value, -1);
    return seconds >= 0 ? seconds : -1;
  }

  private void cycle(String name, String direction)
  {
    if (name == null)
      return;
    switch (name)
    {
      case "pause":
        setPause(!mPause);
        break;
      case "mute":
        setMute(!mMute);
        break;
      case "sub-visibility":
        setSubVisible(!mSubVisible);
        break;
      case "sub":
      case "sid":
        if (mPlayerId >= 0)
          rpc("Player.SetSubtitle", obj("playerid", mPlayerId, "subtitle",
                                        "down".equals(direction) ? "previous" : "next", "enable", true));
        refreshDetailsSoon();
        break;
      case "audio":
      case "aid":
        if (mPlayerId >= 0)
          rpc("Player.SetAudioStream", obj("playerid", mPlayerId, "stream",
                                           "down".equals(direction) ? "previous" : "next"));
        refreshDetailsSoon();
        break;
      default:
        break;
    }
  }

  private void add(String name, double delta)
  {
    if (name == null)
      return;
    switch (name)
    {
      case "volume":
        setVolume(mVolume + delta);
        break;
      case "speed":
        setSpeed(mSpeed + delta);
        break;
      case "audio-delay":
        setAudioDelay(mAudioDelay + delta);
        break;
      case "sub-delay":
        setSubDelay(mSubDelay + delta);
        break;
      case "chapter":
      {
        int current = parseInt(mPublished.get("chapter"));
        int target = Math.max(0, current + (int) Math.round(delta));
        if (mChapterList.length() > 0 && target < mChapterList.length())
          seekChapter(target);
        else
          action(delta >= 0 ? "chapterorbigstepforward" : "chapterorbigstepback");
        break;
      }
      case "time-pos":
        seekAbsolute(mTimePos + delta);
        break;
      default:
        break;
    }
  }

  private void subAdd(String url, String title, String lang)
  {
    if (url == null || mPlayerId < 0)
      return;
    String path;
    try
    {
      path = subtitlePath(url, title, lang);
    }
    catch (IOException e)
    {
      mSink.log(LOG_ERROR, "cannot load subtitle " + url + ": " + e.getMessage());
      return;
    }
    mExternalSubs.put(new File(path).getName(), url);
    rpc("Player.AddSubtitle", obj("playerid", mPlayerId, "subtitle", path));
    refreshDetailsSoon();
  }

  private String externalSubtitle(String name)
  {
    if (name == null || name.isEmpty())
      return null;
    for (Map.Entry<String, String> e : mExternalSubs.entrySet())
    {
      String base = e.getKey();
      int dot = base.lastIndexOf('.');
      if (base.equals(name) || (dot > 0 && name.startsWith(base.substring(0, dot))))
        return e.getValue();
    }
    return null;
  }

  private void refreshDetailsSoon()
  {
    mLastDetails = 0;
    schedulePoll(150);
  }

  private void action(String action)
  {
    rpc("Input.ExecuteAction", obj("action", action));
  }

  // ---- Paths ------------------------------------------------------------------------------------

  private String kodiPath(String url, Map<String, String> opts) throws IOException
  {
    Uri uri = Uri.parse(url);
    String scheme = uri.getScheme() == null ? "" : uri.getScheme().toLowerCase(Locale.ROOT);
    switch (scheme)
    {
      case "":
        return url;
      case "file":
        return uri.getPath();
      case "content":
        return contentPath(uri);
      case "http":
      case "https":
        return withHeaders(url, opts);
      default:
        return url;
    }
  }

  /** content:// URIs: the file's path when MediaStore knows it, else the open descriptor. */
  private String contentPath(Uri uri) throws IOException
  {
    ContentResolver resolver = mContext.getContentResolver();
    try (Cursor c = resolver.query(uri, new String[] {MediaStore.MediaColumns.DATA}, null, null, null))
    {
      if (c != null && c.moveToFirst())
      {
        String data = c.getString(0);
        if (data != null && new File(data).canRead())
          return data;
      }
    }
    catch (RuntimeException ignored)
    {
    }
    closeFd();
    mOpenFd = resolver.openFileDescriptor(uri, "r");
    if (mOpenFd == null)
      throw new IOException("no descriptor for " + uri);
    return "/proc/self/fd/" + mOpenFd.getFd();
  }

  /** Kodi takes request headers as "url|Name=value&Name2=value2". */
  private String withHeaders(String url, Map<String, String> opts)
  {
    Map<String, String> headers = new LinkedHashMap<>();
    String ua = opts.containsKey("user-agent") ? opts.get("user-agent") : mUserAgent;
    if (ua != null && !ua.isEmpty())
      headers.put("User-Agent", ua);
    String ref = opts.containsKey("referrer") ? opts.get("referrer") : mReferrer;
    if (ref != null && !ref.isEmpty())
      headers.put("Referer", ref);
    String fields = opts.containsKey("http-header-fields") ? opts.get("http-header-fields") : mHeaderFields;
    for (String field : splitList(fields))
    {
      int colon = field.indexOf(':');
      if (colon > 0)
        headers.put(field.substring(0, colon).trim(), field.substring(colon + 1).trim());
    }
    if (headers.isEmpty())
      return url;
    StringBuilder sb = new StringBuilder(url).append('|');
    boolean first = true;
    for (Map.Entry<String, String> e : headers.entrySet())
    {
      if (!first)
        sb.append('&');
      first = false;
      sb.append(encode(e.getKey())).append('=').append(encode(e.getValue()));
    }
    return sb.toString();
  }

  /** Kodi names external subtitles after their file, so copy content:// subtitles to a file. */
  private String subtitlePath(String url, String title, String lang) throws IOException
  {
    Uri uri = Uri.parse(url);
    String scheme = uri.getScheme() == null ? "" : uri.getScheme().toLowerCase(Locale.ROOT);
    if (scheme.isEmpty())
      return url;
    if ("file".equals(scheme))
      return uri.getPath();
    if (!"content".equals(scheme))
      return url;
    String name = fileName(url);
    String ext = name.contains(".") ? name.substring(name.lastIndexOf('.')) : ".srt";
    String base = title != null && !title.isEmpty() ? title : name.replaceAll("\\.[^.]*$", "");
    base = base.replaceAll("[^\\w .-]", "_");
    if (lang != null && !lang.isEmpty())
      base = base + "." + lang;
    File dir = new File(mContext.getCacheDir(), "kodi-subtitles");
    if (!dir.isDirectory() && !dir.mkdirs())
      throw new IOException("cannot create " + dir);
    File out = new File(dir, base + ext);
    try (InputStream in = mContext.getContentResolver().openInputStream(uri);
         OutputStream os = new FileOutputStream(out))
    {
      if (in == null)
        throw new IOException("cannot read " + url);
      byte[] buf = new byte[1 << 14];
      int n;
      while ((n = in.read(buf)) > 0)
        os.write(buf, 0, n);
    }
    return out.getPath();
  }

  private void closeFd()
  {
    if (mOpenFd != null)
    {
      try
      {
        mOpenFd.close();
      }
      catch (IOException ignored)
      {
      }
      mOpenFd = null;
    }
  }

  // ---- JSON-RPC -------------------------------------------------------------------------------

  private int activePlayer()
  {
    JSONArray players = rpcArray("Player.GetActivePlayers", null);
    if (players == null)
      return -1;
    int fallback = -1;
    for (int i = 0; i < players.length(); i++)
    {
      JSONObject p = players.optJSONObject(i);
      if (p == null)
        continue;
      String type = p.optString("type");
      if ("video".equals(type))
        return p.optInt("playerid", 1);
      if ("audio".equals(type))
        fallback = p.optInt("playerid", 0);
    }
    return fallback;
  }

  /** Runs a JSON-RPC method; returns its "result" object (an empty one for "OK"), or null. */
  JSONObject rpc(String method, JSONObject params)
  {
    Object result = call(method, params);
    if (result instanceof JSONObject)
      return (JSONObject) result;
    return result != null ? new JSONObject() : null;
  }

  private JSONArray rpcArray(String method, JSONObject params)
  {
    Object result = call(method, params);
    return result instanceof JSONArray ? (JSONArray) result : null;
  }

  private Object call(String method, JSONObject params)
  {
    JSONObject request = obj("jsonrpc", "2.0", "id", ++mRequestId, "method", method);
    if (params != null)
      put(request, "params", params);
    String response = EngineBridge.jsonRpc(toJson(request));
    if (response == null)
      return null;
    try
    {
      JSONObject r = new JSONObject(response);
      if (r.has("error"))
      {
        Log.w(TAG, method + " failed: " + r.opt("error") + " params: "
                   + redact(params != null ? toJson(params) : "{}"));
        return null;
      }
      return r.opt("result");
    }
    catch (JSONException e)
    {
      Log.w(TAG, method + ": unreadable response " + response);
      return null;
    }
  }

  // ---- Helpers --------------------------------------------------------------------------------

  /**
   * Kodi's JSON-RPC methods tell numbers apart by their JSON form and reject an integer where they
   * take a double (Player.SetTempo, Player.SetAudioDelay), but org.json writes 2.0 as 2; doubles
   * are written here with their fraction.
   */
  static String toJson(Object value)
  {
    StringBuilder sb = new StringBuilder();
    writeJson(sb, value);
    return sb.toString();
  }

  private static void writeJson(StringBuilder sb, Object value)
  {
    if (value instanceof JSONObject)
    {
      JSONObject o = (JSONObject) value;
      sb.append('{');
      Iterator<String> keys = o.keys();
      while (keys.hasNext())
      {
        String key = keys.next();
        sb.append(JSONObject.quote(key)).append(':');
        writeJson(sb, o.opt(key));
        if (keys.hasNext())
          sb.append(',');
      }
      sb.append('}');
    }
    else if (value instanceof JSONArray)
    {
      JSONArray a = (JSONArray) value;
      sb.append('[');
      for (int i = 0; i < a.length(); i++)
      {
        if (i > 0)
          sb.append(',');
        writeJson(sb, a.opt(i));
      }
      sb.append(']');
    }
    else if (value instanceof Double || value instanceof Float)
    {
      double d = ((Number) value).doubleValue();
      sb.append(Double.isNaN(d) || Double.isInfinite(d) ? "null" : Double.toString(d));
    }
    else if (value instanceof Number || value instanceof Boolean)
    {
      sb.append(value);
    }
    else if (value == null || value == JSONObject.NULL)
    {
      sb.append("null");
    }
    else
    {
      sb.append(JSONObject.quote(value.toString()));
    }
  }
  private void publish(String name, String value)
  {
    if (mPublished.containsKey(name) && equalsNullable(mPublished.get(name), value))
      return;
    mPublished.put(name, value);
    mSink.property(name, value);
  }

  private String mediaTitle(String kodiTitle)
  {
    if (mForcedTitle != null)
      return mForcedTitle;
    if (kodiTitle != null && !kodiTitle.isEmpty())
      return kodiTitle;
    return mPath != null ? fileName(mPath) : null;
  }

  private static boolean equalsNullable(String a, String b)
  {
    return a == null ? b == null : a.equals(b);
  }

  /** Drops credentials and request headers ("url|Header=value") from logged JSON. */
  private static String redact(String json)
  {
    return json.replace("\\/", "/")
        .replaceAll("://[^/\"@]*@", "://***@")
        .replaceAll("\\|[^\"]*", "|***");
  }

  static JSONObject obj(Object... keyValues)
  {
    JSONObject o = new JSONObject();
    for (int i = 0; i + 1 < keyValues.length; i += 2)
      put(o, (String) keyValues[i], keyValues[i + 1]);
    return o;
  }

  static JSONArray arr(Object... values)
  {
    JSONArray a = new JSONArray();
    for (Object v : values)
      a.put(v);
    return a;
  }

  static void put(JSONObject o, String key, Object value)
  {
    try
    {
      o.put(key, value);
    }
    catch (JSONException e)
    {
      throw new IllegalArgumentException(e);
    }
  }

  private static JSONObject timeObject(double seconds)
  {
    long ms = Math.round(seconds * 1000);
    return obj("hours", (int) (ms / 3_600_000), "minutes", (int) (ms / 60_000 % 60),
               "seconds", (int) (ms / 1000 % 60), "milliseconds", (int) (ms % 1000));
  }

  private static double seconds(JSONObject time)
  {
    if (time == null)
      return 0;
    return time.optInt("hours") * 3600.0 + time.optInt("minutes") * 60.0 + time.optInt("seconds")
        + time.optInt("milliseconds") / 1000.0;
  }

  private static int index(JSONObject stream)
  {
    return stream != null && stream.has("index") ? stream.optInt("index", -1) : -1;
  }

  private static String arg(String[] args, int i)
  {
    return i < args.length ? args[i] : null;
  }

  private static boolean isInteger(String s)
  {
    try
    {
      Long.parseLong(s);
      return true;
    }
    catch (NumberFormatException e)
    {
      return false;
    }
  }

  static boolean parseFlag(String value, boolean fallback)
  {
    if (value == null)
      return fallback;
    switch (value.toLowerCase(Locale.ROOT))
    {
      case "yes":
      case "true":
      case "1":
        return true;
      case "no":
      case "false":
      case "0":
        return false;
      default:
        return fallback;
    }
  }

  static double parseDouble(String value, double fallback)
  {
    if (value == null)
      return fallback;
    try
    {
      return Double.parseDouble(value.trim());
    }
    catch (NumberFormatException e)
    {
      return fallback;
    }
  }

  private static int parseInt(String value)
  {
    if (value == null)
      return -1;
    try
    {
      return (int) Double.parseDouble(value.trim());
    }
    catch (NumberFormatException e)
    {
      return -1;
    }
  }

  private static String flag(boolean b)
  {
    return b ? "yes" : "no";
  }

  private static String num(double d)
  {
    if (d == Math.rint(d) && Math.abs(d) < 1e15)
      return String.valueOf((long) d);
    return String.format(Locale.ROOT, "%.6f", d);
  }

  private static String encode(String s)
  {
    try
    {
      return URLEncoder.encode(s, StandardCharsets.UTF_8.name());
    }
    catch (java.io.UnsupportedEncodingException e)
    {
      return s;
    }
  }

  private static boolean isNetwork(String url)
  {
    String lower = url.toLowerCase(Locale.ROOT);
    return lower.startsWith("http://") || lower.startsWith("https://") || lower.startsWith("ftp://")
        || lower.startsWith("smb://") || lower.startsWith("nfs://") || lower.startsWith("rtsp://")
        || lower.startsWith("rtmp://") || lower.startsWith("udp://");
  }

  static String fileName(String url)
  {
    if (url == null)
      return null;
    String path = url;
    int bar = path.indexOf('|');
    if (bar >= 0)
      path = path.substring(0, bar);
    Uri uri = Uri.parse(path);
    String last = uri.getLastPathSegment();
    if (last != null && !last.isEmpty())
    {
      int slash = last.lastIndexOf('/');
      return slash >= 0 ? last.substring(slash + 1) : last;
    }
    return path;
  }

  /** mpv option list "a=1,b="x,y",c=%3%a,b" to a map. */
  static Map<String, String> parseOptionList(String list)
  {
    Map<String, String> out = new LinkedHashMap<>();
    if (list == null || list.isEmpty())
      return out;
    int i = 0;
    int n = list.length();
    while (i < n)
    {
      int eq = list.indexOf('=', i);
      int comma = list.indexOf(',', i);
      if (eq < 0 || (comma >= 0 && comma < eq))
      {
        int end = comma < 0 ? n : comma;
        out.put(list.substring(i, end), "yes");
        i = end + 1;
        continue;
      }
      String key = list.substring(i, eq);
      int v = eq + 1;
      String value;
      if (v < n && list.charAt(v) == '"')
      {
        int close = list.indexOf('"', v + 1);
        if (close < 0)
          close = n;
        value = list.substring(v + 1, close);
        i = close + 1;
      }
      else if (v < n && list.charAt(v) == '%')
      {
        int pct = list.indexOf('%', v + 1);
        int len = pct > v ? parseInt(list.substring(v + 1, pct)) : -1;
        if (len >= 0 && pct + 1 + len <= n)
        {
          value = list.substring(pct + 1, pct + 1 + len);
          i = pct + 1 + len;
        }
        else
        {
          int end = list.indexOf(',', v);
          value = list.substring(v, end < 0 ? n : end);
          i = end < 0 ? n : end;
        }
      }
      else
      {
        int end = list.indexOf(',', v);
        value = list.substring(v, end < 0 ? n : end);
        i = end < 0 ? n : end;
      }
      out.put(key, value);
      if (i < n && list.charAt(i) == ',')
        i++;
    }
    return out;
  }

  /** mpv string list "a,b,c" with backslash-escaped commas. */
  static List<String> splitList(String list)
  {
    List<String> out = new ArrayList<>();
    if (list == null || list.isEmpty())
      return out;
    StringBuilder cur = new StringBuilder();
    for (int i = 0; i < list.length(); i++)
    {
      char c = list.charAt(i);
      if (c == '\\' && i + 1 < list.length())
        cur.append(list.charAt(++i));
      else if (c == ',')
      {
        out.add(cur.toString());
        cur.setLength(0);
      }
      else
        cur.append(c);
    }
    out.add(cur.toString());
    return out;
  }
}

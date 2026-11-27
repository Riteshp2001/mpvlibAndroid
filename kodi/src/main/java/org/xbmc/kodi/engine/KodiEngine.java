package org.xbmc.kodi.engine;

import android.content.Context;
import android.graphics.PixelFormat;
import android.graphics.Point;
import android.graphics.Rect;
import android.hardware.display.DisplayManager;
import android.media.Image;
import android.media.ImageReader;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.os.Process;
import android.os.RemoteException;
import android.os.SystemClock;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;
import android.view.Display;
import android.view.Surface;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;
import org.xbmc.kodi.EngineBridge;
import org.xbmc.kodi.Main;
import org.xbmc.kodi.XBMCMainView;
import org.xbmc.kodi.XBMCMediaSession;
import org.xbmc.kodi.XBMCVideoView;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/**
 * The Kodi instance of this (":kodi") process.
 *
 * <p>Kodi starts headless: its GUI layer first renders into an off-screen placeholder until the
 * client attaches its own Surface. Everything that talks to Kodi runs on one engine thread.
 */
final class KodiEngine implements PlayerBridge.Sink, XBMCVideoView.Listener
{
  static final int LAYER_GUI = 0;
  static final int LAYER_VIDEO = 1;

  private static final String TAG = "KodiEngine";
  private static final long STARTUP_TIMEOUT_MS = 180_000;
  private static final long SURFACE_TIMEOUT_MS = 4000;
  private static KodiEngine sInstance;

  private enum State
  {
    IDLE,
    STARTING,
    READY,
    FAILED
  }

  private final Context mContext;
  private final Handler mHandler;
  private final Handler mMain = new Handler(Looper.getMainLooper());
  private final PlayerBridge mBridge;
  private final FdStreamServer mFdServer = new FdStreamServer();
  private final XBMCMainView mMainView = new XBMCMainView();
  private final Map<String, String> mKodiSettings = new LinkedHashMap<>();
  private final List<Runnable> mUntilReady = new ArrayList<>();
  private volatile State mState = State.IDLE;
  private volatile IKodiEngineCallback mCallback;
  private IBinder.DeathRecipient mClientDeath;
  private Main mHost;
  private ImageReader mPlaceholder;
  private volatile SurfaceHolderAdapter mGuiSurface;
  private SurfaceHolderAdapter mVideoSurface;
  private boolean mGuiApplied;
  private boolean mVideoApplied;
  private String mFailure;

  static synchronized KodiEngine get(Context context)
  {
    if (sInstance == null)
      sInstance = new KodiEngine(context.getApplicationContext());
    return sInstance;
  }

  private KodiEngine(Context context)
  {
    mContext = context;
    HandlerThread thread = new HandlerThread("KodiEngine");
    thread.start();
    mHandler = new Handler(thread.getLooper());
    mBridge = new PlayerBridge(context, mHandler, this, mFdServer);
  }

  // ---- Client API (binder threads) -------------------------------------------------------------

  void start(Bundle options, IKodiEngineCallback callback)
  {
    setCallback(callback);
    final Map<String, String> settings = new LinkedHashMap<>();
    Bundle kodiSettings = options != null ? options.getBundle("kodi-settings") : null;
    if (kodiSettings != null)
    {
      for (String key : kodiSettings.keySet())
        settings.put(key, String.valueOf(kodiSettings.get(key)));
    }
    mHandler.post(() -> {
      synchronized (mKodiSettings)
      {
        mKodiSettings.putAll(settings);
      }
      switch (mState)
      {
        case IDLE:
          mState = State.STARTING;
          Thread starter = new Thread(this::startup, "KodiEngineStart");
          starter.start();
          break;
        case READY:
          applyKodiSettings(settings);
          property("kodi-engine-state", "ready");
          mBridge.republish();
          break;
        case FAILED:
          log(PlayerBridge.LOG_ERROR, "Kodi failed to start: " + mFailure);
          event(PlayerBridge.EVENT_SHUTDOWN, null);
          break;
        default:
          break;
      }
    });
  }

  /** Blocks until Kodi no longer uses the previous Surface of the layer (bounded). */
  void setSurface(int layer, Surface surface, int width, int height)
  {
    // Before Kodi starts the size stays unknown; it is read when Kodi gets the Surface.
    if (surface != null && (width <= 0 || height <= 0) && KodiNative.isLoaded())
    {
      long size = KodiNative.nativeSurfaceSize(surface);
      width = (int) (size >>> 32);
      height = (int) size;
    }
    final SurfaceHolderAdapter holder = surface != null && surface.isValid()
        ? new SurfaceHolderAdapter(surface, Math.max(width, 0), Math.max(height, 0)) : null;
    Log.i(TAG, (layer == LAYER_VIDEO ? "video" : "GUI") + " layer: "
               + (holder != null ? width + "x" + height : "none"));
    runAndWait(() -> {
      if (layer == LAYER_VIDEO)
      {
        mVideoSurface = holder;
        if (mState == State.READY)
          applyVideoSurface();
      }
      else
      {
        mGuiSurface = holder;
        if (mState == State.READY)
          applyGuiSurface();
      }
    });
  }

  void resizeSurface(int layer, int width, int height)
  {
    if (width <= 0 || height <= 0)
      return;
    mHandler.post(() -> {
      if (layer == LAYER_VIDEO)
      {
        if (mVideoSurface != null)
        {
          mVideoSurface.setSize(width, height);
          if (mVideoApplied)
            XBMCVideoView.resizeHostSurface(width, height);
        }
      }
      else if (mGuiSurface != null)
      {
        if (mGuiApplied)
        {
          mMainView.resize(width, height);
          mHost.setDisplaySize(width, height);
        }
        else
        {
          mGuiSurface.setSize(width, height);
        }
      }
    });
  }

  void command(String[] args)
  {
    whenReady(() -> mBridge.command(args));
  }

  void setProperty(String name, String value)
  {
    if (name == null)
      return;
    if (name.startsWith("kodi-setting/"))
    {
      final String id = name.substring("kodi-setting/".length());
      mHandler.post(() -> {
        synchronized (mKodiSettings)
        {
          mKodiSettings.put(id, value);
        }
        if (mState == State.READY)
        {
          Map<String, String> one = new LinkedHashMap<>();
          one.put(id, value);
          applyKodiSettings(one);
        }
      });
      return;
    }
    whenReady(() -> mBridge.setProperty(name, value));
  }

  String jsonRpc(String request)
  {
    if (mState != State.READY)
      return null;
    AtomicReference<String> result = new AtomicReference<>();
    runAndWait(() -> result.set(EngineBridge.jsonRpc(request)));
    return result.get();
  }

  String registerFd(ParcelFileDescriptor fd, String name)
  {
    try
    {
      return mFdServer.register(fd, name);
    }
    catch (IOException e)
    {
      Log.e(TAG, "cannot serve descriptor " + name, e);
      try
      {
        fd.close();
      }
      catch (IOException ignored)
      {
      }
      return null;
    }
  }

  /**
   * Stops playback and ends this process: libkodi.so cannot be restarted in a process that ran it.
   * Kodi keeps no state the host needs (settings are written on every start), so it is not asked
   * to save any.
   */
  void quit()
  {
    mHandler.post(() -> {
      Log.i(TAG, "quit requested");
      if (mState == State.READY)
        mBridge.command(new String[] {"stop"});
      killProcess();
    });
  }

  // ---- PlayerBridge.Sink ------------------------------------------------------------------------

  @Override
  public void property(String name, String value)
  {
    IKodiEngineCallback cb = mCallback;
    if (cb == null)
      return;
    try
    {
      cb.onProperty(name, value);
    }
    catch (RemoteException e)
    {
      Log.w(TAG, "client gone", e);
    }
  }

  @Override
  public void event(int eventId, JSONObject data)
  {
    IKodiEngineCallback cb = mCallback;
    if (cb == null)
      return;
    try
    {
      cb.onEvent(eventId, data != null ? data.toString() : null);
    }
    catch (RemoteException e)
    {
      Log.w(TAG, "client gone", e);
    }
  }

  @Override
  public void log(int level, String text)
  {
    IKodiEngineCallback cb = mCallback;
    if (cb == null)
      return;
    try
    {
      cb.onLog("kodi", level, text);
    }
    catch (RemoteException ignored)
    {
    }
  }

  // ---- XBMCVideoView.Listener -------------------------------------------------------------------

  @Override
  public void onVideoViewActive(boolean active)
  {
    Log.d(TAG, "video layer " + (active ? "in use" : "released"));
    if (!active)
      property("android-video-rect", null);
  }

  /**
   * Kodi moves its video view to the picture's rectangle (MediaCodec fills its whole Surface).
   * Besides the callback, this is published as mpv's "android-video-rect"
   * ("w:h:left:top:right:bottom" within the GUI layer), which mpv clients already lay out.
   */
  @Override
  public void onVideoRect(int left, int top, int right, int bottom)
  {
    Rect gui = mHost.getDisplayRect();
    Log.d(TAG, "video rect " + left + "," + top + "-" + right + "," + bottom + " in " + gui.width()
               + "x" + gui.height());
    property("android-video-rect", gui.width() > 0 && gui.height() > 0 && right > left && bottom > top
        ? gui.width() + ":" + gui.height() + ":" + left + ":" + top + ":" + right + ":" + bottom
        : null);
    IKodiEngineCallback cb = mCallback;
    if (cb == null)
      return;
    try
    {
      cb.onVideoRect(left, top, right, bottom);
    }
    catch (RemoteException ignored)
    {
    }
  }

  // ---- Startup (KodiEngineStart thread) ---------------------------------------------------------

  private void startup()
  {
    long begin = SystemClock.elapsedRealtime();
    try
    {
      AssetInstaller.install(mContext);

      File data = new File(mContext.getFilesDir(), "kodi-engine");
      if (!data.isDirectory() && !data.mkdirs())
        throw new IllegalStateException("cannot create " + data);
      // Read by Kodi's SetupEnv(): $HOME, which holds .kodi/ (userdata, add-ons, databases).
      System.setProperty("xbmc.data", data.getAbsolutePath());
      setenv("KODI_ENGINE_MODE", "1");

      Map<String, String> settings = EngineSettings.enforced();
      settings.putAll(snapshotSettings());
      EngineSettings.write(new File(data, ".kodi/userdata"), settings);

      KodiNative.load();
      KodiNative.setFinishListener(this::onKodiFinishRequested);
      XBMCMediaSession.setListener(mBridge);
      XBMCVideoView.setListener(this);

      // Kodi's GUI resolution follows the Surface it starts with. Start with an off-screen
      // placeholder the size of the client's Surface (else of the display, in landscape), and
      // move to the client's Surface once Kodi is ready.
      Point size = initialGuiSize();
      mPlaceholder = ImageReader.newInstance(size.x, size.y, PixelFormat.RGBA_8888, 3);
      mPlaceholder.setOnImageAvailableListener(reader -> {
        Image image = reader.acquireLatestImage();
        if (image != null)
          image.close();
      }, mHandler);
      final SurfaceHolderAdapter placeholder =
          new SurfaceHolderAdapter(mPlaceholder.getSurface(), size.x, size.y);

      runOnMainAndWait(() -> {
        mHost = new Main(mContext);
        mHost.setDisplaySize(size.x, size.y);
        File external = mContext.getExternalFilesDir(null);
        File obb = mContext.getObbDir();
        if (!KodiNative.nativeCreate(mHost, mContext.getAssets(),
                                     mContext.getFilesDir().getAbsolutePath(),
                                     external != null ? external.getAbsolutePath() : "",
                                     obb != null ? obb.getAbsolutePath() : "",
                                     Build.VERSION.SDK_INT))
          throw new IllegalStateException("libkodi.so did not start");
        mHost.setFrameCallbacks(true);
        KodiNative.nativeLifecycle(KodiNative.EVENT_START, 0);
        KodiNative.nativeLifecycle(KodiNative.EVENT_RESUME, 0);
        KodiNative.nativeLifecycle(KodiNative.EVENT_FOCUS, 1);
      });
      // Kodi's main thread waits for this before it initializes.
      mMainView.setSurface(placeholder);

      waitUntilReady(begin);
      Log.i(TAG, "Kodi ready in " + (SystemClock.elapsedRealtime() - begin) + " ms");
      mHandler.post(this::onReady);
    }
    catch (Throwable t)
    {
      Log.e(TAG, "Kodi failed to start", t);
      mHandler.post(() -> {
        mState = State.FAILED;
        mFailure = String.valueOf(t.getMessage());
        property("kodi-engine-state", "failed");
        log(PlayerBridge.LOG_ERROR, "Kodi failed to start: " + mFailure);
        event(PlayerBridge.EVENT_SHUTDOWN, null);
      });
    }
  }

  private void waitUntilReady(long begin) throws InterruptedException
  {
    // Kodi registers its JSON-RPC methods in name order while it starts; JSONRPC.Ping answers
    // long before Settings.* and XBMC.* exist.
    String[] probes = {
        "{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"JSONRPC.Ping\"}",
        "{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"Settings.GetSettingValue\","
            + "\"params\":{\"setting\":\"videoplayer.usemediacodec\"}}",
        "{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"XBMC.GetInfoBooleans\","
            + "\"params\":{\"booleans\":[\"Player.HasMedia\"]}}"};
    while (SystemClock.elapsedRealtime() - begin < STARTUP_TIMEOUT_MS)
    {
      if (mHost.isGuiCreated() && answers(probes))
        return;
      Thread.sleep(200);
    }
    throw new IllegalStateException("Kodi did not become ready");
  }

  private static boolean answers(String[] requests)
  {
    for (String request : requests)
    {
      String response = EngineBridge.jsonRpc(request);
      if (response == null || !response.contains("\"result\""))
        return false;
    }
    return true;
  }

  /** Engine thread. */
  private void onReady()
  {
    mState = State.READY;
    property("kodi-engine-state", "ready");
    applyKodiSettings(snapshotSettings());
    applyGuiSurface();
    applyVideoSurface();
    mBridge.onEngineReady();
    List<Runnable> pending = new ArrayList<>(mUntilReady);
    mUntilReady.clear();
    for (Runnable r : pending)
      r.run();
  }

  // ---- Surfaces (engine thread) -------------------------------------------------------------

  private void applyGuiSurface()
  {
    if (mGuiSurface != null)
    {
      resolveSize(mGuiSurface);
      mMainView.setSurface(mGuiSurface);
      mHost.setDisplaySize(mGuiSurface.getWidth(), mGuiSurface.getHeight());
      mGuiApplied = true;
      releasePlaceholder();
    }
    else if (mGuiApplied)
    {
      // Kodi keeps playing (audio, subtitles state) without a GUI layer.
      mMainView.clearSurface();
      mGuiApplied = false;
    }
  }

  private void applyVideoSurface()
  {
    if (mVideoSurface != null)
      resolveSize(mVideoSurface);
    XBMCVideoView.setHostSurface(mVideoSurface);
    mVideoApplied = mVideoSurface != null;
    // Kodi picks MediaCodec (Surface) only when it can get a video surface.
    Map<String, String> s = new LinkedHashMap<>();
    s.put("videoplayer.usemediacodecsurface", mVideoApplied ? "true" : "false");
    applyKodiSettings(s);
  }

  /** A Surface given before Kodi started has no size yet; Kodi is running when it is applied. */
  private static void resolveSize(SurfaceHolderAdapter holder)
  {
    if (holder.getWidth() > 0 && holder.getHeight() > 0)
      return;
    long size = KodiNative.nativeSurfaceSize(holder.getSurface());
    holder.setSize((int) (size >>> 32), (int) size);
  }

  private void releasePlaceholder()
  {
    if (mPlaceholder != null)
    {
      mPlaceholder.close();
      mPlaceholder = null;
    }
  }

  // ---- Kodi settings ------------------------------------------------------------------------

  private Map<String, String> snapshotSettings()
  {
    synchronized (mKodiSettings)
    {
      return new LinkedHashMap<>(mKodiSettings);
    }
  }

  private void applyKodiSettings(Map<String, String> settings)
  {
    for (Map.Entry<String, String> e : settings.entrySet())
    {
      Object value = settingValue(e.getValue());
      JSONObject params = PlayerBridge.obj("setting", e.getKey(), "value", value);
      if (mBridge.rpc("Settings.SetSettingValue", params) == null)
        Log.w(TAG, "cannot set Kodi setting " + e.getKey() + "=" + e.getValue());
      else
        Log.d(TAG, "Kodi setting " + e.getKey() + "=" + e.getValue());
    }
  }

  private static Object settingValue(String value)
  {
    if ("true".equals(value) || "false".equals(value))
      return Boolean.parseBoolean(value);
    // List settings, e.g. videoplayer.allowedhdrformats = [0,1]
    if (value.startsWith("["))
    {
      try
      {
        return new JSONArray(value);
      }
      catch (JSONException ignored)
      {
      }
    }
    try
    {
      return Integer.parseInt(value);
    }
    catch (NumberFormatException e)
    {
      return value;
    }
  }

  // ---- Lifecycle helpers --------------------------------------------------------------------

  private void whenReady(Runnable r)
  {
    mHandler.post(() -> {
      if (mState == State.READY)
        r.run();
      else if (mState == State.STARTING || mState == State.IDLE)
        mUntilReady.add(r);
    });
  }

  private void setCallback(IKodiEngineCallback callback)
  {
    IKodiEngineCallback old = mCallback;
    if (old != null && mClientDeath != null)
      old.asBinder().unlinkToDeath(mClientDeath, 0);
    mCallback = callback;
    mClientDeath = null;
    if (callback == null)
      return;
    IBinder.DeathRecipient death = () -> {
      // The host application died: nobody can control playback any more.
      Log.w(TAG, "client died, stopping Kodi");
      quit();
    };
    try
    {
      callback.asBinder().linkToDeath(death, 0);
      mClientDeath = death;
    }
    catch (RemoteException e)
    {
      quit();
    }
  }

  /** Kodi wants to quit (it called ANativeActivity_finish). */
  private void onKodiFinishRequested()
  {
    Log.i(TAG, "Kodi asked to finish");
    mMain.post(() -> {
      mHost.setFrameCallbacks(false);
      KodiNative.nativeLifecycle(KodiNative.EVENT_FOCUS, 0);
      KodiNative.nativeLifecycle(KodiNative.EVENT_PAUSE, 0);
      KodiNative.nativeLifecycle(KodiNative.EVENT_STOP, 0);
      // Kodi exits the process once its event loop sees this.
      KodiNative.nativeLifecycle(KodiNative.EVENT_DESTROY, 0);
    });
  }

  private static void killProcess()
  {
    Log.i(TAG, "ending the Kodi process");
    Process.killProcess(Process.myPid());
  }

  private void runAndWait(Runnable r)
  {
    if (Looper.myLooper() == mHandler.getLooper())
    {
      r.run();
      return;
    }
    CountDownLatch done = new CountDownLatch(1);
    mHandler.post(() -> {
      try
      {
        r.run();
      }
      finally
      {
        done.countDown();
      }
    });
    try
    {
      if (!done.await(SURFACE_TIMEOUT_MS, TimeUnit.MILLISECONDS))
        Log.w(TAG, "engine thread busy; continuing without waiting");
    }
    catch (InterruptedException e)
    {
      Thread.currentThread().interrupt();
    }
  }

  private void runOnMainAndWait(Runnable r) throws InterruptedException
  {
    CountDownLatch done = new CountDownLatch(1);
    AtomicReference<RuntimeException> error = new AtomicReference<>();
    mMain.post(() -> {
      try
      {
        r.run();
      }
      catch (RuntimeException e)
      {
        error.set(e);
      }
      finally
      {
        done.countDown();
      }
    });
    done.await();
    if (error.get() != null)
      throw error.get();
  }

  private Point initialGuiSize()
  {
    SurfaceHolderAdapter client = mGuiSurface;
    if (client != null && client.getWidth() > 0 && client.getHeight() > 0)
      return new Point(client.getWidth(), client.getHeight());
    return landscapeDisplaySize();
  }

  private Point landscapeDisplaySize()
  {
    Point size = new Point(1920, 1080);
    DisplayManager dm = (DisplayManager) mContext.getSystemService(Context.DISPLAY_SERVICE);
    Display display = dm != null ? dm.getDisplay(Display.DEFAULT_DISPLAY) : null;
    if (display != null)
    {
      Display.Mode mode = display.getMode();
      int w = mode.getPhysicalWidth();
      int h = mode.getPhysicalHeight();
      if (w > 0 && h > 0)
        size.set(Math.max(w, h), Math.min(w, h));
    }
    return size;
  }

  private static void setenv(String name, String value)
  {
    try
    {
      Os.setenv(name, value, true);
    }
    catch (ErrnoException e)
    {
      Log.w(TAG, "setenv " + name, e);
    }
  }
}

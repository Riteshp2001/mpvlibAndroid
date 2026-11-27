package org.xbmc.kodi.engine;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.ParcelFileDescriptor;
import android.os.RemoteException;
import android.system.Os;
import android.util.Log;
import android.view.Surface;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;
import org.json.JSONTokener;

import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashSet;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.CopyOnWriteArraySet;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * Kodi as a playback library, with the same shape as mpv-android's {@code MPVLib}.
 *
 * <p>Kodi runs headless in the host application's {@code :kodi} process (see
 * {@link KodiEngineService}); it shows no user interface and only renders video and subtitles to
 * the Surfaces given to {@link #attachSurface}. Commands, properties and events use mpv's names
 * and formats and are translated to Kodi's JSON-RPC API by the engine:
 * <ul>
 *   <li>commands: {@code loadfile}, {@code stop}, {@code seek}, {@code set}, {@code cycle},
 *       {@code add}, {@code multiply}, {@code sub-add}, {@code frame-step}, {@code quit};</li>
 *   <li>properties: {@code pause}, {@code time-pos}, {@code duration}, {@code percent-pos},
 *       {@code volume}, {@code mute}, {@code speed}, {@code aid}, {@code sid}, {@code vid},
 *       {@code track-list} (including paths such as {@code track-list/0/lang}),
 *       {@code video-params}, {@code media-title}, {@code paused-for-cache}, {@code seeking},
 *       {@code eof-reached}, {@code idle-active}, ...;</li>
 *   <li>events: {@link #EVENT_START_FILE}, {@link #EVENT_FILE_LOADED},
 *       {@link #EVENT_PLAYBACK_RESTART}, {@link #EVENT_SEEK}, {@link #EVENT_END_FILE} (with mpv's
 *       {@code reason}), {@link #EVENT_IDLE} and {@link #EVENT_SHUTDOWN}.</li>
 * </ul>
 * Anything else can be reached with raw Kodi JSON-RPC through {@link #jsonRpc(String)}, and Kodi
 * settings with {@code setOptionString("kodi-setting/<id>", value)}.
 *
 * <p>Property reads return the latest values reported by the engine and never block. A written
 * value is readable at once, until the engine reports the value Kodi applied. Observers are called
 * on a dedicated thread, in order. {@code fd://N} arguments (descriptors of this process) are
 * handed to the engine, which serves them to Kodi.
 */
public final class KodiLib
{
  public static final int LAYER_GUI = KodiEngine.LAYER_GUI;
  public static final int LAYER_VIDEO = KodiEngine.LAYER_VIDEO;

  public static final int FORMAT_NONE = 0;
  public static final int FORMAT_STRING = 1;
  public static final int FORMAT_OSD_STRING = 2;
  public static final int FORMAT_FLAG = 3;
  public static final int FORMAT_INT64 = 4;
  public static final int FORMAT_DOUBLE = 5;
  public static final int FORMAT_NODE = 6;

  public static final int EVENT_SHUTDOWN = PlayerBridge.EVENT_SHUTDOWN;
  public static final int EVENT_START_FILE = PlayerBridge.EVENT_START_FILE;
  public static final int EVENT_END_FILE = PlayerBridge.EVENT_END_FILE;
  public static final int EVENT_FILE_LOADED = PlayerBridge.EVENT_FILE_LOADED;
  public static final int EVENT_IDLE = PlayerBridge.EVENT_IDLE;
  public static final int EVENT_VIDEO_RECONFIG = PlayerBridge.EVENT_VIDEO_RECONFIG;
  public static final int EVENT_AUDIO_RECONFIG = PlayerBridge.EVENT_AUDIO_RECONFIG;
  public static final int EVENT_SEEK = PlayerBridge.EVENT_SEEK;
  public static final int EVENT_PLAYBACK_RESTART = PlayerBridge.EVENT_PLAYBACK_RESTART;

  /** Receives property changes (for observed properties) and events. */
  public interface EventObserver
  {
    /** The property became unavailable, or is observed with {@link #FORMAT_NONE}. */
    void eventProperty(String property);

    void eventProperty(String property, long value);

    void eventProperty(String property, boolean value);

    void eventProperty(String property, String value);

    void eventProperty(String property, double value);

    /** Node value: Map, List, String, Long, Double or Boolean. */
    void eventPropertyNode(String property, Object value);

    /** {@code data} is the event's node map (for example END_FILE's "reason"), or empty. */
    void event(int eventId, Map<String, Object> data);
  }

  public interface LogObserver
  {
    void logMessage(String prefix, int level, String text);
  }

  /** Where Kodi wants the video layer, in pixels of the GUI layer's Surface. */
  public interface VideoRectObserver
  {
    void onVideoRect(int left, int top, int right, int bottom);
  }

  private static final String TAG = "KodiLib";
  private static final long SURFACE_TIMEOUT_MS = 5000;
  private static final long QUIT_TIMEOUT_MS = 3000;
  private static final Set<String> NODE_PROPERTIES = new HashSet<>(Arrays.asList(
      "track-list", "current-tracks", "chapter-list", "video-params", "video-out-params",
      "audio-params", "metadata", "playlist", "edition-list", "demuxer-cache-state", "af", "vf"));
  private static final Set<String> FD_COMMANDS = new HashSet<>(Arrays.asList(
      "loadfile", "sub-add", "audio-add"));

  private static final Object sLock = new Object();
  private static final List<EngineCall> sPending = new ArrayList<>();
  private static final Bundle sKodiSettings = new Bundle();
  private static final Map<String, String> sProperties = new ConcurrentHashMap<>();
  private static final Map<String, Object> sNodes = new ConcurrentHashMap<>();
  // Like mpv, one property may be observed in several formats; each gets its own events.
  private static final Map<String, Set<Integer>> sObserved = new ConcurrentHashMap<>();
  private static final CopyOnWriteArrayList<EventObserver> sObservers = new CopyOnWriteArrayList<>();
  private static final CopyOnWriteArrayList<LogObserver> sLogObservers = new CopyOnWriteArrayList<>();
  private static final CopyOnWriteArrayList<VideoRectObserver> sRectObservers =
      new CopyOnWriteArrayList<>();
  private static final SurfaceState[] sSurfaces = {new SurfaceState(), new SurfaceState()};
  private static Handler sWorker;
  private static Handler sEvents;
  private static Context sContext;
  private static ServiceConnection sConnection;
  private static IKodiEngine sEngine;
  private static boolean sCreated;

  private interface EngineCall
  {
    void run(IKodiEngine engine) throws RemoteException;
  }

  private static final class SurfaceState
  {
    Surface surface;
    int width;
    int height;
  }

  private KodiLib()
  {
  }

  // ---- Lifecycle --------------------------------------------------------------------------------

  /** Starts the engine process (if needed) and connects to it. Idempotent. */
  public static void create(Context context)
  {
    synchronized (sLock)
    {
      if (sCreated)
        return;
      ensureThreads();
      sCreated = true;
      sContext = context.getApplicationContext();
      sProperties.clear();
      sNodes.clear();
      sProperties.put("kodi-engine-state", "starting");
      final ServiceConnection connection = new ServiceConnection()
      {
        @Override
        public void onServiceConnected(ComponentName name, IBinder binder)
        {
          sWorker.post(() -> connected(this, IKodiEngine.Stub.asInterface(binder)));
        }

        @Override
        public void onServiceDisconnected(ComponentName name)
        {
          sWorker.post(() -> disconnected(this));
        }

        @Override
        public void onBindingDied(ComponentName name)
        {
          sWorker.post(() -> disconnected(this));
        }
      };
      sConnection = connection;
      Intent intent = new Intent(sContext, KodiEngineService.class);
      if (!sContext.bindService(intent, connection,
                                Context.BIND_AUTO_CREATE | Context.BIND_IMPORTANT))
      {
        sCreated = false;
        sConnection = null;
        throw new IllegalStateException("cannot bind " + KodiEngineService.class.getName());
      }
    }
  }

  /** Present for symmetry with MPVLib: the engine starts as soon as {@link #create} connects. */
  public static void init()
  {
  }

  /**
   * Stops playback and ends the engine process (Kodi cannot restart inside a process that ran it).
   * Waits up to a few seconds for the process to end, so that a following {@link #create} starts a
   * new one. Do not call on the main thread.
   */
  public static void destroy()
  {
    final ServiceConnection connection;
    final Context context;
    synchronized (sLock)
    {
      if (!sCreated)
        return;
      sCreated = false;
      connection = sConnection;
      context = sContext;
      sConnection = null;
      sPending.clear();
      for (SurfaceState s : sSurfaces)
        s.surface = null;
    }
    final CountDownLatch ended = new CountDownLatch(1);
    sWorker.post(() -> {
      IKodiEngine engine = sEngine;
      sEngine = null;
      if (engine != null)
      {
        try
        {
          engine.asBinder().linkToDeath(ended::countDown, 0);
          engine.quit();
        }
        catch (RemoteException e)
        {
          ended.countDown();
        }
      }
      else
      {
        ended.countDown();
      }
      try
      {
        context.unbindService(connection);
      }
      catch (IllegalArgumentException ignored)
      {
      }
    });
    try
    {
      if (!ended.await(QUIT_TIMEOUT_MS, TimeUnit.MILLISECONDS))
        Log.w(TAG, "Kodi engine did not end in time");
    }
    catch (InterruptedException e)
    {
      Thread.currentThread().interrupt();
    }
    sProperties.clear();
    sNodes.clear();
  }

  public static boolean isCreated()
  {
    synchronized (sLock)
    {
      return sCreated;
    }
  }

  // ---- Surfaces -----------------------------------------------------------------------------

  /** Sets the GUI layer (video and subtitles). Its size is read from the Surface. */
  public static void attachSurface(Surface surface)
  {
    attachSurface(LAYER_GUI, surface, 0, 0);
  }

  /**
   * Sets the Surface of a layer. {@link #LAYER_VIDEO} is optional: with it, Kodi decodes video
   * with MediaCodec straight to that Surface (place it below the GUI layer, whose Surface must
   * then be translucent) and reports where to put it through {@link VideoRectObserver}.
   * Returns once Kodi no longer renders to the previous Surface.
   */
  public static void attachSurface(int layer, Surface surface, int width, int height)
  {
    SurfaceState state = sSurfaces[layer];
    synchronized (sLock)
    {
      state.surface = surface;
      state.width = width;
      state.height = height;
    }
    callAndWait(engine -> engine.setSurface(layer, surface, width, height));
  }

  public static void detachSurface()
  {
    detachSurface(LAYER_GUI);
  }

  public static void detachSurface(int layer)
  {
    synchronized (sLock)
    {
      sSurfaces[layer].surface = null;
    }
    callAndWait(engine -> engine.setSurface(layer, null, 0, 0));
  }

  public static void setSurfaceSize(int width, int height)
  {
    setSurfaceSize(LAYER_GUI, width, height);
  }

  public static void setSurfaceSize(int layer, int width, int height)
  {
    synchronized (sLock)
    {
      sSurfaces[layer].width = width;
      sSurfaces[layer].height = height;
    }
    call(engine -> engine.resizeSurface(layer, width, height));
  }

  // ---- Commands and properties -------------------------------------------------------------

  public static void command(String... args)
  {
    final String[] copy = args.clone();
    call(engine -> engine.command(withServedDescriptors(engine, copy)));
  }

  /** Replaces fd://N arguments of commands that open files with URLs served by the engine. */
  private static String[] withServedDescriptors(IKodiEngine engine, String[] args)
      throws RemoteException
  {
    if (args.length < 2 || !FD_COMMANDS.contains(args[0]))
      return args;
    for (int i = 1; i < args.length; i++)
    {
      String arg = args[i];
      if (arg == null || !arg.startsWith("fd://"))
        continue;
      int fd;
      try
      {
        fd = Integer.parseInt(arg.substring("fd://".length()));
      }
      catch (NumberFormatException e)
      {
        continue;
      }
      // Kodi reads the descriptor through the engine; the caller keeps its own, as with mpv.
      String name = "media";
      try
      {
        String target = Os.readlink("/proc/self/fd/" + fd);
        int slash = target.lastIndexOf('/');
        if (slash >= 0 && slash < target.length() - 1)
          name = target.substring(slash + 1);
      }
      catch (Exception ignored)
      {
      }
      try (ParcelFileDescriptor dup = ParcelFileDescriptor.fromFd(fd))
      {
        String url = engine.registerFd(dup, name);
        if (url != null)
          args[i] = url;
      }
      catch (IOException e)
      {
        Log.w(TAG, "cannot pass " + arg + " to Kodi", e);
      }
    }
    return args;
  }

  /**
   * Before or after start: {@code kodi-setting/<id>} sets a Kodi setting (for example
   * {@code kodi-setting/audiooutput.passthrough} = {@code true}); other names are treated as
   * properties.
   */
  public static int setOptionString(String name, String value)
  {
    if (name.startsWith("kodi-setting/"))
    {
      synchronized (sLock)
      {
        sKodiSettings.putString(name.substring("kodi-setting/".length()), value);
      }
      call(engine -> engine.setProperty(name, value));
      return 0;
    }
    if ("hwdec".equals(name))
    {
      String enabled = "no".equals(value) ? "false" : "true";
      setOptionString("kodi-setting/videoplayer.usemediacodec", enabled);
      return 0;
    }
    setPropertyString(name, value);
    return 0;
  }

  public static void setPropertyString(String name, String value)
  {
    // Node properties keep the engine's structured value; a written string is not one.
    if (!NODE_PROPERTIES.contains(name))
      updateProperty(name, value);
    call(engine -> engine.setProperty(name, value));
  }

  public static void setPropertyBoolean(String name, boolean value)
  {
    setPropertyString(name, value ? "yes" : "no");
  }

  public static void setPropertyInt(String name, int value)
  {
    setPropertyString(name, String.valueOf(value));
  }

  public static void setPropertyDouble(String name, double value)
  {
    setPropertyString(name, formatDouble(value));
  }

  public static String getPropertyString(String name)
  {
    Object value = resolve(name);
    if (value == null)
      return null;
    if (value instanceof String)
      return (String) value;
    if (value instanceof Boolean)
      return (Boolean) value ? "yes" : "no";
    if (value instanceof Double || value instanceof Float)
      return formatDouble(((Number) value).doubleValue());
    if (value instanceof Number)
      return String.valueOf(((Number) value).longValue());
    return toJson(value);
  }

  public static Boolean getPropertyBoolean(String name)
  {
    Object value = resolve(name);
    if (value instanceof Boolean)
      return (Boolean) value;
    if (value instanceof Number)
      return ((Number) value).doubleValue() != 0;
    if (value instanceof String)
    {
      switch (((String) value).toLowerCase(Locale.ROOT))
      {
        case "yes":
        case "true":
          return true;
        case "no":
        case "false":
          return false;
        default:
          return null;
      }
    }
    return null;
  }

  public static Integer getPropertyInt(String name)
  {
    Double d = getPropertyDouble(name);
    return d != null ? (int) Math.round(d) : null;
  }

  public static Long getPropertyLong(String name)
  {
    Double d = getPropertyDouble(name);
    return d != null ? Math.round(d) : null;
  }

  public static Double getPropertyDouble(String name)
  {
    Object value = resolve(name);
    if (value instanceof Number)
      return ((Number) value).doubleValue();
    if (value instanceof Boolean)
      return (Boolean) value ? 1.0 : 0.0;
    if (value instanceof String)
    {
      try
      {
        return Double.parseDouble((String) value);
      }
      catch (NumberFormatException e)
      {
        return null;
      }
    }
    return null;
  }

  /** Map, List, String, Long, Double, Boolean, or null when unavailable. */
  public static Object getPropertyNode(String name)
  {
    return resolve(name);
  }

  /** Reports changes of {@code name} to observers in {@code format}, starting with its value now. */
  public static void observeProperty(String name, int format)
  {
    sObserved.computeIfAbsent(name, n -> new CopyOnWriteArraySet<>()).add(format);
    ensureThreads();
    sEvents.post(() -> dispatchProperty(name, format));
  }

  public static void addObserver(EventObserver observer)
  {
    sObservers.addIfAbsent(observer);
  }

  public static void removeObserver(EventObserver observer)
  {
    sObservers.remove(observer);
  }

  public static void addLogObserver(LogObserver observer)
  {
    sLogObservers.addIfAbsent(observer);
  }

  public static void removeLogObserver(LogObserver observer)
  {
    sLogObservers.remove(observer);
  }

  public static void addVideoRectObserver(VideoRectObserver observer)
  {
    sRectObservers.addIfAbsent(observer);
  }

  public static void removeVideoRectObserver(VideoRectObserver observer)
  {
    sRectObservers.remove(observer);
  }

  /**
   * Runs a raw Kodi JSON-RPC request (for example
   * {@code {"jsonrpc":"2.0","id":1,"method":"Player.GetItem","params":{"playerid":1}}}) and returns
   * the JSON response, or null when the engine is not ready. Blocks; do not call on the main thread.
   */
  public static String jsonRpc(String request)
  {
    IKodiEngine engine = sEngine;
    if (engine == null)
      return null;
    try
    {
      return engine.jsonRpc(request);
    }
    catch (RemoteException e)
    {
      return null;
    }
  }

  // ---- Engine connection (worker thread) ------------------------------------------------------

  private static void connected(ServiceConnection connection, IKodiEngine engine)
  {
    List<EngineCall> pending;
    Bundle options = new Bundle();
    SurfaceState[] surfaces = new SurfaceState[sSurfaces.length];
    synchronized (sLock)
    {
      if (!sCreated || sConnection != connection)
        return;
      sEngine = engine;
      pending = new ArrayList<>(sPending);
      sPending.clear();
      options.putBundle("kodi-settings", new Bundle(sKodiSettings));
      for (int i = 0; i < sSurfaces.length; i++)
      {
        SurfaceState copy = new SurfaceState();
        copy.surface = sSurfaces[i].surface;
        copy.width = sSurfaces[i].width;
        copy.height = sSurfaces[i].height;
        surfaces[i] = copy;
      }
    }
    try
    {
      engine.start(options, sCallback);
      for (int layer = 0; layer < surfaces.length; layer++)
      {
        SurfaceState s = surfaces[layer];
        if (s.surface != null && s.surface.isValid())
          engine.setSurface(layer, s.surface, s.width, s.height);
      }
      for (EngineCall c : pending)
        c.run(engine);
    }
    catch (RemoteException e)
    {
      Log.w(TAG, "engine died while connecting", e);
      disconnected(connection);
    }
  }

  private static void disconnected(ServiceConnection connection)
  {
    synchronized (sLock)
    {
      if (sConnection != connection || sEngine == null)
        return;
      sEngine = null;
    }
    Log.w(TAG, "Kodi engine stopped unexpectedly");
    boolean playing = "no".equals(sProperties.get("idle-active"));
    sProperties.put("kodi-engine-state", "failed");
    sEvents.post(() -> {
      if (playing)
      {
        Map<String, Object> data = new LinkedHashMap<>();
        data.put("reason", "error");
        data.put("file_error", "Kodi engine stopped");
        for (EventObserver o : sObservers)
          o.event(EVENT_END_FILE, data);
      }
      for (EventObserver o : sObservers)
        o.event(EVENT_SHUTDOWN, Collections.emptyMap());
    });
  }

  private static void call(EngineCall call)
  {
    ensureThreads();
    sWorker.post(() -> {
      IKodiEngine engine;
      synchronized (sLock)
      {
        if (!sCreated)
          return;
        engine = sEngine;
        if (engine == null)
        {
          sPending.add(call);
          return;
        }
      }
      try
      {
        call.run(engine);
      }
      catch (RemoteException e)
      {
        Log.w(TAG, "engine call failed", e);
      }
    });
  }

  /** For Surface changes, which the caller's Surface callbacks must wait for. */
  private static void callAndWait(EngineCall call)
  {
    ensureThreads();
    if (Thread.currentThread() == sWorker.getLooper().getThread())
    {
      runNow(call);
      return;
    }
    CountDownLatch done = new CountDownLatch(1);
    sWorker.post(() -> {
      try
      {
        runNow(call);
      }
      finally
      {
        done.countDown();
      }
    });
    try
    {
      if (!done.await(SURFACE_TIMEOUT_MS, TimeUnit.MILLISECONDS))
        Log.w(TAG, "Kodi engine did not release the Surface in time");
    }
    catch (InterruptedException e)
    {
      Thread.currentThread().interrupt();
    }
  }

  /** Surface state is replayed on connection, so a call without an engine is dropped. */
  private static void runNow(EngineCall call)
  {
    IKodiEngine engine = sEngine;
    if (engine == null)
      return;
    try
    {
      call.run(engine);
    }
    catch (RemoteException e)
    {
      Log.w(TAG, "engine call failed", e);
    }
  }

  private static void ensureThreads()
  {
    synchronized (sLock)
    {
      if (sWorker != null)
        return;
      HandlerThread worker = new HandlerThread("KodiLib");
      worker.start();
      sWorker = new Handler(worker.getLooper());
      HandlerThread events = new HandlerThread("KodiLibEvents");
      events.start();
      sEvents = new Handler(events.getLooper());
    }
  }

  private static final IKodiEngineCallback.Stub sCallback = new IKodiEngineCallback.Stub()
  {
    @Override
    public void onProperty(String name, String value)
    {
      updateProperty(name, value);
    }

    @Override
    public void onEvent(int eventId, String data)
    {
      final Map<String, Object> map = new LinkedHashMap<>();
      if (data != null)
      {
        Object parsed = parseJson(data);
        if (parsed instanceof Map)
        {
          for (Map.Entry<?, ?> e : ((Map<?, ?>) parsed).entrySet())
            map.put(String.valueOf(e.getKey()), e.getValue());
        }
      }
      sEvents.post(() -> {
        for (EventObserver o : sObservers)
        {
          try
          {
            o.event(eventId, map);
          }
          catch (RuntimeException e)
          {
            Log.e(TAG, "observer failed", e);
          }
        }
      });
    }

    @Override
    public void onLog(String prefix, int level, String text)
    {
      sEvents.post(() -> {
        for (LogObserver o : sLogObservers)
          o.logMessage(prefix, level, text);
      });
    }

    @Override
    public void onVideoRect(int left, int top, int right, int bottom)
    {
      sEvents.post(() -> {
        for (VideoRectObserver o : sRectObservers)
          o.onVideoRect(left, top, right, bottom);
      });
    }
  };

  // ---- Property cache -------------------------------------------------------------------------

  private static void updateProperty(String name, String value)
  {
    if (value == null)
    {
      sProperties.remove(name);
      sNodes.remove(name);
    }
    else
    {
      sProperties.put(name, value);
      if (NODE_PROPERTIES.contains(name))
      {
        Object node = parseJson(value);
        if (node != null)
          sNodes.put(name, node);
        else
          sNodes.remove(name);
      }
    }
    if (sEvents == null)
      return;
    final List<String> affected = new ArrayList<>();
    if (sObserved.containsKey(name))
      affected.add(name);
    String prefix = name + "/";
    for (String observed : sObserved.keySet())
    {
      if (observed.startsWith(prefix))
        affected.add(observed);
    }
    if (affected.isEmpty())
      return;
    sEvents.post(() -> {
      for (String p : affected)
        dispatchProperty(p);
    });
  }

  private static void dispatchProperty(String name)
  {
    Set<Integer> formats = sObserved.get(name);
    if (formats == null)
      return;
    for (int format : formats)
      dispatchProperty(name, format);
  }

  private static void dispatchProperty(String name, int format)
  {
    Object value = resolve(name);
    for (EventObserver o : sObservers)
    {
      try
      {
        dispatch(o, name, format, value);
      }
      catch (RuntimeException e)
      {
        Log.e(TAG, "observer failed for " + name, e);
      }
    }
  }

  private static void dispatch(EventObserver o, String name, int format, Object value)
  {
    if (value == null || format == FORMAT_NONE)
    {
      o.eventProperty(name);
      return;
    }
    switch (format)
    {
      case FORMAT_FLAG:
      {
        Boolean b = getPropertyBoolean(name);
        if (b != null)
          o.eventProperty(name, b);
        else
          o.eventProperty(name);
        break;
      }
      case FORMAT_INT64:
      {
        Long l = getPropertyLong(name);
        if (l != null)
          o.eventProperty(name, l);
        else
          o.eventProperty(name);
        break;
      }
      case FORMAT_DOUBLE:
      {
        Double d = getPropertyDouble(name);
        if (d != null)
          o.eventProperty(name, d);
        else
          o.eventProperty(name);
        break;
      }
      case FORMAT_NODE:
        o.eventPropertyNode(name, value);
        break;
      default:
      {
        String s = getPropertyString(name);
        if (s != null)
          o.eventProperty(name, s);
        else
          o.eventProperty(name);
        break;
      }
    }
  }

  /** Value of a property or of a path into a node property ("track-list/0/lang"). */
  private static Object resolve(String name)
  {
    if (name == null)
      return null;
    Object node = sNodes.get(name);
    if (node != null)
      return node;
    String raw = sProperties.get(name);
    if (raw != null)
      return raw;
    int slash = name.indexOf('/');
    if (slash <= 0)
      return null;
    Object current = sNodes.get(name.substring(0, slash));
    for (String part : name.substring(slash + 1).split("/"))
    {
      if (current instanceof List)
      {
        List<?> list = (List<?>) current;
        if ("count".equals(part))
          return (long) list.size();
        int index;
        try
        {
          index = Integer.parseInt(part);
        }
        catch (NumberFormatException e)
        {
          return null;
        }
        current = index >= 0 && index < list.size() ? list.get(index) : null;
      }
      else if (current instanceof Map)
      {
        current = ((Map<?, ?>) current).get(part);
      }
      else
      {
        return null;
      }
    }
    return current;
  }

  // ---- JSON -----------------------------------------------------------------------------------

  private static Object parseJson(String json)
  {
    try
    {
      return fromJson(new JSONTokener(json).nextValue());
    }
    catch (JSONException e)
    {
      return null;
    }
  }

  private static Object fromJson(Object value) throws JSONException
  {
    if (value instanceof JSONObject)
    {
      JSONObject o = (JSONObject) value;
      Map<String, Object> map = new LinkedHashMap<>();
      Iterator<String> keys = o.keys();
      while (keys.hasNext())
      {
        String key = keys.next();
        map.put(key, fromJson(o.get(key)));
      }
      return Collections.unmodifiableMap(map);
    }
    if (value instanceof JSONArray)
    {
      JSONArray a = (JSONArray) value;
      List<Object> list = new ArrayList<>(a.length());
      for (int i = 0; i < a.length(); i++)
        list.add(fromJson(a.get(i)));
      return Collections.unmodifiableList(list);
    }
    if (value == JSONObject.NULL)
      return null;
    if (value instanceof Integer || value instanceof Long)
      return ((Number) value).longValue();
    if (value instanceof Number)
      return ((Number) value).doubleValue();
    return value;
  }

  private static String toJson(Object value)
  {
    Object json = JSONObject.wrap(value);
    return json != null ? json.toString() : null;
  }

  private static String formatDouble(double d)
  {
    if (d == Math.rint(d) && Math.abs(d) < 1e15)
      return String.valueOf((long) d);
    return String.format(Locale.ROOT, "%.6f", d);
  }
}

package org.xbmc.kodi;

import android.app.Dialog;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.Intent;
import android.graphics.Rect;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Choreographer;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;

/**
 * Headless host of libkodi.so, replacing Kodi's NativeActivity of the same name.
 *
 * <p>libkodi.so uses this object as its Activity: through JNI it calls Context methods and the
 * handful of Activity methods declared below, by name. Nothing is shown: the engine hands Kodi
 * Surfaces owned by the host application through {@link XBMCMainView} and {@link XBMCVideoView}.
 * The native methods must match the ones libkodi.so registers on this class.
 */
public class Main extends ContextWrapper implements Choreographer.FrameCallback
{
  private static final String TAG = "Kodi";

  public static Main MainActivity = null;

  native void _onNewIntent(Intent intent);

  native void _onActivityResult(int requestCode, int resultCode, Intent resultData);

  native void _doFrame(long frameTimeNanos);

  native void _onVisibleBehindCanceled();

  private native void _callNative(long funcAddr, long variantAddr);

  private final Handler mHandler = new Handler(Looper.getMainLooper());
  private final Window mWindow;
  private final Intent mIntent = new Intent();
  private volatile int mDisplayWidth;
  private volatile int mDisplayHeight;
  private volatile boolean mWindowRequested;
  private boolean mFrameCallbacks;

  /** Must be created on the main thread. */
  public Main(Context base)
  {
    super(base);
    // Kodi reads and writes window attributes (keep screen on, preferred refresh rate) and the
    // decor view's display. This window is never attached, so those requests have no effect: the
    // host application owns the visible window.
    mWindow = new Dialog(base).getWindow();
    mWindow.getDecorView();
    MainActivity = this;
  }

  /** Size of the Surface Kodi renders its GUI layer to, used to place the video surface. */
  public void setDisplaySize(int width, int height)
  {
    mDisplayWidth = width;
    mDisplayHeight = height;
  }

  /** Vsync for Kodi's video clock. Main thread only. */
  public void setFrameCallbacks(boolean enabled)
  {
    if (mFrameCallbacks == enabled)
      return;
    mFrameCallbacks = enabled;
    Choreographer.getInstance().removeFrameCallback(this);
    if (enabled)
      Choreographer.getInstance().postFrameCallback(this);
  }

  @Override
  public void doFrame(long frameTimeNanos)
  {
    if (!mFrameCallbacks)
      return;
    Choreographer.getInstance().postFrameCallback(this);
    _doFrame(frameTimeNanos);
  }

  /**
   * Kodi first asks for its window while creating its GUI, after logging and settings exist;
   * only then may JSON-RPC requests be made.
   */
  public boolean isGuiCreated()
  {
    return mWindowRequested;
  }

  // ---- Activity methods called by libkodi.so ------------------------------------------------

  /**
   * libkodi.so instantiates "<package name>.XBMCBroadcastReceiver" and
   * "<package name>.XBMCInputDeviceListener". Both classes exist in this package, so the host
   * application needs no copies of them.
   */
  @Override
  public String getPackageName()
  {
    return "org.xbmc.kodi";
  }

  public Window getWindow()
  {
    mWindowRequested = true;
    return mWindow;
  }

  public WindowManager getWindowManager()
  {
    return (WindowManager) getSystemService(WINDOW_SERVICE);
  }

  public Intent getIntent()
  {
    return mIntent;
  }

  public View findViewById(int id)
  {
    return null;
  }

  public boolean moveTaskToBack(boolean nonRoot)
  {
    return false;
  }

  public void startActivityForResult(Intent intent, int requestCode)
  {
    Log.w(TAG, "Main: ignoring startActivityForResult " + intent);
  }

  public boolean requestVisibleBehind(boolean visible)
  {
    // Playback is controlled by the host application; Kodi must never pause on its own.
    return visible;
  }

  public Rect getDisplayRect()
  {
    return new Rect(0, 0, mDisplayWidth, mDisplayHeight);
  }

  public void registerMediaButtonEventReceiver()
  {
  }

  public void unregisterMediaButtonEventReceiver()
  {
  }

  public void runNativeOnUiThread(final long funcAddr, final long variantAddr)
  {
    mHandler.post(new Runnable()
    {
      @Override
      public void run()
      {
        _callNative(funcAddr, variantAddr);
      }
    });
  }
}

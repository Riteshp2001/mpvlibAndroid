package org.xbmc.kodi;

import android.graphics.PixelFormat;
import android.util.Log;
import android.view.Surface;

import org.xbmc.kodi.engine.SurfaceHolderAdapter;

/**
 * Kodi's MediaCodec output surface, replacing the SurfaceView of the same name.
 *
 * <p>libkodi.so asks for a video view when it opens a "MediaCodec (Surface)" decoder. The engine
 * answers with the video Surface provided by the host application (if any) and reports where
 * Kodi wants the video placed. The native methods must match libkodi.so's.
 */
public class XBMCVideoView
{
  native void _surfaceChanged(android.view.SurfaceHolder holder, int format, int width, int height);

  native void _surfaceCreated(android.view.SurfaceHolder holder);

  native void _surfaceDestroyed(android.view.SurfaceHolder holder);

  /** Receives what Kodi does with the host's video surface. */
  public interface Listener
  {
    void onVideoViewActive(boolean active);

    /** Video destination in GUI-surface pixels. */
    void onVideoRect(int left, int top, int right, int bottom);
  }

  private static final String TAG = "Kodi";
  private static final Object sLock = new Object();
  private static SurfaceHolderAdapter sHostHolder;
  private static XBMCVideoView sActive;
  private static volatile Listener sListener;

  /** Read by libkodi.so. */
  public boolean mIsCreated = false;

  private SurfaceHolderAdapter mHolder;

  public static void setListener(Listener listener)
  {
    sListener = listener;
  }

  /** Whether a host video surface is available for MediaCodec (Surface) decoding. */
  public static boolean hasHostSurface()
  {
    synchronized (sLock)
    {
      return sHostHolder != null;
    }
  }

  /** Sets (or with null, removes) the host's video surface; a decoder using it is switched. */
  public static void setHostSurface(SurfaceHolderAdapter holder)
  {
    XBMCVideoView active;
    synchronized (sLock)
    {
      sHostHolder = holder;
      active = sActive;
    }
    if (active != null)
      active.switchSurface(holder);
  }

  public static void resizeHostSurface(int width, int height)
  {
    XBMCVideoView active;
    synchronized (sLock)
    {
      if (sHostHolder == null)
        return;
      sHostHolder.setSize(width, height);
      active = sActive;
    }
    if (active != null)
      active.surfaceResized();
  }

  // ---- Called by libkodi.so -------------------------------------------------------------------

  public static XBMCVideoView createVideoView()
  {
    XBMCVideoView view = new XBMCVideoView();
    synchronized (sLock)
    {
      if (sHostHolder != null)
      {
        view.mHolder = sHostHolder;
        view.mIsCreated = true;
      }
      sActive = view;
    }
    Log.d(TAG, "XBMCVideoView: created, surface " + (view.mIsCreated ? "available" : "missing"));
    return view;
  }

  public void add()
  {
    Listener listener = sListener;
    if (listener != null)
      listener.onVideoViewActive(true);
  }

  public void release()
  {
    synchronized (sLock)
    {
      if (sActive == this)
        sActive = null;
    }
    Listener listener = sListener;
    if (listener != null)
      listener.onVideoViewActive(false);
  }

  public synchronized Surface getSurface()
  {
    return mIsCreated && mHolder != null ? mHolder.getSurface() : null;
  }

  public void setSurfaceRect(int left, int top, int right, int bottom)
  {
    Listener listener = sListener;
    if (listener != null)
      listener.onVideoRect(left, top, right, bottom);
  }

  // ---------------------------------------------------------------------------------------------

  private synchronized void switchSurface(SurfaceHolderAdapter holder)
  {
    if (mHolder != null && mIsCreated)
    {
      mIsCreated = false;
      _surfaceDestroyed(mHolder);
    }
    mHolder = holder;
    if (holder != null)
    {
      mIsCreated = true;
      _surfaceCreated(holder);
      _surfaceChanged(holder, PixelFormat.RGBA_8888, holder.getWidth(), holder.getHeight());
    }
  }

  private synchronized void surfaceResized()
  {
    if (mHolder != null && mIsCreated)
      _surfaceChanged(mHolder, PixelFormat.RGBA_8888, mHolder.getWidth(), mHolder.getHeight());
  }
}

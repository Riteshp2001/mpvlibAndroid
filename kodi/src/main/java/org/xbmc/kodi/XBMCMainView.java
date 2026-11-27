package org.xbmc.kodi;

import android.graphics.PixelFormat;
import android.util.Log;

import org.xbmc.kodi.engine.SurfaceHolderAdapter;

/**
 * Kodi's GUI/GL layer, replacing the SurfaceView of the same name.
 *
 * <p>Instead of owning a view, it forwards a Surface provided by the engine to the callbacks
 * libkodi.so registers on this class. Kodi draws subtitles, and video that is not decoded to
 * {@link XBMCVideoView}, into this layer. The native methods must match libkodi.so's.
 */
public class XBMCMainView
{
  native void _attach();

  native void _surfaceChanged(android.view.SurfaceHolder holder, int format, int width, int height);

  native void _surfaceCreated(android.view.SurfaceHolder holder);

  native void _surfaceDestroyed(android.view.SurfaceHolder holder);

  private static final String TAG = "Kodi";

  /** Read by libkodi.so. */
  public boolean mIsCreated = false;

  private SurfaceHolderAdapter mHolder;
  private boolean mAttached;

  public synchronized SurfaceHolderAdapter holder()
  {
    return mHolder;
  }

  /** Gives Kodi a new GUI surface, replacing the current one. */
  public synchronized void setSurface(SurfaceHolderAdapter holder)
  {
    if (mHolder != null)
      clearSurface();
    Log.d(TAG, "XBMCMainView: surface " + holder.getWidth() + "x" + holder.getHeight());
    if (!mAttached)
    {
      _attach();
      mAttached = true;
    }
    mHolder = holder;
    mIsCreated = true;
    _surfaceCreated(holder);
    _surfaceChanged(holder, PixelFormat.RGBA_8888, holder.getWidth(), holder.getHeight());
  }

  public synchronized void resize(int width, int height)
  {
    if (mHolder == null || (mHolder.getWidth() == width && mHolder.getHeight() == height))
      return;
    Log.d(TAG, "XBMCMainView: resize " + width + "x" + height);
    mHolder.setSize(width, height);
    _surfaceChanged(mHolder, PixelFormat.RGBA_8888, width, height);
  }

  /** Takes the surface away from Kodi; returns once Kodi no longer renders to it. */
  public synchronized void clearSurface()
  {
    if (mHolder == null)
      return;
    Log.d(TAG, "XBMCMainView: surface removed");
    mIsCreated = false;
    SurfaceHolderAdapter holder = mHolder;
    mHolder = null;
    _surfaceDestroyed(holder);
  }
}

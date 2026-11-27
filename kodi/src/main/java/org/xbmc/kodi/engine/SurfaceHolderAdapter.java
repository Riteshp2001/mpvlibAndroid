package org.xbmc.kodi.engine;

import android.graphics.Canvas;
import android.graphics.Rect;
import android.view.Surface;
import android.view.SurfaceHolder;

/**
 * SurfaceHolder over a Surface owned by another component (usually the host application's
 * SurfaceView, received over Binder). libkodi.so only calls {@link #getSurface()}.
 */
public final class SurfaceHolderAdapter implements SurfaceHolder
{
  private final Surface mSurface;
  private volatile int mWidth;
  private volatile int mHeight;

  public SurfaceHolderAdapter(Surface surface, int width, int height)
  {
    mSurface = surface;
    mWidth = width;
    mHeight = height;
  }

  public void setSize(int width, int height)
  {
    mWidth = width;
    mHeight = height;
  }

  public int getWidth()
  {
    return mWidth;
  }

  public int getHeight()
  {
    return mHeight;
  }

  @Override
  public Surface getSurface()
  {
    return mSurface;
  }

  @Override
  public Rect getSurfaceFrame()
  {
    return new Rect(0, 0, mWidth, mHeight);
  }

  @Override
  public boolean isCreating()
  {
    return false;
  }

  @Override
  public void addCallback(Callback callback)
  {
  }

  @Override
  public void removeCallback(Callback callback)
  {
  }

  @Override
  @SuppressWarnings("deprecation")
  public void setType(int type)
  {
  }

  @Override
  public void setFixedSize(int width, int height)
  {
  }

  @Override
  public void setSizeFromLayout()
  {
  }

  @Override
  public void setFormat(int format)
  {
  }

  @Override
  public void setKeepScreenOn(boolean screenOn)
  {
  }

  @Override
  public Canvas lockCanvas()
  {
    return null;
  }

  @Override
  public Canvas lockCanvas(Rect dirty)
  {
    return null;
  }

  @Override
  public void unlockCanvasAndPost(Canvas canvas)
  {
  }
}

package org.xbmc.kodi.engine;

import android.app.Service;
import android.content.Intent;
import android.os.Bundle;
import android.os.IBinder;
import android.os.ParcelFileDescriptor;
import android.view.Surface;

/** Hosts the Kodi engine in the ":kodi" process; clients bind through {@link KodiLib}. */
public final class KodiEngineService extends Service
{
  private KodiEngine mEngine;

  private final IKodiEngine.Stub mBinder = new IKodiEngine.Stub()
  {
    @Override
    public void start(Bundle options, IKodiEngineCallback callback)
    {
      mEngine.start(options, callback);
    }

    @Override
    public void setSurface(int layer, Surface surface, int width, int height)
    {
      mEngine.setSurface(layer, surface, width, height);
    }

    @Override
    public void resizeSurface(int layer, int width, int height)
    {
      mEngine.resizeSurface(layer, width, height);
    }

    @Override
    public void command(String[] args)
    {
      mEngine.command(args);
    }

    @Override
    public void setProperty(String name, String value)
    {
      mEngine.setProperty(name, value);
    }

    @Override
    public String jsonRpc(String request)
    {
      return mEngine.jsonRpc(request);
    }

    @Override
    public String registerFd(ParcelFileDescriptor fd, String name)
    {
      return mEngine.registerFd(fd, name);
    }

    @Override
    public void quit()
    {
      mEngine.quit();
    }
  };

  @Override
  public void onCreate()
  {
    super.onCreate();
    mEngine = KodiEngine.get(this);
  }

  @Override
  public IBinder onBind(Intent intent)
  {
    return mBinder;
  }

  /** The last client unbound: no one can control playback any more. */
  @Override
  public void onDestroy()
  {
    mEngine.quit();
    super.onDestroy();
  }
}

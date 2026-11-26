package org.xbmc.kodi;

import android.app.Application;
import android.content.Context;
import android.os.Build;
import android.util.Log;

import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.Properties;

/**
 * Host-application details for the embedded Kodi runtime.
 *
 * <p>Official Kodi builds bake {@code org.xbmc.kodi} into provider authorities. When Kodi is
 * embedded, every component runs in the host application's {@code :kodi} process, so values that
 * must be unique per installed application are derived from the host package at runtime.
 */
public final class KodiHost
{
  private static final String TAG = "Kodi";

  /** Kodi's assets, repackaged as one archive so they cannot collide with the host's assets. */
  public static final String ASSETS_ZIP = "kodi/kodi-assets.zip";
  static final String ASSETS_INFO = "kodi/kodi-assets.properties";

  private static String sProcessName;

  private KodiHost()
  {
  }

  /**
   * Root of the content-provider authorities declared by the embedded manifest
   * ({@code ${applicationId}.kodi.media}, {@code .file}, {@code .ytdl}).
   * Usable from static initializers, which run before a provider receives its Context.
   */
  public static String authorityRoot()
  {
    return hostPackageName() + ".kodi";
  }

  public static String hostPackageName()
  {
    String process = processName();
    int colon = process.indexOf(':');
    return colon >= 0 ? process.substring(0, colon) : process;
  }

  /** Number of entries in {@link #ASSETS_ZIP}, used for the first-run progress bar. */
  static int assetEntryCount(Context context)
  {
    Properties info = new Properties();
    try (InputStream in = context.getAssets().open(ASSETS_INFO))
    {
      info.load(in);
      return Integer.parseInt(info.getProperty("entries", "1"));
    }
    catch (IOException | NumberFormatException e)
    {
      Log.w(TAG, "KodiHost: cannot read " + ASSETS_INFO, e);
      return 1;
    }
  }

  private static synchronized String processName()
  {
    if (sProcessName == null)
    {
      if (Build.VERSION.SDK_INT >= 28)
        sProcessName = Application.getProcessName();
      else
        sProcessName = readCmdline();
    }
    return sProcessName;
  }

  private static String readCmdline()
  {
    byte[] buf = new byte[256];
    try (FileInputStream in = new FileInputStream("/proc/self/cmdline"))
    {
      int len = in.read(buf);
      int end = 0;
      while (end < len && buf[end] != 0)
        end++;
      return new String(buf, 0, end, StandardCharsets.UTF_8).trim();
    }
    catch (IOException e)
    {
      throw new IllegalStateException("Cannot determine the host process name", e);
    }
  }
}

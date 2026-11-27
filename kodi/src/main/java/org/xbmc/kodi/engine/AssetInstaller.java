package org.xbmc.kodi.engine;

import android.content.Context;
import android.content.pm.PackageManager;
import android.util.Log;

import org.xbmc.kodi.KodiHost;

import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Extracts Kodi's assets archive to {@code <cacheDir>/apk/assets}, where libkodi.so expects them
 * (KODI_HOME; Kodi also derives PYTHONHOME from it).
 */
final class AssetInstaller
{
  private static final String TAG = "KodiEngine";
  private static final String STAMP = ".mpvlib-kodi-assets";

  private AssetInstaller()
  {
  }

  static File root(Context context)
  {
    return new File(context.getCacheDir(), "apk");
  }

  /** Returns false when the assets were already in place. */
  static boolean install(Context context) throws IOException
  {
    File root = root(context);
    File stamp = new File(root, STAMP);
    String key = stampKey(context);
    if (key.equals(read(stamp)) && new File(root, "assets/system").isDirectory())
      return false;

    long start = System.currentTimeMillis();
    Log.i(TAG, "extracting Kodi assets to " + root);
    deleteRecursively(root);
    if (!root.mkdirs())
      throw new IOException("cannot create " + root);
    String rootPath = root.getCanonicalPath() + File.separator;
    byte[] buf = new byte[1 << 16];
    int count = 0;
    try (ZipInputStream zip = new ZipInputStream(
        new BufferedInputStream(context.getAssets().open(KodiHost.ASSETS_ZIP), 1 << 16)))
    {
      ZipEntry entry;
      while ((entry = zip.getNextEntry()) != null)
      {
        File out = new File(root, entry.getName());
        if (!out.getCanonicalPath().startsWith(rootPath))
          throw new IOException("unexpected archive entry " + entry.getName());
        if (entry.isDirectory())
        {
          out.mkdirs();
          continue;
        }
        File parent = out.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs())
          throw new IOException("cannot create " + parent);
        try (OutputStream os = new FileOutputStream(out))
        {
          int n;
          while ((n = zip.read(buf)) > 0)
            os.write(buf, 0, n);
        }
        count++;
      }
    }
    write(stamp, key);
    Log.i(TAG, "extracted " + count + " files in " + (System.currentTimeMillis() - start) + " ms");
    return true;
  }

  private static String stampKey(Context context)
  {
    long updated = 0;
    try
    {
      updated = context.getPackageManager()
          .getPackageInfo(context.getPackageName(), 0).lastUpdateTime;
    }
    catch (PackageManager.NameNotFoundException ignored)
    {
    }
    return KodiHost.assetChecksum(context) + ":" + updated;
  }

  private static String read(File file)
  {
    if (!file.isFile())
      return null;
    try (InputStream in = new FileInputStream(file))
    {
      byte[] data = new byte[(int) Math.min(file.length(), 4096)];
      int n = in.read(data);
      return n > 0 ? new String(data, 0, n, StandardCharsets.UTF_8) : "";
    }
    catch (IOException e)
    {
      return null;
    }
  }

  private static void write(File file, String text) throws IOException
  {
    try (OutputStream out = new FileOutputStream(file))
    {
      out.write(text.getBytes(StandardCharsets.UTF_8));
    }
  }

  private static void deleteRecursively(File file)
  {
    File[] children = file.listFiles();
    if (children != null)
    {
      for (File child : children)
        deleteRecursively(child);
    }
    file.delete();
  }
}

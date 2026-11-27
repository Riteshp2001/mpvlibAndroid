package org.xbmc.kodi.engine;

import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.util.Log;

import java.io.BufferedReader;
import java.io.Closeable;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.security.SecureRandom;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

/**
 * Serves file descriptors sent by the client (mpv's {@code fd://N} URLs) to Kodi over loopback
 * HTTP with range requests.
 *
 * <p>A descriptor is only valid in the process that opened it, and reopening it through
 * {@code /proc/self/fd} repeats the access check that made the client fall back to a descriptor in
 * the first place. Kodi reads HTTP natively, including seeking. Each descriptor gets an unguessable
 * URL; the server listens on 127.0.0.1 only.
 */
final class FdStreamServer
{
  private static final String TAG = "KodiEngine";
  private static final int BUFFER_SIZE = 1 << 16;

  private static final class Entry implements Closeable
  {
    final ParcelFileDescriptor pfd;
    final FileChannel channel;
    final long size;
    // Next byte of a descriptor that cannot seek (pipe); -1 when seekable.
    long streamPosition;

    Entry(ParcelFileDescriptor pfd)
    {
      this.pfd = pfd;
      this.channel = new FileInputStream(pfd.getFileDescriptor()).getChannel();
      long statSize = pfd.getStatSize();
      long seekable = -1;
      try
      {
        channel.position(0);
        seekable = statSize;
      }
      catch (IOException ignored)
      {
      }
      this.size = seekable;
      this.streamPosition = seekable >= 0 ? -1 : 0;
    }

    @Override
    public void close()
    {
      try
      {
        channel.close();
      }
      catch (IOException ignored)
      {
      }
      try
      {
        pfd.close();
      }
      catch (IOException ignored)
      {
      }
    }
  }

  private final Map<String, Entry> mEntries = new ConcurrentHashMap<>();
  private final SecureRandom mRandom = new SecureRandom();
  private ServerSocket mServer;

  /** Takes ownership of {@code pfd}; returns the URL Kodi reads it from. */
  synchronized String register(ParcelFileDescriptor pfd, String name) throws IOException
  {
    ensureStarted();
    byte[] bytes = new byte[16];
    mRandom.nextBytes(bytes);
    StringBuilder token = new StringBuilder();
    for (byte b : bytes)
      token.append(String.format(Locale.ROOT, "%02x", b));
    Entry entry = new Entry(pfd);
    mEntries.put(token.toString(), entry);
    String fileName = name == null || name.isEmpty() ? "media" : name;
    Log.i(TAG, "serving descriptor as " + fileName + " (" + entry.size + " bytes)");
    return "http://127.0.0.1:" + mServer.getLocalPort() + "/" + token + "/" + Uri.encode(fileName);
  }

  /** Closes the descriptor served at {@code url}, if any. */
  void release(String url)
  {
    if (url == null)
      return;
    for (Map.Entry<String, Entry> e : mEntries.entrySet())
    {
      if (url.contains("/" + e.getKey() + "/") && mEntries.remove(e.getKey(), e.getValue()))
        e.getValue().close();
    }
  }

  /** Closes every descriptor except the ones whose URLs are given. */
  void releaseExcept(String... keep)
  {
    for (Map.Entry<String, Entry> e : mEntries.entrySet())
    {
      boolean kept = false;
      for (String url : keep)
        kept |= url != null && url.contains("/" + e.getKey() + "/");
      if (!kept && mEntries.remove(e.getKey(), e.getValue()))
        e.getValue().close();
    }
  }

  private void ensureStarted() throws IOException
  {
    if (mServer != null)
      return;
    mServer = new ServerSocket(0, 16, InetAddress.getByName("127.0.0.1"));
    Thread accept = new Thread(() -> {
      while (true)
      {
        try
        {
          Socket socket = mServer.accept();
          Thread worker = new Thread(() -> serve(socket), "KodiFdStream");
          worker.setDaemon(true);
          worker.start();
        }
        catch (IOException e)
        {
          Log.w(TAG, "descriptor server stopped", e);
          return;
        }
      }
    }, "KodiFdServer");
    accept.setDaemon(true);
    accept.start();
  }

  private void serve(Socket socket)
  {
    try (Socket s = socket)
    {
      BufferedReader in = new BufferedReader(
          new InputStreamReader(s.getInputStream(), StandardCharsets.ISO_8859_1));
      String request = in.readLine();
      if (request == null)
        return;
      long rangeStart = 0;
      long rangeEnd = -1;
      String line;
      while ((line = in.readLine()) != null && !line.isEmpty())
      {
        int colon = line.indexOf(':');
        if (colon > 0 && line.substring(0, colon).trim().equalsIgnoreCase("Range"))
        {
          String value = line.substring(colon + 1).trim().toLowerCase(Locale.ROOT);
          if (value.startsWith("bytes="))
          {
            String[] parts = value.substring(6).split("-", 2);
            try
            {
              rangeStart = parts[0].isEmpty() ? 0 : Long.parseLong(parts[0].trim());
              rangeEnd = parts.length > 1 && !parts[1].trim().isEmpty()
                  ? Long.parseLong(parts[1].trim()) : -1;
            }
            catch (NumberFormatException ignored)
            {
            }
          }
        }
      }

      String[] parts = request.split(" ");
      OutputStream out = s.getOutputStream();
      boolean head = parts.length > 0 && "HEAD".equals(parts[0]);
      if (parts.length < 2 || !(head || "GET".equals(parts[0])))
      {
        respond(out, "405 Method Not Allowed", -1, null);
        return;
      }
      String path = parts[1];
      int slash = path.indexOf('/', 1);
      Entry entry = mEntries.get(slash > 1 ? path.substring(1, slash) : path.substring(1));
      if (entry == null)
      {
        respond(out, "404 Not Found", -1, null);
        return;
      }
      if (entry.size >= 0)
        serveSeekable(entry, out, head, rangeStart, rangeEnd);
      else
        serveStream(entry, out, head, rangeStart);
    }
    catch (IOException e)
    {
      // Kodi closes connections when it seeks.
    }
  }

  private void serveSeekable(Entry entry, OutputStream out, boolean head, long start, long end)
      throws IOException
  {
    long size = entry.size;
    if (start >= size && size > 0)
    {
      respond(out, "416 Range Not Satisfiable", -1, "Content-Range: bytes */" + size + "\r\n");
      return;
    }
    long last = end < 0 || end >= size ? size - 1 : end;
    long length = Math.max(0, last - start + 1);
    boolean partial = start > 0 || last < size - 1;
    String headers = "Accept-Ranges: bytes\r\n"
        + (partial ? "Content-Range: bytes " + start + "-" + last + "/" + size + "\r\n" : "");
    respond(out, partial ? "206 Partial Content" : "200 OK", length, headers);
    if (head)
      return;
    ByteBuffer buffer = ByteBuffer.allocate(BUFFER_SIZE);
    long position = start;
    while (position <= last)
    {
      buffer.clear();
      buffer.limit((int) Math.min(BUFFER_SIZE, last - position + 1));
      int n = entry.channel.read(buffer, position);
      if (n <= 0)
        break;
      out.write(buffer.array(), 0, n);
      position += n;
    }
    out.flush();
  }

  /** Pipes and sockets: one sequential reader, from the current position only. */
  private void serveStream(Entry entry, OutputStream out, boolean head, long start)
      throws IOException
  {
    synchronized (entry)
    {
      if (start != entry.streamPosition)
      {
        respond(out, "416 Range Not Satisfiable", -1, null);
        return;
      }
      respond(out, "200 OK", -1, null);
      if (head)
        return;
      ByteBuffer buffer = ByteBuffer.allocate(BUFFER_SIZE);
      int n;
      while ((n = entry.channel.read(buffer)) > 0)
      {
        out.write(buffer.array(), 0, n);
        entry.streamPosition += n;
        buffer.clear();
      }
      out.flush();
    }
  }

  private static void respond(OutputStream out, String status, long length, String headers)
      throws IOException
  {
    StringBuilder sb = new StringBuilder("HTTP/1.1 ").append(status).append("\r\n")
        .append("Content-Type: application/octet-stream\r\n")
        .append("Connection: close\r\n");
    if (length >= 0)
      sb.append("Content-Length: ").append(length).append("\r\n");
    if (headers != null)
      sb.append(headers);
    sb.append("\r\n");
    out.write(sb.toString().getBytes(StandardCharsets.ISO_8859_1));
  }
}

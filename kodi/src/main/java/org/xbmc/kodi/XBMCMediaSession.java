package org.xbmc.kodi;

import android.content.Intent;
import android.media.MediaMetadata;
import android.media.session.PlaybackState;

/**
 * Replaces Kodi's MediaSession wrapper of the same name.
 *
 * <p>The host application owns the media session, so this one is not registered with the system.
 * libkodi.so still reports every playback state and metadata change here, which the engine uses
 * as immediate playback events. The native methods must match libkodi.so's.
 */
public class XBMCMediaSession
{
  native void _onPlayRequested();

  native void _onPauseRequested();

  native void _onNextRequested();

  native void _onPreviousRequested();

  native void _onForwardRequested();

  native void _onRewindRequested();

  native void _onStopRequested();

  native void _onSeekRequested(long pos);

  native boolean _onMediaButtonEvent(Intent intent);

  /** Receives Kodi's media-session updates. */
  public interface Listener
  {
    void onSessionActive(boolean active);

    void onPlaybackState(PlaybackState state);

    void onMetadata(MediaMetadata metadata);
  }

  private static volatile Listener sListener;

  public static void setListener(Listener listener)
  {
    sListener = listener;
  }

  public XBMCMediaSession()
  {
  }

  // ---- Called by libkodi.so -------------------------------------------------------------------

  public void activate(boolean state)
  {
    Listener listener = sListener;
    if (listener != null)
      listener.onSessionActive(state);
  }

  private void updatePlaybackState(PlaybackState state)
  {
    Listener listener = sListener;
    if (listener != null)
      listener.onPlaybackState(state);
  }

  private void updateMetadata(MediaMetadata metadata)
  {
    Listener listener = sListener;
    if (listener != null)
      listener.onMetadata(metadata);
  }

  private void updateIntent(Intent intent)
  {
  }
}

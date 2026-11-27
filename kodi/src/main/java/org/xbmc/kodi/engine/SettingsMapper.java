package org.xbmc.kodi.engine;

import android.util.Log;

import org.json.JSONObject;

import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * mpv options that Kodi has as settings: the subtitle style and the audio output. Kodi keeps one
 * global subtitle style, so mpv's separate sub-* options are folded into its settings.
 *
 * <p>Kodi refuses to change a setting that one of its dependencies hides, so parents are written
 * before their children (background type before its colours, passthrough before the codecs).
 * Settings Kodi locks during playback (subtitle margin, style override) apply from the next file.
 */
final class SettingsMapper
{
  private static final String TAG = "KodiEngine";
  // mpv's default sub-font-size corresponds to Kodi's default subtitle size.
  private static final double MPV_DEFAULT_FONT_SIZE = 55;
  private static final double KODI_DEFAULT_FONT_SIZE = 42;
  // mpv's default border (3) and a 2 px shadow look like Kodi's 25% border and 15% shadow.
  private static final double KODI_BORDER_PER_MPV_PX = 25.0 / 3;
  private static final double KODI_SHADOW_PER_MPV_PX = 7.5;
  private static final double KODI_DEFAULT_MARGIN = 4.95;

  private final PlayerBridge mBridge;
  private final Map<String, Object> mApplied = new HashMap<>();

  private double mFontSize = MPV_DEFAULT_FONT_SIZE;
  private double mScale = 1;
  private boolean mBold;
  private boolean mItalic;
  private int mTextColor = 0xFFFFFFFF;
  private int mBorderColor = 0xFF000000;
  private int mBackColor = 0x00000000;
  private int mShadowColor = 0xFF000000;
  private double mBorderSize = 3;
  private double mShadowOffset;
  private String mBorderStyle = "outline-and-shadow";

  SettingsMapper(PlayerBridge bridge)
  {
    mBridge = bridge;
  }

  /** Returns false for options that are not Kodi settings. */
  boolean set(String name, String value)
  {
    switch (name)
    {
      case "sub-font-size":
        mFontSize = PlayerBridge.parseDouble(value, mFontSize);
        applyFontSize();
        return true;
      case "sub-scale":
        mScale = PlayerBridge.parseDouble(value, mScale);
        applyFontSize();
        return true;
      case "sub-bold":
        mBold = PlayerBridge.parseFlag(value, mBold);
        applyFontStyle();
        return true;
      case "sub-italic":
        mItalic = PlayerBridge.parseFlag(value, mItalic);
        applyFontStyle();
        return true;
      case "sub-color":
        mTextColor = parseColor(value, mTextColor);
        setting("subtitles.colorpick", hex(opaque(mTextColor)));
        setting("subtitles.opacity", alphaPercent(mTextColor));
        return true;
      case "sub-border-color":
      case "sub-outline-color":
        mBorderColor = parseColor(value, mBorderColor);
        applyBorder();
        applyBackground();
        return true;
      case "sub-border-size":
      case "sub-outline-size":
        mBorderSize = PlayerBridge.parseDouble(value, mBorderSize);
        applyBorder();
        return true;
      case "sub-back-color":
        mBackColor = parseColor(value, mBackColor);
        applyBackground();
        return true;
      case "sub-shadow-offset":
        mShadowOffset = PlayerBridge.parseDouble(value, mShadowOffset);
        applyBackground();
        return true;
      case "sub-shadow-color":
        mShadowColor = parseColor(value, mShadowColor);
        applyBackground();
        return true;
      case "sub-border-style":
        mBorderStyle = value;
        applyBorder();
        applyBackground();
        return true;
      case "sub-pos":
        // mpv: percent of the screen height, 100 = normal bottom position.
        double pos = PlayerBridge.parseDouble(value, 100);
        setting("subtitles.marginvertical",
                Math.round(clamp(KODI_DEFAULT_MARGIN + 100 - pos, 0, 50) * 20) / 20.0);
        return true;
      case "sub-ass-override":
        setting("subtitles.overridestyles", assOverride(value));
        return true;
      case "audio-spdif":
        applyPassthrough(value);
        return true;
      case "audio-channels":
        applyChannels(value);
        return true;
      default:
        return false;
    }
  }

  private void applyFontSize()
  {
    double size = mFontSize * mScale * KODI_DEFAULT_FONT_SIZE / MPV_DEFAULT_FONT_SIZE;
    // Kodi takes 12..74 in steps of 2.
    int kodi = (int) clamp(Math.round(size / 2) * 2, 12, 74);
    setting("subtitles.fontsize", kodi);
  }

  private void applyFontStyle()
  {
    setting("subtitles.style", (mBold ? 1 : 0) | (mItalic ? 2 : 0));
  }

  private void applyBorder()
  {
    boolean box = !"outline-and-shadow".equals(mBorderStyle);
    setting("subtitles.bordersize",
            box ? 0 : (int) clamp(Math.round(mBorderSize * KODI_BORDER_PER_MPV_PX), 0, 100));
    if (!box)
      setting("subtitles.bordercolorpick", hex(mBorderColor));
  }

  /** mpv's border styles: background-box uses sub-back-color, opaque-box the border colour. */
  private void applyBackground()
  {
    int type;
    int box = 0;
    if ("background-box".equals(mBorderStyle))
    {
      type = 2;
      box = mBackColor;
    }
    else if ("opaque-box".equals(mBorderStyle))
    {
      type = 3;
      box = mBorderColor;
    }
    else
    {
      type = mShadowOffset > 0 ? 1 : 0;
    }
    setting("subtitles.backgroundtype", type);
    if (type >= 2)
    {
      setting("subtitles.bgcolorpick", hex(opaque(box)));
      setting("subtitles.bgopacity", alphaPercent(box));
    }
    else if (type == 1)
    {
      setting("subtitles.shadowcolor", hex(opaque(mShadowColor)));
      setting("subtitles.shadowopacity", alphaPercent(mShadowColor));
      setting("subtitles.shadowsize",
              (int) clamp(Math.round(mShadowOffset * KODI_SHADOW_PER_MPV_PX), 0, 100));
    }
  }

  /** Kodi: 0 none, 1 positions, 2 styles, 3 styles and positions. */
  private static int assOverride(String value)
  {
    switch (value == null ? "" : value)
    {
      case "force":
        return 2;
      case "strip":
        return 3;
      default:
        return 0;
    }
  }

  /** mpv's audio-spdif lists the codecs to pass through (ac3, eac3, dts, dts-hd, truehd). */
  private void applyPassthrough(String value)
  {
    List<String> codecs = PlayerBridge.splitList(value == null ? "" : value.toLowerCase(Locale.ROOT));
    boolean any = !codecs.isEmpty();
    setting("audiooutput.passthrough", any);
    if (!any)
      return;
    setting("audiooutput.ac3passthrough", codecs.contains("ac3"));
    setting("audiooutput.eac3passthrough", codecs.contains("eac3"));
    setting("audiooutput.dtspassthrough", codecs.contains("dts") || codecs.contains("dts-hd"));
    setting("audiooutput.dtshdpassthrough", codecs.contains("dts-hd"));
    setting("audiooutput.truehdpassthrough", codecs.contains("truehd"));
  }

  /** mpv's audio-channels; Kodi's setting is the speaker layout it may output. */
  private void applyChannels(String value)
  {
    int layout;
    switch (value == null ? "" : value)
    {
      case "mono":
      case "stereo":
      case "2.0":
        layout = 1;
        break;
      case "5.1":
        layout = 8;
        break;
      default:
        // auto, auto-safe and anything larger: up to 7.1, as far as the output device takes it.
        layout = 10;
        break;
    }
    setting("audiooutput.channels", layout);
  }

  private void setting(String id, Object value)
  {
    if (value.equals(mApplied.get(id)))
      return;
    JSONObject params = PlayerBridge.obj("setting", id, "value", value);
    if (mBridge.rpc("Settings.SetSettingValue", params) != null)
    {
      mApplied.put(id, value);
      Log.d(TAG, "Kodi setting " + id + "=" + value);
    }
    else
    {
      Log.d(TAG, "Kodi did not take " + id + "=" + value);
    }
  }

  // ---- Colours: mpv "#RRGGBB", "#AARRGGBB" or "r/g/b[/a]" / "gray[/a]" (0..1) ---------------

  static int parseColor(String value, int fallback)
  {
    if (value == null)
      return fallback;
    String v = value.trim();
    try
    {
      if (v.startsWith("#"))
      {
        String digits = v.substring(1);
        if (digits.length() == 6)
          return 0xFF000000 | Integer.parseInt(digits, 16);
        if (digits.length() == 8)
          return (int) Long.parseLong(digits, 16);
        return fallback;
      }
      String[] parts = v.split("/");
      double[] c = new double[parts.length];
      for (int i = 0; i < parts.length; i++)
        c[i] = clamp(Double.parseDouble(parts[i]), 0, 1);
      double r;
      double g;
      double b;
      double a = 1;
      if (parts.length <= 2)
      {
        r = g = b = c[0];
        if (parts.length == 2)
          a = c[1];
      }
      else
      {
        r = c[0];
        g = c[1];
        b = c[2];
        if (parts.length >= 4)
          a = c[3];
      }
      return (channel(a) << 24) | (channel(r) << 16) | (channel(g) << 8) | channel(b);
    }
    catch (NumberFormatException e)
    {
      return fallback;
    }
  }

  private static int channel(double v)
  {
    return (int) Math.round(v * 255);
  }

  private static int opaque(int argb)
  {
    return 0xFF000000 | argb;
  }

  private static int alphaPercent(int argb)
  {
    return (int) Math.round((argb >>> 24) * 100 / 255.0);
  }

  private static String hex(int argb)
  {
    return String.format(Locale.ROOT, "%08X", argb);
  }

  private static double clamp(double v, double min, double max)
  {
    return Math.max(min, Math.min(max, v));
  }
}

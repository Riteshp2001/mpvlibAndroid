package org.xbmc.kodi.engine;

import android.util.Log;

import org.json.JSONArray;
import org.json.JSONException;
import org.w3c.dom.Document;
import org.w3c.dom.Element;
import org.w3c.dom.NodeList;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashMap;
import java.util.Map;

import javax.xml.parsers.DocumentBuilderFactory;
import javax.xml.transform.OutputKeys;
import javax.xml.transform.Transformer;
import javax.xml.transform.TransformerFactory;
import javax.xml.transform.dom.DOMSource;
import javax.xml.transform.stream.StreamResult;

/** Kodi settings files written before every engine start. */
final class EngineSettings
{
  private static final String TAG = "KodiEngine";

  static final String HEADLESS_SKIN = "skin.mpvlib.headless";

  /**
   * Values Kodi must use to run without a user interface. Applied on every start, so neither an
   * older guisettings.xml nor a setting changed at runtime can bring Kodi's GUI back.
   */
  static Map<String, String> enforced()
  {
    Map<String, String> s = new LinkedHashMap<>();
    s.put("lookandfeel.skin", HEADLESS_SKIN);
    s.put("lookandfeel.enablerssfeeds", "false");
    // Screensavers and the pause dimmer would draw over the video.
    s.put("screensaver.mode", "");
    s.put("screensaver.usedimonpause", "false");
    s.put("powermanagement.displaysoff", "0");
    s.put("powermanagement.shutdowntime", "0");
    s.put("musicplayer.visualisation", "");
    // Display modes belong to the host application's window.
    s.put("videoplayer.adjustrefreshrate", "0");
    // Required for playback speeds other than 1 with audio (Kodi's "tempo").
    s.put("videoplayer.usedisplayasclock", "true");
    s.put("general.addonupdates", "2");
    s.put("general.addonnotifications", "false");
    // No network services: the engine is controlled only through KodiLib.
    s.put("services.zeroconf", "false");
    s.put("services.esenabled", "false");
    s.put("services.upnp", "false");
    s.put("services.webserver", "false");
    s.put("services.airplay", "false");
    s.put("myvideos.extractflags", "false");
    s.put("myvideos.extractthumb", "false");
    s.put("debug.showloginfo", "false");
    return s;
  }

  private EngineSettings()
  {
  }

  /** Writes userdata/guisettings.xml with {@code values} and userdata/advancedsettings.xml. */
  static void write(File userdata, Map<String, String> values) throws IOException
  {
    if (!userdata.isDirectory() && !userdata.mkdirs())
      throw new IOException("cannot create " + userdata);
    writeGuiSettings(new File(userdata, "guisettings.xml"), values);
    writeAdvancedSettings(new File(userdata, "advancedsettings.xml"));
  }

  private static void writeGuiSettings(File file, Map<String, String> values) throws IOException
  {
    try
    {
      DocumentBuilderFactory factory = DocumentBuilderFactory.newInstance();
      Document doc = null;
      if (file.isFile())
      {
        try
        {
          doc = factory.newDocumentBuilder().parse(file);
        }
        catch (Exception e)
        {
          Log.w(TAG, "replacing unreadable " + file, e);
        }
      }
      if (doc == null || doc.getDocumentElement() == null ||
          !"settings".equals(doc.getDocumentElement().getTagName()))
      {
        doc = factory.newDocumentBuilder().newDocument();
        Element root = doc.createElement("settings");
        root.setAttribute("version", "2");
        doc.appendChild(root);
      }

      Element root = doc.getDocumentElement();
      Map<String, Element> existing = new LinkedHashMap<>();
      NodeList nodes = root.getElementsByTagName("setting");
      for (int i = 0; i < nodes.getLength(); i++)
      {
        Element e = (Element) nodes.item(i);
        existing.put(e.getAttribute("id"), e);
      }
      for (Map.Entry<String, String> entry : values.entrySet())
      {
        Element e = existing.get(entry.getKey());
        if (e == null)
        {
          e = doc.createElement("setting");
          e.setAttribute("id", entry.getKey());
          root.appendChild(e);
        }
        e.removeAttribute("default");
        while (e.getFirstChild() != null)
          e.removeChild(e.getFirstChild());
        String value = fileValue(entry.getValue());
        if (!value.isEmpty())
          e.appendChild(doc.createTextNode(value));
      }

      Transformer transformer = TransformerFactory.newInstance().newTransformer();
      transformer.setOutputProperty(OutputKeys.ENCODING, "UTF-8");
      transformer.setOutputProperty(OutputKeys.INDENT, "yes");
      File tmp = new File(file.getPath() + ".tmp");
      try (OutputStream out = new FileOutputStream(tmp))
      {
        transformer.transform(new DOMSource(doc), new StreamResult(out));
      }
      if (!tmp.renameTo(file))
        throw new IOException("cannot replace " + file);
    }
    catch (IOException e)
    {
      throw e;
    }
    catch (Exception e)
    {
      throw new IOException("cannot write " + file, e);
    }
  }

  /** List settings are given as JSON arrays ("[0,1]"); guisettings.xml separates them by commas. */
  private static String fileValue(String value)
  {
    if (!value.startsWith("["))
      return value;
    try
    {
      JSONArray list = new JSONArray(value);
      StringBuilder sb = new StringBuilder();
      for (int i = 0; i < list.length(); i++)
      {
        if (i > 0)
          sb.append(',');
        sb.append(list.get(i));
      }
      return sb.toString();
    }
    catch (JSONException e)
    {
      return value;
    }
  }

  private static void writeAdvancedSettings(File file) throws IOException
  {
    // No splash screen: it would be drawn into the host's surface while Kodi starts.
    // Tempo up to 2.1x exclusive (Kodi's limit; its default is 1.55).
    String xml = "<advancedsettings version=\"1.0\">\n"
        + "  <splash>false</splash>\n"
        + "  <video>\n"
        + "    <maxtempo>2.1</maxtempo>\n"
        + "  </video>\n"
        + "</advancedsettings>\n";
    try (OutputStream out = new FileOutputStream(file))
    {
      out.write(xml.getBytes(StandardCharsets.UTF_8));
    }
  }
}

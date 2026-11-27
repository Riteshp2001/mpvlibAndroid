package org.xbmc.kodi;

/** Engine access to package-private parts of Kodi's Java glue. */
public final class EngineBridge
{
  private static XBMCJsonRPC sJsonRpc;

  private EngineBridge()
  {
  }

  /** Runs a JSON-RPC request inside Kodi and returns the JSON response. */
  public static String jsonRpc(String request)
  {
    XBMCJsonRPC rpc;
    synchronized (EngineBridge.class)
    {
      if (sJsonRpc == null)
        sJsonRpc = new XBMCJsonRPC();
      rpc = sJsonRpc;
    }
    return rpc._requestJSON(request);
  }
}

# libkodi.so registers its JNI natives on, and calls back into, these classes by name.
-keep class org.xbmc.kodi.** { *; }
# It also instantiates <applicationId>.XBMCInputDeviceListener and <applicationId>.XBMCBroadcastReceiver,
# which the host application provides as subclasses of the org.xbmc.kodi classes.
-keep class * extends org.xbmc.kodi.XBMCInputDeviceListener { <init>(); }
-keep class * extends org.xbmc.kodi.XBMCBroadcastReceiver { <init>(); }
-keepclasseswithmembernames,includedescriptorclasses class * {
    native <methods>;
}
# Gson models used by Kodi's JSON-RPC client.
-keepattributes Signature,*Annotation*

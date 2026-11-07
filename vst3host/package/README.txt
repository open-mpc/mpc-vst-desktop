OpenMPC Host (VST3) - test build @VERSION@
=========================================

What this is
  A VST3 plugin for 64-bit Windows DAWs that runs Akai MPC / Force plugins (the ARM ".so" VST2 files built for the MPC and
  Force standalone devices) unchanged, by emulating the ARM processor. One VST3, "OpenMPC Host", hosts one MPC plugin at a time
  and you switch between them inside it. It opens on the OpenMPC Plugin Manager's place: the Manager tab, where you can browse
  the OpenMPC catalog and install plugins, which then appear in the Plugin tab.

Install
  1. Copy the folder "OpenMPC Host.vst3" to  C:\Program Files\Common Files\VST3\   (or any folder your DAW scans).
  2. Rescan plugins in your DAW. Add "OpenMPC Host" as an instrument.
  Needs 64-bit Windows 10 or 11 and a 64-bit DAW. Nothing else is installed. Delete the folder to remove it.

Using it
  Skin tab     the hosted plugin's own screen (as on the MPC / Force). Click and drag; the tabs under the picture change page.
  Plugin tab   the plugins on offer (click to switch) and the Q-Link section: 4 or 8 knobs (Mode), Bank < >, "Next bank", and
               the Q-Link page. The knobs drive what the plugin's skin maps to the Q-Links on the current page.
  Manager tab  Refresh reads the OpenMPC catalog (needs the internet), select a plugin and Install; it is downloaded, checked
               (sha256) and added. Plugins are 64-bit ARM (aarch64) builds.
  In the DAW   every plugin parameter, the 8 Q-Links, Q-Link Bank, Q-Link Next Bank (a button), Q-Link Mode, Q-Link Page, Plugin
               (the hosted plugin) and Bypass are VST3 parameters you can automate or map to a controller. MIDI notes, pitch
               bend, aftertouch and CCs reach the plugin.

Where things are
  Installed plugins and what they write:  %APPDATA%\OpenMPC Host\root\sdcard\Synths\<plugin folder>\
  (a plugin sees this as /sdcard/Synths/...). Set the environment variable OPENMPC_HOST_ROOT to use another folder.
  Your own plugin folders (a .so with its "Plugin Skins" folder) can be put there too. Plugins that need data (ROMs) read it
  from that root, for example JV-880 ROMs under  ...\root\sdcard\vst\jv880-roms\roms\ .
  A support log is  %APPDATA%\OpenMPC Host\root\openmpc-host.log .

Limits of this test build
  - Emulation costs CPU: light plugins are fine, heavy ones (JV-880 ~50% of real time, Osirus more than 100%) are not usable.
  - About a third of plugins do not restore every setting from a saved project; some need files or services the host does not provide.
  - A page's pictures load the first time it is shown (about a second). Dropdown lists in a skin, the mouse wheel, and resizing do
    not work yet. No macOS or Linux build in this package.
  - Not yet tested in every DAW. Please report the DAW and what happened.

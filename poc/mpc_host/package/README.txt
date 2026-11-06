MPC plugin host for Windows - test build @VERSION@
==================================================

What this is
  A prototype that runs Akai MPC / Force plugins (the ARM ".so" VST2 files built for the MPC and Force standalone
  devices) on a Windows PC, unchanged, by emulating the ARM processor. It is NOT a DAW plugin yet and has no audio
  output device: it plays a short test melody through a plugin, writes the result to a WAV file, reports how much CPU
  it took, and decides whether the plugin works well enough to be worth using.

  It supports 32-bit ARM plugins (MPC / Force Gen 1: "armv7") and 64-bit ARM plugins (Gen 2: "aarch64").
  Needs 64-bit Windows 10 or 11 on an x86-64 PC. Tested on Windows 11. Nothing is installed; delete the folder to remove it.

Quick test (30 seconds)
  Double-click  test.bat
  It runs eight built-in test plugins (four kinds, each as a 32-bit and a 64-bit ARM plugin) that check parameters and saved
  state, threads, files, and C++ exceptions. You should see "ALL 8 TESTS PASSED". If not, copy the text of the window and
  send it back.

Try a real plugin (you supply the .so; none is included)
  check.bat  <plugin.so>      Asks "should the host use this plugin?" and prints a verdict with reasons (details below).
                              You can also drag a plugin file onto check.bat.
  render.bat <plugin.so> [s]  Plays a test melody through the plugin for s seconds (default 5), writes <name>_emulated.wav
                              next to these files, and prints the CPU use per audio block.
  Or from a command prompt:
     mpc_host.exe <plugin.so> - 5 myprefix       (renders myprefix_emulated.wav)
     mpc_host.exe --check <plugin.so>            (verdict as one line of JSON)
     mpc_host.exe --version

  Plugins that need data (ROMs, banks) read it from the "data" folder here: what a plugin sees as /sdcard/... is
  data\sdcard\... For example the JV-880 plugin wants   data\sdcard\vst\jv880-roms\roms\jv880_rom1.bin  (and the other
  ROM files); the Osirus plugin wants Virus ROMs in   data\sdcard\vst\osirus\roms\ .  The plugin's own folder is seen as
  /plugin/. To use another folder as the plugin's "/", set MPC_HOST_ROOT=C:\some\folder before running. No ROMs are included
  and none are needed for the tests.

What the verdict means  (check.bat / --check)
  ok        works, and comfortably fast on this PC.
  degraded  usable with a warning: it uses more than half of a 128-sample audio block's time (so it may glitch on a slower
            or busier PC), or its saved state does not fully come back.
  blocked   do not use: it fails to start, crashes, hangs, calls something the host does not provide, or is too slow for
            real time on this PC (at least 80% of a block on average, or any block over its time).
  The speed numbers are about THIS PC; a faster PC gives better verdicts. They are in the JSON (median_pct, p99_pct).

Results on the developer's PC (Intel i7-10700KF, Windows 11), 128-sample blocks at 44.1 kHz
  Model-303 ok (16%) | Maze Voice ok (6%) | Dexed DX7 ok (<1%) | JV-880 degraded (49%, needs its ROMs) | Osirus blocked (120%, needs Virus ROMs;
  its DSP emulation is too heavy to run in real time through an emulated CPU) | cratedigger blocked (needs files and programs it cannot get here)

Settings (environment variables, all optional)
  MPC_HOST_ROOT     folder that is the plugin's "/" (default: the "data" folder for check.bat / render.bat)
  MPC_HOST_LIBS     semicolon-separated list of extra libraries to load into the plugin first (default: the C++ runtime in guestlibs)
  MPC_HOST_ENFORCE  set to 1: a normal run first does the check and refuses a blocked plugin; MPC_HOST_FORCE=1 overrides that
  MPC_HOST_LAX      set to 1: an unsupported call is logged once and returns 0 instead of stopping the plugin
  MPC_HOST_STATS    set to 1: print how much CPU each of the plugin's threads used
  MPC_HOST_TRACE    set to 1: print every library call the plugin makes (very verbose)

What is in this folder
  mpc_host.exe              the host (statically linked; nothing else to install)
  guest_helpers*.so         two tiny ARM helper libraries the host loads for the plugin (qsort, bsearch)
  guestlibs\                the C++ runtime (libstdc++, libgcc_s) for 32-bit and 64-bit ARM plugins; loaded for you
  fixtures\                 the eight test plugins used by test.bat (source: the repository's poc\mpc_host folder)
  data\                     where plugins find their data (see above)
  licenses\, THIRD_PARTY.txt

Known limits of this prototype
  - Speed depends on the PC. The emulation costs several times what native code would; light plugins are fine, heavy
    emulators (Osirus, JV-880 on a slow PC) are not real time.
  - Plugins that start other programs, use the MIDI sequencer interface of the device, or need the device's own files
    (for example cratedigger, rhythmbank) cannot work and are reported as blocked.
  - Plugins get a fake, empty "device": there is no real /sdcard, only the data folder.
  - The program is not code-signed, so Windows SmartScreen or antivirus software may warn about it the first time.
  - Only tested on one PC; please report anything odd (what you ran, the exact output, your Windows version and CPU).

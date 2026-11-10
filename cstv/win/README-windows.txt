RawInput2BunnyhopAPE + CS:TV for Counter-Strike: Source (64-bit Windows)
=======================================================================

WHAT IT DOES
  RawInput2BunnyhopAPE.exe starts CS:S (with -insecure) and adds:
    - m_rawinput 2 mouse interpolation (set "m_rawinput 2" in game)
    - F5  autobhop prediction (off by default)
    - F6  fullscreen hook (off by default)
    - F7  viewpunch remover (on by default)
    - F8  client-side trigger prediction (OFF by default; fixed: no more phantom teleports near triggers)
    - F9  CS:TV on/off: in-world video screens (YouTube, Twitch, ...) on servers running the CS:TV plugin
  The state of each key is printed in the in-game chat / console.

BEFORE YOU START (once)
  1. Windows 10 (version 1803 or newer) or Windows 11, 64-bit, and the normal 64-bit CS:S.
  2. Microsoft Visual C++ Redistributable 2015-2022 (x64):
     https://aka.ms/vs/17/release/vc_redist.x64.exe
  3. Unzip this whole folder somewhere permanent (keep the files together; the cstv folder must stay
     next to the exe).
  4. Windows Defender / your antivirus may flag RawInput2BunnyhopAPE.exe: it injects code into the game,
     which is what cheats do too. Allow it if you trust where you got it.

USING IT
  - Close CS:S, run RawInput2BunnyhopAPE.exe, and leave its window open (closing it early is fine for
    CS:TV, but it shows useful messages).
  - Your normal Steam launch options are read and used.
  - ONLY join servers marked insecure. The preload on a VAC-secured server = VAC ban.
  - CS:TV: on a server with the CS:TV plugin, type !tvhelp in chat. F9 turns it off/on for you.

IF SOMETHING DOESN'T WORK
  - The launcher window prints what it did ("CS:TV: helper started", "client loaded into the game").
  - CS:TV's helper log: %TEMP%\cstv\cstvd.log   (paste %TEMP%\cstv into Explorer's address bar)
  - Game console messages start with [cstv]; add -condebug to your launch options to get them in
    cstrike\console.log.
  - Send both files to whoever gave you this.

WHAT'S INSIDE
  RawInput2BunnyhopAPE.exe, cstv_client.dll   the launcher and the in-game part of CS:TV
  cstv\cstvd.exe                              CS:TV helper: plays the video (libmpv-2.dll) and hands frames
                                              to the game; quits by itself after the game closes
  cstv\yt-dlp.exe, deno.exe, streamlink\      fetch YouTube / Twitch streams (nothing is saved to disk)
  cstv\python\, cstv_*.py, fonts\             chat panel, info card, faster YouTube loads
  LICENSES\                                   licenses and where to get the source code

CS:TV for Counter-Strike: Source (64-bit Linux)
===============================================

In-game video screens. Place a screen with !tv and play YouTube, Twitch or most other video sites on it
with !link <url>. Everyone in the same room who runs CS:TV watches in sync. The server doesn't need
anything installed.

BEFORE YOU START
  Install mpv (libmpv), yt-dlp, streamlink, Python 3 with Pillow, and the Noto Sans fonts.
  Your system needs glibc 2.34 or newer (Ubuntu 22.04, Debian 12, or anything more recent).

USING IT
  1. Unzip this folder somewhere permanent.
  2. Set Steam's launch options for CS:S to:  /path/to/this/folder/cstv-launch %command% -insecure
  3. In game, press F7 to open the window, or type !tvhelp in chat for the commands.
     F8 opens the Progress Log on servers with its plugin. Esc, F7 or F8 close either one.

ONLY join servers that allow -insecure. CS:TV hooks the game, so never use it on VAC-secured servers.

If F7 or F8 is already bound to something of yours, CS:TV leaves it alone. Use: bind <key> cstv (or cstv_book)

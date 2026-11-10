// cstv — CS:TV: Garry's-Mod-style video screens.
//   !tv [small|medium|large|huge]  place the screen where you aim (one shared screen; placing again moves it)
//   !tv [size] float [distance]    the screen floats in the air in front of you (300 units, or <distance>),
//                              facing you; it shows the right way round from both sides
//   !tv link <url>, !tv remove the same as !link / !tvremove
//   !tv +30, !tv -10, !tv 1:23 seek forward / back (seconds, or m:ss) / jump to a time, for everyone
//   !link <url>                play a YouTube / Twitch / any yt-dlp link on it (YouTube playlists too:
//                              when a video ends, the next one plays for everyone)
//   !queue <url>               play this after the current video ends; a playlist that gets interrupted
//                              by the queue picks up again afterwards
//   !queuemenu (or !queue)     what's coming up (queue + the playlist's/radio's next videos): play now,
//                              move up, remove;  !clearqueue empties the queue
//   !tvhelp                    a manual in a menu
//   !tvadmin                   (admins) who may do what: per player, and the server default for everyone
//                              else / new players; saved in data/cstv.cfg
//   !tvnext                    skip: next queued link, or the next video of a YouTube playlist
//   !tvprev                    back to the previous video (where it was left, if it was skipped)
//   !tvremove                  remove the screen (every CS:TV screen in the world) and stop the video
//   !tvvol <0-100>             your own volume
//   !tvset (also !tvyt, !tvttv) settings for every site: quality, subtitles, live chat panel, volume,
//                              "Up next" messages (yours); speed, loop, next video (everyone); Twitch info
// The server's clock for a new link starts when the first viewer's video actually starts (cstv_started),
// not when the link was typed, so slow loads don't leave everyone "behind" and seeking forward at once.
// Chat panel beside the TV: Twitch channels, and YouTube live streams (shown once the stream turns out to
// be live and has chat; the client hides it otherwise).
//   look at the screen and press +attack or +use to pause / resume (for everyone)
//
// Only players running the CS:TV client mod (Linux: built into rawinput2; Windows: the CS:TV RawInput2
// launcher) see the video; everyone else sees the placeholder image. This plugin is plain SourcePawn
// (no gamedata, no offsets), so the server itself can run on Linux or Windows. The server is the source of truth: it sends the screen position, the
// link (base64url, in <=180-char pieces) and play/pause/position/speed to modded clients as reliable
// VGUIMenu user messages named "cstv"; the client mod swallows them. Clients announce themselves
// with the console command `cstv_hello <version>` after the server's "hello?".
#include <sourcemod>
#include <sdktools>
#include <sdkhooks>
#include <clientprefs>

#pragma semicolon 1
#pragma newdecls required

#define PLUGIN_VERSION "1.12"

public Plugin myinfo =
{
	name = "CS:TV",
	author = "claude",
	description = "in-world video screens (needs the CS:TV client mod to watch)",
	version = PLUGIN_VERSION,
	url = ""
};

#define SCREEN_MAT   "materials/cstv/screen.vmt"
#define CHAT_MAT     "materials/cstv/chat.vmt"
#define TEX_W        512.0     // placeholder sizes = each sprite's quad in units at scale 1
#define TEX_H        288.0
#define CHAT_TEX_W   192.0
#define CHUNK        180
#define MAX_URL      1024
#define CLICK_RANGE  2048.0
#define FLOAT_DIST   300.0     // !tv float: how far in front of the player
#define FLOAT_MIN    48.0
#define FLOAT_MAX    3000.0
#define FLOAT_GAP    2.0       // a floating screen's two back-to-back sprites are this far apart
#define QUEUE_MAX    20
#define HISTORY_MAX  20
#define LOAD_TIMEOUT 25.0   // start the clock anyway if no viewer reports the video started
// How far a viewer's video may drift from the server clock before their helper corrects it (ms).
// With two or more people watching, they should see the same moment; alone, nobody to be in step with,
// so the video is left alone unless it drifts a lot.
#define SYNC_TOL_SHARED 150
#define SYNC_TOL_ALONE  5000

enum { LINK_OTHER, LINK_TWITCH_LIVE, LINK_TWITCH_VOD, LINK_YOUTUBE };

int g_iScreen = INVALID_ENT_REFERENCE;
int g_iChat = INVALID_ENT_REFERENCE;
int g_iScreenBack = INVALID_ENT_REFERENCE;   // floating screens only: the sprites seen from behind
int g_iChatBack = INVALID_ENT_REFERENCE;
bool g_bHaveScreen;
bool g_bFloat;
float g_vPos[3], g_vAng[3], g_vNormal[3], g_vRight[3], g_vUp[3], g_vChatPos[3];
float g_fScale, g_fW, g_fH;

char g_sUrl[MAX_URL];
int g_iKind;
int g_iRev;
bool g_bPaused;
float g_fSpeed = 1.0;
float g_fBasePos;      // position (s) at g_fBaseTime
float g_fBaseTime;     // engine time
float g_fLastClick;

bool g_bModded[MAXPLAYERS + 1];
bool g_bHinted[MAXPLAYERS + 1];
int g_iLastButtons[MAXPLAYERS + 1];
bool g_bChatOn[MAXPLAYERS + 1];
bool g_bSubsOn[MAXPLAYERS + 1];
bool g_bUpNextOn[MAXPLAYERS + 1];   // "Up next: ..." lines when a playlist / the queue moves on by itself
char g_sQuality[MAXPLAYERS + 1][16];
Cookie g_hVolCookie, g_hChatCookie, g_hSubsCookie, g_hUpNextCookie;

ArrayList g_hQueue;      // queued links
ArrayList g_hQueueBy;    // who queued them
char g_sResume[MAX_URL]; // the playlist video to go back to once the queue is empty
char g_sResumeTitle[192];
ArrayList g_hHistory;    // links played before the current one (for !tvprev)
ArrayList g_hHistoryPos; // where each was left (0 = it finished)
ArrayList g_hHistoryTitle;
ArrayList g_hQueueUid;   // a number per queued link (menus and title lookups refer to it)
ArrayList g_hQueueTitle; // its title once a viewer's helper looked it up ("" until then)
int g_iUidSeq;
int g_iCurTitleUid;      // title lookup for the current link
char g_sCurTitle[192];
char g_sResumeList[64];  // the playlist the queue interrupted (continues when the queue is empty)
// the playlist's next videos, sent by a viewer's helper after each video starts (see Cmd_PlList)
char g_sWinList[64];
int g_iWinRev = -1, g_iWinFrom;
ArrayList g_hWinIds, g_hWinIdx, g_hWinTitles;
StringMap g_hPlRemoved;  // playlist videos removed with !queuemenu
char g_sRemovedList[64];
char g_sUpNext[MAX_URL];   // what plays next (queue head / playlist window); viewers' helpers look it up early
int g_iHelpFirst[MAXPLAYERS + 1];

bool g_bLoading;         // waiting for a viewer's video to start: the clock doesn't run meanwhile
float g_fLoadStart;
int g_iSeekId;           // bumped on every seek: clients jump exactly instead of the lenient resync
bool g_bLoop;            // !tvset "Loop this video": when it ends it starts over (off again for the next video)
bool g_bLoopHold;        // restarted for the loop, waiting for a viewer's video to play again
float g_fLoopAt;
int g_iSyncTol = -1;     // tolerance last sent to the viewers (SYNC_TOL_*)
int g_iSkipRev = -1;     // link revision a playlist skip was asked for (its "next" = skipped, not finished)

// ------------------------------------------------------------------ permissions (!tvadmin)
// What a player may do to the shared screen. Admins (anyone with access to sm_tvadmin: ADMFLAG_GENERIC
// unless admin_overrides.cfg says otherwise) can always do everything. Everyone else gets what an admin
// set for them in !tvadmin, or else the server default. Saved by SteamID in data/cstv.cfg (readable:
// allowed actions as words, or "all" / "none"). Personal settings (volume, quality, subtitles, chat) are
// never restricted: they only change what that player sees and hears.
#define PERM_LINK     (1 << 0)   // !link, !tv link
#define PERM_QUEUE    (1 << 1)   // !queue <url>, the queue menu's actions, !clearqueue
#define PERM_SKIP     (1 << 2)   // !tvnext, !tvprev, "Next video"
#define PERM_PAUSE    (1 << 3)   // !tvpause, clicking the screen
#define PERM_SEEK     (1 << 4)   // !tv +30 / -10 / 1:23
#define PERM_PLAYBACK (1 << 5)   // playback speed and loop (!tvset)
#define PERM_SCREEN   (1 << 6)   // !tv small|medium|large|huge, !tvremove
#define PERM_COUNT    7
#define PERM_ALL      ((1 << PERM_COUNT) - 1)
char g_sPermKey[][] = {"link", "queue", "skip", "pause", "seek", "playback", "screen"};
char g_sPermLabel[][] = {"Play links", "Queue videos", "Skip / previous", "Pause / resume", "Seek", "Speed & loop", "Place / remove the TV"};
char g_sPermVerb[][] = {"play links", "queue videos", "skip videos", "pause the video", "seek", "change the speed or loop", "place or remove the TV"};

int g_iDefaultPerms = PERM_ALL;
StringMap g_hPerms;         // SteamID -> allowed actions, for players an admin set
StringMap g_hPermNames;     // SteamID -> name when last seen (for the file and the menu)
char g_sPermFile[PLATFORM_MAX_PATH];
char g_sAdminTarget[MAXPLAYERS + 1][32];   // who !tvadmin is editing: "" = the server default, else a SteamID

public void OnPluginStart()
{
	RegConsoleCmd("sm_tv", Cmd_Tv, "sm_tv [small|medium|large] - place the video screen where you aim");
	RegConsoleCmd("sm_link", Cmd_Link, "sm_link <url> - play a video link on the screen");
	RegConsoleCmd("sm_tvremove", Cmd_Remove, "remove the video screen");
	RegConsoleCmd("sm_tvvol", Cmd_Vol, "sm_tvvol <0-100> - your CS:TV volume");
	RegConsoleCmd("sm_tvpause", Cmd_Pause, "pause / resume the video");
	RegConsoleCmd("sm_tvset", Cmd_Settings, "CS:TV settings: quality, subtitles, live chat, volume, Up next messages; speed, loop");
	RegConsoleCmd("sm_tvsettings", Cmd_Settings, "same as sm_tvset");
	RegConsoleCmd("sm_tvttv", Cmd_Settings, "same as sm_tvset (was the Twitch menu)");
	RegConsoleCmd("sm_tvyt", Cmd_Settings, "same as sm_tvset (was the YouTube menu)");
	RegConsoleCmd("sm_tvnext", Cmd_TvNext, "skip to the next queued link / playlist video");
	RegConsoleCmd("sm_queue", Cmd_Queue, "sm_queue <url> - play after the current video (no url = show the queue, clear = empty it)");
	RegConsoleCmd("sm_tvqueue", Cmd_Queue, "same as sm_queue");
	g_hQueue = new ArrayList(ByteCountToCells(MAX_URL));
	g_hQueueBy = new ArrayList(ByteCountToCells(MAX_NAME_LENGTH));
	g_hHistory = new ArrayList(ByteCountToCells(MAX_URL));
	g_hHistoryPos = new ArrayList();
	g_hHistoryTitle = new ArrayList(ByteCountToCells(192));
	g_hQueueUid = new ArrayList();
	g_hQueueTitle = new ArrayList(ByteCountToCells(192));
	g_hWinIds = new ArrayList(ByteCountToCells(24));
	g_hWinIdx = new ArrayList();
	g_hWinTitles = new ArrayList(ByteCountToCells(192));
	g_hPlRemoved = new StringMap();
	g_hPerms = new StringMap();
	g_hPermNames = new StringMap();
	LoadPerms();
	RegAdminCmd("sm_tvadmin", Cmd_TvAdmin, ADMFLAG_GENERIC, "CS:TV: who may do what (per player, and the server default)");
	RegConsoleCmd("sm_queuemenu", Cmd_QueueMenu, "what's coming up: play now / move up / remove");
	RegConsoleCmd("sm_clearqueue", Cmd_ClearQueue, "empty the queue");
	RegConsoleCmd("sm_tvhelp", Cmd_TvHelp, "CS:TV manual");
	RegConsoleCmd("cstv_title", Cmd_Title);
	RegConsoleCmd("cstv_plist", Cmd_PlList);
	RegConsoleCmd("cstv_pitem", Cmd_PlItem);
	RegConsoleCmd("cstv_pend", Cmd_PlEnd);
	RegConsoleCmd("sm_tvprev", Cmd_TvPrev, "back to the previous video");
	RegConsoleCmd("cstv_started", Cmd_Started);   // a modded client's video started: run the clock
	RegConsoleCmd("cstv_hello", Cmd_Hello);
	RegConsoleCmd("cstv_next", Cmd_Next);   // a modded client's helper: the playlist video ended, this is next
	g_hVolCookie = new Cookie("cstv_volume", "CS:TV volume 0-100", CookieAccess_Private);
	g_hChatCookie = new Cookie("cstv_livechat", "CS:TV live chat panel 1/0 (default off)", CookieAccess_Private);
	g_hSubsCookie = new Cookie("cstv_subs", "CS:TV subtitles 1/0", CookieAccess_Private);
	g_hUpNextCookie = new Cookie("cstv_upnext", "CS:TV 'Up next' chat lines 1/0 (default on)", CookieAccess_Private);
	HookEvent("round_start", Event_RoundStart, EventHookMode_PostNoCopy);
	CreateTimer(15.0, Timer_Sync, _, TIMER_REPEAT);
	CreateTimer(1.0, Timer_ChatFlags, _, TIMER_REPEAT);
	for (int i = 1; i <= MaxClients; i++)
		if (IsClientInGame(i) && !IsFakeClient(i)) { LoadPrefs(i); Probe(i); }
	RemoveOrphans();   // screens left behind by a previous load of this plugin
}

// Kill every CS:TV sprite this plugin instance isn't tracking (e.g. after a plugin reload).
int RemoveOrphans(bool all = false)
{
	int ent = -1, n = 0;
	char model[PLATFORM_MAX_PATH], cls[64];
	int max = GetMaxEntities();
	for (ent = MaxClients + 1; ent < max; ent++) {
		if (!IsValidEntity(ent)) continue;
		GetEntityClassname(ent, cls, sizeof cls);
		if (StrContains(cls, "env_sprite", false) != 0) continue;
		GetEntPropString(ent, Prop_Data, "m_ModelName", model, sizeof model);
		if (StrContains(model, "cstv/", false) == -1) continue;
		if (!all && IsScreenSprite(ent)) continue;
		float o[3];
		GetEntPropVector(ent, Prop_Data, "m_vecAbsOrigin", o);
		LogMessage("removing CS:TV sprite #%d %s (%s) at %.0f %.0f %.0f", ent, cls, model, o[0], o[1], o[2]);
		AcceptEntityInput(ent, "Kill");
		n++;
	}
	return n;
}

// When the last real player leaves, take the screen down.
public void OnClientDisconnect_Post(int client)
{
	g_bModded[client] = false;
	UpdateSyncTol();
	if (!g_bHaveScreen) return;
	for (int i = 1; i <= MaxClients; i++)
		if (IsClientInGame(i) && !IsFakeClient(i)) return;
	KillScreenSprites();
	g_bHaveScreen = false;
	g_sUrl[0] = '\0';
	ClearQueue();
	LogMessage("CS:TV: server empty, screen removed");
}

public void OnMapStart()
{
	AddFileToDownloadsTable("materials/cstv/screen.vmt");
	AddFileToDownloadsTable("materials/cstv/screen.vtf");
	AddFileToDownloadsTable("materials/cstv/chat.vmt");
	AddFileToDownloadsTable("materials/cstv/chat.vtf");
	PrecacheModel(SCREEN_MAT, true);
	PrecacheModel(CHAT_MAT, true);
	ForgetScreenSprites();
	g_bHaveScreen = false;
	g_sUrl[0] = '\0';
	ClearQueue();
}

public void OnClientPutInServer(int client)
{
	g_bModded[client] = false;
	g_bHinted[client] = false;
	g_iLastButtons[client] = 0;
	g_bChatOn[client] = false;
	g_bSubsOn[client] = false;
	g_bUpNextOn[client] = true;
	strcopy(g_sQuality[client], sizeof g_sQuality[], "auto");
	if (!IsFakeClient(client)) {
		CreateTimer(3.0, Timer_Probe, GetClientUserId(client));
		CreateTimer(15.0, Timer_Probe, GetClientUserId(client));
	}
}

public void OnClientCookiesCached(int client) { LoadPrefs(client); }

void LoadPrefs(int client)
{
	if (!AreClientCookiesCached(client)) return;
	char v[8];
	g_hChatCookie.Get(client, v, sizeof v);
	g_bChatOn[client] = StrEqual(v, "1");     // default off
	g_hSubsCookie.Get(client, v, sizeof v);
	g_bSubsOn[client] = StrEqual(v, "1");     // default off
	g_hUpNextCookie.Get(client, v, sizeof v);
	g_bUpNextOn[client] = !StrEqual(v, "0");  // default on
}

// The lines that come by themselves when a playlist, radio mix or the queue moves on to the next video.
// A radio mix does that every song, so players can turn them off (!tvset > Up next messages).
void UpNextNotice(const char[] fmt, any ...)
{
	char buf[256];
	for (int i = 1; i <= MaxClients; i++) {
		if (!IsClientInGame(i) || IsFakeClient(i) || !g_bUpNextOn[i]) continue;
		SetGlobalTransTarget(i);
		VFormat(buf, sizeof buf, fmt, 2);
		PrintToChat(i, "%s", buf);
	}
}

public Action Timer_Probe(Handle t, int userid)
{
	int client = GetClientOfUserId(userid);
	if (client && !g_bModded[client]) Probe(client);
	return Plugin_Stop;
}

// ------------------------------------------------------------------ messages to modded clients
void Send(int client, const char[] key, const char[] value)
{
	BfWrite bf = UserMessageToBfWrite(StartMessageOne("VGUIMenu", client, USERMSG_RELIABLE));
	bf.WriteString("cstv");
	bf.WriteByte(0);
	bf.WriteByte(1);
	bf.WriteString(key);
	bf.WriteString(value);
	EndMessage();
}

void Probe(int client) { Send(client, "hello?", ""); }

float CurrentPos()
{
	return (g_bPaused || g_bLoading) ? g_fBasePos : g_fBasePos + (GetEngineTime() - g_fBaseTime) * g_fSpeed;
}

bool ChatVisibleTo(int client)
{
	return g_bModded[client] && g_bChatOn[client] && g_bHaveScreen && g_sUrl[0] && (g_iKind == LINK_TWITCH_LIVE || g_iKind == LINK_YOUTUBE);
}

void SendScreen(int client)
{
	char v[64];
	if (g_bHaveScreen) Format(v, sizeof v, "%.1f %.1f %.1f", g_vPos[0], g_vPos[1], g_vPos[2]);
	else strcopy(v, sizeof v, "none");
	Send(client, "screen", v);
	SendChatPanel(client);
}

void SendChatPanel(int client)
{
	char v[64];
	if (ChatVisibleTo(client)) Format(v, sizeof v, "%.1f %.1f %.1f", g_vChatPos[0], g_vChatPos[1], g_vChatPos[2]);
	else strcopy(v, sizeof v, "none");
	Send(client, "chatpanel", v);
}

void SendState(int client)
{
	char v[64];
	bool running = !g_bPaused && !g_bLoading;
	int pos = RoundToFloor(CurrentPos() * 1000.0);
	// the message reaches this player half a round trip from now: send where the clock will be by then
	// (GetClientAvgLatency's outgoing figure is the time to the client's ack, i.e. about a round trip)
	if (running) pos += RoundToNearest(GetClientAvgLatency(client, NetFlow_Outgoing) * 500.0 * g_fSpeed);
	Format(v, sizeof v, "%d %d %d %d %d %d", g_iRev, g_bPaused ? 1 : 0, pos, RoundToNearest(g_fSpeed * 100.0), g_iSeekId, running ? 1 : 0);
	Send(client, "state", v);
}

int ModdedViewers()
{
	int n = 0;
	for (int i = 1; i <= MaxClients; i++)
		if (IsClientInGame(i) && !IsFakeClient(i) && g_bModded[i]) n++;
	return n;
}

// tell the viewers' helpers how tightly to follow the server clock (on joins and leaves)
void UpdateSyncTol(int alsoTo = 0)
{
	int tol = ModdedViewers() >= 2 ? SYNC_TOL_SHARED : SYNC_TOL_ALONE;
	char v[16];
	IntToString(tol, v, sizeof v);
	if (tol != g_iSyncTol) {
		g_iSyncTol = tol;
		for (int i = 1; i <= MaxClients; i++)
			if (IsClientInGame(i) && !IsFakeClient(i) && g_bModded[i]) Send(i, "tol", v);
	} else if (alsoTo) {
		Send(alsoTo, "tol", v);
	}
}

void SendUrl(int client)
{
	char b64[MAX_URL * 2];
	Base64UrlEncode(g_sUrl, b64, sizeof b64);
	int len = strlen(b64);
	int n = (len + CHUNK - 1) / CHUNK;
	char piece[CHUNK + 1], v[CHUNK + 32];
	for (int i = 0; i < n; i++) {
		strcopy(piece, sizeof piece, b64[i * CHUNK]);
		Format(v, sizeof v, "%d %d %d %s", g_iRev, i, n, piece);
		Send(client, "url", v);
	}
}

void SendOpts(int client)
{
	char v[32];
	Format(v, sizeof v, "chat %d", ChatVisibleTo(client) ? 1 : 0);
	Send(client, "opt", v);
	Format(v, sizeof v, "subs %d", g_bSubsOn[client] ? 1 : 0);
	Send(client, "opt", v);
	Format(v, sizeof v, "quality %s", g_sQuality[client]);
	Send(client, "opt", v);
}

void SendEverything(int client)
{
	SendScreen(client);
	char vol[8];
	g_hVolCookie.Get(client, vol, sizeof vol);
	if (vol[0]) Send(client, "uservol", vol);
	SendOpts(client);
	if (g_sUrl[0]) {
		SendUrl(client);
		SendState(client);
	}
	UpdateUpNext(client);
}

void ForModded(int what)   // 0 screen (+chat panel), 1 state, 2 url+state+opts, 3 stop
{
	for (int i = 1; i <= MaxClients; i++) {
		if (!IsClientInGame(i) || IsFakeClient(i)) continue;
		if (!g_bModded[i]) {
			if (what == 2 && !g_bHinted[i]) {
				g_bHinted[i] = true;
				PrintToChat(i, " \x04[CS:TV]\x01 A video is playing on the screen. Watching needs the CS:TV client mod.");
			}
			continue;
		}
		switch (what) {
			case 0: SendScreen(i);
			case 1: SendState(i);
			case 2: { SendOpts(i); SendChatPanel(i); SendUrl(i); SendState(i); }
			case 3: { Send(i, "stop", ""); SendChatPanel(i); }
		}
	}
}

public Action Timer_Sync(Handle t)
{
	if (g_sUrl[0]) ForModded(1);
	return Plugin_Continue;
}

public Action Cmd_Hello(int client, int args)
{
	if (!client || g_bModded[client]) return Plugin_Handled;
	g_bModded[client] = true;
	LoadPrefs(client);
	char confirmed[32];
	LogMessage("%L has the CS:TV client mod%s", client,
		GetClientAuthId(client, AuthId_Steam2, confirmed, sizeof confirmed) ? "" : " (Steam hasn't confirmed their ID)");
	SendEverything(client);
	UpdateSyncTol(client);
	return Plugin_Handled;
}

// ------------------------------------------------------------------ the screen and the chat panel
int SpawnSprite(const char[] model, float scale, const float pos[3], const float ang[3])
{
	int ent = CreateEntityByName("env_sprite_oriented");
	if (ent == -1) return INVALID_ENT_REFERENCE;
	char s[16];
	FloatToString(scale, s, sizeof s);
	DispatchKeyValue(ent, "model", model);
	DispatchKeyValue(ent, "scale", s);
	DispatchKeyValue(ent, "rendermode", "0");
	DispatchKeyValue(ent, "rendercolor", "255 255 255");
	DispatchKeyValue(ent, "renderamt", "255");
	DispatchKeyValue(ent, "spawnflags", "1");
	DispatchKeyValue(ent, "targetname", "cstv");
	DispatchSpawn(ent);
	TeleportEntity(ent, pos, ang, NULL_VECTOR);
	AcceptEntityInput(ent, "ShowSprite");
	return EntIndexToEntRef(ent);
}

void KillRef(int &ref)
{
	int e = EntRefToEntIndex(ref);
	if (e != INVALID_ENT_REFERENCE) AcceptEntityInput(e, "Kill");
	ref = INVALID_ENT_REFERENCE;
}

bool IsScreenSprite(int ent)
{
	return ent == EntRefToEntIndex(g_iScreen) || ent == EntRefToEntIndex(g_iChat)
		|| ent == EntRefToEntIndex(g_iScreenBack) || ent == EntRefToEntIndex(g_iChatBack);
}

void KillScreenSprites()
{
	KillRef(g_iScreen);
	KillRef(g_iChat);
	KillRef(g_iScreenBack);
	KillRef(g_iChatBack);
}

void ForgetScreenSprites()
{
	g_iScreen = INVALID_ENT_REFERENCE;
	g_iChat = INVALID_ENT_REFERENCE;
	g_iScreenBack = INVALID_ENT_REFERENCE;
	g_iChatBack = INVALID_ENT_REFERENCE;
}

// a TV sprite and its chat panel: same height as the TV, attached to its right edge (right as seen by
// the viewer); `right` = the viewer's right for that sprite
void SpawnPair(const float pos[3], const float ang[3], const float right[3], int &screen, int &chat)
{
	screen = SpawnSprite(SCREEN_MAT, g_fScale, pos, ang);
	float chatW = CHAT_TEX_W * g_fScale, chatPos[3];
	for (int i = 0; i < 3; i++) chatPos[i] = pos[i] + right[i] * (g_fW / 2.0 + chatW / 2.0 + 2.0);
	chat = SpawnSprite(CHAT_MAT, g_fScale, chatPos, ang);
	int e = EntRefToEntIndex(chat);
	if (e != INVALID_ENT_REFERENCE) {
		SetEdictFlags(e, GetEdictFlags(e) & ~FL_EDICT_ALWAYS);   // or SetTransmit is never asked
		SDKHook(e, SDKHook_SetTransmit, Hook_ChatTransmit);
	}
}

void SpawnScreen()
{
	KillScreenSprites();
	RemoveOrphans();
	SpawnPair(g_vPos, g_vAng, g_vRight, g_iScreen, g_iChat);
	for (int i = 0; i < 3; i++) g_vChatPos[i] = g_vPos[i] + g_vRight[i] * (g_fW / 2.0 + CHAT_TEX_W * g_fScale / 2.0 + 2.0);
	if (!g_bFloat) return;
	// A sprite seen from behind shows its picture mirrored. On a wall nobody is behind it; a floating
	// screen gets a second, reversed TV + chat just behind the first: from either side the nearer one
	// faces you and hides the other. Both use the same materials, so modded clients show the video on both.
	float back[3], ang[3], right[3];
	for (int i = 0; i < 3; i++) {
		back[i] = g_vPos[i] - g_vNormal[i] * FLOAT_GAP;
		right[i] = -g_vRight[i];
	}
	ang[0] = -g_vAng[0];
	ang[1] = g_vAng[1] + 180.0;
	ang[2] = 0.0;
	SpawnPair(back, ang, right, g_iScreenBack, g_iChatBack);
}

public Action Hook_ChatTransmit(int entity, int client)
{
	if (GetEdictFlags(entity) & FL_EDICT_ALWAYS) SetEdictFlags(entity, GetEdictFlags(entity) & ~FL_EDICT_ALWAYS);
	return ChatVisibleTo(client) ? Plugin_Continue : Plugin_Handled;
}

public Action Timer_ChatFlags(Handle t)
{
	if (g_bLoading && g_sUrl[0] && GetEngineTime() - g_fLoadStart > LOAD_TIMEOUT) StartClock();
	int chats[2];
	chats[0] = EntRefToEntIndex(g_iChat);
	chats[1] = EntRefToEntIndex(g_iChatBack);
	for (int i = 0; i < 2; i++)
		if (chats[i] != INVALID_ENT_REFERENCE && (GetEdictFlags(chats[i]) & FL_EDICT_ALWAYS))
			SetEdictFlags(chats[i], GetEdictFlags(chats[i]) & ~FL_EDICT_ALWAYS);
	return Plugin_Continue;
}

public void Event_RoundStart(Event e, const char[] name, bool dontBroadcast)
{
	if (g_bHaveScreen) SpawnScreen();   // round restarts delete plugin-made entities
}

public Action Cmd_Tv(int client, int args)
{
	if (!client) return Plugin_Handled;
	char size[16] = "medium";
	if (args >= 1) GetCmdArg(1, size, sizeof size);
	if (StrEqual(size, "remove", false) || StrEqual(size, "off", false) || StrEqual(size, "clear", false) || StrEqual(size, "delete", false))
		return Cmd_Remove(client, 0);
	// !tv +30 / -10 / 1:23. The whole typed text, not argument 1: the console tokenizer splits on ':',
	// so "1:23" arrives as the arguments "1", ":", "23".
	char raw[64];
	GetCmdArgString(raw, sizeof raw);
	StripQuotes(raw);
	ReplaceString(raw, sizeof raw, " ", "");
	if (raw[0] == '+' || raw[0] == '-' || (IsCharNumeric(raw[0]) && FindCharInString(raw, ':') != -1)) {
		if (Can(client, PERM_SEEK)) SeekCmd(client, raw);
		return Plugin_Handled;
	}
	if (StrEqual(size, "link", false) || StrEqual(size, "play", false)) {   // !tv link <url>
		char rest[MAX_URL + 16];
		GetCmdArgString(rest, sizeof rest);
		TrimString(rest);
		int sp = FindCharInString(rest, ' ');
		if (sp == -1) { PrintToChat(client, " \x04[CS:TV]\x01 Usage: \x04!tv link <url>"); return Plugin_Handled; }
		if (!Can(client, PERM_LINK)) return Plugin_Handled;
		if (!g_bHaveScreen && (!Can(client, PERM_SCREEN) || !PlaceScreen(client, 0.5, "medium"))) return Plugin_Handled;
		PlayLink(client, rest[sp + 1]);
		return Plugin_Handled;
	}
	// !tv [size] [float [distance]], words in any order
	// Sprite scales reach players rounded to steps of 0.25 (8 bits, 0.25..64, ROUNDUP in the engine's
	// DT_Sprite), so only multiples of 0.25 come out as sized: 0.1875 and 0.375 both showed as 0.25.
	float scale = 0.5, dist = FLOAT_DIST;
	bool floating = false, bad = false;
	strcopy(size, sizeof size, "medium");
	char word[16];
	for (int a = 1; a <= args && !bad; a++) {
		GetCmdArg(a, word, sizeof word);
		if (StrEqual(word, "small", false)) scale = 0.25;               // 128 x  72 units
		else if (StrEqual(word, "medium", false)) scale = 0.5;          // 256 x 144 units
		else if (StrEqual(word, "large", false)) scale = 0.75;          // 384 x 216 units
		else if (StrEqual(word, "huge", false)) scale = 1.0;            // 512 x 288 units
		else if (StrEqual(word, "float", false) || StrEqual(word, "floating", false) || StrEqual(word, "air", false)) { floating = true; continue; }
		else if (floating && IsCharNumeric(word[0])) { dist = StringToFloat(word); continue; }
		else { bad = true; continue; }
		strcopy(size, sizeof size, word);
		StringToLower(size);
	}
	if (bad) {
		PrintToChat(client, " \x04[CS:TV]\x01 Usage: \x04!tv small|medium|large|huge\x01 (add \x04float\x01 to hang it in the air), \x04!tv link <url>\x01, \x04!tv remove\x01, \x04!tv +30\x01 / \x04-10\x01 / \x041:23\x01. All commands: \x04!tvhelp");
		return Plugin_Handled;
	}
	if (Can(client, PERM_SCREEN)) PlaceScreen(client, scale, size, floating, dist);
	return Plugin_Handled;
}

void StringToLower(char[] s)
{
	for (int i = 0; s[i]; i++) s[i] = CharToLower(s[i]);
}

bool PlaceScreen(int client, float scale, const char[] size, bool floating = false, float dist = FLOAT_DIST)
{
	float eye[3], ang[3], hit[3], nrm[3];
	GetClientEyePosition(client, eye);
	GetClientEyeAngles(client, ang);
	TR_TraceRayFilter(eye, ang, MASK_SOLID, RayType_Infinite, Filter_NotPlayers);
	bool didHit = TR_DidHit();
	if (didHit) {
		TR_GetEndPosition(hit);
		TR_GetPlaneNormal(null, nrm);
	}

	if (floating) {
		// in the air `dist` units along the aim, or just short of whatever the aim hits first
		if (dist < FLOAT_MIN) dist = FLOAT_MIN;
		if (dist > FLOAT_MAX) dist = FLOAT_MAX;
		bool shortened = false;
		if (didHit) {
			float room = GetVectorDistance(eye, hit) - 8.0;
			if (room < dist) { dist = room; shortened = true; }
		}
		if (dist < FLOAT_MIN) { PrintToChat(client, " \x04[CS:TV]\x01 Too close to a wall to float a screen: aim at open space."); return false; }
		// Faces the player; kept upright unless they aim well up or down. The sprite faces INTO its
		// angles (= the player's aim), the viewer's side is the opposite way.
		float fwd[3];
		GetAngleVectors(ang, fwd, NULL_VECTOR, NULL_VECTOR);
		g_vAng[0] = FloatAbs(ang[0]) < 20.0 ? 0.0 : ang[0];
		g_vAng[1] = ang[1];
		g_vAng[2] = 0.0;
		float face[3];
		GetAngleVectors(g_vAng, face, g_vRight, g_vUp);
		for (int i = 0; i < 3; i++) {
			g_vPos[i] = eye[i] + fwd[i] * dist;
			g_vNormal[i] = -face[i];
		}
		g_bFloat = true;
		g_fScale = scale;
		g_fW = TEX_W * g_fScale;
		g_fH = TEX_H * g_fScale;
		g_bHaveScreen = true;
		SpawnScreen();
		ForModded(0);
		PrintToChatAll(" \x04[CS:TV]\x01 %N placed a floating %s TV%s. \x04!tvhelp\x01 for commands", client, size,
			shortened ? " (in front of the wall it was aimed at)" : "");
		return true;
	}

	if (!didHit) { PrintToChat(client, " \x04[CS:TV]\x01 Aim at a wall, floor or ceiling (or \x04!tv %s float\x01 to hang it in the air).", size); return false; }
	g_fScale = scale;
	g_bFloat = false;

	// Sprites draw both faces; the viewer must see the FRONT or the picture is mirrored,
	// so the sprite faces INTO the surface (angles of the reversed normal).
	float back[3];
	back[0] = -nrm[0]; back[1] = -nrm[1]; back[2] = -nrm[2];
	GetVectorAngles(back, g_vAng);
	if (FloatAbs(nrm[2]) > 0.7) g_vAng[1] = ang[1];   // floor/ceiling: picture top points where the player looks
	for (int i = 0; i < 3; i++) {
		g_vNormal[i] = nrm[i];
		g_vPos[i] = hit[i] + nrm[i] * 2.0;
	}
	float fwd[3];
	GetAngleVectors(g_vAng, fwd, g_vRight, g_vUp);
	g_fW = TEX_W * g_fScale;
	g_fH = TEX_H * g_fScale;
	g_bHaveScreen = true;
	SpawnScreen();
	ForModded(0);
	PrintToChatAll(" \x04[CS:TV]\x01 %N placed a %s TV. \x04!tvhelp\x01 for commands", client, size);
	return true;
}

public bool Filter_NotPlayers(int entity, int mask) { return entity > MaxClients; }

public Action Cmd_Remove(int client, int args)
{
	if (client && !Can(client, PERM_SCREEN)) return Plugin_Handled;
	int n = RemoveOrphans(true);   // every CS:TV sprite, tracked or not
	ForgetScreenSprites();
	g_bHaveScreen = false;
	g_sUrl[0] = '\0';
	ClearQueue();
	ForModded(3);
	ForModded(0);
	if (client) PrintToChatAll(" \x04[CS:TV]\x01 %N removed the screen%s.", client, n > (g_bFloat ? 4 : 2) ? "s (including leftovers)" : "");
	return Plugin_Handled;
}

// ------------------------------------------------------------------ links and playback
bool UrlOk(const char[] u)
{
	int n = strlen(u);
	if (n < 8 || n >= MAX_URL - 1) return false;
	if (StrContains(u, "https://", false) != 0 && StrContains(u, "http://", false) != 0) return false;
	for (int i = 0; i < n; i++)
		if (u[i] <= ' ' || u[i] == 0x7f) return false;
	return true;
}

int LinkKind(const char[] u)
{
	if (StrContains(u, "youtube.com/", false) != -1 || StrContains(u, "youtu.be/", false) != -1) return LINK_YOUTUBE;
	int t = StrContains(u, "twitch.tv/", false);
	if (t != -1) {
		char rest[MAX_URL];
		strcopy(rest, sizeof rest, u[t + 10]);
		if (rest[0] == '\0') return LINK_OTHER;
		if (StrContains(rest, "videos/", false) == 0 || StrContains(rest, "/") != -1) return LINK_TWITCH_VOD;
		return LINK_TWITCH_LIVE;
	}
	return LINK_OTHER;
}

public Action Cmd_Link(int client, int args)
{
	if (!client || !Can(client, PERM_LINK)) return Plugin_Handled;
	if (!g_bHaveScreen) { PrintToChat(client, " \x04[CS:TV]\x01 Place a screen first with \x04!tv small|medium|large\x01."); return Plugin_Handled; }
	char url[MAX_URL];
	GetCmdArgString(url, sizeof url);
	PlayLink(client, url);
	return Plugin_Handled;
}

// ------------------------------------------------------------------ links, titles, history
void CleanTitle(char[] t)
{
	for (int i = 0; t[i] != '\0'; i++)
		if (t[i] > 0 && t[i] < ' ') t[i] = ' ';   // no chat color codes
}

// how a link is named in chat and menus: its title once known, the shortened link until then
void LinkLabel(const char[] url, const char[] title, char[] out, int maxlen)
{
	if (title[0]) strcopy(out, maxlen, title);
	else ShortUrl(url, out, maxlen);
}

// record: remember the current link for !tvprev (finished: it ran to the end, so going back restarts it)
void SetLink(const char[] url, bool keepSpeed, float startPos = 0.0, bool record = true, bool finished = false, const char[] title = "")
{
	if (record && g_sUrl[0]) {
		if (g_hHistory.Length >= HISTORY_MAX) { g_hHistory.Erase(0); g_hHistoryPos.Erase(0); g_hHistoryTitle.Erase(0); }
		g_hHistory.PushString(g_sUrl);
		g_hHistoryPos.Push(finished ? 0.0 : CurrentPos());
		g_hHistoryTitle.PushString(g_sCurTitle);
	}
	strcopy(g_sUrl, sizeof g_sUrl, url);
	strcopy(g_sCurTitle, sizeof g_sCurTitle, title);
	g_iKind = LinkKind(url);
	g_iRev++;
	g_bPaused = false;
	g_bLoop = false;
	g_bLoopHold = false;
	if (!keepSpeed) g_fSpeed = 1.0;
	g_fBasePos = startPos;
	g_fBaseTime = GetEngineTime();
	g_bLoading = true;   // the clock starts when a viewer's video does (cstv_started)
	g_fLoadStart = GetEngineTime();
	DropWindowThrough(url);   // playlist videos up to this one are behind us now
	ForModded(2);
	UpdateUpNext();
	if (!title[0]) {
		g_iCurTitleUid = ++g_iUidSeq;
		RequestTitle(g_iCurTitleUid, url);
	}
}

void StartClock()
{
	g_bLoading = false;
	g_bLoopHold = false;
	g_fBaseTime = GetEngineTime();
	ForModded(1);
}

public Action Cmd_Started(int client, int args)
{
	if (!client || !g_bModded[client] || args < 1 || !g_bLoading) return Plugin_Handled;
	char a[16];
	GetCmdArg(1, a, sizeof a);
	if (StringToInt(a) == g_iRev) StartClock();   // the first viewer whose video is up (or whose seek is buffered)
	return Plugin_Handled;
}

// "30", "1:30", "1:02:03" -> seconds
bool ParseClock(const char[] s, float &out)
{
	int parts[3], n = 0, cur = 0;
	bool digit = false;
	for (int i = 0; s[i] != '\0'; i++) {
		if (IsCharNumeric(s[i])) { cur = cur * 10 + (s[i] - '0'); digit = true; }
		else if (s[i] == ':' && digit && n < 2) { parts[n++] = cur; cur = 0; digit = false; }
		else return false;
	}
	if (!digit) return false;
	parts[n++] = cur;
	float t = 0.0;
	for (int i = 0; i < n; i++) t = t * 60.0 + float(parts[i]);
	out = t;
	return true;
}

void ClockLabel(float secs, char[] out, int maxlen)
{
	int t = RoundToFloor(secs);
	if (t >= 3600) Format(out, maxlen, "%d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60);
	else Format(out, maxlen, "%d:%02d", t / 60, t % 60);
}

// !tv +30 / !tv -10 / !tv 1:23. The clock holds at the new spot until a viewer's video has buffered
// there and plays again (cstv_started), so nobody ends up behind and jumping afterwards.
void SeekCmd(int client, const char[] arg)
{
	if (!g_sUrl[0]) { PrintToChat(client, " \x04[CS:TV]\x01 Nothing is playing."); return; }
	if (g_iKind == LINK_TWITCH_LIVE) { PrintToChat(client, " \x04[CS:TV]\x01 A live stream can't be seeked."); return; }
	bool rel = arg[0] == '+' || arg[0] == '-';
	float t;
	if (!ParseClock(arg[rel ? 1 : 0], t)) { PrintToChat(client, " \x04[CS:TV]\x01 Usage: \x04!tv +30\x01, \x04!tv -10\x01 or \x04!tv 1:23"); return; }
	float pos = rel ? CurrentPos() + (arg[0] == '-' ? -t : t) : t;
	if (pos < 0.0) pos = 0.0;
	g_fBasePos = pos;
	g_fBaseTime = GetEngineTime();
	g_bLoading = true;
	g_fLoadStart = GetEngineTime();
	g_iSeekId++;
	ForModded(1);
	char lbl[16];
	ClockLabel(pos, lbl, sizeof lbl);
	if (rel) PrintToChatAll(" \x04[CS:TV]\x01 %N skipped %s %d s (now at %s).", client, arg[0] == '-' ? "back" : "forward", RoundToFloor(t), lbl);
	else PrintToChatAll(" \x04[CS:TV]\x01 %N jumped to %s.", client, lbl);
}

public Action Cmd_TvPrev(int client, int args)
{
	if (!client || !Can(client, PERM_SKIP)) return Plugin_Handled;
	if (!g_bHaveScreen) { PrintToChat(client, " \x04[CS:TV]\x01 Place a screen first with \x04!tv small|medium|large\x01."); return Plugin_Handled; }
	int last = g_hHistory.Length - 1;
	if (last < 0) { PrintToChat(client, " \x04[CS:TV]\x01 Nothing to go back to."); return Plugin_Handled; }
	char url[MAX_URL], title[192];
	g_hHistory.GetString(last, url, sizeof url);
	g_hHistoryTitle.GetString(last, title, sizeof title);
	float pos = g_hHistoryPos.Get(last);
	g_hHistory.Erase(last);
	g_hHistoryPos.Erase(last);
	g_hHistoryTitle.Erase(last);
	// the current video comes back on "next": a playlist finds it by itself, anything else goes first in the queue
	if (g_sUrl[0]) {
		char a[64], b[64];
		bool samePlaylist = YtList(g_sUrl, a, sizeof a) && YtList(url, b, sizeof b) && StrEqual(a, b);
		if (!samePlaylist) {
			char name[MAX_NAME_LENGTH];
			GetClientName(client, name, sizeof name);
			if (g_hQueue.Length >= QUEUE_MAX) QueueErase(g_hQueue.Length - 1);
			QueuePush(g_sUrl, name, true, g_sCurTitle);
		}
	}
	SetLink(url, true, pos, false, false, title);
	char lbl[16], label[100];
	ClockLabel(pos, lbl, sizeof lbl);
	LinkLabel(url, title, label, sizeof label);
	if (pos > 1.0) PrintToChatAll(" \x04[CS:TV]\x01 %N went back to \x04%s\x01 (from %s).", client, label, lbl);
	else PrintToChatAll(" \x04[CS:TV]\x01 %N went back to \x04%s", client, label);
	return Plugin_Handled;
}

void PlayLink(int client, const char[] input)
{
	char url[MAX_URL];
	strcopy(url, sizeof url, input);
	TrimString(url);
	StripQuotes(url);
	if (!UrlOk(url)) { PrintToChat(client, " \x04[CS:TV]\x01 That doesn't look like an http(s) link."); return; }
	SetLink(url, false);
	g_sResumeList[0] = '\0';   // a new link replaces whatever playlist was interrupted
	g_sResume[0] = '\0';
	char shortUrl[64];
	ShortUrl(url, shortUrl, sizeof shortUrl);
	PrintToChatAll(" \x04[CS:TV]\x01 %N is playing \x04%s", client, shortUrl);
	AnnounceOptions(url);
}

void AnnounceOptions(const char[] url)
{
	int kind = LinkKind(url);
	char list[64];
	if (kind == LINK_YOUTUBE && YtList(url, list, sizeof list))
		PrintToChatAll(" \x04[CS:TV]\x01 Playlist: \x04!queuemenu\x01 shows what's next, \x04!tvnext\x01 skips, \x04!tvset\x01 for settings");
	else PrintToChatAll(" \x04[CS:TV]\x01 Settings: \x04!tvset\x01 (quality, %s, volume)", kind == LINK_TWITCH_LIVE ? "chat, stream info" : "subtitles, loop, speed");
}

// ------------------------------------------------------------------ titles (looked up by a viewer's helper)
int AnyModded(int prefer = 0)
{
	if (prefer > 0 && IsClientInGame(prefer) && g_bModded[prefer]) return prefer;
	for (int i = 1; i <= MaxClients; i++)
		if (IsClientInGame(i) && g_bModded[i]) return i;
	return 0;
}

void RequestTitle(int uid, const char[] url)
{
	int target = AnyModded();
	if (!target) return;
	char b64[MAX_URL * 2], v[220];
	Base64UrlEncode(url, b64, sizeof b64);
	if (strlen(b64) > 180) return;   // one message holds ~200 characters; very long links stay untitled
	Format(v, sizeof v, "%d %s", uid, b64);
	Send(target, "title", v);
}

// cstv_title <uid> <title b64url>
public Action Cmd_Title(int client, int args)
{
	if (!client || !g_bModded[client] || args < 2) return Plugin_Handled;
	char a[16], tb[256], title[192];
	GetCmdArg(1, a, sizeof a);
	GetCmdArg(2, tb, sizeof tb);
	if (!Base64UrlDecode(tb, title, sizeof title)) return Plugin_Handled;
	CleanTitle(title);
	int uid = StringToInt(a);
	if (uid == g_iCurTitleUid) strcopy(g_sCurTitle, sizeof g_sCurTitle, title);
	int i = QueueFind(uid);
	if (i != -1) g_hQueueTitle.SetString(i, title);
	return Plugin_Handled;
}

// ------------------------------------------------------------------ YouTube playlists / radio mixes
// list= of a YouTube link
bool YtList(const char[] url, char[] list, int maxlen)
{
	list[0] = '\0';
	if (LinkKind(url) != LINK_YOUTUBE) return false;
	int q = FindCharInString(url, '?');
	if (q == -1) return false;
	for (int i = q; url[i] != '\0'; i++) {
		if ((url[i] == '?' || url[i] == '&') && strncmp(url[i + 1], "list=", 5) == 0) {
			int o = 0, j = i + 6;
			while (url[j] != '\0' && url[j] != '&' && url[j] != '#' && o < maxlen - 1) list[o++] = url[j++];
			list[o] = '\0';
			return o > 0;
		}
	}
	return false;
}

// the video id of a YouTube link (v= or youtu.be/<id>)
bool YtVid(const char[] url, char[] out, int maxlen)
{
	out[0] = '\0';
	int start = -1;
	int p = StrContains(url, "youtu.be/", false);
	if (p != -1) start = p + 9;
	else {
		int q = FindCharInString(url, '?');
		for (int i = (q == -1 ? 0 : q); q != -1 && url[i] != '\0'; i++)
			if ((url[i] == '?' || url[i] == '&') && url[i + 1] == 'v' && url[i + 2] == '=') { start = i + 3; break; }
	}
	if (start == -1) return false;
	int o = 0;
	for (int j = start; url[j] != '\0' && url[j] != '&' && url[j] != '?' && url[j] != '#' && url[j] != '/' && o < maxlen - 1; j++) out[o++] = url[j];
	out[o] = '\0';
	return o > 0;
}

bool IdOk(const char[] s)
{
	if (!s[0]) return false;
	for (int i = 0; s[i] != '\0'; i++)
		if (!IsCharAlpha(s[i]) && !IsCharNumeric(s[i]) && s[i] != '-' && s[i] != '_') return false;
	return true;
}

bool IsRadio(const char[] list) { return strncmp(list, "RD", 2) == 0; }

bool IsRemoved(const char[] id)
{
	int v;
	return g_sRemovedList[0] && StrEqual(g_sRemovedList, g_sWinList) && g_hPlRemoved.GetValue(id, v);
}

// The window: the next videos of the playlist (sent by a viewer's helper after each video starts).
// cstv_plist <rev> <list> <current id>, cstv_pitem <rev> <index> <id> <title b64url | ->, cstv_pend <rev>
public Action Cmd_PlList(int client, int args)
{
	if (!client || !g_bModded[client] || args < 3) return Plugin_Handled;
	char a[16], list[64], cur[64];
	GetCmdArg(1, a, sizeof a);
	GetCmdArg(2, list, sizeof list);
	if (StringToInt(a) != g_iRev || !YtList(g_sUrl, cur, sizeof cur) || !StrEqual(cur, list)) return Plugin_Handled;
	if (g_iWinRev == g_iRev && g_iWinFrom != client && g_iWinFrom > 0 && IsClientInGame(g_iWinFrom)) return Plugin_Handled;   // another viewer's came first
	g_iWinRev = g_iRev;
	g_iWinFrom = client;
	if (!StrEqual(g_sWinList, list)) { g_hPlRemoved.Clear(); strcopy(g_sRemovedList, sizeof g_sRemovedList, list); }
	strcopy(g_sWinList, sizeof g_sWinList, list);
	g_hWinIds.Clear();
	g_hWinIdx.Clear();
	g_hWinTitles.Clear();
	return Plugin_Handled;
}

public Action Cmd_PlItem(int client, int args)
{
	if (!client || args < 4) return Plugin_Handled;
	char a[16], ix[16], id[24], tb[256], title[192];
	GetCmdArg(1, a, sizeof a);
	if (StringToInt(a) != g_iRev || g_iWinRev != g_iRev || client != g_iWinFrom || g_hWinIds.Length >= 12) return Plugin_Handled;
	GetCmdArg(2, ix, sizeof ix);
	GetCmdArg(3, id, sizeof id);
	GetCmdArg(4, tb, sizeof tb);
	if (!IdOk(id)) return Plugin_Handled;
	title[0] = '\0';
	if (!StrEqual(tb, "-")) Base64UrlDecode(tb, title, sizeof title);
	CleanTitle(title);
	g_hWinIds.PushString(id);
	g_hWinIdx.Push(StringToInt(ix));
	g_hWinTitles.PushString(title);
	return Plugin_Handled;
}

public Action Cmd_PlEnd(int client, int args)
{
	if (client && client == g_iWinFrom) UpdateUpNext();   // the playlist's next videos are known now
	return Plugin_Handled;
}

// the first window video that isn't the current one or removed; returns its slot (-1 = none)
int WindowNext(char[] url, int ulen, char[] title, int tlen)
{
	if (!g_sWinList[0]) return -1;
	char cur[24], id[24];
	YtVid(g_sUrl, cur, sizeof cur);
	for (int i = 0; i < g_hWinIds.Length; i++) {
		g_hWinIds.GetString(i, id, sizeof id);
		if (StrEqual(id, cur) || IsRemoved(id)) continue;
		Format(url, ulen, "https://www.youtube.com/watch?v=%s&list=%s&index=%d", id, g_sWinList, g_hWinIdx.Get(i));
		g_hWinTitles.GetString(i, title, tlen);
		return i;
	}
	return -1;
}

// is the window about what's playing (or about the playlist the queue interrupted)?
bool WindowActive()
{
	if (!g_sWinList[0]) return false;
	char list[64];
	return (YtList(g_sUrl, list, sizeof list) && StrEqual(list, g_sWinList)) || StrEqual(g_sResumeList, g_sWinList);
}

void DropWindowThrough(const char[] url)
{
	char id[24], w[24], list[64];
	if (!YtVid(url, id, sizeof id) || !YtList(url, list, sizeof list) || !StrEqual(list, g_sWinList)) return;
	for (int i = 0; i < g_hWinIds.Length; i++) {
		g_hWinIds.GetString(i, w, sizeof w);
		if (!StrEqual(w, id)) continue;
		for (int k = 0; k <= i; k++) { g_hWinIds.Erase(0); g_hWinIdx.Erase(0); g_hWinTitles.Erase(0); }
		return;
	}
}

// a modded client's helper reports the current video ended (or a skip it was asked for):
// cstv_next <rev> <next playlist url b64url | -> <title b64url | ->
public Action Cmd_Next(int client, int args)
{
	if (!client || !g_bModded[client] || args < 2 || !g_sUrl[0]) return Plugin_Handled;
	char a[16];
	GetCmdArg(1, a, sizeof a);
	if (StringToInt(a) != g_iRev) return Plugin_Handled;   // another viewer's report already moved us on
	if (g_bLoop && g_iSkipRev != g_iRev) {
		if (g_bLoopHold || GetEngineTime() - g_fLoopAt < 3.0) return Plugin_Handled;   // another viewer reporting the same end
		if (!g_bLoading) { LoopRestart(); return Plugin_Handled; }
		// it never started (couldn't be played): move on as usual
	}
	char b64[MAX_URL * 2], url[MAX_URL], tb64[256], title[192];
	GetCmdArg(2, b64, sizeof b64);
	bool haveNext = false;
	if (!StrEqual(b64, "-") && Base64UrlDecode(b64, url, sizeof url) && UrlOk(url)) {
		char cur[64], nxt[64];
		haveNext = YtList(g_sUrl, cur, sizeof cur) && YtList(url, nxt, sizeof nxt) && StrEqual(cur, nxt);
	}
	title[0] = '\0';
	if (haveNext && args >= 3) {
		GetCmdArg(3, tb64, sizeof tb64);
		if (!StrEqual(tb64, "-")) Base64UrlDecode(tb64, title, sizeof title);
	}
	CleanTitle(title);
	Advance(haveNext ? url : "", title, g_iSkipRev != g_iRev);
	return Plugin_Handled;
}

// looping: the video ended, so everyone seeks back to the start (the clock holds until it plays again)
void LoopRestart()
{
	g_fBasePos = 0.0;
	g_fBaseTime = GetEngineTime();
	g_bLoading = true;
	g_bLoopHold = true;
	g_fLoadStart = GetEngineTime();
	g_fLoopAt = GetEngineTime();
	g_iSeekId++;
	ForModded(1);
}

// The current video is over (or skipped). Order: the queue, the playlist, an interrupted playlist.
// The playlist's next video comes from the window (respects removals); the helper's suggestion is
// the fallback. Returns false when there is nothing to play.
bool Advance(const char[] helperNext, const char[] helperTitle, bool finished)
{
	char plUrl[MAX_URL], plTitle[192], list[64];
	bool inPlaylist = YtList(g_sUrl, list, sizeof list);
	bool havePl = inPlaylist && WindowNext(plUrl, sizeof plUrl, plTitle, sizeof plTitle) != -1;
	if (!havePl && helperNext[0]) {
		char id[24];
		if (!YtVid(helperNext, id, sizeof id) || !IsRemoved(id)) {
			strcopy(plUrl, sizeof plUrl, helperNext);
			strcopy(plTitle, sizeof plTitle, helperTitle);
			havePl = true;
		}
	}
	if (g_hQueue.Length > 0) {
		if (inPlaylist) {   // the playlist picks up again after the queue
			strcopy(g_sResumeList, sizeof g_sResumeList, list);
			if (havePl) { strcopy(g_sResume, sizeof g_sResume, plUrl); strcopy(g_sResumeTitle, sizeof g_sResumeTitle, plTitle); }
			else g_sResume[0] = '\0';
		}
		PlayQueued(0, finished, true);
		return true;
	}
	if (havePl) {
		SetLink(plUrl, true, 0.0, true, finished, plTitle);
		if (plTitle[0]) UpNextNotice(" \x04[CS:TV]\x01 Up next: \x04%s", plTitle);
		else UpNextNotice(" \x04[CS:TV]\x01 Up next in the playlist.");
		return true;
	}
	if (g_sResumeList[0]) {   // back to the interrupted playlist: its next video per the window, else what we noted
		char url[MAX_URL], title[192];
		bool ok = StrEqual(g_sResumeList, g_sWinList) && WindowNext(url, sizeof url, title, sizeof title) != -1;
		if (!ok && g_sResume[0]) { strcopy(url, sizeof url, g_sResume); strcopy(title, sizeof title, g_sResumeTitle); ok = true; }
		g_sResumeList[0] = '\0';
		g_sResume[0] = '\0';
		if (ok) {
			SetLink(url, true, 0.0, true, finished, title);
			if (title[0]) UpNextNotice(" \x04[CS:TV]\x01 Back to the playlist: \x04%s", title);
			else UpNextNotice(" \x04[CS:TV]\x01 Back to the playlist.");
			return true;
		}
	}
	return false;
}

// Tell modded viewers what plays next, so their helper does YouTube's ~2 s lookup while the current
// video is still playing (skips and auto-advance then start that much sooner).
void UpdateUpNext(int only = 0)
{
	char url[MAX_URL], title[192];
	url[0] = '\0';
	if (g_hQueue.Length > 0) g_hQueue.GetString(0, url, sizeof url);
	else if (WindowNext(url, sizeof url, title, sizeof title) == -1) url[0] = '\0';
	if (!only && StrEqual(url, g_sUpNext)) return;
	if (!only) strcopy(g_sUpNext, sizeof g_sUpNext, url);
	if (!url[0]) return;
	char b64[MAX_URL * 2];
	Base64UrlEncode(url, b64, sizeof b64);
	if (strlen(b64) > 180) return;
	for (int i = 1; i <= MaxClients; i++)
		if (IsClientInGame(i) && g_bModded[i] && (!only || i == only)) Send(i, "upnext", b64);
}

// ------------------------------------------------------------------ the queue
void QueuePush(const char[] url, const char[] by, bool front = false, const char[] title = "")
{
	int uid = ++g_iUidSeq;
	if (front && g_hQueue.Length > 0) {
		g_hQueue.ShiftUp(0);
		g_hQueueBy.ShiftUp(0);
		g_hQueueUid.ShiftUp(0);
		g_hQueueTitle.ShiftUp(0);
		g_hQueue.SetString(0, url);
		g_hQueueBy.SetString(0, by);
		g_hQueueUid.Set(0, uid);
		g_hQueueTitle.SetString(0, title);
	} else {
		g_hQueue.PushString(url);
		g_hQueueBy.PushString(by);
		g_hQueueUid.Push(uid);
		g_hQueueTitle.PushString(title);
	}
	if (!title[0]) RequestTitle(uid, url);
	UpdateUpNext();
}

void QueueErase(int i)
{
	g_hQueue.Erase(i);
	g_hQueueBy.Erase(i);
	g_hQueueUid.Erase(i);
	g_hQueueTitle.Erase(i);
	UpdateUpNext();
}

int QueueFind(int uid)
{
	for (int i = 0; i < g_hQueueUid.Length; i++)
		if (g_hQueueUid.Get(i) == uid) return i;
	return -1;
}

void PlayQueued(int i, bool finished, bool announce)
{
	char url[MAX_URL], by[MAX_NAME_LENGTH], title[192], label[100];
	g_hQueue.GetString(i, url, sizeof url);
	g_hQueueBy.GetString(i, by, sizeof by);
	g_hQueueTitle.GetString(i, title, sizeof title);
	QueueErase(i);
	SetLink(url, false, 0.0, true, finished, title);
	LinkLabel(url, title, label, sizeof label);
	if (announce) UpNextNotice(" \x04[CS:TV]\x01 From the queue (%s): \x04%s\x01%s", by, label, g_hQueue.Length ? "" : " (queue now empty)");
	AnnounceOptions(url);
}

// the queue only (the !clearqueue command)
void ClearQueuedLinks()
{
	g_hQueue.Clear();
	g_hQueueBy.Clear();
	g_hQueueUid.Clear();
	g_hQueueTitle.Clear();
	UpdateUpNext();
}

// everything (map change, screen removed, server empty)
void ClearQueue()
{
	if (g_hQueue == null) return;
	ClearQueuedLinks();
	g_sResume[0] = '\0';
	g_sResumeList[0] = '\0';
	g_hHistory.Clear();
	g_hHistoryPos.Clear();
	g_hHistoryTitle.Clear();
	g_sWinList[0] = '\0';
	g_hWinIds.Clear();
	g_hWinIdx.Clear();
	g_hWinTitles.Clear();
	g_hPlRemoved.Clear();
	g_sCurTitle[0] = '\0';
	g_bLoading = false;
}

void ShortUrl(const char[] url, char[] out, int maxlen)
{
	char tmp[MAX_URL];
	strcopy(tmp, sizeof tmp, url);
	ReplaceString(tmp, sizeof tmp, "https://", "");
	ReplaceString(tmp, sizeof tmp, "http://", "");
	ReplaceString(tmp, sizeof tmp, "www.", "");
	strcopy(out, maxlen, tmp);
	if (strlen(tmp) >= maxlen - 1 && maxlen > 4) {
		out[maxlen - 4] = '.';
		out[maxlen - 3] = '.';
		out[maxlen - 2] = '.';
	}
}

public Action Cmd_Queue(int client, int args)
{
	if (!client) return Plugin_Handled;
	char arg[MAX_URL];
	GetCmdArgString(arg, sizeof arg);
	TrimString(arg);
	StripQuotes(arg);
	if (!arg[0]) { ShowQueueMenu(client); return Plugin_Handled; }
	if (StrEqual(arg, "clear", false)) return Cmd_ClearQueue(client, 0);
	if (!Can(client, PERM_QUEUE)) return Plugin_Handled;
	if (!g_bHaveScreen) { PrintToChat(client, " \x04[CS:TV]\x01 Place a screen first with \x04!tv small|medium|large\x01."); return Plugin_Handled; }
	if (!UrlOk(arg)) { PrintToChat(client, " \x04[CS:TV]\x01 That doesn't look like an http(s) link."); return Plugin_Handled; }
	if (!g_sUrl[0]) {   // nothing playing: just play it
		PlayLink(client, arg);
		return Plugin_Handled;
	}
	if (g_hQueue.Length >= QUEUE_MAX) { PrintToChat(client, " \x04[CS:TV]\x01 The queue is full (%d).", QUEUE_MAX); return Plugin_Handled; }
	char name[MAX_NAME_LENGTH];
	GetClientName(client, name, sizeof name);
	QueuePush(arg, name);
	char shortUrl[64];
	ShortUrl(arg, shortUrl, sizeof shortUrl);
	PrintToChatAll(" \x04[CS:TV]\x01 %N queued \x04%s\x01 (#%d). See it with \x04!queuemenu", client, shortUrl, g_hQueue.Length);
	if (g_iKind == LINK_TWITCH_LIVE) PrintToChat(client, " \x04[CS:TV]\x01 A live stream doesn't end by itself: use \x04!tvnext\x01 to move on.");
	return Plugin_Handled;
}

public Action Cmd_ClearQueue(int client, int args)
{
	if (!client || !Can(client, PERM_QUEUE)) return Plugin_Handled;
	if (!g_hQueue.Length) { PrintToChat(client, " \x04[CS:TV]\x01 The queue is already empty."); return Plugin_Handled; }
	int n = g_hQueue.Length;
	ClearQueuedLinks();
	PrintToChatAll(" \x04[CS:TV]\x01 %N cleared the queue (%d link%s).", client, n, n == 1 ? "" : "s");
	return Plugin_Handled;
}

// ------------------------------------------------------------------ !queuemenu
public Action Cmd_QueueMenu(int client, int args)
{
	if (client) ShowQueueMenu(client);
	return Plugin_Handled;
}

void ShowQueueMenu(int client)
{
	Menu m = new Menu(Menu_QueueH);
	char now[100];
	if (g_sUrl[0]) LinkLabel(g_sUrl, g_sCurTitle, now, sizeof now);
	else strcopy(now, sizeof now, "nothing");
	m.SetTitle("CS:TV - what's next\nNow: %s\n ", now);
	char info[40], disp[160], url[MAX_URL], title[192], by[MAX_NAME_LENGTH], label[100];
	for (int i = 0; i < g_hQueue.Length; i++) {
		g_hQueue.GetString(i, url, sizeof url);
		g_hQueueBy.GetString(i, by, sizeof by);
		g_hQueueTitle.GetString(i, title, sizeof title);
		LinkLabel(url, title, label, sizeof label);
		Format(info, sizeof info, "q:%d", g_hQueueUid.Get(i));
		Format(disp, sizeof disp, "%s  (%s)", label, by);
		m.AddItem(info, disp);
	}
	int shown = 0;
	if (WindowActive() && g_hWinIds.Length == 0) {   // the helper looked and found nothing after this video
		m.AddItem("", "(can't see the rest of this playlist: is it private?)", ITEMDRAW_DISABLED);
		shown++;
	} else if (WindowActive()) {
		char id[24], cur[24];
		YtVid(g_sUrl, cur, sizeof cur);
		for (int i = 0; i < g_hWinIds.Length && shown < 10; i++) {
			g_hWinIds.GetString(i, id, sizeof id);
			if (StrEqual(id, cur) || IsRemoved(id)) continue;
			g_hWinTitles.GetString(i, title, sizeof title);
			Format(info, sizeof info, "p:%s", id);
			Format(disp, sizeof disp, "[%s] %s", IsRadio(g_sWinList) ? "radio" : "playlist", title[0] ? title : id);
			m.AddItem(info, disp);
			shown++;
		}
	} else {
		char list[64];
		if (YtList(g_sUrl, list, sizeof list)) { m.AddItem("", "(looking up the playlist...)", ITEMDRAW_DISABLED); shown++; }
	}
	if (!g_hQueue.Length && !shown) m.AddItem("", "Nothing queued. Add a link: !queue <url>", ITEMDRAW_DISABLED);
	if (g_hQueue.Length) m.AddItem("clear", "Clear the queue");
	m.Display(client, 60);
}

public int Menu_QueueH(Menu m, MenuAction action, int client, int item)
{
	if (action == MenuAction_End) delete m;
	else if (action == MenuAction_Select) {
		char info[40];
		m.GetItem(item, info, sizeof info);
		if (StrEqual(info, "clear")) { Cmd_ClearQueue(client, 0); ShowQueueMenu(client); }
		else if (info[0]) ShowQueueItem(client, info);
	}
	return 0;
}

void ShowQueueItem(int client, const char[] info)
{
	Menu m = new Menu(Menu_QueueItemH);
	char act[48], label[100], url[MAX_URL], title[192], by[MAX_NAME_LENGTH];
	if (info[0] == 'q') {
		int i = QueueFind(StringToInt(info[2]));
		if (i == -1) { delete m; PrintToChat(client, " \x04[CS:TV]\x01 That isn't in the queue anymore."); ShowQueueMenu(client); return; }
		g_hQueue.GetString(i, url, sizeof url);
		g_hQueueBy.GetString(i, by, sizeof by);
		g_hQueueTitle.GetString(i, title, sizeof title);
		LinkLabel(url, title, label, sizeof label);
		m.SetTitle("%s\nqueued by %s (#%d)\n ", label, by, i + 1);
		Format(act, sizeof act, "play|%s", info);
		m.AddItem(act, "Play now");
		Format(act, sizeof act, "next|%s", info);
		m.AddItem(act, "Play next", i > 0 ? ITEMDRAW_DEFAULT : ITEMDRAW_DISABLED);
		Format(act, sizeof act, "remove|%s", info);
		m.AddItem(act, "Remove from the queue");
	} else {
		int slot = WindowSlot(info[2]);
		if (slot == -1) { delete m; PrintToChat(client, " \x04[CS:TV]\x01 That isn't coming up anymore."); ShowQueueMenu(client); return; }
		g_hWinTitles.GetString(slot, title, sizeof title);
		if (!title[0]) strcopy(title, sizeof title, info[2]);
		m.SetTitle("%s\n(%s)\n ", title, IsRadio(g_sWinList) ? "radio" : "playlist");
		Format(act, sizeof act, "play|%s", info);
		m.AddItem(act, "Play now (skips the ones before it)");
		Format(act, sizeof act, "remove|%s", info);
		m.AddItem(act, IsRadio(g_sWinList) ? "Remove from the radio" : "Remove from the playlist");
	}
	m.ExitBackButton = true;
	m.Display(client, 60);
}

int WindowSlot(const char[] id)
{
	char w[24];
	for (int i = 0; i < g_hWinIds.Length; i++) {
		g_hWinIds.GetString(i, w, sizeof w);
		if (StrEqual(w, id)) return i;
	}
	return -1;
}

public int Menu_QueueItemH(Menu m, MenuAction action, int client, int item)
{
	if (action == MenuAction_End) delete m;
	else if (action == MenuAction_Cancel && item == MenuCancel_ExitBack) ShowQueueMenu(client);
	else if (action == MenuAction_Select) {
		char info[48];
		m.GetItem(item, info, sizeof info);
		int bar = FindCharInString(info, '|');
		if (bar == -1) return 0;
		char target[40];
		strcopy(target, sizeof target, info[bar + 1]);
		info[bar] = '\0';
		QueueAction(client, info, target);
		ShowQueueMenu(client);
	}
	return 0;
}

void QueueAction(int client, const char[] what, const char[] target)
{
	if (!Can(client, PERM_QUEUE)) return;
	char url[MAX_URL], title[192], label[100];
	if (target[0] == 'q') {
		int i = QueueFind(StringToInt(target[2]));
		if (i == -1) { PrintToChat(client, " \x04[CS:TV]\x01 That isn't in the queue anymore."); return; }
		g_hQueue.GetString(i, url, sizeof url);
		g_hQueueTitle.GetString(i, title, sizeof title);
		LinkLabel(url, title, label, sizeof label);
		if (StrEqual(what, "play")) {
			char list[64];
			if (YtList(g_sUrl, list, sizeof list) && !g_sResumeList[0]) {   // a playlist gets interrupted: it continues afterwards
				strcopy(g_sResumeList, sizeof g_sResumeList, list);
				g_sResume[0] = '\0';
			}
			PrintToChatAll(" \x04[CS:TV]\x01 %N played \x04%s\x01 from the queue.", client, label);
			PlayQueued(i, false, false);
		} else if (StrEqual(what, "next")) {
			char by[MAX_NAME_LENGTH];
			g_hQueueBy.GetString(i, by, sizeof by);
			int uid = g_hQueueUid.Get(i);
			QueueErase(i);
			QueuePush(url, by, true, title);
			g_hQueueUid.Set(0, uid);   // keep its identity (and a pending title lookup)
			PrintToChatAll(" \x04[CS:TV]\x01 %N moved \x04%s\x01 to the front of the queue.", client, label);
		} else if (StrEqual(what, "remove")) {
			QueueErase(i);
			PrintToChatAll(" \x04[CS:TV]\x01 %N removed \x04%s\x01 from the queue.", client, label);
		}
		return;
	}
	int slot = WindowSlot(target[2]);
	if (slot == -1) { PrintToChat(client, " \x04[CS:TV]\x01 That isn't coming up anymore."); return; }
	g_hWinTitles.GetString(slot, title, sizeof title);
	strcopy(label, sizeof label, title);
	if (!label[0]) strcopy(label, sizeof label, target[2]);
	if (StrEqual(what, "play")) {
		Format(url, sizeof url, "https://www.youtube.com/watch?v=%s&list=%s&index=%d", target[2], g_sWinList, g_hWinIdx.Get(slot));
		g_sResumeList[0] = '\0';   // we're in the playlist again
		g_sResume[0] = '\0';
		SetLink(url, true, 0.0, true, false, title);
		PrintToChatAll(" \x04[CS:TV]\x01 %N skipped ahead to \x04%s", client, label);
	} else if (StrEqual(what, "remove")) {
		if (!StrEqual(g_sRemovedList, g_sWinList)) { g_hPlRemoved.Clear(); strcopy(g_sRemovedList, sizeof g_sRemovedList, g_sWinList); }
		g_hPlRemoved.SetValue(target[2], 1);
		UpdateUpNext();
		PrintToChatAll(" \x04[CS:TV]\x01 %N removed \x04%s\x01 from the %s.", client, label, IsRadio(g_sWinList) ? "radio" : "playlist");
	}
}

// ------------------------------------------------------------------ next / skip
void RequestSkip(int client)
{
	if (!Can(client, PERM_SKIP)) return;
	if (!g_sUrl[0]) { PrintToChat(client, " \x04[CS:TV]\x01 Nothing is playing."); return; }
	char list[64], u[MAX_URL], t[192];
	bool inPlaylist = YtList(g_sUrl, list, sizeof list);
	if (!inPlaylist || g_hQueue.Length > 0 || WindowNext(u, sizeof u, t, sizeof t) != -1) {   // the server knows what's next
		if (Advance("", "", false)) PrintToChatAll(" \x04[CS:TV]\x01 %N skipped.", client);
		else PrintToChat(client, " \x04[CS:TV]\x01 Nothing to skip to: the queue is empty (\x04!queue <url>\x01).");
		return;
	}
	// a playlist whose next video isn't known here yet: ask a viewer's helper
	int target = AnyModded(client);
	if (!target) { PrintToChat(client, " \x04[CS:TV]\x01 Nobody with the CS:TV client mod is here to look up the playlist."); return; }
	g_iSkipRev = g_iRev;
	Send(target, "skip", "");
	PrintToChatAll(" \x04[CS:TV]\x01 %N skipped to the next video.", client);
}

public Action Cmd_TvNext(int client, int args)
{
	if (client) RequestSkip(client);
	return Plugin_Handled;
}

// ------------------------------------------------------------------ !tvhelp
public Action Cmd_TvHelp(int client, int args)
{
	if (client) ShowHelp(client, 0);
	return Plugin_Handled;
}

void ShowHelp(int client, int first)
{
	Menu m = new Menu(Menu_HelpH);
	m.SetTitle("CS:TV help - pick a topic\n ");
	m.AddItem("screen", "The screen (!tv, !tvremove)");
	m.AddItem("link", "Playing links (!link)");
	m.AddItem("sites", "Which sites work");
	m.AddItem("queue", "The queue (!queue, !queuemenu)");
	m.AddItem("skip", "Next / previous (!tvnext, !tvprev)");
	m.AddItem("seek", "Seeking (!tv +30 / -10 / 1:23)");
	m.AddItem("pause", "Pausing (click the screen)");
	m.AddItem("vol", "Your volume (!tvvol)");
	m.AddItem("set", "Settings (!tvset)");
	m.AddItem("perms", "What you're allowed to do");
	m.AddItem("about", "About CS:TV");
	m.DisplayAt(client, first, 120);
}

public int Menu_HelpH(Menu m, MenuAction action, int client, int item)
{
	if (action == MenuAction_End) delete m;
	else if (action == MenuAction_Select) {
		char key[16];
		m.GetItem(item, key, sizeof key);
		g_iHelpFirst[client] = m.Selection;
		ShowHelpPage(client, key);
	}
	return 0;
}

void ShowHelpPage(int client, const char[] key)
{
	Panel p = new Panel();
	if (StrEqual(key, "screen")) {
		p.SetTitle("The screen");
		p.DrawText("!tv small | medium | large | huge");
		p.DrawText("   puts the screen where you aim: wall, floor or ceiling.");
		p.DrawText("!tv large float (or !tv large float 500)");
		p.DrawText("   hangs it in the air 300 (or 500) units in front of you,");
		p.DrawText("   facing you; it reads right from both sides.");
		p.DrawText("   There is one screen; placing it again moves it.");
		p.DrawText("!tvremove (or !tv remove)");
		p.DrawText("   takes it down and stops the video.");
	} else if (StrEqual(key, "link")) {
		p.SetTitle("Playing links");
		p.DrawText("!link <url> (or !tv link <url>)");
		p.DrawText("   YouTube videos, playlists, radio mixes and live");
		p.DrawText("   streams, Twitch channels and VODs, most video sites.");
		p.DrawText("Everyone sees the same moment of the video.");
		p.DrawText("Watching needs the CS:TV client mod (Linux or Windows).");
	} else if (StrEqual(key, "queue")) {
		p.SetTitle("The queue");
		p.DrawText("!queue <url> - plays after the current video");
		p.DrawText("!queuemenu (or !queue) - what's coming up, including");
		p.DrawText("   the next videos of a playlist or radio. Pick one to");
		p.DrawText("   play it now, move it up, or remove it.");
		p.DrawText("!clearqueue - empties the queue");
		p.DrawText("Queued links cut into a playlist, which continues after.");
	} else if (StrEqual(key, "skip")) {
		p.SetTitle("Next / previous");
		p.DrawText("!tvnext - the next queued link or playlist video");
		p.DrawText("!tvprev - back to the previous video; one that was");
		p.DrawText("   skipped resumes where it was left");
	} else if (StrEqual(key, "seek")) {
		p.SetTitle("Seeking (for everyone)");
		p.DrawText("!tv +30   forward 30 seconds");
		p.DrawText("!tv -10   back 10 seconds");
		p.DrawText("!tv 1:23  jump to 1:23 (or 1:02:03)");
		p.DrawText("Live streams can't be seeked.");
	} else if (StrEqual(key, "pause")) {
		p.SetTitle("Pausing");
		p.DrawText("Look at the screen and press attack or use");
		p.DrawText("to pause or resume for everyone.");
		p.DrawText("!tvpause does the same from anywhere.");
	} else if (StrEqual(key, "vol")) {
		p.SetTitle("Your volume");
		p.DrawText("!tvvol <0-100> - only changes it for you.");
		p.DrawText("The sound also fades with distance from the screen.");
	} else if (StrEqual(key, "set")) {
		p.SetTitle("Settings: !tvset (or !tvyt / !tvttv)");
		p.DrawText("One menu for every site.");
		p.DrawText("Just for you, remembered: quality, subtitles,");
		p.DrawText("   live chat panel, volume, 'Up next' messages");
		p.DrawText("For everyone: speed, loop, next video");
		p.DrawText("   (not for live streams)");
		p.DrawText("Twitch live: stream info card (viewers, uptime,");
		p.DrawText("   game, title) on the screen for 10 seconds");
	} else if (StrEqual(key, "sites")) {
		p.SetTitle("Which sites work");   // the HUD menu shows ~500 characters at most
		p.DrawText("Whatever yt-dlp opens (~1,800 sites).");
		p.DrawText("Tried: YouTube, Twitch live, Bilibili.");
		p.DrawText("Should work: Twitch VODs, Kick, Vimeo,");
		p.DrawText("   Dailymotion, X, Reddit, TikTok, Niconico,");
		p.DrawText("   SoundCloud (sound only).");
		p.DrawText("Never: Netflix, Disney+, Spotify (paid/DRM).");
		p.DrawText("Each player's PC loads the video itself:");
		p.DrawText("   region blocks are per player.");
		p.DrawText("Live chat panel: Twitch and YouTube only.");
	} else if (StrEqual(key, "perms")) {
		p.SetTitle("What you're allowed to do");
		int have = PermsOf(client);
		char line[96];
		for (int i = 0; i < PERM_COUNT; i++) {
			Format(line, sizeof line, "%s %s", (have & (1 << i)) ? "[yes]" : "[no] ", g_sPermLabel[i]);
			p.DrawText(line);
		}
		p.DrawText("Volume, quality, subtitles and chat are always yours.");
		p.DrawText(IsTvAdmin(client) ? "You're an admin: !tvadmin sets this for others." : "Admins decide this (!tvadmin).");
	} else {
		p.SetTitle("About CS:TV");
		p.DrawText("Version " ... PLUGIN_VERSION);
	}
	p.DrawText(" ");
	p.DrawItem("Back");
	p.DrawItem("Exit");
	p.Send(client, Panel_HelpH, 120);
	delete p;
}

public int Panel_HelpH(Menu m, MenuAction action, int client, int item)
{
	if (action == MenuAction_Select && item == 1) ShowHelp(client, g_iHelpFirst[client]);
	return 0;
}

void TogglePause(int client)
{
	if (!g_sUrl[0] || !Can(client, PERM_PAUSE)) return;
	if (g_bPaused) {
		g_fBaseTime = GetEngineTime();
		g_bPaused = false;
	} else {
		g_fBasePos = CurrentPos();
		g_bPaused = true;
	}
	ForModded(1);
	PrintToChatAll(" \x04[CS:TV]\x01 %N %s the video.", client, g_bPaused ? "paused" : "resumed");
}

void SetSpeed(int client, float speed)
{
	if (!Can(client, PERM_PLAYBACK)) return;
	g_fBasePos = CurrentPos();
	g_fBaseTime = GetEngineTime();
	g_fSpeed = speed;
	ForModded(1);
	char lbl[16];
	SpeedLabel(speed, lbl, sizeof lbl);
	PrintToChatAll(" \x04[CS:TV]\x01 %N set the playback speed to %s.", client, lbl);
}

public Action Cmd_Pause(int client, int args)
{
	if (client) TogglePause(client);
	return Plugin_Handled;
}

public Action Cmd_Vol(int client, int args)
{
	if (!client) return Plugin_Handled;
	char a[8];
	GetCmdArg(1, a, sizeof a);
	int v = StringToInt(a);
	if (v < 0) v = 0;
	if (v > 100) v = 100;
	IntToString(v, a, sizeof a);
	g_hVolCookie.Set(client, a);
	if (g_bModded[client]) Send(client, "uservol", a);
	PrintToChat(client, " \x04[CS:TV]\x01 Your volume: %d%%", v);
	return Plugin_Handled;
}

// ------------------------------------------------------------------ menus
void SpeedLabel(float speed, char[] out, int maxlen)   // 1 -> "1x", 1.25 -> "1.25x", 0.5 -> "0.5x"
{
	Format(out, maxlen, "%.2f", speed);
	int n = strlen(out);
	while (n > 0 && out[n - 1] == '0') out[--n] = '\0';
	if (n > 0 && out[n - 1] == '.') out[--n] = '\0';
	StrCat(out, maxlen, "x");
}

void QualityLabel(int client, char[] out, int maxlen)
{
	if (StrEqual(g_sQuality[client], "source")) strcopy(out, maxlen, "Source");
	else if (StrEqual(g_sQuality[client], "auto")) strcopy(out, maxlen, "Auto");
	else if (StrEqual(g_sQuality[client], "audio")) strcopy(out, maxlen, "Audio only");
	else Format(out, maxlen, "%sp", g_sQuality[client]);
}

// One settings menu for every site (!tvset; !tvyt and !tvttv open it too). The settings were always
// per player and site-independent (one quality, one live-chat switch...); only the menus were split.
public Action Cmd_Settings(int client, int args)
{
	if (client) ShowSettingsMenu(client);
	return Plugin_Handled;
}

int UserVolume(int client)
{
	char v[8];
	g_hVolCookie.Get(client, v, sizeof v);
	return v[0] ? StringToInt(v) : 100;
}

void ShowSettingsMenu(int client, int first = 0)
{
	Menu m = new Menu(Menu_Settings);
	char item[64], lbl[32];
	if (g_bModded[client]) {
		m.SetTitle("CS:TV settings\nYours only (any site), remembered:\n ");
		QualityLabel(client, lbl, sizeof lbl);
		Format(item, sizeof item, "Quality: %s", lbl);
		m.AddItem("quality", item);
		m.AddItem("subs", g_bSubsOn[client] ? "Subtitles: ON" : "Subtitles: OFF");
		m.AddItem("chat", g_bChatOn[client] ? "Live chat panel: ON" : "Live chat panel: OFF");
		Format(item, sizeof item, "Volume: %d%%", UserVolume(client));
		m.AddItem("vol", item);
	} else {
		m.SetTitle("CS:TV settings\nThe video settings need the CS:TV client mod.\n ");
	}
	m.AddItem("upnext", g_bUpNextOn[client] ? "Up next messages: ON" : "Up next messages: OFF");
	// for everyone, only when they apply
	if (g_sUrl[0] && g_iKind != LINK_TWITCH_LIVE) {
		SpeedLabel(g_fSpeed, lbl, sizeof lbl);
		Format(item, sizeof item, "Speed: %s (everyone)", lbl);
		m.AddItem("speed", item);
		m.AddItem("loop", g_bLoop ? "Loop this video: ON (everyone)" : "Loop this video: OFF (everyone)");
	}
	char list[64];
	if (g_sUrl[0] && (YtList(g_sUrl, list, sizeof list) || g_hQueue.Length)) m.AddItem("next", "Next video (everyone)");
	if (g_bModded[client] && g_sUrl[0] && g_iKind == LINK_TWITCH_LIVE) m.AddItem("info", "Stream info on the screen");
	m.DisplayAt(client, first, 30);
}

public int Menu_Settings(Menu m, MenuAction action, int client, int item)
{
	if (action == MenuAction_End) delete m;
	else if (action == MenuAction_Select) {
		char key[16];
		m.GetItem(item, key, sizeof key);
		int page = m.Selection;   // reopen on the same page
		if (StrEqual(key, "quality")) {
			Menu q = new Menu(Menu_Quality);
			q.SetTitle("Quality (yours only, any site)");
			q.AddItem("auto", "Auto (best up to 1080p)");
			q.AddItem("source", "Source (the best there is)");
			q.AddItem("1080", "1080p");
			q.AddItem("720", "720p");
			q.AddItem("480", "480p");
			q.AddItem("360", "360p");
			q.AddItem("audio", "Audio only");
			q.ExitBackButton = true;
			q.Display(client, 30);
		} else if (StrEqual(key, "vol")) {
			Menu v = new Menu(Menu_Volume);
			v.SetTitle("Your volume (yours only; it also fades with distance)");
			v.AddItem("100", "100%");
			v.AddItem("75", "75%");
			v.AddItem("50", "50%");
			v.AddItem("25", "25%");
			v.AddItem("10", "10%");
			v.AddItem("0", "Mute");
			v.ExitBackButton = true;
			v.Display(client, 30);
		} else if (StrEqual(key, "speed")) {
			if (!Can(client, PERM_PLAYBACK)) { ShowSettingsMenu(client, page); return 0; }
			Menu sp = new Menu(Menu_Speed);
			sp.SetTitle("Playback speed (changes it for everyone)");
			sp.AddItem("0.5", "0.5x");
			sp.AddItem("0.75", "0.75x");
			sp.AddItem("1.0", "1x (normal)");
			sp.AddItem("1.25", "1.25x");
			sp.AddItem("1.5", "1.5x");
			sp.AddItem("1.75", "1.75x");
			sp.AddItem("2.0", "2x");
			sp.ExitBackButton = true;
			sp.Display(client, 30);
		} else {
			if (StrEqual(key, "subs")) {
				g_bSubsOn[client] = !g_bSubsOn[client];
				g_hSubsCookie.Set(client, g_bSubsOn[client] ? "1" : "0");
				SendOpts(client);
			} else if (StrEqual(key, "chat")) {
				g_bChatOn[client] = !g_bChatOn[client];
				g_hChatCookie.Set(client, g_bChatOn[client] ? "1" : "0");
				SendOpts(client);
				SendChatPanel(client);
			} else if (StrEqual(key, "upnext")) {
				g_bUpNextOn[client] = !g_bUpNextOn[client];
				g_hUpNextCookie.Set(client, g_bUpNextOn[client] ? "1" : "0");
			} else if (StrEqual(key, "loop")) {
				if (Can(client, PERM_PLAYBACK)) {
					g_bLoop = !g_bLoop;
					PrintToChatAll(" \x04[CS:TV]\x01 %N turned looping %s for this video.", client, g_bLoop ? "on" : "off");
				}
			} else if (StrEqual(key, "next")) {
				RequestSkip(client);
				return 0;
			} else if (StrEqual(key, "info")) {
				Send(client, "info", "");
				PrintToChat(client, " \x04[CS:TV]\x01 Stream info is shown on the screen for 10 seconds.");
			}
			ShowSettingsMenu(client, page);
		}
	}
	return 0;
}

public int Menu_Quality(Menu m, MenuAction action, int client, int item)
{
	if (action == MenuAction_End) delete m;
	else if (action == MenuAction_Cancel && item == MenuCancel_ExitBack) ShowSettingsMenu(client);
	else if (action == MenuAction_Select) {
		m.GetItem(item, g_sQuality[client], sizeof g_sQuality[]);
		SendOpts(client);
		char q[32];
		QualityLabel(client, q, sizeof q);
		PrintToChat(client, " \x04[CS:TV]\x01 Quality: %s%s", q, g_sUrl[0] ? " (switching for you)" : "");
		ShowSettingsMenu(client);
	}
	return 0;
}

public int Menu_Volume(Menu m, MenuAction action, int client, int item)
{
	if (action == MenuAction_End) delete m;
	else if (action == MenuAction_Cancel && item == MenuCancel_ExitBack) ShowSettingsMenu(client);
	else if (action == MenuAction_Select) {
		char v[8];
		m.GetItem(item, v, sizeof v);
		g_hVolCookie.Set(client, v);
		if (g_bModded[client]) Send(client, "uservol", v);
		ShowSettingsMenu(client);
	}
	return 0;
}

public int Menu_Speed(Menu m, MenuAction action, int client, int item)
{
	if (action == MenuAction_End) delete m;
	else if (action == MenuAction_Cancel && item == MenuCancel_ExitBack) ShowSettingsMenu(client);
	else if (action == MenuAction_Select) {
		char v[8];
		m.GetItem(item, v, sizeof v);
		SetSpeed(client, StringToFloat(v));
		ShowSettingsMenu(client);
	}
	return 0;
}

// ------------------------------------------------------------------ click on the screen = pause / resume
public void OnPlayerRunCmdPost(int client, int buttons)
{
	int pressed = buttons & ~g_iLastButtons[client] & (IN_ATTACK | IN_USE);
	g_iLastButtons[client] = buttons;
	if (!pressed || !g_bHaveScreen || !g_sUrl[0] || IsFakeClient(client)) return;
	if (GetEngineTime() - g_fLastClick < 0.3) return;
	if (AimsAtScreen(client)) {
		g_fLastClick = GetEngineTime();
		TogglePause(client);
	}
}

bool AimsAtScreen(int client)
{
	float eye[3], ang[3], dir[3];
	GetClientEyePosition(client, eye);
	GetClientEyeAngles(client, ang);
	GetAngleVectors(ang, dir, NULL_VECTOR, NULL_VECTOR);
	float nrm[3];
	nrm = g_vNormal;
	float denom = GetVectorDotProduct(dir, nrm);
	if (denom > -0.01) {                                 // looking away from the screen's front
		if (!g_bFloat || denom < 0.01) return false;
		NegateVector(nrm);                               // a floating screen works from behind too
		denom = -denom;
	}
	float toPlane[3];
	SubtractVectors(g_vPos, eye, toPlane);
	float t = GetVectorDotProduct(toPlane, nrm) / denom;
	if (t < 0.0 || t > CLICK_RANGE) return false;
	float hit[3], rel[3];
	for (int i = 0; i < 3; i++) hit[i] = eye[i] + dir[i] * t;
	SubtractVectors(hit, g_vPos, rel);
	if (FloatAbs(GetVectorDotProduct(rel, g_vRight)) > g_fW / 2.0) return false;
	if (FloatAbs(GetVectorDotProduct(rel, g_vUp)) > g_fH / 2.0) return false;
	// walls between the player and the screen block the click
	float front[3];
	for (int i = 0; i < 3; i++) front[i] = hit[i] + nrm[i] * (g_bFloat ? 4.0 + FLOAT_GAP : 4.0);
	TR_TraceRayFilter(eye, front, MASK_SOLID, RayType_EndPoint, Filter_NotPlayers);
	return !TR_DidHit();
}

// ------------------------------------------------------------------ base64url
bool Base64UrlDecode(const char[] input, char[] out, int maxlen)
{
	int n = strlen(input), o = 0, acc = 0, bits = 0;
	for (int i = 0; i < n; i++) {
		int c = input[i], v;
		if (c >= 'A' && c <= 'Z') v = c - 'A';
		else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
		else if (c >= '0' && c <= '9') v = c - '0' + 52;
		else if (c == '-') v = 62;
		else if (c == '_') v = 63;
		else return false;
		acc = ((acc << 6) | v) & 0xffffff;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			if (o >= maxlen - 1) return false;
			out[o++] = (acc >> bits) & 0xff;
		}
	}
	out[o] = '\0';
	return o > 0;
}

void Base64UrlEncode(const char[] input, char[] out, int maxlen)
{
	char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
	int n = strlen(input), o = 0, acc = 0, bits = 0;
	for (int i = 0; i < n && o < maxlen - 2; i++) {
		acc = ((acc << 8) | (input[i] & 0xff)) & 0xffffff;
		bits += 8;
		while (bits >= 6 && o < maxlen - 2) {
			bits -= 6;
			out[o++] = T[(acc >> bits) & 63];
		}
	}
	if (bits > 0 && o < maxlen - 1) out[o++] = T[(acc << (6 - bits)) & 63];
	out[o] = '\0';
}

// ------------------------------------------------------------------ permissions: storage and checks
bool IsTvAdmin(int client)
{
	return client == 0 || CheckCommandAccess(client, "sm_tvadmin", ADMFLAG_GENERIC);
}

// The Steam2 id that per-player settings are saved under. On an -insecure internet server Steam never
// confirms ids ("Authentication and VAC not requested"), so SourceMod's validated id can stay missing for a
// whole visit (seen 2026-10-17: a friend joining through Steam's relay never showed up in !tvadmin, and his
// own settings would never have applied). Then use the id the engine was given. It isn't proven, but on
// an insecure server nothing is, and it only decides CS:TV permissions (admin rights still need Steam).
bool PlayerId(int client, char[] id, int maxlen)
{
	if (GetClientAuthId(client, AuthId_Steam2, id, maxlen)) return true;
	if (!GetClientAuthId(client, AuthId_Steam2, id, maxlen, false)) return false;
	return StrContains(id, "STEAM_") == 0 && StrContains(id, "STEAM_ID_") != 0;   // not STEAM_ID_PENDING / _LAN
}

int PermsOf(int client)
{
	if (IsTvAdmin(client)) return PERM_ALL;
	char id[32];
	int p;
	if (PlayerId(client, id, sizeof id) && g_hPerms.GetValue(id, p)) return p;
	return g_iDefaultPerms;
}

// may this player do it? If not, they're told why (at most every 3 s: clicking the screen repeats)
float g_fDeniedAt[MAXPLAYERS + 1];

bool Can(int client, int perm)
{
	if (PermsOf(client) & perm) return true;
	if (GetEngineTime() - g_fDeniedAt[client] >= 3.0) {
		g_fDeniedAt[client] = GetEngineTime();
		int i = 0;
		while (i < PERM_COUNT - 1 && !(perm & (1 << i))) i++;
		PrintToChat(client, " \x04[CS:TV]\x01 You can't %s here: an admin turned that off for you.", g_sPermVerb[i]);
	}
	return false;
}

// "all", "none", or the allowed actions as words: "link queue pause"
int ParsePerms(const char[] s)
{
	if (StrEqual(s, "all", false)) return PERM_ALL;
	int p = 0;
	for (int i = 0; i < PERM_COUNT; i++)
		if (StrContains(s, g_sPermKey[i], false) != -1) p |= 1 << i;
	return p;
}

void FormatPerms(int p, char[] out, int maxlen)
{
	if ((p & PERM_ALL) == PERM_ALL) { strcopy(out, maxlen, "all"); return; }
	if (!(p & PERM_ALL)) { strcopy(out, maxlen, "none"); return; }
	out[0] = '\0';
	for (int i = 0; i < PERM_COUNT; i++) {
		if (!(p & (1 << i))) continue;
		if (out[0]) StrCat(out, maxlen, " ");
		StrCat(out, maxlen, g_sPermKey[i]);
	}
}

void LoadPerms()
{
	BuildPath(Path_SM, g_sPermFile, sizeof g_sPermFile, "data/cstv.cfg");
	g_hPerms.Clear();
	g_hPermNames.Clear();
	g_iDefaultPerms = PERM_ALL;
	KeyValues kv = new KeyValues("cstv");
	if (kv.ImportFromFile(g_sPermFile)) {
		char s[256], id[32], name[MAX_NAME_LENGTH];
		kv.GetString("default", s, sizeof s, "all");
		g_iDefaultPerms = ParsePerms(s);
		if (kv.JumpToKey("players") && kv.GotoFirstSubKey()) {
			do {
				kv.GetSectionName(id, sizeof id);
				kv.GetString("perms", s, sizeof s, "all");
				kv.GetString("name", name, sizeof name, "");
				g_hPerms.SetValue(id, ParsePerms(s));
				g_hPermNames.SetString(id, name);
			} while (kv.GotoNextKey());
		}
	}
	delete kv;
}

void SavePerms()
{
	KeyValues kv = new KeyValues("cstv");
	char s[256], id[32], name[MAX_NAME_LENGTH];
	FormatPerms(g_iDefaultPerms, s, sizeof s);
	kv.SetString("default", s);
	kv.JumpToKey("players", true);
	StringMapSnapshot snap = g_hPerms.Snapshot();
	for (int i = 0; i < snap.Length; i++) {
		int p;
		snap.GetKey(i, id, sizeof id);
		g_hPerms.GetValue(id, p);
		if (!g_hPermNames.GetString(id, name, sizeof name)) name[0] = '\0';
		FormatPerms(p, s, sizeof s);
		kv.JumpToKey(id, true);
		kv.SetString("name", name);
		kv.SetString("perms", s);
		kv.GoBack();
	}
	delete snap;
	kv.Rewind();
	if (!kv.ExportToFile(g_sPermFile)) LogError("could not save %s", g_sPermFile);
	delete kv;
}

// keep the names in data/cstv.cfg current for players an admin set
public void OnClientPostAdminCheck(int client)
{
	char id[32], name[MAX_NAME_LENGTH], old[MAX_NAME_LENGTH];
	int p;
	if (IsFakeClient(client) || !PlayerId(client, id, sizeof id) || !g_hPerms.GetValue(id, p)) return;
	GetClientName(client, name, sizeof name);
	if (!g_hPermNames.GetString(id, old, sizeof old) || !StrEqual(old, name)) {
		g_hPermNames.SetString(id, name);
		SavePerms();
	}
}

int FindClientBySteamId(const char[] id)
{
	char other[32];
	for (int i = 1; i <= MaxClients; i++)
		if (IsClientInGame(i) && !IsFakeClient(i) && PlayerId(i, other, sizeof other) && StrEqual(other, id))
			return i;
	return 0;
}

// ------------------------------------------------------------------ !tvadmin
public Action Cmd_TvAdmin(int client, int args)
{
	if (client) ShowAdminMenu(client);
	return Plugin_Handled;
}

void ShowAdminMenu(int client)
{
	Menu m = new Menu(Menu_AdminH);
	m.SetTitle("CS:TV admin: who may do what\nAdmins can always do everything.\n ");
	m.AddItem("default", "Server default (everyone, new players too)");
	m.AddItem("online", "A player who's here...");
	m.AddItem("saved", "Players with their own settings...");
	m.Display(client, 60);
}

public int Menu_AdminH(Menu m, MenuAction action, int client, int item)
{
	if (action == MenuAction_End) delete m;
	else if (action == MenuAction_Select) {
		char info[16];
		m.GetItem(item, info, sizeof info);
		if (StrEqual(info, "default")) { g_sAdminTarget[client][0] = '\0'; ShowPermMenu(client, 0); }
		else if (StrEqual(info, "online")) ShowOnlineList(client);
		else ShowSavedList(client);
	}
	return 0;
}

void PlayerTag(const char[] id, char[] out, int maxlen)
{
	int p;
	if (!g_hPerms.GetValue(id, p)) out[0] = '\0';
	else if (!(p & PERM_ALL)) strcopy(out, maxlen, " (watch only)");
	else if ((p & PERM_ALL) == PERM_ALL) strcopy(out, maxlen, " (everything)");
	else strcopy(out, maxlen, " (own settings)");
}

void ShowOnlineList(int client)
{
	Menu m = new Menu(Menu_PlayerListH);
	m.SetTitle("CS:TV admin: pick a player\n ");
	char id[32], item[112], tag[24];
	int n = 0;
	for (int i = 1; i <= MaxClients; i++) {
		if (!IsClientInGame(i) || IsFakeClient(i)) continue;
		n++;
		if (!PlayerId(i, id, sizeof id)) {   // still connecting: no Steam id at all yet
			Format(item, sizeof item, "%N (no Steam ID yet, try again in a moment)", i);
			m.AddItem("", item, ITEMDRAW_DISABLED);
			continue;
		}
		bool adm = IsTvAdmin(i);
		PlayerTag(id, tag, sizeof tag);
		Format(item, sizeof item, "%N%s%s", i, i == client ? " (you)" : "", adm ? " (admin: can always do everything)" : tag);
		m.AddItem(id, item, adm ? ITEMDRAW_DISABLED : ITEMDRAW_DEFAULT);
	}
	if (!n) m.AddItem("", "(nobody here)", ITEMDRAW_DISABLED);
	m.ExitBackButton = true;
	m.Display(client, 60);
}

void ShowSavedList(int client)
{
	Menu m = new Menu(Menu_PlayerListH);
	m.SetTitle("CS:TV admin: players with their own settings\n(online or not)\n ");
	StringMapSnapshot snap = g_hPerms.Snapshot();
	char id[32], name[MAX_NAME_LENGTH], item[96], tag[24];
	for (int i = 0; i < snap.Length; i++) {
		snap.GetKey(i, id, sizeof id);
		if (!g_hPermNames.GetString(id, name, sizeof name) || !name[0]) strcopy(name, sizeof name, id);
		PlayerTag(id, tag, sizeof tag);
		Format(item, sizeof item, "%s%s%s", name, tag, FindClientBySteamId(id) ? "" : " - offline");
		m.AddItem(id, item);
	}
	if (!snap.Length) m.AddItem("", "(nobody: everyone uses the server default)", ITEMDRAW_DISABLED);
	delete snap;
	m.ExitBackButton = true;
	m.Display(client, 60);
}

public int Menu_PlayerListH(Menu m, MenuAction action, int client, int item)
{
	if (action == MenuAction_End) delete m;
	else if (action == MenuAction_Cancel && item == MenuCancel_ExitBack) ShowAdminMenu(client);
	else if (action == MenuAction_Select) {
		char id[32];
		m.GetItem(item, id, sizeof id);
		if (!id[0]) return 0;
		int target = FindClientBySteamId(id);
		if (target) {
			char name[MAX_NAME_LENGTH];
			GetClientName(target, name, sizeof name);
			g_hPermNames.SetString(id, name);
		}
		strcopy(g_sAdminTarget[client], sizeof g_sAdminTarget[], id);
		ShowPermMenu(client, 0);
	}
	return 0;
}

void ShowPermMenu(int client, int first)
{
	char target[32], who[160], name[MAX_NAME_LENGTH];
	strcopy(target, sizeof target, g_sAdminTarget[client]);
	int perms;
	bool own = false;
	if (!target[0]) {
		perms = g_iDefaultPerms;
		strcopy(who, sizeof who, "Server default\n(everyone without their own settings, new players too)");
	} else {
		own = g_hPerms.GetValue(target, perms);
		if (!own) perms = g_iDefaultPerms;
		if (!g_hPermNames.GetString(target, name, sizeof name) || !name[0]) strcopy(name, sizeof name, target);
		Format(who, sizeof who, "%s\n%s", name, own ? "own settings" : "uses the server default (changing anything gives them their own)");
	}
	Menu m = new Menu(Menu_PermH);
	m.SetTitle("CS:TV admin: %s\n ", who);
	char item[64], info[8];
	for (int i = 0; i < PERM_COUNT; i++) {
		Format(item, sizeof item, "%s: %s", g_sPermLabel[i], (perms & (1 << i)) ? "allowed" : "BLOCKED");
		IntToString(i, info, sizeof info);
		m.AddItem(info, item);
	}
	m.AddItem("all", "Allow everything");
	m.AddItem("none", "Watch only (block everything)");
	if (own) m.AddItem("reset", "Back to the server default");
	m.ExitBackButton = true;
	m.DisplayAt(client, first, 60);
}

public int Menu_PermH(Menu m, MenuAction action, int client, int item)
{
	if (action == MenuAction_End) delete m;
	else if (action == MenuAction_Cancel && item == MenuCancel_ExitBack) {
		if (g_sAdminTarget[client][0]) ShowSavedOrOnline(client); else ShowAdminMenu(client);
	} else if (action == MenuAction_Select) {
		if (!IsTvAdmin(client)) return 0;
		char info[8], target[32];
		m.GetItem(item, info, sizeof info);
		strcopy(target, sizeof target, g_sAdminTarget[client]);
		int perms;
		if (!target[0] || !g_hPerms.GetValue(target, perms)) perms = g_iDefaultPerms;
		bool reset = StrEqual(info, "reset");
		if (StrEqual(info, "all")) perms = PERM_ALL;
		else if (StrEqual(info, "none")) perms = 0;
		else if (!reset) perms ^= 1 << StringToInt(info);
		char text[256];
		if (!target[0]) {
			g_iDefaultPerms = perms;
			FormatPerms(perms, text, sizeof text);
			LogAction(client, -1, "\"%L\" set the CS:TV server default to: %s", client, text);
			PrintToChat(client, " \x04[CS:TV]\x01 Server default: %s", text);
		} else {
			int who = FindClientBySteamId(target);
			if (reset) g_hPerms.Remove(target);
			else g_hPerms.SetValue(target, perms);
			if (reset) strcopy(text, sizeof text, "server default"); else FormatPerms(perms, text, sizeof text);
			LogAction(client, who ? who : -1, "\"%L\" set CS:TV permissions of %s to: %s", client, target, text);
			if (who && who != client)
				PrintToChat(who, " \x04[CS:TV]\x01 An admin changed what you can do with the TV. See \x04!tvhelp\x01 > What you're allowed to do.");
		}
		SavePerms();
		ShowPermMenu(client, m.Selection);
	}
	return 0;
}

void ShowSavedOrOnline(int client)
{
	if (FindClientBySteamId(g_sAdminTarget[client])) ShowOnlineList(client); else ShowSavedList(client);
}

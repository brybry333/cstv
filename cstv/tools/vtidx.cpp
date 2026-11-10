// Print the vtable slot index of every SDK method CS:TV's client calls, computed by the compiler from
// the real headers. Build like the client (same -D/-I flags as client/Makefile) and run.
//
// Checked against the CS:S 64-bit Linux binaries on 2026-10-18 (re-check after game updates):
// * vtable lengths (vtdump.py): CHLClient 79 = header 79; ConCommand 14 / ConCommandBase 11 = header;
//   CCvar 35 = ICvar 35; CEngineClient 147, CVRenderView 53, CEngineTraceClient 23, CMatRenderContext
//   242 and CMatQueuedMesh 38+14 are header + the implementation's own virtuals at the end.
// * code in the slots (slots.py + objdump):
//   IVEngineClient 6 ServerCmd: snprintf(buf, 255, "cmd %s") + forward; 8 GetPlayerInfo: index-1 checked
//   against maxclients, zeroes/copies 132 bytes (= sizeof(player_info_t)); 12 GetLocalPlayer: slot+1;
//   26 IsInGame: signon state == 6; 51 GetLevelName: level name or "" while not connected;
//   72 GetNetChannelInfo: returns the client's net channel. INetChannelInfo 1 GetAddress: CNetChan's
//   cached address string.
//   IVRenderView 9 SceneEnd: client.so's CViewRender::RenderView calls render+0x48 then +0x80
//   (SceneEnd, DrawLights) right after engine->DrawPortals() and DisableFog(), +0x40 (SceneBegin) earlier.
//   IBaseClientDLL 20 IN_KeyEvent: tail call to input->KeyEvent (IInput slot 11) with the same arguments.
//   ICvar 5 AllocateDLLIdentifier: returns a counter++; 6 RegisterConCommand: IsRegistered (slot 7),
//   sets +0x10, GetName (slot 5), refuses a name that exists; 7 UnregisterConCommand: clears +0x10 and
//   unlinks; 10 FindCommandBase: walks the list comparing GetName.
//   ConCommandBase in client.so: GetName = [this+0x18], IsFlagSet = [this+0x28], IsRegistered = byte
//   [this+0x10]; ConCommand::Dispatch (13) calls [this+0x30] by the flags byte at +0x40.
//   IMaterialSystem 71 FindMaterial: forwards to FindMaterialEx with context 0.
//   IMatRenderContext 9 Bind (material, proxy data); 20 MatrixMode (stores the mode, picks the stack);
//   21 PushMatrix; 22 PopMatrix (decrements the stack depth); 31 LoadIdentity; 56 GetDynamicMesh (4 args).
//   IMaterial 12 IncrementReferenceCount (CMaterial: lock add [this+0x24]).
//   IMesh 13 Draw: not looked at; only the vtable lengths above.
// * how often client.so calls each slot through its engine globals (vtcalls.py): EngineTraceClient003
//   slot 4 (TraceRay) 75 times, slot 0 (GetPointContents) 23; VEngineClient014 7 ClientCmd 63,
//   106 ClientCmd_Unrestricted 20, 8 GetPlayerInfo 17.
// * SayText is user message 3: client.so registers Geiger, Train, HudText, SayText, SayText2, TextMsg, ...
//   in that order (ids = registration order).
#include <cstdio>
#include <cstdint>
#include <cstddef>
#include "materialsystem/imaterialsystem.h"
#include "materialsystem/imesh.h"
#include "materialsystem/imaterial.h"
#include "materialsystem/imaterialvar.h"
#include "materialsystem/itexture.h"
#include "vtf/vtf.h"
#include "cdll_int.h"
#include "ivrenderview.h"
#include "engine/IEngineTrace.h"
#include "inetchannelinfo.h"
#include "icvar.h"
#include "tier1/convar.h"
#include "gametrace.h"

template <typename T> static long slot(T pmf)
{
    union { T p; struct { uintptr_t ptr, adj; } s; } u;
    u.p = pmf;
    return (long)((u.s.ptr - 1) / sizeof(void *));   // Itanium ABI: virtual PMF = 1 + vtable byte offset
}
#define S(expr) printf("%-60s %ld\n", #expr, slot(expr))

int main()
{
    S(&IBaseClientDLL::LevelInitPostEntity); S(&IBaseClientDLL::LevelShutdown); S(&IBaseClientDLL::IN_KeyEvent);
    S(&IBaseClientDLL::FrameStageNotify); S(&IBaseClientDLL::DispatchUserMessage); S(&IBaseClientDLL::GetPlayerView);
    S(&IVEngineClient::ServerCmd); S(&IVEngineClient::GetPlayerInfo); S(&IVEngineClient::Con_IsVisible); S(&IVEngineClient::GetLocalPlayer);
    S(&IVEngineClient::IsInGame); S(&IVEngineClient::GetLevelName); S(&IVEngineClient::GetNetChannelInfo);
    S(&INetChannelInfo::GetAddress);
    S(&IVRenderView::SceneBegin); S(&IVRenderView::SceneEnd); S(&IVRenderView::DrawLights);
    S(&IEngineTrace::TraceRay);
    S(&ICvar::AllocateDLLIdentifier); S(&ICvar::RegisterConCommand); S(&ICvar::UnregisterConCommand);
    S((ConCommandBase *(ICvar::*)(const char *))&ICvar::FindCommandBase);
    S(&ConCommandBase::IsFlagSet); S(&ConCommandBase::GetName); S(&ConCommandBase::IsRegistered); S(&ConCommand::Dispatch);
    S(&IMaterialSystem::FindMaterial); S(&IMaterialSystem::CreateProceduralTexture); S(&IMaterialSystem::GetRenderContext);
    S(&IMaterial::GetName); S(&IMaterial::IsErrorMaterial); S(&IMaterial::IncrementReferenceCount);
    S((IMaterialVar *(IMaterial::*)(const char *, bool *, bool))&IMaterial::FindVar);
    S(&IMaterialVar::SetTextureValue); S(&IMaterialVar::GetTextureValue);
    S(&IMatRenderContext::Bind); S(&IMatRenderContext::MatrixMode); S(&IMatRenderContext::PushMatrix); S(&IMatRenderContext::PopMatrix);
    S(&IMatRenderContext::LoadIdentity); S(&IMatRenderContext::GetDynamicMesh); S(&IMatRenderContext::GetCallQueue);
    S((void (IMesh::*)(int, int))&IMesh::Draw);
    S(&ITexture::SetTextureRegenerator); S(&ITexture::Download); S(&ITexture::GetActualWidth); S(&ITexture::GetActualHeight);
    S(&ITexture::GetImageFormat); S(&ITexture::IncrementReferenceCount); S(&ITexture::DecrementReferenceCount);
    S(&ITextureRegenerator::RegenerateTextureBits); S(&ITextureRegenerator::Release);
    S((unsigned char *(IVTFTexture::*)())&IVTFTexture::ImageData); S(&IVTFTexture::Width); S(&IVTFTexture::Height);
    S(&IVTFTexture::RowSizeInBytes);
    printf("sizeof player_info_t %zu (friendsID at %zu), ConCommand %zu, bf_read %zu\n", sizeof(player_info_t),
           offsetof(player_info_t, friendsID), sizeof(ConCommand), sizeof(bf_read));
}

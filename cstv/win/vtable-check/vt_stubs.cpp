// one stub per virtual call cstv_client.cpp makes: the slot clang-cl (MSVC ABI) uses shows in the disassembly
#include "materialsystem/imaterialsystem.h"
#include "materialsystem/imaterial.h"
#include "materialsystem/imaterialvar.h"
#include "materialsystem/itexture.h"
#include "vtf/vtf.h"
#include "cdll_int.h"
#include "view_shared.h"
#include "tier1/callqueue.h"
#define S(name, cls, call) extern "C" __declspec(noinline) void name(cls *p) { volatile auto r = (call, 0); (void)r; }
extern "C" __declspec(noinline) void IBaseClientDLL__LevelInitPostEntity(IBaseClientDLL *p) { p->LevelInitPostEntity(); }
extern "C" __declspec(noinline) void IBaseClientDLL__LevelShutdown(IBaseClientDLL *p) { p->LevelShutdown(); }
extern "C" __declspec(noinline) void IBaseClientDLL__FrameStageNotify(IBaseClientDLL *p) { p->FrameStageNotify(FRAME_RENDER_START); }
extern "C" __declspec(noinline) bool IBaseClientDLL__DispatchUserMessage(IBaseClientDLL *p, bf_read &m) { return p->DispatchUserMessage(0, m); }
extern "C" __declspec(noinline) bool IBaseClientDLL__GetPlayerView(IBaseClientDLL *p, CViewSetup &v) { return p->GetPlayerView(v); }
extern "C" __declspec(noinline) void IVEngineClient__ClientCmd_Unrestricted(IVEngineClient *p) { p->ClientCmd_Unrestricted("x"); }
extern "C" __declspec(noinline) int IMaterialSystem__FirstMaterial(IMaterialSystem *p) { return p->FirstMaterial(); }
extern "C" __declspec(noinline) int IMaterialSystem__NextMaterial(IMaterialSystem *p) { return p->NextMaterial(0); }
extern "C" __declspec(noinline) int IMaterialSystem__InvalidMaterial(IMaterialSystem *p) { return p->InvalidMaterial(); }
extern "C" __declspec(noinline) void *IMaterialSystem__GetMaterial(IMaterialSystem *p) { return p->GetMaterial(0); }
extern "C" __declspec(noinline) void *IMaterialSystem__CreateProceduralTexture(IMaterialSystem *p) { return p->CreateProceduralTexture("x", "y", 1, 1, IMAGE_FORMAT_DXT1, 0); }
extern "C" __declspec(noinline) void *IMaterialSystem__GetRenderContext(IMaterialSystem *p) { return p->GetRenderContext(); }
extern "C" __declspec(noinline) const char *IMaterial__GetName(IMaterial *p) { return p->GetName(); }
extern "C" __declspec(noinline) bool IMaterial__IsErrorMaterial(IMaterial *p) { return p->IsErrorMaterial(); }
extern "C" __declspec(noinline) void *IMaterial__FindVar(IMaterial *p) { bool f; return p->FindVar("x", &f, false); }
extern "C" __declspec(noinline) void IMaterial__SetMaterialVarFlag(IMaterial *p) { p->SetMaterialVarFlag(MATERIAL_VAR_NO_DRAW, true); }
extern "C" __declspec(noinline) void *IMaterialVar__GetTextureValue(IMaterialVar *p) { return p->GetTextureValue(); }
extern "C" __declspec(noinline) void IMaterialVar__SetTextureValue(IMaterialVar *p, ITexture *t) { p->SetTextureValue(t); }
extern "C" __declspec(noinline) void ITexture__SetTextureRegenerator(ITexture *p, ITextureRegenerator *r) { p->SetTextureRegenerator(r); }
extern "C" __declspec(noinline) void ITexture__IncrementReferenceCount(ITexture *p) { p->IncrementReferenceCount(); }
extern "C" __declspec(noinline) void ITexture__DecrementReferenceCount(ITexture *p) { p->DecrementReferenceCount(); }
extern "C" __declspec(noinline) int ITexture__GetActualWidth(ITexture *p) { return p->GetActualWidth(); }
extern "C" __declspec(noinline) int ITexture__GetActualHeight(ITexture *p) { return p->GetActualHeight(); }
extern "C" __declspec(noinline) int ITexture__GetImageFormat(ITexture *p) { return p->GetImageFormat(); }
extern "C" __declspec(noinline) void ITexture__Download(ITexture *p) { p->Download(); }
extern "C" __declspec(noinline) void *IMatRenderContext__GetCallQueue(IMatRenderContext *p) { return p->GetCallQueue(); }
extern "C" __declspec(noinline) void IMatRenderContext__Release(IMatRenderContext *p) { p->Release(); }
extern "C" __declspec(noinline) void ICallQueue__QueueFunctor(ICallQueue *p, CFunctor *f) { p->QueueFunctor(f); }
extern "C" __declspec(noinline) int IVTFTexture__Width(IVTFTexture *p) { return p->Width(); }
extern "C" __declspec(noinline) int IVTFTexture__Height(IVTFTexture *p) { return p->Height(); }
extern "C" __declspec(noinline) void *IVTFTexture__ImageData(IVTFTexture *p) { return p->ImageData(); }
extern "C" __declspec(noinline) int IVTFTexture__RowSizeInBytes(IVTFTexture *p) { return p->RowSizeInBytes(0); }
// classes we implement: the slot the game will call
extern "C" __declspec(noinline) void CFunctor__call(CFunctor *f) { (*f)(); }
extern "C" __declspec(noinline) void CFunctor__Release(CFunctor *f) { f->Release(); }
extern "C" __declspec(noinline) void ITextureRegenerator__Regen(ITextureRegenerator *r) { r->RegenerateTextureBits(0, 0, 0); }
extern "C" __declspec(noinline) void ITextureRegenerator__Release(ITextureRegenerator *r) { r->Release(); }

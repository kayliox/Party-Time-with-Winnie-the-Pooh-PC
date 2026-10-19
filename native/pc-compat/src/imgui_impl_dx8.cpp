// Dear ImGui renderer for Direct3D 8 (the layer's in-game overlay), ported from Dear ImGui's imgui_impl_dx9 (MIT).
// Direct3D 8 has no scissor test, no dynamic textures and no separate alpha blending: vertices are pre-transformed
// (XYZRHW), each draw command is clipped by a viewport of its clip rectangle, and font atlas textures live in the
// managed pool (they also survive a device Reset).
#include "imgui_impl_dx8.h"

#include <d3d8.h>

#include <vector>

#include "imgui.h"

namespace {

struct Vertex {
    float x, y, z, rhw;
    D3DCOLOR col;
    float u, v;
};
const DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

struct BackendData {
    IDirect3DDevice8* device = nullptr;
    DWORD stateBlock = 0;
    std::vector<Vertex> vertices;
};

BackendData* Data() { return ImGui::GetCurrentContext() ? static_cast<BackendData*>(ImGui::GetIO().BackendRendererUserData) : nullptr; }

D3DCOLOR ToArgb(ImU32 c) { return (c & 0xFF00FF00) | ((c & 0xFF0000) >> 16) | ((c & 0xFF) << 16); }

void CopyRegion(const ImU32* src, int srcPitch, ImU32* dst, int dstPitch, int w, int h) {
    for (int y = 0; y < h; y++) {
        const ImU32* s = reinterpret_cast<const ImU32*>(reinterpret_cast<const unsigned char*>(src) + srcPitch * y);
        ImU32* d = reinterpret_cast<ImU32*>(reinterpret_cast<unsigned char*>(dst) + dstPitch * y);
        for (int x = 0; x < w; x++) d[x] = ToArgb(s[x]);  // RGBA (ImGui) -> A8R8G8B8
    }
}

void UpdateTexture(ImTextureData* tex) {
    BackendData* bd = Data();
    if (tex->Status == ImTextureStatus_WantCreate) {
        IDirect3DTexture8* t = nullptr;
        if (FAILED(bd->device->CreateTexture(UINT(tex->Width), UINT(tex->Height), 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t)))
            return;
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(t->LockRect(0, &lr, nullptr, 0))) {
            CopyRegion(static_cast<const ImU32*>(tex->GetPixels()), tex->Width * 4, static_cast<ImU32*>(lr.pBits), lr.Pitch,
                       tex->Width, tex->Height);
            t->UnlockRect(0);
        }
        tex->SetTexID(reinterpret_cast<ImTextureID>(t));
        tex->SetStatus(ImTextureStatus_OK);
    } else if (tex->Status == ImTextureStatus_WantUpdates) {
        auto* t = reinterpret_cast<IDirect3DTexture8*>(tex->TexID);
        RECT r{tex->UpdateRect.x, tex->UpdateRect.y, tex->UpdateRect.x + tex->UpdateRect.w, tex->UpdateRect.y + tex->UpdateRect.h};
        D3DLOCKED_RECT lr;
        if (t && SUCCEEDED(t->LockRect(0, &lr, &r, 0))) {
            for (ImTextureRect& u : tex->Updates)
                CopyRegion(static_cast<const ImU32*>(tex->GetPixelsAt(u.x, u.y)), tex->Width * 4,
                           static_cast<ImU32*>(lr.pBits) + (u.x - r.left) + (u.y - r.top) * (lr.Pitch / 4), lr.Pitch, u.w, u.h);
            t->UnlockRect(0);
        }
        tex->SetStatus(ImTextureStatus_OK);
    } else if (tex->Status == ImTextureStatus_WantDestroy) {
        if (tex->TexID != ImTextureID_Invalid) {
            if (auto* t = reinterpret_cast<IDirect3DTexture8*>(tex->TexID)) t->Release();
            tex->SetTexID(ImTextureID_Invalid);
        }
        tex->SetStatus(ImTextureStatus_Destroyed);
    }
}

void SetupRenderState(IDirect3DDevice8* d) {
    d->SetVertexShader(kFvf);
    d->SetPixelShader(0);
    d->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    d->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
    d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    d->SetRenderState(D3DRS_FOGENABLE, FALSE);
    d->SetRenderState(D3DRS_RANGEFOGENABLE, FALSE);
    d->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
    d->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    d->SetRenderState(D3DRS_CLIPPING, TRUE);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    d->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    d->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
    d->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
    d->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTEXF_NONE);
    d->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
    d->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
    d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    d->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
}

}  // namespace

bool ImGui_ImplDX8_Init(IDirect3DDevice8* device) {
    ImGuiIO& io = ImGui::GetIO();
    IMGUI_CHECKVERSION();
    auto* bd = IM_NEW(BackendData)();
    io.BackendRendererUserData = bd;
    io.BackendRendererName = "imgui_impl_dx8";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    pio.Renderer_TextureMaxWidth = pio.Renderer_TextureMaxHeight = 4096;
    bd->device = device;
    device->AddRef();
    return true;
}

void ImGui_ImplDX8_InvalidateDeviceObjects() {
    BackendData* bd = Data();
    if (!bd || !bd->device) return;
    if (bd->stateBlock) bd->device->DeleteStateBlock(bd->stateBlock);
    bd->stateBlock = 0;
}

void ImGui_ImplDX8_Shutdown() {
    BackendData* bd = Data();
    if (!bd) return;
    for (ImTextureData* tex : ImGui::GetPlatformIO().Textures)
        if (tex->RefCount == 1) {
            tex->SetStatus(ImTextureStatus_WantDestroy);
            UpdateTexture(tex);
        }
    ImGui_ImplDX8_InvalidateDeviceObjects();
    if (bd->device) bd->device->Release();
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = nullptr;
    io.BackendRendererUserData = nullptr;
    io.BackendFlags &= ~ImGuiBackendFlags_RendererHasTextures;
    IM_DELETE(bd);
}

void ImGui_ImplDX8_NewFrame() {}

void ImGui_ImplDX8_RenderDrawData(ImDrawData* draw_data) {
    if (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f) return;
    BackendData* bd = Data();
    IDirect3DDevice8* d = bd->device;
    if (draw_data->Textures)
        for (ImTextureData* tex : *draw_data->Textures)
            if (tex->Status != ImTextureStatus_OK) UpdateTexture(tex);
    if (!bd->stateBlock && FAILED(d->CreateStateBlock(D3DSBT_ALL, &bd->stateBlock))) return;
    d->CaptureStateBlock(bd->stateBlock);
    D3DVIEWPORT8 saved;
    d->GetViewport(&saved);
    SetupRenderState(d);
    const ImVec2 off = draw_data->DisplayPos;
    const DWORD maxW = DWORD(draw_data->DisplaySize.x), maxH = DWORD(draw_data->DisplaySize.y);
    for (const ImDrawList* list : draw_data->CmdLists) {
        bd->vertices.resize(size_t(list->VtxBuffer.Size));
        for (int i = 0; i < list->VtxBuffer.Size; i++) {
            const ImDrawVert& s = list->VtxBuffer[i];
            // pixel centres sit on integer coordinates in D3D8: -0.5 keeps the atlas texel-exact
            bd->vertices[size_t(i)] = {s.pos.x - off.x - 0.5f, s.pos.y - off.y - 0.5f, 0.0f, 1.0f, ToArgb(s.col), s.uv.x, s.uv.y};
        }
        for (int c = 0; c < list->CmdBuffer.Size; c++) {
            const ImDrawCmd* cmd = &list->CmdBuffer[c];
            if (cmd->UserCallback) {
                if (cmd->UserCallback == ImDrawCallback_ResetRenderState) SetupRenderState(d);
                else cmd->UserCallback(list, cmd);
                continue;
            }
            float x0 = cmd->ClipRect.x - off.x, y0 = cmd->ClipRect.y - off.y;
            float x1 = cmd->ClipRect.z - off.x, y1 = cmd->ClipRect.w - off.y;
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > float(maxW)) x1 = float(maxW);
            if (y1 > float(maxH)) y1 = float(maxH);
            if (x1 <= x0 || y1 <= y0) continue;
            // no scissor test in D3D8: the viewport bounds where pre-transformed triangles may be drawn
            D3DVIEWPORT8 vp{DWORD(x0), DWORD(y0), DWORD(x1 - x0 + 0.5f), DWORD(y1 - y0 + 0.5f), 0.0f, 1.0f};
            d->SetViewport(&vp);
            d->SetTexture(0, reinterpret_cast<IDirect3DTexture8*>(cmd->GetTexID()));
            d->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, UINT(list->VtxBuffer.Size), cmd->ElemCount / 3,
                                      list->IdxBuffer.Data + cmd->IdxOffset, sizeof(ImDrawIdx) == 2 ? D3DFMT_INDEX16 : D3DFMT_INDEX32,
                                      bd->vertices.data(), sizeof(Vertex));
        }
    }
    d->SetViewport(&saved);
    d->ApplyStateBlock(bd->stateBlock);
}

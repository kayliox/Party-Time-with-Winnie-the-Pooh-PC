// Dear ImGui renderer for Direct3D 8 (the layer's in-game overlay), ported from Dear ImGui's imgui_impl_dx9 (MIT).
#pragma once

struct IDirect3DDevice8;
struct ImDrawData;

bool ImGui_ImplDX8_Init(IDirect3DDevice8* device);
void ImGui_ImplDX8_Shutdown();
void ImGui_ImplDX8_NewFrame();
void ImGui_ImplDX8_RenderDrawData(ImDrawData* draw_data);
// textures live in the managed pool and survive Reset; nothing else is held across frames
void ImGui_ImplDX8_InvalidateDeviceObjects();

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 WarcraftXL
//
// wxl.unit-outline: a retail-style outline around the unit under the mouse and the target --
// green when friendly, yellow when neutral, red when hostile.
//
// How it draws, per frame:
//   1. Lua (OnUpdate, through wxl.script) says which units: UnitGUID and UnitReaction of
//      "mouseover" and "target".
//   2. At WorldRenderBegin each guid becomes the model it draws with (wxl.object, wxl.gx).
//   3. wxl.gx hands every model batch the client draws. A batch of one of those models is drawn a
//      second time, with the same buffers, shaders and bones, into a mask render target with a
//      flat colour: the unit's silhouette, depth-tested against the scene so walls hide it.
//   4. At WorldRenderEnd -- the world drawn, the interface not yet -- a full-screen pass paints
//      every pixel just outside a silhouette in its colour.
//
// The device is a pure device: nothing can be read back from it. Everything set on it is set
// knowingly, and wxl.gx's invalidate_state makes the client send its own state again afterwards.

#include <windows.h>

#include <d3d9.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wxl/objects.hpp"
#include "wxl/sdk.hpp"
#include "wxl/services/gx.h"
#include "wxl/services/script.hpp"

namespace
{
    constexpr const char* kTag = "unit-outline";

    // --- Settings -------------------------------------------------------------------------------

    constexpr float kThickness   = 2.0f;   // pixels
    constexpr float kOpacity     = 0.95f;
    constexpr float kAlphaKeyRef = 0.875f; // the client's alpha-key reference, 224/255
    constexpr float    kNearbyRange = 30.0f; // yards: living units this close are outlined too
    constexpr uint32_t kMaxNearby   = 32;    // the nearest ones, at most

    // What the player chose in Interface Options > Game > Unit Outlines, kept in
    // <mod>\settings.ini. Read on the render path and written from Lua, both on the main thread.
    struct Settings
    {
        bool enabled      = true;  // the master switch
        bool mouseover    = true;  // the unit under the mouse
        bool target       = true;  // the target
        bool nearby       = true;  // living units within kNearbyRange
        bool throughWalls = false; // the silhouette ignores what stands in front
    };

    Settings g_settings;

    struct SettingKey
    {
        const char* name;
        bool Settings::*field;
    };

    const SettingKey kSettingKeys[] = {
        {"enabled", &Settings::enabled},     {"mouseover", &Settings::mouseover},
        {"target", &Settings::target},       {"nearby", &Settings::nearby},
        {"throughWalls", &Settings::throughWalls},
    };

    std::string SettingsPath()
    {
        HMODULE self = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCSTR>(&SettingsPath), &self))
            return {};
        char path[MAX_PATH];
        const DWORD n = GetModuleFileNameA(self, path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return {};
        std::string dir(path, n);
        // <mod>\bin\x64\unit-outline.dll -> <mod>
        for (int up = 0; up < 3; ++up)
        {
            const size_t slash = dir.find_last_of("\\/");
            if (slash == std::string::npos) return {};
            dir.resize(slash);
        }
        return dir + "\\settings.ini";
    }

    void LoadSettings()
    {
        const std::string path = SettingsPath();
        if (path.empty()) return;
        for (const SettingKey& key : kSettingKeys)
            g_settings.*key.field =
                GetPrivateProfileIntA("Outline", key.name, g_settings.*key.field ? 1 : 0, path.c_str()) != 0;
    }

    void SaveSettings()
    {
        const std::string path = SettingsPath();
        if (path.empty()) return;
        for (const SettingKey& key : kSettingKeys)
            WritePrivateProfileStringA("Outline", key.name, g_settings.*key.field ? "1" : "0", path.c_str());
    }

    // Lua's UnitReaction: 1-2 hostile, 3-4 neutral (unfriendly counts as neutral), 5-8 friendly.
    enum Tint : uint32_t
    {
        kHostile,
        kNeutral,
        kFriendly,
        kTints
    };

    const char* const kTintRgb[kTints] = {"1.0, 0.12, 0.10", "1.0, 0.85, 0.10", "0.20, 1.0, 0.20"};

    /// The client's own scale (wxl.object): 0-1 hostile, 2-3 neutral, 4-7 friendly.
    Tint TintOfNative(uint32_t reaction)
    {
        return reaction <= 1 ? kHostile : reaction <= 3 ? kNeutral : kFriendly;
    }

    bool TintOf(double reaction, Tint& out)
    {
        if (reaction <= 0.0) return false;
        out = reaction <= 2.0 ? kHostile : reaction <= 4.0 ? kNeutral : kFriendly;
        return true;
    }

    // --- Services -------------------------------------------------------------------------------

    wxl::Service<wxl_gx_api> g_gx(WXL_GX_SERVICE, WXL_GX_SERVICE_VERSION);
    const wxl::ScriptService g_script;

    IDirect3DDevice9* Device()
    {
        return g_gx ? static_cast<IDirect3DDevice9*>(g_gx->d3d9_device()) : nullptr;
    }

    template <class T>
    void Release(T*& p)
    {
        if (p) p->Release();
        p = nullptr;
    }

    // --- Diagnostics: what each stage did, logged once a second while anything is wanted ----

    struct Stats
    {
        uint32_t paints, mouseover, target, ticks, wanted, resolved, modeled, batches, matched, skippedBlend, masked, failed, painted;
    };

    Stats    g_stats{};
    uint32_t g_statsFrames = 0;
    std::string g_luaError;

    void LuaError(wxl::ScriptCall& call)
    {
        const auto text = call.String(0);
        std::string error = text ? std::string(*text) : "?";
        if (error != g_luaError) wxl::Log(WXL_LOG_WARN, kTag, "Lua: " + error);
        g_luaError = error;
    }

    void FlushStats()
    {
        if (++g_statsFrames < 60) return;
        g_statsFrames = 0;
        if (g_stats.wanted || g_stats.matched || g_stats.painted || !g_stats.ticks)
        {
            char line[384];
            std::snprintf(line, sizeof line,
                          "last 60 world renders: paints %u, mouseover %u, target %u, lua ticks %u, guids wanted %u, "
                          "objects %u, models %u, batches seen %u, matched %u (blended skipped %u), mask draws %u "
                          "(failed %u), edge passes %u",
                          g_stats.paints, g_stats.mouseover, g_stats.target, g_stats.ticks, g_stats.wanted, g_stats.resolved, g_stats.modeled, g_stats.batches,
                          g_stats.matched, g_stats.skippedBlend, g_stats.masked, g_stats.failed, g_stats.painted);
            wxl::Log(WXL_LOG_DEBUG, kTag, line);
        }
        g_stats = Stats{};
    }

    // --- Which units, as Lua last said ----------------------------------------------------------

    struct Wanted
    {
        uint64_t guid;
        double   reaction;
    };

    Wanted g_wanted[2]; // mouseover, target

    uint64_t ParseGuid(std::string_view text)
    {
        char buffer[32];
        if (text.empty() || text.size() >= sizeof buffer) return 0;
        std::memcpy(buffer, text.data(), text.size());
        buffer[text.size()] = 0;
        return std::strtoull(buffer, nullptr, 16); // "0xF130..." -- the prefix is accepted
    }

    /// WXL_UnitOutline_Update(mouseoverGuid, mouseoverReaction, targetGuid, targetReaction); arguments count from 0.
    // The client's mouseover is a pick of the model under the cursor, made every frame; on an
    // animating model it misses every few frames. A miss shorter than this keeps the last unit.
    constexpr uint64_t kMouseoverHoldMs = 250;
    uint64_t           g_mouseoverSeenAt = 0;

    void LuaUpdate(wxl::ScriptCall& call)
    {
        ++g_stats.ticks;
        const uint64_t now = GetTickCount64();
        for (uint32_t i = 0; i < 2; ++i)
        {
            const auto     guid     = call.String(2 * i);
            const auto     reaction = call.Number(1 + 2 * i);
            const uint64_t parsed   = guid ? ParseGuid(*guid) : 0;
            if (i == 0)
            {
                if (parsed)
                    g_mouseoverSeenAt = now;
                else if (g_wanted[0].guid && now - g_mouseoverSeenAt < kMouseoverHoldMs)
                    continue;
            }
            g_wanted[i].guid     = parsed;
            g_wanted[i].reaction = reaction ? *reaction : 0.0;
        }
    }

    /// WXL_UnitOutline_Get() -> enabled, mouseover, target, nearby, throughWalls
    void LuaGet(wxl::ScriptCall& call)
    {
        for (const SettingKey& key : kSettingKeys) call.PushBool(g_settings.*key.field);
    }

    /// WXL_UnitOutline_Set(name, on): one setting by its name in kSettingKeys, saved at once.
    void LuaSet(wxl::ScriptCall& call)
    {
        const auto name = call.String(0);
        const auto on   = call.Bool(1);
        if (!name || !on) return;
        for (const SettingKey& key : kSettingKeys)
        {
            if (*name != key.name) continue;
            g_settings.*key.field = *on;
            SaveSettings();
            wxl::Log(WXL_LOG_INFO, kTag, std::string(key.name) + (*on ? " on" : " off"));
            return;
        }
    }

    // pcall'd: a missing function on the glue screens, or anything else, never reaches the
    // client's error frame once a frame.
    constexpr std::string_view kLuaDefine = R"lua(
-- Interface Options > Game > Unit Outlines. Made on the first tick that finds the options frame
-- loaded; registered from this (client-run, secure) code, so it joins the Game tab's categories.
local function WXL_UnitOutline_MakePanel()
    if WXL_UnitOutlinePanel or not InterfaceOptions_AddCategory or not InterfaceOptionsFramePanelContainer then
        return
    end
    local panel = CreateFrame("Frame", "WXL_UnitOutlinePanel", InterfaceOptionsFramePanelContainer)
    panel.name = "Unit Outlines"
    panel:Hide()

    local title = panel:CreateFontString(nil, "ARTWORK", "GameFontNormalLarge")
    title:SetPoint("TOPLEFT", 16, -16)
    title:SetText("Unit Outlines")
    local sub = panel:CreateFontString(nil, "ARTWORK", "GameFontHighlightSmall")
    sub:SetPoint("TOPLEFT", title, "BOTTOMLEFT", 0, -8)
    sub:SetPoint("RIGHT", panel, "RIGHT", -32, 0)
    sub:SetJustifyH("LEFT")
    sub:SetText("A retail-style outline around units: green friendly, yellow neutral, red hostile.")

    local options = {
        { "enabled", "Show unit outlines" },
        { "mouseover", "The unit under your mouse", true },
        { "target", "Your target", true },
        { "nearby", "Living units within 30 yards", true },
        { "throughWalls", "Show outlines through walls", true },
    }
    local defaults = { enabled = true, mouseover = true, target = true, nearby = true, throughWalls = false }
    local checks, previous = {}, nil
    for i, option in ipairs(options) do
        local check = CreateFrame("CheckButton", "WXL_UnitOutlinePanelCheck" .. i, panel, "InterfaceOptionsCheckButtonTemplate")
        if i == 1 then
            check:SetPoint("TOPLEFT", sub, "BOTTOMLEFT", -2, -12)
        else
            check:SetPoint("TOPLEFT", previous, "BOTTOMLEFT", i == 2 and 16 or 0, -4)
        end
        _G[check:GetName() .. "Text"]:SetText(option[2])
        check.key, check.dependent = option[1], option[3]
        check:SetScript("OnClick", function(self)
            WXL_UnitOutline_Set(self.key, self:GetChecked() and true or false)
            panel.refresh()
        end)
        checks[i], previous = check, check
    end

    function panel.refresh()
        local values = { WXL_UnitOutline_Get() }
        local on = values[1]
        for i, check in ipairs(checks) do
            check:SetChecked(values[i])
            if check.dependent then
                if on then
                    check:Enable()
                    _G[check:GetName() .. "Text"]:SetTextColor(1, 1, 1)
                else
                    check:Disable()
                    _G[check:GetName() .. "Text"]:SetTextColor(0.5, 0.5, 0.5)
                end
            end
        end
    end
    function panel.default()
        for key, value in pairs(defaults) do
            WXL_UnitOutline_Set(key, value)
        end
    end
    panel.okay = function() end
    panel.cancel = function() end
    panel:SetScript("OnShow", panel.refresh)

    InterfaceOptions_AddCategory(panel)
end

function WXL_UnitOutline_Tick()
    if not WXL_UnitOutlinePanel then
        WXL_UnitOutline_MakePanel()
    end
    if not WXL_UnitOutline_Banner and UIParent then
        local banner = CreateFrame("Frame", "WXL_UnitOutline_Banner", UIParent)
        banner:SetFrameStrata("TOOLTIP")
        banner:SetWidth(1)
        banner:SetHeight(1)
        banner:SetPoint("TOP", UIParent, "TOP", 0, -4)
        local text = banner:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
        text:SetPoint("TOP", banner, "TOP", 0, 0)
        text:SetText("Unit outline mod loaded")
    end
    local function info(unit)
        if UnitExists(unit) and not UnitIsUnit(unit, "player") then
            return UnitGUID(unit) or "", UnitReaction(unit, "player") or 4
        end
        return "", 0
    end
    local mg, mr = info("mouseover")
    local tg, tr = info("target")
    WXL_UnitOutline_Update(mg, mr, tg, tr)
end
)lua";

    // The error, if any, goes back through WXL_UnitOutline_Error so it reaches the log.
    constexpr std::string_view kLuaTick =
        "local ok, err = pcall(WXL_UnitOutline_Tick) if not ok then WXL_UnitOutline_Error(tostring(err)) end";

    bool g_luaReady = false;

    // --- What to outline this frame -------------------------------------------------------------

    struct Outlined
    {
        void* root;
        Tint  tint;
    };

    // Sorted by root once a frame, so a batch finds its unit by binary search.
    Outlined g_outlined[2 + kMaxNearby];
    uint32_t g_outlinedCount = 0;

    void AddOutlined(void* root, Tint tint)
    {
        if (!root || g_outlinedCount >= sizeof g_outlined / sizeof g_outlined[0]) return;
        for (uint32_t i = 0; i < g_outlinedCount; ++i)
            if (g_outlined[i].root == root) return;
        g_outlined[g_outlinedCount++] = Outlined{root, tint};
    }

    const Outlined* FindOutlined(void* root)
    {
        const Outlined* end = g_outlined + g_outlinedCount;
        const Outlined* at  = std::lower_bound(static_cast<const Outlined*>(g_outlined), end, root,
                                               [](const Outlined& o, void* r) { return o.root < r; });
        return at != end && at->root == root ? at : nullptr;
    }

    // --- Device resources -----------------------------------------------------------------------

    using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
                                          LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

    D3DCompileFn g_compile       = nullptr;
    bool         g_compileTried  = false;
    bool         g_broken        = false; // a shader would not compile: say so once, draw nothing

    // [profile 3_0][alpha key][tint]; the batch's own vertex shader decides the profile, since
    // D3D9 pairs vs_3_0 only with ps_3_0.
    IDirect3DPixelShader9* g_maskPs[2][2][kTints] = {};
    IDirect3DPixelShader9* g_edgePs               = nullptr;
    bool                   g_profile3             = false;
    bool                   g_profileKnown         = false;

    // The mask matches the render target the world is drawn into, multisampling included, so the
    // client's depth buffer can be used with it. A multisampled mask is resolved into the texture.
    IDirect3DSurface9* g_maskSurface = nullptr;
    IDirect3DTexture9* g_maskTexture = nullptr;
    IDirect3DSurface9* g_maskLevel0  = nullptr;
    D3DSURFACE_DESC    g_maskDesc    = {};

    bool g_maskCleared = false; // this frame
    bool g_maskDrawn   = false; // this frame

    void ReleaseTargets()
    {
        Release(g_maskSurface);
        Release(g_maskLevel0);
        Release(g_maskTexture);
        g_maskDesc = {};
    }

    void ReleaseAll()
    {
        ReleaseTargets();
        for (auto& profile : g_maskPs)
            for (auto& key : profile)
                for (IDirect3DPixelShader9*& ps : key) Release(ps);
        Release(g_edgePs);
    }

    IDirect3DPixelShader9* Compile(IDirect3DDevice9* device, const std::string& source, const char* target)
    {
        if (!g_compileTried)
        {
            g_compileTried = true;
            if (HMODULE module = LoadLibraryW(L"d3dcompiler_47.dll"))
                g_compile = reinterpret_cast<D3DCompileFn>(GetProcAddress(module, "D3DCompile"));
            if (!g_compile) wxl::Log(WXL_LOG_ERROR, kTag, "d3dcompiler_47.dll is missing: no outlines");
        }
        if (!g_compile) return nullptr;

        ID3DBlob* code   = nullptr;
        ID3DBlob* errors = nullptr;
        const HRESULT hr = g_compile(source.data(), source.size(), "unit-outline", nullptr, nullptr,
                                     "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        IDirect3DPixelShader9* ps = nullptr;
        if (SUCCEEDED(hr) && code)
            device->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &ps);
        else
            wxl::Log(WXL_LOG_ERROR, kTag,
                     std::string("shader did not compile: ") +
                         (errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?"));
        Release(code);
        Release(errors);
        return ps;
    }

    IDirect3DPixelShader9* MaskShader(IDirect3DDevice9* device, bool alphaKey, Tint tint)
    {
        IDirect3DPixelShader9*& ps = g_maskPs[g_profile3][alphaKey][tint];
        if (ps) return ps;

        std::string source = "sampler2D s0 : register(s0);\n"
                             "float4 main(float2 uv : TEXCOORD0) : COLOR {\n";
        if (alphaKey)
        {
            char clip[64];
            std::snprintf(clip, sizeof clip, "    clip(tex2D(s0, uv).a - %.4f);\n", kAlphaKeyRef);
            source += clip;
        }
        source += std::string("    return float4(") + kTintRgb[tint] + ", 1.0);\n}\n";
        ps = Compile(device, source, g_profile3 ? "ps_3_0" : "ps_2_0");
        if (!ps) g_broken = true;
        return ps;
    }

    // Pre-transformed: no vertex shader, so ps_2_0. TEXCOORD1 carries one pixel in texture units.
    constexpr const char* kEdgeSource = R"hlsl(
sampler2D mask : register(s0);

float4 main(float2 uv : TEXCOORD0, float2 px : TEXCOORD1) : COLOR {
    float4 here = tex2D(mask, uv);
    float4 best = 0;
    const float2 ring[12] = {
        float2( 1.000,  0.000), float2( 0.866,  0.500), float2( 0.500,  0.866),
        float2( 0.000,  1.000), float2(-0.500,  0.866), float2(-0.866,  0.500),
        float2(-1.000,  0.000), float2(-0.866, -0.500), float2(-0.500, -0.866),
        float2( 0.000, -1.000), float2( 0.500, -0.866), float2( 0.866, -0.500)
    };
    for (int i = 0; i < 12; i++) {
        float4 s = tex2D(mask, uv + ring[i] * px);
        best = lerp(best, s, step(best.a, s.a));
    }
    for (int j = 0; j < 12; j += 3) {
        float4 s = tex2D(mask, uv + ring[j] * px * 0.5);
        best = lerp(best, s, step(best.a, s.a));
    }
    // Only outside the silhouette: the unit itself stays as the client drew it.
#if DEBUG_VIEW
    // The pass itself as a faint tint, the mask's silhouettes filled.
    return here.a > 0 ? float4(here.rgb, 0.6) : float4(1.0, 0.0, 1.0, 0.15);
#else
    return float4(best.rgb, best.a * (1.0 - here.a) * OPACITY);
#endif
}
)hlsl";

    bool EnsureEdgeShader(IDirect3DDevice9* device)
    {
        if (g_edgePs) return true;
        char opacity[64];
        char debug[8] = {};
        const bool debugView = GetEnvironmentVariableA("WXL_OUTLINE_DEBUG", debug, sizeof debug) && debug[0] == '1';
        std::snprintf(opacity, sizeof opacity, "#define OPACITY %.3f\n#define DEBUG_VIEW %d\n", kOpacity,
                      debugView ? 1 : 0);
        g_edgePs = Compile(device, std::string(opacity) + kEdgeSource, "ps_2_0");
        if (!g_edgePs) g_broken = true;
        return g_edgePs != nullptr;
    }

    /// The mask, sized and sampled like @p target. False when it cannot be made.
    bool EnsureMask(IDirect3DDevice9* device, IDirect3DSurface9* target)
    {
        D3DSURFACE_DESC desc{};
        if (FAILED(target->GetDesc(&desc))) return false;
        if (g_maskSurface && desc.Width == g_maskDesc.Width && desc.Height == g_maskDesc.Height &&
            desc.MultiSampleType == g_maskDesc.MultiSampleType &&
            desc.MultiSampleQuality == g_maskDesc.MultiSampleQuality)
            return true;

        ReleaseTargets();
        if (FAILED(device->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                         D3DPOOL_DEFAULT, &g_maskTexture, nullptr)) ||
            FAILED(g_maskTexture->GetSurfaceLevel(0, &g_maskLevel0)))
        {
            ReleaseTargets();
            return false;
        }
        if (desc.MultiSampleType == D3DMULTISAMPLE_NONE)
        {
            g_maskSurface = g_maskLevel0;
            g_maskSurface->AddRef();
        }
        else if (FAILED(device->CreateRenderTarget(desc.Width, desc.Height, D3DFMT_A8R8G8B8, desc.MultiSampleType,
                                                   desc.MultiSampleQuality, FALSE, &g_maskSurface, nullptr)))
        {
            ReleaseTargets();
            return false;
        }
        g_maskDesc = desc;
        return true;
    }

    // --- The silhouette: a watched batch drawn again into the mask ------------------------------

    void WXL_CALL OnModelDraw(const wxl_gx_model_draw* draw, void*)
    {
        if (g_outlinedCount == 0 || g_broken || !draw) return;
        ++g_stats.batches;

        const Outlined* hit = FindOutlined(draw->root_model);
        // Blended batches -- glows, particles' cards, fades -- would outline their whole quad.
        if (!hit) return;
        ++g_stats.matched;
        if (draw->blend_mode >= 2)
        {
            ++g_stats.skippedBlend;
            return;
        }

        IDirect3DDevice9* device = Device();
        if (!device) return;

        if (!g_profileKnown)
        {
            D3DCAPS9 caps{};
            device->GetDeviceCaps(&caps);
            g_profile3     = caps.VertexShaderVersion >= D3DVS_VERSION(3, 0) &&
                             caps.PixelShaderVersion >= D3DPS_VERSION(3, 0);
            g_profileKnown = true;
        }

        IDirect3DSurface9* target = nullptr;
        if (FAILED(device->GetRenderTarget(0, &target)) || !target) return;
        if (!EnsureMask(device, target))
        {
            target->Release();
            return;
        }

        const bool             alphaKey = draw->blend_mode == 1;
        IDirect3DPixelShader9* ps       = MaskShader(device, alphaKey, hit->tint);
        if (!ps)
        {
            target->Release();
            return;
        }

        device->SetRenderTarget(0, g_maskSurface);
        if (!g_maskCleared)
        {
            // Before the viewport: a new target's viewport is all of it, so this clears it all.
            device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
            g_maskCleared = true;
        }

        D3DVIEWPORT9 viewport{draw->viewport_x,     draw->viewport_y,     draw->viewport_width,
                              draw->viewport_height, draw->viewport_min_z, draw->viewport_max_z};
        device->SetViewport(&viewport);
        device->SetPixelShader(ps);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_ZFUNC, g_settings.throughWalls ? D3DCMP_ALWAYS : D3DCMP_LESSEQUAL);

        HRESULT hr = device->DrawIndexedPrimitive(static_cast<D3DPRIMITIVETYPE>(draw->primitive_type),
                                                  draw->base_vertex, draw->min_index, draw->vertex_count,
                                                  draw->start_index, draw->primitive_count);
        if (FAILED(hr) && g_profile3)
        {
            // The batch ran without a vs_3_0 shader (shaders off): ps_2_0 from now on.
            g_profile3 = false;
            wxl::Log(WXL_LOG_INFO, kTag, "the client draws models without vs_3_0; using ps_2_0");
            if (IDirect3DPixelShader9* fallback = MaskShader(device, alphaKey, hit->tint))
            {
                device->SetPixelShader(fallback);
                hr = device->DrawIndexedPrimitive(static_cast<D3DPRIMITIVETYPE>(draw->primitive_type),
                                                  draw->base_vertex, draw->min_index, draw->vertex_count,
                                                  draw->start_index, draw->primitive_count);
            }
        }
        if (SUCCEEDED(hr))
        {
            g_maskDrawn = true;
            ++g_stats.masked;
        }
        else
        {
            ++g_stats.failed;
        }

        device->SetRenderTarget(0, target);
        target->Release();
        g_gx->invalidate_state();
    }

    // --- The outline: the mask's edge painted over the world ------------------------------------

    struct EdgeVertex
    {
        float x, y, z, rhw;
        float u, v;
        float px, py;
    };

    void PaintEdges()
    {
        IDirect3DDevice9* device = Device();
        if (!device || !g_maskTexture || !EnsureEdgeShader(device)) return;

        IDirect3DSurface9* target = nullptr;
        if (FAILED(device->GetRenderTarget(0, &target)) || !target) return;

        if (g_maskSurface != g_maskLevel0)
            device->StretchRect(g_maskSurface, nullptr, g_maskLevel0, nullptr, D3DTEXF_NONE);

        const float w  = static_cast<float>(g_maskDesc.Width);
        const float h  = static_cast<float>(g_maskDesc.Height);
        const float px = kThickness / w;
        const float py = kThickness / h;
        const EdgeVertex quad[4] = {
            {-0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f, px, py},
            {w - 0.5f, -0.5f, 0.0f, 1.0f, 1.0f, 0.0f, px, py},
            {-0.5f, h - 0.5f, 0.0f, 1.0f, 0.0f, 1.0f, px, py},
            {w - 0.5f, h - 0.5f, 0.0f, 1.0f, 1.0f, 1.0f, px, py},
        };

        device->SetVertexShader(nullptr);
        device->SetPixelShader(g_edgePs);
        device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX2);
        device->SetTexture(0, g_maskTexture);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        if (SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof quad[0]))) ++g_stats.painted;

        device->SetTexture(0, nullptr);
        target->Release();
        g_gx->invalidate_state();
    }

    // --- Scripts --------------------------------------------------------------------------------

    class OutlineLua final : public wxl::LuaScript
    {
    public:
        void OnScriptVmReady(wxl_script_context context, void*) override
        {
            g_luaReady = false;
            g_wanted[0] = g_wanted[1] = Wanted{};
            if (context != WXL_SCRIPT_CONTEXT_GAME) return;
            if (g_script.RegisterFunction<&LuaUpdate>("WXL_UnitOutline_Update") != WXL_OK ||
                g_script.RegisterFunction<&LuaError>("WXL_UnitOutline_Error") != WXL_OK ||
                g_script.RegisterFunction<&LuaGet>("WXL_UnitOutline_Get") != WXL_OK ||
                g_script.RegisterFunction<&LuaSet>("WXL_UnitOutline_Set") != WXL_OK)
            {
                wxl::Log(WXL_LOG_WARN, kTag, "could not register the Lua functions");
                return;
            }
            g_luaReady = g_script.Run(kLuaDefine, "unit-outline") == WXL_OK;
            wxl::Log(g_luaReady ? WXL_LOG_INFO : WXL_LOG_WARN, kTag,
                     g_luaReady ? "Lua side installed" : "Lua side did not run");
        }

        void OnScriptVmClosing(wxl_script_context, void*) override
        {
            g_luaReady = false;
            g_wanted[0] = g_wanted[1] = Wanted{};
        }
    };

    class OutlineWorld final : public wxl::WorldScript
    {
    public:
        void OnUpdate(float) override
        {
            if (g_luaReady) g_script.Run(kLuaTick, "unit-outline");
        }

        void OnWorldLeave(uint32_t, wxl_vec3) override { g_wanted[0] = g_wanted[1] = Wanted{}; }
    };

    class OutlineRender final : public wxl::RenderScript
    {
    public:
        void OnWorldRenderBegin(uint64_t, float) override
        {
            g_maskCleared   = false;
            g_maskDrawn     = false;
            g_outlinedCount = 0;
            if (!g_settings.enabled) return;              // nothing outlined, nothing drawn
            if (g_wanted[0].guid) ++g_stats.mouseover;
            if (g_wanted[1].guid) ++g_stats.target;
            for (uint32_t i = 0; i < 2; ++i)
            {
                const Wanted& wanted = g_wanted[i];
                if (!(i == 0 ? g_settings.mouseover : g_settings.target)) continue;
                Tint tint;
                if (!wanted.guid || !TintOf(wanted.reaction, tint)) continue;
                ++g_stats.wanted;
                const wxl::Object object = wxl::Object::Resolve(wanted.guid);
                if (!object.HasHandle()) continue;
                ++g_stats.resolved;
                void* const root = g_gx->object_model(object.Handle());
                if (!root) continue;
                ++g_stats.modeled;
                AddOutlined(root, tint);
            }
            if (g_settings.nearby) AddNearby();
            std::sort(g_outlined, g_outlined + g_outlinedCount,
                      [](const Outlined& a, const Outlined& b) { return a.root < b.root; });
        }

        /// Every living unit within kNearbyRange of the player, nearest first, coloured by how it
        /// regards the player.
        static void AddNearby()
        {
            const wxl::Player me = wxl::Player::Local();
            const wxl::Expected<wxl_vec3> here = me.Position();
            if (!me.HasHandle() || !here) return;

            static std::vector<std::pair<float, wxl::Unit>> nearby;
            nearby.clear();
            const wxl::ObjectService objects;
            for (const wxl::Unit& unit : objects.InView<wxl::Unit>())
            {
                if (unit.Handle() == me.Handle() || unit.Health().ValueOr(0) == 0) continue;
                const wxl::Expected<wxl_vec3> at = unit.Position();
                if (!at) continue;
                const float dx = at->x - here->x, dy = at->y - here->y, dz = at->z - here->z;
                const float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 <= kNearbyRange * kNearbyRange) nearby.emplace_back(d2, unit);
            }
            const size_t keep = std::min<size_t>(nearby.size(), kMaxNearby);
            std::partial_sort(nearby.begin(), nearby.begin() + keep, nearby.end(),
                              [](const auto& a, const auto& b) { return a.first < b.first; });
            for (size_t i = 0; i < keep; ++i)
            {
                const wxl::Expected<uint32_t> reaction = nearby[i].second.ReactionTo(me);
                if (!reaction) continue;
                AddOutlined(g_gx->object_model(nearby[i].second.Handle()), TintOfNative(*reaction));
            }
        }

        void OnWorldRenderEnd(uint64_t, float) override
        {
            if (g_maskDrawn) PaintEdges();
            g_outlinedCount = 0;
            FlushStats();
        }

        void OnFrameBegin(uint64_t, float) override { ++g_stats.paints; }

        void OnSwapchainLost(uint32_t, uint32_t) override { ReleaseTargets(); }
    };

    OutlineLua    g_lua;
    OutlineWorld  g_world;
    OutlineRender g_render;
    uint32_t      g_watch = 0;

    class UnitOutlineMod final : public wxl::Mod
    {
    public:
        wxl_result OnLoad() override
        {
            if (!g_gx || !g_script)
            {
                wxl::Log(WXL_LOG_ERROR, kTag, "wxl.gx or wxl.script is not published; nothing to do");
                return WXL_E_UNSUPPORTED;
            }
            const wxl_result watched = g_gx->watch_model_draws(&OnModelDraw, nullptr, &g_watch);
            if (watched != WXL_OK) return watched;

            LoadSettings();
            wxl::ScriptMgr::Add(&g_lua);
            wxl::ScriptMgr::Add(&g_world);
            wxl::ScriptMgr::Add(&g_render);
            wxl::Log(WXL_LOG_INFO, kTag, g_settings.enabled
                ? "outlining the mouseover, the target and units within 30 yards (Interface Options > Unit Outlines)"
                : "outlines are off (Interface Options > Unit Outlines)");
            return WXL_OK;
        }

        void OnShutdown() override { ReleaseAll(); }

        void OnUnload() override
        {
            if (g_gx && g_watch) g_gx->unwatch_model_draws(g_watch);
            g_watch = 0;
            ReleaseAll();
        }
    };
}

WXL_MOD(UnitOutlineMod, "wxl.unit-outline", "Unit outline")

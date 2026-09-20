-- ============================================================================
-- VRDLSS5 - DLSS 5 Neural Reconstruction for UEVR (Native VR Menu)
-- ============================================================================

if _G.VRDLSS5_INITIALIZED then
    return
end
_G.VRDLSS5_INITIALIZED = true

local settings = {
    Enabled = false,
    WorkingScale = 0.75,
    RunBeforeSR = true,
    Preset = 2, -- 0=Default, 1=Quality, 2=Performance
    ResidualAcrossRR = false,
    Intensity = 1.0,
    Style = 0, -- 0=Standard, 1=Natural, 2=Cinematic
    LocalStructure = 1.0,
    LocalTone = 0.0,
    AutoMask = true,
    SkinStructure = -1.0,
    Passes = 1,
    TransferStrength = 1.0,
    ColourStrength = 1.0,
    FrameGuard = true
}

local presets_list = { "0 - Default", "1 - Quality", "2 - Performance (VR Recommended)" }
local styles_list = { "0 - Standard", "1 - Natural", "2 - Cinematic" }
local passes_list = { "1 - Single Pass", "2 - Double Pass (Dense)", "3 - Triple Pass (Hyper)" }

local ini_path = "OptiScaler.ini"

-- Load settings from OptiScaler.ini
local function load_settings()
    local f = io.open(ini_path, "r")
    if not f then return end

    local in_dlssnr = false
    for line in f:lines() do
        local section = line:match("^%s*%[([^%]]+)%]")
        if section then
            in_dlssnr = (section == "DlssNr")
        elseif in_dlssnr then
            local k, v = line:match("^%s*([^=]+)%s*=%s*(.-)%s*$")
            if k and v then
                if k == "Enabled" then settings.Enabled = (v:lower() == "true" or v == "1")
                elseif k == "WorkingScale" then settings.WorkingScale = tonumber(v) or 0.75
                elseif k == "RunBeforeSR" then settings.RunBeforeSR = (v:lower() == "true" or v == "1")
                elseif k == "Preset" then settings.Preset = tonumber(v) or 2
                elseif k == "ResidualAcrossRR" then settings.ResidualAcrossRR = (v:lower() == "true" or v == "1")
                elseif k == "Intensity" then settings.Intensity = tonumber(v) or 1.0
                elseif k == "Style" then settings.Style = tonumber(v) or 0
                elseif k == "LocalStructure" then settings.LocalStructure = tonumber(v) or 1.0
                elseif k == "LocalTone" then settings.LocalTone = tonumber(v) or 0.0
                elseif k == "AutoMask" then settings.AutoMask = (v:lower() == "true" or v == "1")
                elseif k == "SkinStructure" then settings.SkinStructure = tonumber(v) or -1.0
                elseif k == "Passes" then settings.Passes = tonumber(v) or 1
                elseif k == "TransferStrength" then settings.TransferStrength = tonumber(v) or 1.0
                elseif k == "ColourStrength" then settings.ColourStrength = tonumber(v) or 1.0
                end
            end
        end
    end
    f:close()
end

-- Save settings to OptiScaler.ini
local function save_settings()
    local lines = {}
    local f = io.open(ini_path, "r")
    local dlssnr_found = false
    local in_dlssnr = false

    if f then
        for line in f:lines() do
            local section = line:match("^%s*%[([^%]]+)%]")
            if section then
                if in_dlssnr then in_dlssnr = false end
                if section == "DlssNr" then
                    in_dlssnr = true
                    dlssnr_found = true
                end
                table.insert(lines, line)
            elseif in_dlssnr then
                local k = line:match("^%s*([^=]+)%s*=")
                if k then
                    if k == "Enabled" then table.insert(lines, string.format("Enabled = %s", settings.Enabled and "true" or "false"))
                    elseif k == "WorkingScale" then table.insert(lines, string.format("WorkingScale = %.2f", settings.WorkingScale))
                    elseif k == "RunBeforeSR" then table.insert(lines, string.format("RunBeforeSR = %s", settings.RunBeforeSR and "true" or "false"))
                    elseif k == "Preset" then table.insert(lines, string.format("Preset = %d", settings.Preset))
                    elseif k == "ResidualAcrossRR" then table.insert(lines, string.format("ResidualAcrossRR = %s", settings.ResidualAcrossRR and "true" or "false"))
                    elseif k == "Intensity" then table.insert(lines, string.format("Intensity = %.2f", settings.Intensity))
                    elseif k == "Style" then table.insert(lines, string.format("Style = %d", settings.Style))
                    elseif k == "LocalStructure" then table.insert(lines, string.format("LocalStructure = %.2f", settings.LocalStructure))
                    elseif k == "LocalTone" then table.insert(lines, string.format("LocalTone = %.2f", settings.LocalTone))
                    elseif k == "AutoMask" then table.insert(lines, string.format("AutoMask = %s", settings.AutoMask and "true" or "false"))
                    elseif k == "SkinStructure" then table.insert(lines, string.format("SkinStructure = %.2f", settings.SkinStructure))
                    elseif k == "Passes" then table.insert(lines, string.format("Passes = %d", settings.Passes))
                    elseif k == "TransferStrength" then table.insert(lines, string.format("TransferStrength = %.2f", settings.TransferStrength))
                    elseif k == "ColourStrength" then table.insert(lines, string.format("ColourStrength = %.2f", settings.ColourStrength))
                    else table.insert(lines, line)
                    end
                else
                    table.insert(lines, line)
                end
            else
                table.insert(lines, line)
            end
        end
        f:close()
    end

    if not dlssnr_found then
        table.insert(lines, "\n[DlssNr]")
        table.insert(lines, string.format("Enabled = %s", settings.Enabled and "true" or "false"))
        table.insert(lines, string.format("WorkingScale = %.2f", settings.WorkingScale))
        table.insert(lines, string.format("RunBeforeSR = %s", settings.RunBeforeSR and "true" or "false"))
        table.insert(lines, string.format("Preset = %d", settings.Preset))
        table.insert(lines, string.format("ResidualAcrossRR = %s", settings.ResidualAcrossRR and "true" or "false"))
        table.insert(lines, string.format("Intensity = %.2f", settings.Intensity))
        table.insert(lines, string.format("Style = %d", settings.Style))
        table.insert(lines, string.format("LocalStructure = %.2f", settings.LocalStructure))
        table.insert(lines, string.format("LocalTone = %.2f", settings.LocalTone))
        table.insert(lines, string.format("AutoMask = %s", settings.AutoMask and "true" or "false"))
        table.insert(lines, string.format("SkinStructure = %.2f", settings.SkinStructure))
        table.insert(lines, string.format("Passes = %d", settings.Passes))
        table.insert(lines, string.format("TransferStrength = %.2f", settings.TransferStrength))
        table.insert(lines, string.format("ColourStrength = %.2f", settings.ColourStrength))
    end

    local out = io.open(ini_path, "w")
    if out then
        for _, l in ipairs(lines) do
            out:write(l .. "\n")
        end
        out:close()
    end
end

-- Sync with C++ plugin via custom event
local function sync_with_plugin()
    if uevr and uevr.params and uevr.params.functions and uevr.params.functions.dispatch_custom_event then
        local payload = string.format("%.2f;%d;%d;%d;%.2f;%d;%.2f;%.2f;%d;%.2f;%d;%.2f;%.2f",
            settings.WorkingScale,
            settings.Enabled and 1 or 0,
            settings.RunBeforeSR and 1 or 0,
            settings.Preset,
            settings.Intensity,
            settings.Style,
            settings.LocalStructure,
            settings.LocalTone,
            settings.AutoMask and 1 or 0,
            settings.SkinStructure,
            settings.Passes,
            settings.TransferStrength,
            settings.ColourStrength
        )
        uevr.params.functions.dispatch_custom_event("VRDLSS5_SYNC", payload)
    end
end

load_settings()

-- Main UI Drawing Function
local function draw_dlss5_ui()
    imgui.text("DLSS 5 Neural Reconstruction (Pre-SR)")
    imgui.text("AI Engine for UEVR & OptiScaler")
    imgui.separator()

    local changed = false

    local c_en, n_en = imgui.checkbox("Enable Neural Engine (DLSS 5)", settings.Enabled)
    if c_en then settings.Enabled = n_en; changed = true end

    local c_ws, n_ws = imgui.slider_float("VR WorkingScale", settings.WorkingScale, 0.25, 1.50, "%.2fx")
    if c_ws then settings.WorkingScale = n_ws; changed = true end
    imgui.same_line()
    if imgui.button("Reset 0.75x") then settings.WorkingScale = 0.75; changed = true end

    local c_sr, n_sr = imgui.checkbox("Run Before SR (Pre-SR Multipass)", settings.RunBeforeSR)
    if c_sr then settings.RunBeforeSR = n_sr; changed = true end
    imgui.same_line()
    local c_rr, n_rr = imgui.checkbox("ResidualAcrossRR", settings.ResidualAcrossRR)
    if c_rr then settings.ResidualAcrossRR = n_rr; changed = true end

    local preset_idx = settings.Preset + 1
    local c_pr, n_pr = imgui.combo("AI Model Preset", preset_idx, presets_list)
    if c_pr then settings.Preset = n_pr - 1; changed = true end

    local style_idx = settings.Style + 1
    local c_st, n_st = imgui.combo("DLSS 5 Style", style_idx, styles_list)
    if c_st then settings.Style = n_st - 1; changed = true end

    local c_int, n_int = imgui.slider_float("DLSS 5 Intensity", settings.Intensity, 0.0, 2.0, "%.2fx")
    if c_int then settings.Intensity = n_int; changed = true end

    imgui.spacing()
    imgui.separator()

    if imgui.collapsing_header("Advanced Rendering & World Parameters") then
        local c_ls, n_ls = imgui.slider_float("World Structure", settings.LocalStructure, 0.0, 2.0, "%.2fx")
        if c_ls then settings.LocalStructure = n_ls; changed = true end

        local c_lt, n_lt = imgui.slider_float("Shadow Tone", settings.LocalTone, 0.0, 2.0, "%.2fx")
        if c_lt then settings.LocalTone = n_lt; changed = true end

        local c_am, n_am = imgui.checkbox("Auto Mask (Characters & Faces)", settings.AutoMask)
        if c_am then settings.AutoMask = n_am; changed = true end

        local c_sk, n_sk = imgui.slider_float("Skin Structure", settings.SkinStructure, -1.0, 2.0, "%.2fx")
        if c_sk then settings.SkinStructure = n_sk; changed = true end

        local pass_idx = settings.Passes
        local c_pa, n_pa = imgui.combo("AI Passes", pass_idx, passes_list)
        if c_pa then settings.Passes = n_pa; changed = true end

        local c_tr, n_tr = imgui.slider_float("Texture Transfer", settings.TransferStrength, 0.0, 2.0, "%.2fx")
        if c_tr then settings.TransferStrength = n_tr; changed = true end

        local c_co, n_co = imgui.slider_float("Color Strength", settings.ColourStrength, 0.0, 1.0, "%.2fx")
        if c_co then settings.ColourStrength = n_co; changed = true end
    end

    imgui.spacing()
    imgui.separator()

    local c_fg, n_fg = imgui.checkbox("Dynamic VR Frame Guard", settings.FrameGuard)
    if c_fg then settings.FrameGuard = n_fg; changed = true end

    imgui.spacing()
    imgui.text("UEVR Menu: L3 + R3 (Native UEVR)")

    if changed then
        save_settings()
        sync_with_plugin()
    end
end

-- 1. Register as a dedicated named panel in the UEVR sidebar
if uevr and uevr.lua and uevr.lua.add_script_panel then
    uevr.lua.add_script_panel("DLSS 5 Neural Reconstruction", draw_dlss5_ui)
end


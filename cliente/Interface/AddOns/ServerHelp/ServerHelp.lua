--[[
ServerHelp — la pestaña "Ayuda básica" (KnowledgeBaseFrame) de "Solicitud de
ayuda" con los comandos y artículos del servidor.

POR QUÉ UN ADDON Y NO UN PARCHE
La Knowledge Base original del cliente 3.3.5a consulta por HTTP los servidores
de soporte de Blizzard: en un servidor privado siempre falla. No hay ningún
paquete de juego para ella, y cambiar los ficheros FrameXML por MPQ exige
parchear Wow.exe. Un addon carga después de todo FrameXML y puede sustituir la
lógica del KnowledgeBaseFrame sin tocar nada más: mismo XML, mismas texturas,
mismos botones de ticket (que siguen funcionando).

CÓMO HABLA CON EL SERVIDOR
Por el canal de addon que AzerothCore ya procesa (AddonChannelCommandHandler,
prefijo "AzerothCore"): cada petición es el comando ".ayuda ..." del módulo
mod-server-help, y cada línea de respuesta llega como un mensaje de addon con
un contador de 4 caracteres para emparejarla. El servidor filtra por permisos
con la sesión real del jugador: aquí sólo se pinta lo que llega.

  petición:  SendAddonMessage("AzerothCore", "i0001ayuda indice", "WHISPER", yo)
  respuesta: CHAT_MSG_ADDON "AzerothCore"  "a0001" (ack) · "m0001<línea>" (una
             por línea, campos separados por tabulador) · "o0001" (ok) · "f0001" (fallo)

Formato de líneas (mod_server_help.cpp):
  V hash reino entradas nivel      C id padre nombre      E tipo clave cat nivel titulo hot
  R tipo clave                     T titulo · P nivel · G categoria · S sintaxis ·
  D linea (D+ continúa la anterior) · X ejemplo · K keywords · Z fin · ! error
]]

ServerHelp = ServerHelp or {}
local SH = ServerHelp
local L  = SH.L

local PREFIX      = "AzerothCore"
local SEP         = "\t"
local TIMEOUT     = 10        -- segundos sin respuesta antes de darla por perdida
local PER_PAGE    = 20        -- botones KnowledgeBaseArticleListItem1..20
local RETRY_COOLDOWN = 2.5    -- el servidor limita los índices por cuenta

-- ─────────────────────────────────────────────────────────────────────────────
-- Protocolo
-- ─────────────────────────────────────────────────────────────────────────────
local pending = {}
local counter = 0

local function NextCounter()
    counter = counter % 9999 + 1
    return string.format("%04d", counter)
end

-- El addon se auto-susurra y el core devuelve las respuestas por el mismo canal.
-- Se registra el remitente de la PRIMERA respuesta buena y a partir de ahí se
-- exige: así una trama con el prefijo "AzerothCore" enviada por otro jugador (o
-- por otro addon que comparte prefijo) no se cuela como respuesta. La primera
-- vez se acepta cualquiera para no romper la feature si el core cambia el
-- remitente.
local learnedSender = nil
local function ExpectedSender(channel, sender)
    if channel and channel ~= "WHISPER" then
        return false
    end
    if learnedSender == nil then
        return true
    end
    return (not sender) or sender == "" or sender == learnedSender
end

-- Quita saltos de línea y caracteres de control de un comando antes de enviarlo,
-- y lo acota: nada de inyectar líneas en el log del servidor ni tramas raras.
local function CleanCommand(command)
    command = tostring(command or "")
    command = string.gsub(command, "[%z\1-\31]", " ")
    if #command > 160 then
        command = string.sub(command, 1, 160)
    end
    return command
end

-- Manda un comando ".ayuda ..." (sin el punto) y llama a onDone(ok, lines):
-- ok = true (o), false (f) o nil (sin respuesta a tiempo).
local function Send(command, onDone)
    local id = NextCounter()
    pending[id] = { lines = {}, onDone = onDone, started = GetTime(), done = false }
    SendAddonMessage(PREFIX, "i" .. id .. CleanCommand(command), "WHISPER", UnitName("player"))
    return id
end

local function OnAddonMessage(prefix, msg, channel, sender)
    if prefix ~= PREFIX or not msg or #msg < 5 then
        return
    end
    if not ExpectedSender(channel, sender) then
        return
    end
    local op   = string.sub(msg, 1, 1)
    local id   = string.sub(msg, 2, 5)
    local body = string.sub(msg, 6)
    -- id de exactamente 4 dígitos: una trama de otro addon con otro formato
    -- no coincide con ninguna petición pendiente.
    if not string.find(id, "^%d%d%d%d$") then
        return
    end
    local req  = pending[id]
    if not req or req.done then
        return   -- trama ajena, o llega tarde tras "o"/"f" (fuera de secuencia)
    end
    if learnedSender == nil and sender and sender ~= "" then
        learnedSender = sender   -- primera respuesta válida: fija el remitente esperado
    end
    if op == "a" then
        req.acked = true
    elseif op == "m" then
        table.insert(req.lines, body)
    elseif op == "o" or op == "f" then
        req.done = true
        pending[id] = nil
        req.onDone(op == "o", req.lines)
    end
end

local function CheckTimeouts()
    local now = GetTime()
    for id, req in pairs(pending) do
        if now - req.started > TIMEOUT then
            pending[id] = nil
            req.onDone(nil, req.lines)
        end
    end
end

local function Fields(line)
    return { strsplit(SEP, line) }
end

-- ─────────────────────────────────────────────────────────────────────────────
-- Estado
-- ─────────────────────────────────────────────────────────────────────────────
SH.index    = nil     -- { hash, realm, level, cats = {id -> cat}, roots = {}, entries = {}, byKey = {} }
SH.results  = {}      -- entradas que se están listando
SH.page     = 1
SH.listTitle = ""
SH.catSel   = 0       -- 0 = todas
SH.subSel   = 0
SH.loading  = false
SH.retryAt  = nil
SH.pendingSearch = false
SH.detailReq = nil

local function LevelName(level)
    level = tonumber(level) or 0
    if level >= 200 then
        return string.format(L["LEVEL_RBAC"], level)
    end
    return L["LEVEL_" .. math.min(level, 4)] or tostring(level)
end

local function Placeholder(text)
    return (not text) or text == "" or text == KBASE_DEFAULT_SEARCH_TEXT
end

-- ─────────────────────────────────────────────────────────────────────────────
-- Vistas: cargando, error, lista, artículo
-- ─────────────────────────────────────────────────────────────────────────────
local function ShowLoading()
    KnowledgeBaseFrame_DisableButtons()
    KnowledgeBaseArticleListFrame_HideArticleList()
    KnowledgeBaseArticleListFrameTitle:SetText(L["LOADING_SHORT"])
    KnowledgeBaseArticleListFrameCount:SetText("")
    KnowledgeBaseFrame_ShowSearchFrame()
end

local function ShowError(text)
    KnowledgeBaseErrorFrame_SetErrorMessage(text)
    KnowledgeBaseFrame_ShowErrorFrame()
    KBASE_ENABLE_SEARCH = 1
    KnowledgeBaseFrameTopIssuesButton:Enable()
    KnowledgeBaseFrameSearchButton:Enable()
    UIDropDownMenu_EnableDropDown(KnowledgeBaseFrameCategoryDropDown)
end

local function UpdateStatusLine()
    local idx = SH.index
    if idx then
        KnowledgeBaseMotdLabel:SetText(L["SERVER_LABEL"])
        KnowledgeBaseMotdText:SetText(string.format(L["STATUS_FMT"], idx.realm or "", #idx.entries, LevelName(idx.level)))
    else
        KnowledgeBaseMotdText:SetText(nil)
    end
    KnowledgeBaseServerMessageText:SetText(nil)
    KnowledgeBaseUpdateTopPanelPositions()
end

local function EntryTitle(e)
    if e.kind == "C" then
        if e.title and e.title ~= "" then
            return "|cffffffff." .. e.key .. "|r  |cffffd100" .. e.title .. "|r"
        end
        return "|cffffffff." .. e.key .. "|r"
    end
    return "|cffffd100" .. (e.title or "") .. "|r"
end

local function ShowPage()
    local total = #SH.results
    local pages = math.max(1, math.ceil(total / PER_PAGE))
    if SH.page > pages then SH.page = pages end
    if SH.page < 1 then SH.page = 1 end
    KBASE_CURRENT_PAGE = SH.page

    KnowledgeBaseArticleListFrame_HideArticleList()
    local first = (SH.page - 1) * PER_PAGE + 1
    local last  = math.min(total, SH.page * PER_PAGE)
    local shown = 0
    for i = first, last do
        shown = shown + 1
        local e = SH.results[i]
        local frame = _G["KnowledgeBaseArticleListItem" .. shown]
        frame.number = i
        frame.entry = e
        frame.articleId = e.key
        frame.articleHeader = EntryTitle(e)
        frame.isArticleHot = e.hot
        frame.isArticleUpdated = false
        KnowledgeBaseArticleListItem_Update(frame)
        frame:Show()
    end

    KnowledgeBaseArticleListFrameTitle:SetText(SH.listTitle)
    if total > 0 then
        KnowledgeBaseArticleListFrameCount:SetFormattedText(L["ENTRIES_FMT"], first, last, total)
        KnowledgeBaseFrame_ShowSearchFrame()
    else
        KnowledgeBaseArticleListFrameCount:SetText("")
        ShowError(L["NO_RESULTS"])
    end
    KnowledgeBaseFrame_EnableButtons(shown, total)
end

-- ─────────────────────────────────────────────────────────────────────────────
-- Índice
-- ─────────────────────────────────────────────────────────────────────────────
local function IsInCategory(e, catId)
    if catId == 0 then
        return true
    end
    local idx = SH.index
    local c = e.cat
    local guard = 0
    while c and c ~= 0 and guard < 16 do
        if c == catId then
            return true
        end
        local cat = idx.cats[c]
        c = cat and cat.parent or 0
        guard = guard + 1
    end
    return false
end

local function CurrentFilter()
    if SH.subSel ~= 0 then
        return SH.subSel
    end
    return SH.catSel
end

local function FilterByCategory(list)
    local sel = CurrentFilter()
    if sel == 0 then
        return list
    end
    local out = {}
    for _, e in ipairs(list) do
        if IsInCategory(e, sel) then
            table.insert(out, e)
        end
    end
    return out
end

local function CategoryTitle()
    local sel = CurrentFilter()
    if sel == 0 or not SH.index or not SH.index.cats[sel] then
        return L["ALL_ENTRIES"]
    end
    return SH.index.cats[sel].name
end

local function BuildIndex(lines, hash, realm, level)
    local idx = { hash = hash, realm = realm, level = level, cats = {}, roots = {}, children = {}, entries = {}, byKey = {} }
    for _, line in ipairs(lines) do
        local f = Fields(line)
        if f[1] == "C" then
            local cat = { id = tonumber(f[2]) or 0, parent = tonumber(f[3]) or 0, name = f[4] or "" }
            idx.cats[cat.id] = cat
            if cat.parent == 0 then
                table.insert(idx.roots, cat)
            else
                idx.children[cat.parent] = idx.children[cat.parent] or {}
                table.insert(idx.children[cat.parent], cat)
            end
        elseif f[1] == "E" then
            local e = {
                kind  = f[2],
                key   = f[3] or "",
                cat   = tonumber(f[4]) or 0,
                level = tonumber(f[5]) or 0,
                title = f[6] or "",
                hot   = (f[7] == "1"),
            }
            table.insert(idx.entries, e)
            idx.byKey[e.kind .. ":" .. string.lower(e.key)] = e
        end
    end
    SH.index = idx
end

local function ShowHome()
    SH.results = FilterByCategory(SH.index and SH.index.entries or {})
    SH.listTitle = CategoryTitle()
    SH.page = 1
    UpdateStatusLine()
    ShowPage()
end

local function AfterIndex()
    UIDropDownMenu_Initialize(KnowledgeBaseFrameCategoryDropDown, KnowledgeBaseFrameCategoryDropDown_Initialize)
    UIDropDownMenu_Initialize(KnowledgeBaseFrameSubCategoryDropDown, KnowledgeBaseFrameSubCategoryDropDown_Initialize)
    UpdateSubCategoryEnabledState()
    if SH.pendingSearch then
        SH.pendingSearch = false
        KnowledgeBaseFrame_Search(1)
    else
        ShowHome()
    end
end

local function RequestIndex(hash, realm, level)
    Send("ayuda indice", function(ok, lines)
        SH.loading = false
        if ok == nil then
            ShowError(L["NO_SERVER"])
            return
        end
        if not ok then
            if lines[1] and string.find(lines[1], "cooldown", 1, true) then
                SH.retryAt = GetTime() + RETRY_COOLDOWN
                return
            end
            ShowError(lines[1] and string.gsub(lines[1], "^!%s*", "") or L["ERR_GENERIC"])
            return
        end
        BuildIndex(lines, hash, realm, level)
        AfterIndex()
    end)
end

function SH:EnsureIndex(force)
    if self.loading then
        return
    end
    self.loading = true
    ShowLoading()
    Send("ayuda version", function(ok, lines)
        if not ok then
            self.loading = false
            if ok == nil then
                ShowError(L["NO_SERVER"])
            else
                ShowError(lines[1] and string.gsub(lines[1], "^!%s*", "") or L["ERR_GENERIC"])
            end
            return
        end
        local v = lines[1] and Fields(lines[1]) or {}
        if v[1] ~= "V" then
            self.loading = false
            ShowError(L["ERR_GENERIC"])
            return
        end
        local hash, realm, count, level = v[2], v[3], tonumber(v[4]) or 0, tonumber(v[5]) or 0
        if not force and self.index and self.index.hash == hash then
            self.loading = false
            AfterIndex()
            return
        end
        RequestIndex(hash, realm, level)
    end)
end

-- ─────────────────────────────────────────────────────────────────────────────
-- Detalle de una entrada
-- ─────────────────────────────────────────────────────────────────────────────
local function ShowDetail(e, lines)
    local title, level, cat, syntax, keywords = "", "", "", "", ""
    local desc, examples = {}, {}
    for _, line in ipairs(lines) do
        local f = Fields(line)
        local tag, val = f[1], f[2] or ""
        if tag == "T" then title = val
        elseif tag == "P" then level = val
        elseif tag == "G" then cat = val
        elseif tag == "S" then syntax = val
        elseif tag == "D" then table.insert(desc, val)
        elseif tag == "D+" and #desc > 0 then desc[#desc] = desc[#desc] .. val
        elseif tag == "X" then table.insert(examples, val)
        elseif tag == "X+" and #examples > 0 then examples[#examples] = examples[#examples] .. val
        elseif tag == "K" then keywords = val
        elseif tag == "K+" then keywords = keywords .. val
        elseif tag == "S+" then syntax = syntax .. val
        elseif tag == "T+" then title = title .. val
        end
    end

    local head, body
    if e.kind == "C" then
        head = "." .. e.key
        local parts = {}
        if title ~= "" then
            table.insert(parts, "|cffffd100" .. title .. "|r")
        end
        table.insert(parts, "|cffffd100" .. L["PERMISSION"] .. ":|r " .. level .. "    |cffffd100" .. L["CATEGORY"] .. ":|r " .. cat)
        if syntax ~= "" then
            table.insert(parts, "")
            table.insert(parts, "|cffffd100" .. L["USAGE"] .. ":|r")
            table.insert(parts, "  " .. syntax)
        end
        if #desc > 0 then
            table.insert(parts, "")
            table.insert(parts, "|cffffd100" .. L["DESCRIPTION"] .. ":|r")
            for _, d in ipairs(desc) do
                table.insert(parts, d)
            end
        end
        if #examples > 0 then
            table.insert(parts, "")
            table.insert(parts, "|cffffd100" .. L["EXAMPLES"] .. ":|r")
            for _, x in ipairs(examples) do
                table.insert(parts, "  |cffffffff" .. x .. "|r")
            end
        end
        body = table.concat(parts, "\n")
    else
        head = title ~= "" and title or e.title
        local parts = { "|cffffd100" .. L["CATEGORY"] .. ":|r " .. cat, "" }
        for _, d in ipairs(desc) do
            table.insert(parts, d)
        end
        body = table.concat(parts, "\n")
    end

    KnowledgeBaseArticleScrollChildFrameTitle:SetText(head)
    KnowledgeBaseArticleScrollChildFrameText:SetText(body)
    if keywords ~= "" then
        KnowledgeBaseArticleScrollChildFrameArticleId:SetText("|cff808080" .. L["KEYWORDS"] .. ": " .. keywords .. "|r")
    else
        KnowledgeBaseArticleScrollChildFrameArticleId:SetText("")
    end

    -- El hijo del scroll tiene tamaño fijo en el XML: sin esto, un artículo
    -- largo no haría scroll.
    local h = KnowledgeBaseArticleScrollChildFrameTitle:GetHeight()
            + KnowledgeBaseArticleScrollChildFrameText:GetHeight()
            + KnowledgeBaseArticleScrollChildFrameArticleId:GetHeight() + 40
    KnowledgeBaseArticleScrollChildFrame:SetHeight(math.max(334, h))
    KnowledgeBaseArticleScrollFrame:UpdateScrollChildRect()
    KnowledgeBaseArticleScrollFrameScrollBar:SetValue(0)
    KnowledgeBaseFrame_ShowArticleFrame()
end

local function RequestDetail(e)
    local cmd
    if e.kind == "A" then
        cmd = "ayuda articulo " .. e.key
    else
        cmd = "ayuda comando " .. e.key
    end
    KnowledgeBaseFrame_DisableButtons()
    SH.detailReq = Send(cmd, function(ok, lines)
        SH.detailReq = nil
        KBASE_ENABLE_SEARCH = 1
        if ok == nil then
            ShowError(L["NO_SERVER"])
        elseif not ok then
            ShowError(lines[1] and string.gsub(lines[1], "^!%s*", "") or L["NOT_FOUND"])
        else
            ShowDetail(e, lines)
        end
        KnowledgeBaseFrame_EnableButtons(nil, nil)
    end)
end

-- ─────────────────────────────────────────────────────────────────────────────
-- Sustitución de la lógica del KnowledgeBaseFrame
-- ─────────────────────────────────────────────────────────────────────────────
local origOnEvent

local function KB_OnShow(self)
    GetGMStatus()
    GetGMTicket()
    KnowledgeBaseFrameEditBox:SetFocus()
    HelpFrame.back = KnowledgeBaseFrameCancel
    UpdateStatusLine()
    if SH.index then
        AfterIndex()
        -- Comprueba en segundo plano si el índice cambió (nivel de la cuenta, recarga).
        Send("ayuda version", function(ok, lines)
            if ok and lines[1] then
                local v = Fields(lines[1])
                if v[1] == "V" and v[2] ~= SH.index.hash and not SH.loading then
                    SH.loading = true
                    RequestIndex(v[2], v[3], tonumber(v[5]) or 0)
                end
            end
        end)
    else
        SH:EnsureIndex(false)
    end
end

local function KB_OnEvent(self, event, ...)
    -- Del manejador original sólo interesa UPDATE_GM_STATUS (desactiva los
    -- botones de ticket cuando la cola de MJ está cerrada).
    if event == "UPDATE_GM_STATUS" and origOnEvent then
        origOnEvent(self, event, ...)
    end
end

function KnowledgeBaseFrame_Search(resetCurrentPage)
    if not SH.index then
        SH.pendingSearch = true
        SH:EnsureIndex(false)
        return
    end
    local text = KnowledgeBaseFrameEditBox:GetText()
    if Placeholder(text) then
        text = ""
    end
    text = string.gsub(text, "^%s+", "")
    text = string.gsub(text, "%s+$", "")
    if text == "" then
        ShowHome()
        return
    end
    if #text > 64 then
        text = string.sub(text, 1, 64)
    end
    ShowLoading()
    Send("ayuda buscar " .. text, function(ok, lines)
        if ok == nil then
            ShowError(L["NO_SERVER"])
            return
        end
        if not ok then
            ShowError(lines[1] and string.gsub(lines[1], "^!%s*", "") or L["ERR_GENERIC"])
            return
        end
        local found = {}
        for _, line in ipairs(lines) do
            local f = Fields(line)
            if f[1] == "R" then
                local e = SH.index.byKey[(f[2] or "") .. ":" .. string.lower(f[3] or "")]
                if e then
                    table.insert(found, e)
                end
            end
        end
        SH.results = FilterByCategory(found)
        SH.listTitle = L["RESULTS"] .. ": " .. text
        SH.page = 1
        ShowPage()
    end)
end

function KnowledgeBaseFrame_LoadTopIssues()
    SH.catSel, SH.subSel = 0, 0
    UIDropDownMenu_SetSelectedValue(KnowledgeBaseFrameCategoryDropDown, 0)
    UIDropDownMenu_SetText(KnowledgeBaseFrameCategoryDropDown, L["CATEGORY_DD"])
    UIDropDownMenu_SetSelectedValue(KnowledgeBaseFrameSubCategoryDropDown, 0)
    UIDropDownMenu_SetText(KnowledgeBaseFrameSubCategoryDropDown, L["SUBCATEGORY_DD"])
    UpdateSubCategoryEnabledState()
    KnowledgeBaseFrameEditBox:SetText(KBASE_DEFAULT_SEARCH_TEXT)
    if SH.index then
        ShowHome()
    else
        SH:EnsureIndex(false)
    end
end

function KnowledgeBaseArticleListFrame_NextPage()
    SH.page = SH.page + 1
    ShowPage()
end

function KnowledgeBaseArticleListFrame_PreviousPage()
    if SH.page > 1 then
        SH.page = SH.page - 1
        ShowPage()
    end
end

function KnowledgeBaseArticleListItem_OnClick(self)
    PlaySound("igMainMenuOptionCheckBoxOn")
    if self.entry then
        RequestDetail(self.entry)
    end
end

function KnowledgeBaseArticleListItem_OnUpdate(self, elapsed)
    if not self.tooltipDelay then
        return
    end
    self.tooltipDelay = self.tooltipDelay - elapsed
    if self.tooltipDelay > 0 then
        return
    end
    self.tooltipDelay = nil
    local e = self.entry
    if not e then
        return
    end
    GameTooltip:SetOwner(self, "ANCHOR_RIGHT", 15)
    if e.kind == "C" then
        GameTooltip:SetText("." .. e.key, HIGHLIGHT_FONT_COLOR.r, HIGHLIGHT_FONT_COLOR.g, HIGHLIGHT_FONT_COLOR.b, 1)
        if e.title ~= "" then
            GameTooltip:AddLine(e.title, NORMAL_FONT_COLOR.r, NORMAL_FONT_COLOR.g, NORMAL_FONT_COLOR.b, 1)
        end
        GameTooltip:AddLine(L["PERMISSION"] .. ": " .. LevelName(e.level), 0.6, 0.6, 0.6)
        GameTooltip:AddLine(L["TIP_COMMAND"], 0.6, 0.6, 0.6, 1)
    else
        GameTooltip:SetText(e.title, HIGHLIGHT_FONT_COLOR.r, HIGHLIGHT_FONT_COLOR.g, HIGHLIGHT_FONT_COLOR.b, 1)
        if e.hot then
            GameTooltip:AddLine(L["TIP_HOT"])
            GameTooltip:AddTexture("Interface\\HelpFrame\\HotIssueIcon")
        end
        GameTooltip:AddLine(L["TIP_ARTICLE"], 0.6, 0.6, 0.6, 1)
    end
    GameTooltip:SetMinimumWidth(220, 1)
    GameTooltip:Show()
end

-- Desplegables de categoría y subcategoría (value = id de categoría, 0 = todas).
local function AddCategoryButton(dropdown, value, text, func)
    local info = UIDropDownMenu_CreateInfo()
    info.value = value
    info.text = text
    info.func = func
    info.checked = (UIDropDownMenu_GetSelectedValue(dropdown) == value)
    UIDropDownMenu_AddButton(info)
end

function KnowledgeBaseFrameCategoryButton_OnClick(self)
    local value = self.value or 0
    if value == SH.catSel then
        return
    end
    SH.catSel, SH.subSel = value, 0
    UIDropDownMenu_SetSelectedValue(KnowledgeBaseFrameCategoryDropDown, value)
    UIDropDownMenu_SetText(KnowledgeBaseFrameCategoryDropDown, value == 0 and L["CATEGORY_DD"] or (SH.index.cats[value] and SH.index.cats[value].name or ""))
    UIDropDownMenu_SetSelectedValue(KnowledgeBaseFrameSubCategoryDropDown, 0)
    UIDropDownMenu_SetText(KnowledgeBaseFrameSubCategoryDropDown, L["SUBCATEGORY_DD"])
    UpdateSubCategoryEnabledState()
    KnowledgeBaseFrame_Search(1)
end

function KnowledgeBaseFrameSubCategoryButton_OnClick(self)
    local value = self.value or 0
    SH.subSel = value
    UIDropDownMenu_SetSelectedValue(KnowledgeBaseFrameSubCategoryDropDown, value)
    UIDropDownMenu_SetText(KnowledgeBaseFrameSubCategoryDropDown, value == 0 and L["SUBCATEGORY_DD"] or (SH.index.cats[value] and SH.index.cats[value].name or ""))
    KnowledgeBaseFrame_Search(1)
end

function KnowledgeBaseFrameCategoryDropDown_Initialize()
    AddCategoryButton(KnowledgeBaseFrameCategoryDropDown, 0, L["ALL"], KnowledgeBaseFrameCategoryButton_OnClick)
    if not SH.index then
        return
    end
    for _, cat in ipairs(SH.index.roots) do
        AddCategoryButton(KnowledgeBaseFrameCategoryDropDown, cat.id, cat.name, KnowledgeBaseFrameCategoryButton_OnClick)
    end
end

function KnowledgeBaseFrameSubCategoryDropDown_Initialize()
    if not SH.index or SH.catSel == 0 then
        return
    end
    AddCategoryButton(KnowledgeBaseFrameSubCategoryDropDown, 0, L["ALL"], KnowledgeBaseFrameSubCategoryButton_OnClick)
    for _, cat in ipairs(SH.index.children[SH.catSel] or {}) do
        AddCategoryButton(KnowledgeBaseFrameSubCategoryDropDown, cat.id, cat.name, KnowledgeBaseFrameSubCategoryButton_OnClick)
    end
end

function UpdateSubCategoryEnabledState()
    if SH.index and SH.catSel ~= 0 and SH.index.children[SH.catSel] and #SH.index.children[SH.catSel] > 0 then
        UIDropDownMenu_EnableDropDown(KnowledgeBaseFrameSubCategoryDropDown)
    else
        UIDropDownMenu_DisableDropDown(KnowledgeBaseFrameSubCategoryDropDown)
    end
end

-- ─────────────────────────────────────────────────────────────────────────────
-- Arranque
-- ─────────────────────────────────────────────────────────────────────────────
local frame = CreateFrame("Frame", "ServerHelpEventFrame")
frame:RegisterEvent("ADDON_LOADED")
frame:RegisterEvent("CHAT_MSG_ADDON")
frame:SetScript("OnEvent", function(self, event, ...)
    if event == "CHAT_MSG_ADDON" then
        OnAddonMessage(...)
    elseif event == "ADDON_LOADED" then
        local name = ...
        if name ~= "ServerHelp" then
            return
        end
        self:UnregisterEvent("ADDON_LOADED")
        if not KnowledgeBaseFrame then
            return
        end
        -- Que el cliente no dispare la petición HTTP a Blizzard ni sus eventos.
        for _, ev in ipairs({
            "KNOWLEDGE_BASE_SETUP_LOAD_SUCCESS", "KNOWLEDGE_BASE_SETUP_LOAD_FAILURE",
            "KNOWLEDGE_BASE_QUERY_LOAD_SUCCESS", "KNOWLEDGE_BASE_QUERY_LOAD_FAILURE",
            "KNOWLEDGE_BASE_ARTICLE_LOAD_SUCCESS", "KNOWLEDGE_BASE_ARTICLE_LOAD_FAILURE",
            "KNOWLEDGE_BASE_SYSTEM_MOTD_UPDATE", "KNOWLEDGE_BASE_SERVER_MESSAGE" }) do
            KnowledgeBaseFrame:UnregisterEvent(ev)
        end
        KBASE_SETUP_LOADED = 1
        origOnEvent = KnowledgeBaseFrame:GetScript("OnEvent")
        -- OnShow/OnEvent van con function="..." en el XML: hay que sustituirlos
        -- con SetScript, redefinir la global no basta.
        KnowledgeBaseFrame:SetScript("OnShow", KB_OnShow)
        KnowledgeBaseFrame:SetScript("OnEvent", KB_OnEvent)
        KnowledgeBaseFrameTopIssuesButton:SetText(L["HOME"])
        KnowledgeBaseFrameCategoryDropDown_OnLoad(KnowledgeBaseFrameCategoryDropDown)
        UIDropDownMenu_SetText(KnowledgeBaseFrameCategoryDropDown, L["CATEGORY_DD"])
        UIDropDownMenu_SetText(KnowledgeBaseFrameSubCategoryDropDown, L["SUBCATEGORY_DD"])
    end
end)

-- Tiempos de espera y reintentos.
local elapsedAcc = 0
frame:SetScript("OnUpdate", function(self, elapsed)
    elapsedAcc = elapsedAcc + elapsed
    if elapsedAcc < 0.5 then
        return
    end
    elapsedAcc = 0
    CheckTimeouts()
    if SH.retryAt and GetTime() >= SH.retryAt then
        SH.retryAt = nil
        if KnowledgeBaseFrame:IsShown() then
            SH:EnsureIndex(false)
        end
    end
end)

-- Comando de barra para abrir directamente la ayuda del servidor.
SLASH_SERVERHELP1 = "/ayudaservidor"
SLASH_SERVERHELP2 = "/serverhelp"
SlashCmdList["SERVERHELP"] = function()
    if not HelpFrame:IsShown() then
        ToggleHelpFrame()
    end
    HelpFrame_ShowFrame("KBase")
end

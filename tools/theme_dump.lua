-- theme_dump.lua - run one of the game's theme scripts against a recording
-- stand-in for the game engine and print what it set up as JSON.
--
--   texlua theme_dump.lua <game_defaults.cfg> <theme.cfg> <difficulty 0-4>
--
-- Nothing is drawn here.  Every shader, sprite, lane ("column") and animator
-- the script creates is logged, then the script's own event handlers are
-- called (streak of 10, each multiplier, a miss) and the changes they make
-- are logged too.  Timers the script sets (setTimeout) are run on a virtual
-- clock for TIMELINE seconds, so effects it spawns during a song - sparkles,
-- smoke, timed fades - are logged with the time they happen.  prep_assets.py turns that log into a theme file.
-- Works with any Lua 5.3/5.4 interpreter (lua, texlua).

local defaults_path, theme_path, level = arg[1], arg[2], tonumber(arg[3] or "2")

-- Lua 5.1 names the scripts expect
unpack = unpack or table.unpack
loadstring = loadstring or load
math.pow = math.pow or function(a, b) return a ^ b end
math.mod = math.mod or math.fmod
table.getn = table.getn or function(t) return #t end
string.gfind = string.gfind or string.gmatch
if not math.atan2 then math.atan2 = function(y, x) return math.atan(y, x) end end

local TIMELINE, MAX_LOG = 420, 60000        -- seconds of song to simulate; safety cap on log size
local now, timers, timer_seq = 0, {}, 0
local log, next_id, objs = {}, 0, {}
local function emit(e) log[#log + 1] = e end

-- ---------------------------------------------------------------- JSON --
local function is_obj(v) return type(v) == "table" and rawget(v, "__id") ~= nil end
local function json(v, depth)
  depth = depth or 0
  local t = type(v)
  if t == "nil" then return "null" end
  if t == "boolean" then return v and "true" or "false" end
  if t == "number" then
    if v ~= v or v == math.huge or v == -math.huge then return "0" end
    if math.type and math.type(v) == "integer" then return string.format("%d", v) end
    return string.format("%.6g", v)
  end
  if t == "string" then
    return '"' .. v:gsub('[%c"\\]', function(c) return string.format("\\u%04x", c:byte()) end) .. '"'
  end
  if t == "function" or t == "userdata" or t == "thread" then return '"<fn>"' end
  if is_obj(v) then return '{"$obj":' .. rawget(v, "__id") .. '}' end
  if depth > 8 then return "null" end
  local n = #v
  local only_array = n > 0
  if only_array then for k in pairs(v) do if type(k) ~= "number" then only_array = false break end end end
  local parts = {}
  if only_array then
    for i = 1, n do parts[#parts + 1] = json(v[i], depth + 1) end
    return "[" .. table.concat(parts, ",") .. "]"
  end
  local keys = {}
  for k in pairs(v) do keys[#keys + 1] = k end
  table.sort(keys, function(a, b) return tostring(a) < tostring(b) end)
  for _, k in ipairs(keys) do parts[#parts + 1] = json(tostring(k), 0) .. ":" .. json(v[k], depth + 1) end
  return "{" .. table.concat(parts, ",") .. "}"
end

-- -------------------------------------------------------------- objects --
-- A recorded object: reads return what was written, writes and method calls
-- go to the log.  Unknown members become recorded objects themselves, so
-- both `game.audioPlayer.volume` and `game:anything{...}` keep working.
local obj_mt = {}
local function new_obj(kind, fields)
  next_id = next_id + 1
  local o = setmetatable({ __id = next_id, __kind = kind, __f = fields or {} }, obj_mt)
  objs[next_id] = o
  return o
end
obj_mt.__index = function(o, k)
  local f = rawget(o, "__f")
  if f[k] ~= nil then return f[k] end
  local m = new_obj("member")
  rawset(m, "__parent", o)
  rawset(m, "__name", k)
  return m
end
obj_mt.__call = function(m, self, ...)
  local o, k = rawget(m, "__parent"), rawget(m, "__name")
  if not o then return 0 end
  local args = { ... }
  if self ~= o then table.insert(args, 1, self) end
  emit { op = "call", obj = rawget(o, "__id"), kind = rawget(o, "__kind"), method = k, args = args }
  if type(k) == "string" and (k:match("^add") or k:match("^first") or k:match("Named$")) then
    local settings = type(args[1]) == "table" and not is_obj(args[1]) and args[1] or {}
    local child = new_obj(k, settings)
    if k:match("Animator") then
      settings.startTime = settings.startTime or 0
      settings.targetTime = settings.targetTime or settings.duration or 0
    end
    emit { op = "new", id = rawget(child, "__id"), kind = k, parent = rawget(o, "__id"), settings = settings }
    return child
  end
  return 0
end
obj_mt.__newindex = function(o, k, v)
  rawget(o, "__f")[k] = v
  emit { op = "set", obj = rawget(o, "__id"), kind = rawget(o, "__kind"), key = k, value = v }
end
local function num(v) return type(v) == "number" and v or 0 end
obj_mt.__add = function(a, b) return num(a) + num(b) end
obj_mt.__sub = function(a, b) return num(a) - num(b) end
obj_mt.__mul = function(a, b) return num(a) * num(b) end
obj_mt.__div = function(a, b) return num(a) / (num(b) ~= 0 and num(b) or 1) end
obj_mt.__unm = function() return 0 end
obj_mt.__lt = function(a, b) return num(a) < num(b) end
obj_mt.__le = function(a, b) return num(a) <= num(b) end
obj_mt.__concat = function(a, b) return (is_obj(a) and "" or tostring(a)) .. (is_obj(b) and "" or tostring(b)) end

-- shaders
local shaders = {}
TTRShader = {}
function TTRShader:shaderNamed(name) return shaders[name] end
function TTRShader:shaderWithSettings(t)
  local s = new_obj("shader", t)
  if t and t.name then shaders[t.name] = s end
  emit { op = "shader", id = rawget(s, "__id"), settings = t }
  return s
end

-- the game
local track = new_obj("track", { selectedDifficultyLevel = level, title = "Title", artist = "Artist" })
rawget(track, "__f").isBossTrackForDifficultyLevel = function() return 0 end
local player = new_obj("player", { playerNumber = 0 })
game = new_obj("game", {
  numberOfPlayers = 1, challengeMode = 0, isOnlineGame = 0, multiplayerMode = 0,
  isOnSlowDevice = false, track = track, useDefaultFeedback = true, lastFrameTime = 0,
})
local gf = rawget(game, "__f")
gf.weaponsAreInUse = function() return 0 end
gf.checkShowLuaTutorial = function() return 0 end
gf.firstPlayer = function() return player end
gf.playerAtIndex = function() return player end
gf.addSprite = function(self, t)
  local s = new_obj("sprite", t)
  emit { op = "sprite", id = rawget(s, "__id"), settings = t }
  return s
end
gf.addColumn = function(self, t)
  local c = new_obj("column", t)
  emit { op = "column", id = rawget(c, "__id"), settings = t }
  return c
end
-- timers: queued and run later on the virtual clock
local function add_timer(t, fname, params)
  timer_seq = timer_seq + 1
  timers[#timers + 1] = { t = t, seq = timer_seq, fn = fname, params = params }
end
TTRLuaCallGameEntity = { alloc = function() return new_obj("timer", {}) end }
gf.addChild = function(self, child)
  if is_obj(child) and rawget(child, "__kind") == "timer" then
    local f = rawget(child, "__f")
    if type(f.functionName) == "string" then add_timer(tonumber(f.fireTime) or now, f.functionName, f.params) end
  end
  return 0
end
-- run every timer due by time `limit`, oldest first
run_timers = function(limit, stamp)
  local guard = 0
  while #log < MAX_LOG and guard < 200000 do
    guard = guard + 1
    local best
    for i, tm in ipairs(timers) do
      if tm.t <= limit and (not best or tm.t < timers[best].t or (tm.t == timers[best].t and tm.seq < timers[best].seq)) then best = i end
    end
    if not best then break end
    local tm = table.remove(timers, best)
    now = math.max(now, tm.t)
    gf.lastFrameTime = now
    if stamp then emit { op = "time", t = now } end
    local fn = _G[tm.fn]
    if type(fn) == "function" then pcall(fn, tm.params) end
  end
end

gf.spriteNamed = function(self, name)
  for _, o in pairs(objs) do
    if rawget(o, "__kind") == "sprite" and rawget(o, "__f").name == name then return o end
  end
  return new_obj("sprite", { name = name })
end

-- unknown capitalised globals are engine constants (blend modes and so on):
-- hand back their own name so they survive into the log
setmetatable(_G, { __index = function(_, k)
  if type(k) == "string" and (k:match("Blend$") or k:match("Property$") or k:match("Transform$") or k:match("^Ease")) then
    return k
  end
  return nil
end })

local function run_file(path)
  local f = assert(io.open(path, "rb"))
  local src = f:read("*a")
  f:close()
  local chunk, err = loadstring(src, "@" .. path)
  if not chunk then error(err, 0) end
  chunk()
end

local function phase(name, fn)
  emit { op = "phase", name = name }
  local ok, err = pcall(fn)
  if not ok then emit { op = "error", where = name, message = tostring(err) } end
end
local function call(name, ...)
  local fn = _G[name]
  if type(fn) == "function" then fn(...) end
end

phase("init", function()
  run_file(defaults_path)
  setTimeout = function(delay, fname, params) add_timer(now + (tonumber(delay) or 0), fname, params) end
  run_file(theme_path)
  initialize()
end)
phase("init", function() run_timers(0, false) end)      -- zero-delay timers belong to start-up
-- everything the script schedules for later, in time order
phase("timeline", function() run_timers(TIMELINE, true) end)
timers = {}
now = 0
gf.lastFrameTime = 0
-- which way up the theme wants the screen: 320x480, or 480x320 if it called hud_useLandscape()
emit { op = "screen", w = tonumber(rawget(_G, "screenWidth")) or 320, h = tonumber(rawget(_G, "screenHeight")) or 480,
       landscape = rawget(_G, "landscapeMode") and true or false }
-- the theme's own handlers (the stock HUD handlers need engine state we don't model)
-- A handler may start an effect right away or schedule one for later, so
-- after calling it the clock runs for EVENT_WINDOW seconds from zero and
-- everything in that window is logged with its time since the event.
local EVENT_WINDOW = 45
local function event_phase(name, fn)
  timers = {}
  now = 0
  gf.lastFrameTime = 0
  phase(name, fn)
  phase(name, function() run_timers(EVENT_WINDOW, true) end)
  timers = {}
end
event_phase("streak10", function() call("theme_eventPlayerReachedStreakOf10", player) end)
for _, m in ipairs { 2, 3, 4, 8 } do
  event_phase("mult" .. m, function() call("theme_eventPlayerReachedScoreMultiplier", player, m) end)
end
event_phase("miss", function()
  call("theme_eventPlayerMissedTap", player)
  call("theme_eventPlayerReachedScoreMultiplier", player, 1)
end)

local lines = {}
for i, e in ipairs(log) do lines[i] = json(e) end
io.write("[", table.concat(lines, ",\n"), "]\n")

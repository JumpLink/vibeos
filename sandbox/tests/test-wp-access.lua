-- Runs data/sandbox-access.lua against a stand-in for WirePlumber's Lua API and checks what each kind of client
-- sees. Usage: lua5.4 test-wp-access.lua path/to/sandbox-access.lua

local script = arg[1]
local failures = 0
local function check (what, got, want)
  if got ~= want then
    print (string.format ("FAIL %s: got %s, want %s", what, tostring (got), tostring (want)))
    failures = failures + 1
  end
end

-- ---- the stand-in ----
Perm = { NONE = 0, R = 0x100, W = 0x80, X = 0x40, M = 0x08, L = 0x10 }
Perm.RX = Perm.R | Perm.X
Perm.ALL = Perm.R | Perm.W | Perm.X | Perm.M
Features = { ALL = 0xffff }
Log = { open_topic = function ()
  return { info = function () end, notice = function () end, warning = function () end, debug = function () end }
end }

local store = {
  ["vibeos-sandbox/microphone"] = { ["org.example.Recorder"] = { "yes" }, ["org.example.Game"] = { "no" } },
  ["devices/camera"] = { ["org.example.Scanner"] = { "yes" } },
}
local pps_handlers = {}
local pps = {
  call = function (_, method, tbl, id) assert (method == "lookup"); return store[tbl .. "/" .. id] end,
  connect = function (_, signal, f) pps_handlers[signal] = f end,
}
-- WirePlumber's device reservation, where the question for the microphone goes out as a bus name; its timers
local reservations, reservation_log = {}, {}
local reserve_state = "connected"
local reserve = {
  call = function (_, method, name, ...)
    if method == "get-dbus" then
      return { state = reserve_state }
    elseif method == "create-reservation" then
      local rd = { name = name, app_dev_name = select (2, ...), acquired = false }
      rd.call = function (self, m) if m == "acquire" then self.acquired = true end end
      reservations[name] = rd
      table.insert (reservation_log, name)
      return rd
    elseif method == "destroy-reservation" then
      reservations[name] = nil
    end
  end,
}
local timers = {}
Core = { timeout_add = function (ms, f)
  local t = { ms = ms, f = f }
  t.destroy = function (self) self.dead = true end
  table.insert (timers, t)
  return t
end }
local rescans = 0
local event_source = { call = function (_, m) if m == "schedule-rescan" then rescans = rescans + 1 end end }
Plugin = { find = function (name)
  if name == "portal-permissionstore" then return pps end
  if name == "reserve-device" then return reserve end
  if name == "standard-event-source" then return event_source end
end }

local pms = {}
function PermissionManager ()
  local pm = { matches = {}, simple = {}, updates = 0 }
  function pm:set_default_permissions (p) self.default = p end
  function pm:add_interest_match (f, interest) table.insert (self.matches, { f = f, interest = interest }) end
  function pm:add_interest_match_simple (p, interest) table.insert (self.simple, { p = p, interest = interest }) end
  function pm:update_permissions () self.updates = self.updates + 1 end
  function pm:activate () end
  table.insert (pms, pm)
  return pm
end
function Interest (t) return t end
function Constraint (t) return t end
function EventInterest (t) return t end
local clients = {}
function ObjectManager ()
  return {
    activate = function () end,
    lookup = function (_, q) return clients[q[1][3]] end,
    -- the privacy part's links: none here
    iterate = function () return function () end end,
    connect = function () end,
  }
end
local hooks = {}
function SimpleEventHook (t)
  return { register = function () hooks[t.name] = t end }
end
package.preload["common-utils"] = function ()
  return { get_client_access = function (props)
    if props["pipewire.sec.flatpak"] == "true" or props["pipewire.client.access"] == "flatpak" then return "flatpak" end
    return props["pipewire.access"]
  end }
end

dofile (script)

-- ---- the checks ----
local pm = pms[1]
local function client (id, app_id, access)
  local props = { ["pipewire.access.portal.app_id"] = app_id }
  if access == "flatpak" then
    props["pipewire.sec.flatpak"] = "true"
  elseif access == "pulse" then
    props["pipewire.client.access"] = "flatpak"
    props["pipewire.access"] = "unrestricted"
  else
    props["pipewire.access"] = access
  end
  local c = { properties = props, ["bound-id"] = id, errors = {} }
  function c:send_error (node_id, code, message) table.insert (self.errors, { node_id, code, message }) end
  clients[id] = c
  return c
end
local next_node = 100
local function node (props)
  next_node = next_node + 1
  return { properties = props, ["bound-id"] = next_node }
end
local function sees (c, object, kind)
  for _, m in ipairs (pm.matches) do
    if m.interest.type == kind then
      return m.f (pm, c, object)
    end
  end
end

local mic = node { ["media.class"] = "Audio/Source", ["client.id"] = "30" }
local duplex = node { ["media.class"] = "Audio/Duplex", ["client.id"] = "30" }
local speaker = node { ["media.class"] = "Audio/Sink", ["client.id"] = "30" }
local camera = node { ["media.class"] = "Video/Source", ["media.role"] = "Camera", ["client.id"] = "30" }
local screen = node { ["media.class"] = "Video/Source", ["client.id"] = "31" }
local call_stream = node { ["media.class"] = "Stream/Output/Audio", ["client.id"] = "40" }

local game = client (50, "org.example.Game", "flatpak")
local recorder = client (51, "org.example.Recorder", "pulse")
local scanner = client (52, "org.example.Scanner", "portal")
local host = client (53, nil, "unrestricted")
local own_stream = node { ["media.class"] = "Stream/Input/Audio", ["client.id"] = "50" }

check ("game: microphone", sees (game, mic, "node"), Perm.NONE)
check ("game: duplex", sees (game, duplex, "node"), Perm.NONE)
check ("game: speaker", sees (game, speaker, "node"), Perm.RX)
check ("game: camera", sees (game, camera, "node"), Perm.NONE)
check ("game: screen cast", sees (game, screen, "node"), Perm.RX)
check ("game: a call's stream", sees (game, call_stream, "node"), Perm.NONE)
check ("game: its own stream", sees (game, own_stream, "node"), Perm.RX)
local game2 = client (58, "org.example.Game", "flatpak")
check ("game, another connection: its stream", sees (game2, own_stream, "node"), Perm.RX)
check ("recorder: the game's stream", sees (recorder, own_stream, "node"), Perm.NONE)
-- ports and links go with their nodes (seen first, as PipeWire adds them)
local function port (n) return { properties = { ["node.id"] = tostring (n["bound-id"]) } } end
local function link (a, b)
  return { properties = { ["link.output.node"] = tostring (a["bound-id"]), ["link.input.node"] = tostring (b["bound-id"]) } }
end
check ("game: the microphone's port", sees (game, port (mic), "port"), Perm.NONE)
check ("game: the speaker's port", sees (game, port (speaker), "port"), Perm.RX)
check ("game: its own stream's port", sees (game2, port (own_stream), "port"), Perm.RX)
check ("game: a call's stream's port", sees (game, port (call_stream), "port"), Perm.NONE)
check ("game: a call's link", sees (game, link (call_stream, speaker), "link"), Perm.NONE)
check ("game: its own link", sees (game, link (own_stream, speaker), "link"), Perm.RX)
check ("game: a port of a node not seen", sees (game, { properties = { ["node.id"] = "999" } }, "port"), Perm.RX)
check ("host app: the microphone's port", sees (host, port (mic), "port"), Perm.RX)
check ("host app: a call's link", sees (host, link (call_stream, speaker), "link"), Perm.RX)
check ("game: another client", sees (game, host, "client"), Perm.NONE)
check ("game: itself", sees (game, game, "client"), Perm.RX)
check ("recorder (pulse): microphone", sees (recorder, mic, "node"), Perm.RX)
check ("recorder: camera", sees (recorder, camera, "node"), Perm.NONE)
check ("scanner (portal): camera", sees (scanner, camera, "node"), Perm.RX)
check ("scanner: microphone", sees (scanner, mic, "node"), Perm.NONE)
check ("host app: microphone", sees (host, mic, "node"), Perm.RX)
local function factory (name) return { properties = { ["factory.name"] = name } } end
check ("game: link factory", sees (game, factory ("link-factory"), "factory"), Perm.NONE)
check ("game: node factory", sees (game, factory ("spa-node-factory"), "factory"), Perm.NONE)
check ("game: adapter factory", sees (game, factory ("adapter"), "factory"), Perm.NONE)
check ("game: stream factory", sees (game, factory ("client-node"), "factory"), Perm.RX)
check ("host app: link factory", sees (host, factory ("link-factory"), "factory"), Perm.RX)

-- the access hook takes sandboxed clients only, and leaves one already decided alone
local function select_access (c, preset)
  local data = { ["permission-manager"] = preset }
  local event = { get_subject = function () return c end,
                  get_data = function (_, k) return data[k] end,
                  set_data = function (_, k, v) data[k] = v end }
  hooks["client/find-vibeos-sandbox-access"].execute (event)
  return data["permission-manager"]
end
check ("access: game", select_access (game), pm)
check ("access: pulse client", select_access (recorder), pm)
check ("access: host app", select_access (host), nil)
check ("access: decided before", select_access (game, "config"), "config")
local xdp = client (54, "org.example.Scanner", "portal")
xdp.properties["pipewire.access.portal.is_portal"] = "yes"
check ("access: the portal itself", select_access (xdp), nil)
local portal = client (55, "", "portal")
check ("access: portal with no app", select_access (portal), nil)

-- capture streams of apps without the microphone are not linked
local function linked (si)
  local stopped = false
  local event = { get_subject = function () return si end, stop_processing = function () stopped = true end }
  hooks["linking/vibeos-sandbox-capture"].execute (event)
  return not stopped
end
check ("link: game's capture", linked (node { ["media.class"] = "Stream/Input/Audio", ["client.id"] = "50" }), false)
check ("link: game's playback", linked (node { ["media.class"] = "Stream/Output/Audio", ["client.id"] = "50" }), true)
check ("link: recorder's capture", linked (node { ["media.class"] = "Stream/Input/Audio", ["client.id"] = "51" }), true)
check ("link: host's capture", linked (node { ["media.class"] = "Stream/Input/Audio", ["client.id"] = "53" }), true)
check ("link: broken item", linked ({}), true)
check ("link: game's video capture", linked (node { ["media.class"] = "Stream/Input/Video", ["client.id"] = "50" }), false)
check ("link: scanner's video (portal)", linked (node { ["media.class"] = "Stream/Input/Video", ["client.id"] = "52" }), true)
local cammer = client (56, "org.example.Scanner", "flatpak")
check ("link: video with the camera", linked (node { ["media.class"] = "Stream/Input/Video", ["client.id"] = "56" }), true)
local caster = client (57, "org.example.Game", "portal")
check ("link: game's screen cast (portal)", linked (node { ["media.class"] = "Stream/Input/Video", ["client.id"] = "57" }), true)
check ("link: host's video", linked (node { ["media.class"] = "Stream/Input/Video", ["client.id"] = "53" }), true)

-- nodes a sandboxed app makes: streams only
local function added (n)
  local stopped, destroyed = false, false
  n.request_destroy = function () destroyed = true end
  local event = { get_subject = function () return n end, stop_processing = function () stopped = true end }
  hooks["node/vibeos-sandbox-streams-only"].execute (event)
  return destroyed and stopped and "destroyed" or (destroyed or stopped) and "half" or "kept"
end
check ("node: game's sink", added (node { ["media.class"] = "Audio/Sink", ["client.id"] = "50" }), "destroyed")
check ("node: game's source", added (node { ["media.class"] = "Audio/Source/Virtual", ["client.id"] = "50" }), "destroyed")
check ("node: game's camera", added (node { ["media.class"] = "Video/Source", ["client.id"] = "50" }), "destroyed")
check ("node: game's stream", added (node { ["media.class"] = "Stream/Output/Audio", ["client.id"] = "50" }), "kept")
check ("node: pulse app's sink", added (node { ["media.class"] = "Audio/Sink", ["client.id"] = "51" }), "destroyed")
check ("node: host's sink", added (node { ["media.class"] = "Audio/Sink", ["client.id"] = "53" }), "kept")
check ("node: device sink", added (node { ["media.class"] = "Audio/Sink" }), "kept")
check ("node: no class", added (node { ["client.id"] = "50" }), "kept")
local function target_after (t, subject)
  local data = { target = t }
  local event = { get_subject = function () return subject or node {} end,
                  get_data = function (_, k) return data[k] end,
                  set_data = function (_, k, v) data[k] = v end }
  hooks["linking/vibeos-sandbox-target"].execute (event)
  return data.target == nil and "none" or "kept"
end
check ("target: game's sink", target_after (node { ["media.class"] = "Audio/Sink", ["client.id"] = "50" }), "none")
check ("target: device sink", target_after (speaker), "kept")
check ("target: host's sink", target_after (node { ["media.class"] = "Audio/Sink", ["client.id"] = "53" }), "kept")
check ("target: none found", target_after (nil), "none")
-- a capture stream of a sandboxed app records a microphone or a camera, with the permission too; never what plays
local rec_capture = node { ["media.class"] = "Stream/Input/Audio", ["client.id"] = "51" }
check ("target: recorder's capture, the microphone", target_after (mic, rec_capture), "kept")
check ("target: recorder's capture, a duplex device", target_after (duplex, rec_capture), "kept")
check ("target: recorder's capture, a sink's monitor", target_after (speaker, rec_capture), "none")
check ("target: recorder's capture, a call's stream", target_after (call_stream, rec_capture), "none")
check ("target: recorder's capture, no class", target_after (node { ["client.id"] = "30" }, rec_capture), "none")
local host_capture = node { ["media.class"] = "Stream/Input/Audio", ["client.id"] = "53" }
check ("target: host's capture, a sink's monitor", target_after (speaker, host_capture), "kept")
check ("target: game's playback, the speaker",
       target_after (speaker, node { ["media.class"] = "Stream/Output/Audio", ["client.id"] = "50" }), "kept")
local cam_capture = node { ["media.class"] = "Stream/Input/Video", ["client.id"] = "56" }
check ("target: video with the camera, the camera", target_after (camera, cam_capture), "kept")
check ("target: video with the camera, a screen cast", target_after (screen, cam_capture), "none")
check ("target: screen cast portal's video, the screen",
       target_after (screen, node { ["media.class"] = "Stream/Input/Video", ["client.id"] = "57" }), "kept")
check ("target: host's video, a screen cast",
       target_after (screen, node { ["media.class"] = "Stream/Input/Video", ["client.id"] = "53" }), "kept")

-- granting it follows at once
pps_handlers["changed"] (pps, "vibeos-sandbox", "microphone", false,
                         { ["org.example.Game"] = { "yes" } })
check ("granted: game's microphone", sees (game, mic, "node"), Perm.RX)
check ("granted: permissions updated", pm.updates, 1)
check ("granted: rescan", rescans, 1)
check ("granted: game's capture linked", linked (node { ["media.class"] = "Stream/Input/Audio", ["client.id"] = "50" }), true)
pps_handlers["changed"] (pps, "devices", "other", false, {})
check ("other table: no update", pm.updates, 1)

-- a stream announced before its client is known is looked at again, not remembered as another client's
local early = node { ["media.class"] = "Stream/Input/Audio", ["client.id"] = "90" }
local late_client = clients[90]
check ("early stream: its client is not known yet", late_client, nil)
sees (client (91, "org.example.Other", "pulse"), early, "node")
local maker = client (90, "org.example.Early", "pulse")
check ("early stream: the app that made it sees it", sees (maker, early, "node"), Perm.RX)
check ("early stream: another app does not", sees (client (92, "org.example.Other2", "pulse"), early, "node"), Perm.NONE)
local early_port = port (early)
check ("early stream: its port", sees (maker, early_port, "port"), Perm.RX)

-- ---- the microphone asks ----
local function hex (app) return (app:gsub (".", function (c) return string.format ("%02x", c:byte ()) end)) end
local function ask_name (app) return "VibeOSMicAsk" .. hex (app) end
local function reservations_count ()
  local n = 0
  for _ in pairs (reservations) do n = n + 1 end
  return n
end
-- a capture stream that has a node proxy to be destroyed, as WirePlumber's session item has
local function stream (client_id, class)
  local si = node { ["media.class"] = class or "Stream/Input/Audio", ["client.id"] = tostring (client_id) }
  si.properties["object.serial"] = tostring (next_node * 10)
  si.proxy = { ["bound-id"] = si["bound-id"], destroyed = false }
  function si.proxy:request_destroy () self.destroyed = true end
  function si:get_associated_proxy () return self.proxy end
  return si
end
local function errors_of (c) return #c.errors end

-- a stored "no" fails the stream at once, nothing is asked; a stored "yes" links it at once
local game3 = client (70, "org.example.Quiet", "flatpak")
pps_handlers["changed"] (pps, "vibeos-sandbox", "microphone", false,
                         { ["org.example.Quiet"] = { "no" }, ["org.example.Game"] = { "yes" },
                           ["org.example.Recorder"] = { "yes" } })
local s = stream (70)
check ("stored no: not linked", linked (s), false)
check ("stored no: the app is told", errors_of (game3), 1)
check ("stored no: -EACCES for its node", game3.errors[1][1] == s["bound-id"] and game3.errors[1][2] == -13, true)
check ("stored no: the node is destroyed", s.proxy.destroyed, true)
check ("stored no: nothing asked", reservations_count (), 0)
local rec = client (71, "org.example.Recorder", "pulse")
s = stream (71)
check ("stored yes: linked", linked (s), true)
check ("stored yes: no error", errors_of (rec), 0)

-- no entry: the question goes out once, the stream waits unlinked, a second stream waits for the same answer
local voice = client (60, "org.example.Voice", "pulse")
local s1, s2 = stream (60), stream (60)
check ("ask: first stream waits", linked (s1), false)
check ("ask: the question is on the bus", reservations[ask_name ("org.example.Voice")] ~= nil, true)
check ("ask: acquired", reservations[ask_name ("org.example.Voice")].acquired, true)
check ("ask: the app id is the device name", reservations[ask_name ("org.example.Voice")].app_dev_name,
       "org.example.Voice")
check ("ask: second stream waits", linked (s2), false)
check ("ask: a rescan asks nothing again", linked (s1), false)
check ("ask: one question", #reservation_log, 1)
check ("ask: no error yet", errors_of (voice), 0)
check ("ask: nothing destroyed yet", s1.proxy.destroyed or s2.proxy.destroyed, false)
check ("ask: nothing remembered", store["vibeos-sandbox/microphone"]["org.example.Voice"], nil)
check ("ask: a timer", #timers, 1)
local timer_ms = timers[1].ms
check ("ask: the timer is the question's", timer_ms > 30000 and timer_ms <= 40000, true)
-- "yes": the question goes, the streams link on the rescan
local rescans_before = rescans
store["vibeos-sandbox/microphone"]["org.example.Voice"] = { "yes" }
pps_handlers["changed"] (pps, "vibeos-sandbox", "microphone", false, store["vibeos-sandbox/microphone"])
check ("ask yes: the question is withdrawn", reservations_count (), 0)
check ("ask yes: the timer is stopped", timers[1].dead, true)
check ("ask yes: a rescan", rescans > rescans_before, true)
check ("ask yes: linked", linked (s1), true)
check ("ask yes: no error", errors_of (voice), 0)
check ("ask yes: its microphone is seen", sees (voice, mic, "node"), Perm.RX)

-- "no": every held stream fails, and the app remembers
local nope = client (61, "org.example.Nope", "pulse")
local n1, n2 = stream (61), stream (61)
linked (n1); linked (n2)
check ("ask no: asked", reservations[ask_name ("org.example.Nope")] ~= nil, true)
store["vibeos-sandbox/microphone"]["org.example.Nope"] = { "no" }
pps_handlers["changed"] (pps, "vibeos-sandbox", "microphone", false, store["vibeos-sandbox/microphone"])
check ("ask no: the question is withdrawn", reservations_count (), 0)
check ("ask no: both streams are told", errors_of (nope), 2)
check ("ask no: -EACCES", nope.errors[1][2], -13)
check ("ask no: destroyed", n1.proxy.destroyed and n2.proxy.destroyed, true)
check ("ask no: a new stream fails at once", linked (stream (61)), false)
check ("ask no: ... and asks nothing", reservations_count (), 0)

-- dismissed (or the phone locked): the held stream fails, nothing remembered
local shy = client (62, "org.example.Shy", "pulse")
local d1 = stream (62)
linked (d1)
local timers_before = #timers
pps_handlers["changed"] (pps, "vibeos-sandbox-ask", "microphone", false, { ["org.example.Shy"] = { "dismissed" } })
-- (the stand-in's store has no such table: what lookup answers is what the handler was given)
store["vibeos-sandbox-ask/microphone"] = { ["org.example.Shy"] = { "dismissed" } }
pps_handlers["changed"] (pps, "vibeos-sandbox-ask", "microphone", false, store["vibeos-sandbox-ask/microphone"])
check ("dismissed: the stream is told", errors_of (shy), 1)
check ("dismissed: destroyed", d1.proxy.destroyed, true)
check ("dismissed: the question is withdrawn", reservations_count (), 0)
check ("dismissed: nothing remembered", store["vibeos-sandbox/microphone"]["org.example.Shy"], nil)
store["vibeos-sandbox-ask/microphone"] = nil

-- no answer: the timer ends it
local slow = client (63, "org.example.Slow", "pulse")
local t1 = stream (63)
linked (t1)
check ("timeout: asked", reservations[ask_name ("org.example.Slow")] ~= nil, true)
local timer = timers[#timers]
timer.f ()
check ("timeout: the stream is told", errors_of (slow), 1)
check ("timeout: -EACCES", slow.errors[1][2], -13)
check ("timeout: destroyed", t1.proxy.destroyed, true)
check ("timeout: the question is withdrawn", reservations_count (), 0)
check ("timeout: nothing remembered", store["vibeos-sandbox/microphone"]["org.example.Slow"], nil)
-- a later stream asks again
check ("timeout: the next stream waits", linked (stream (63)), false)
check ("timeout: ... and asks again", reservations[ask_name ("org.example.Slow")] ~= nil, true)

-- "ask" stored (Settings' Ask every time) asks too
local again = client (64, "org.example.Again", "pulse")
store["vibeos-sandbox/microphone"]["org.example.Again"] = { "ask" }
pps_handlers["changed"] (pps, "vibeos-sandbox", "microphone", false, store["vibeos-sandbox/microphone"])
check ("stored ask: waits", linked (stream (64)), false)
check ("stored ask: asked", reservations[ask_name ("org.example.Again")] ~= nil, true)
check ("stored ask: no error", errors_of (again), 0)

-- the Microphone switch off: refused at once, and a question open is ended
store["vibeos-privacy/microphone"] = { all = { "blocked" } }
pps_handlers["changed"] (pps, "vibeos-privacy", "microphone", false, store["vibeos-privacy/microphone"])
check ("switch off: the open question is ended", reservations_count (), 0)
check ("switch off: its stream is told", errors_of (again), 1)
local before = errors_of (rec)
check ("switch off: a granted app's capture is not linked", linked (stream (71)), false)
check ("switch off: it is told", errors_of (rec), before + 1)
check ("switch off: nothing asked", reservations_count (), 0)
store["vibeos-privacy/microphone"] = { all = { "allowed" } }
pps_handlers["changed"] (pps, "vibeos-privacy", "microphone", false, store["vibeos-privacy/microphone"])
check ("switch on: linked again", linked (stream (71)), true)
-- the host's own capture is not this script's business
local host_s = stream (53)
check ("host: linked", linked (host_s), true)

-- nothing to ask with: refused at once, no hang
reserve_state = "closed"
local mute = client (65, "org.example.Mute", "pulse")
check ("no bus: not linked", linked (stream (65)), false)
check ("no bus: told", errors_of (mute), 1)
check ("no bus: nothing asked", reservations_count (), 0)
reserve_state = "connected"

-- a camera refused fails too
local cam_s = stream (50, "Stream/Input/Video")
local before_cam = errors_of (game)
check ("video refused: not linked", linked (cam_s), false)
check ("video refused: told", errors_of (game), before_cam + 1)

if failures > 0 then
  os.exit (1)
end
print ("ok")

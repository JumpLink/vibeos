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
local rescans = 0
local event_source = { call = function (_, m) if m == "schedule-rescan" then rescans = rescans + 1 end end }
Plugin = { find = function (name)
  if name == "portal-permissionstore" then return pps end
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
  local c = { properties = props, ["bound-id"] = id }
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

if failures > 0 then
  os.exit (1)
end
print ("ok")

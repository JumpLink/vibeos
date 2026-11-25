-- VibeOS: what an app in vibeos-sandbox sees of PipeWire.
--
-- PipeWire marks a client from a sandbox (it has a /.flatpak-info) with pipewire.access = "flatpak" and its app id
-- (pipewire.access.portal.app_id); pipewire-pulse does the same for each PulseAudio client. WirePlumber's own
-- handling gives such a client read access to everything, the microphone included, and everything to one that calls
-- itself a "Manager". Here every such client gets, instead:
--   - the microphones (Audio/Source…, Audio/Duplex…) only when the app has the microphone permission
--     (permission store: "vibeos-sandbox"/"microphone" = "yes"), the cameras (Video/Source, Camera) only with the
--     camera permission ("devices"/"camera" = "yes");
--   - none of the other clients, nor their streams: an app does not see what else plays or records (a call, another
--     app's music); its own streams it sees from each of its connections. Ports and links go with their nodes;
--   - of the factories, only the one streams are made with (client-node): no links of its own (a sink's monitor to
--     its stream), no nodes made inside PipeWire (a device's source opened by the daemon for it);
--   - everything else read-only, as WirePlumber gives sandboxed apps (playing sound works).
-- The camera portal's remote for one of these apps (pipewire.access = "portal") gets the same.
-- And a sound capture stream of an app without the microphone permission is never linked to anything, so it cannot
-- record what the phone plays either; nor is a video capture stream of its own (not the portal's: a screen cast)
-- without the camera permission. With the permission, its sound capture is linked to a microphone only, never to
-- what plays (a sink's monitor, another app's stream), and its own video capture to a camera only, never to a
-- screen cast: Android keeps both from an app with the permission too (a screen cast goes through the portal,
-- which asks). When the permission store changes, the permissions follow at once.
--
-- A refused capture stream fails at once, as it does in WirePlumber's own linking when it finds no target: the
-- client is sent an error for the stream (-EACCES), pipewire-pulse fails the PulseAudio stream, and the node is
-- destroyed. It never hangs unlinked. Refused is: the microphone (or the camera) stored as "no" or not given, and
-- the Microphone switch in Settings off.
--
-- The microphone asks the first time an app records, as the camera and location do through their portals; sound
-- has no portal. An app whose microphone entry is unset (or "ask") has its capture stream held unlinked while the
-- user is asked, up to ASK_TIMEOUT_MS. The question travels over the session bus as a name this script owns,
-- org.freedesktop.ReserveDevice1.VibeOSMicAsk<the app id in hex> (WirePlumber's device reservation, the one
-- thing a script can put on the bus: the permission store's "set" takes a typed variant a script cannot build). The
-- shell watches for such names, asks "Allow <app> to use the microphone?", and writes the answer to the app's entry
-- in the permission store ("vibeos-sandbox"/"microphone": "yes" or "no", remembered). The answer is followed at
-- once: "yes" links the held streams on the rescan, "no" fails them. A question dismissed (or not asked, as the
-- phone is locked) is answered by the shell with "dismissed" in "vibeos-sandbox-ask"/"microphone", which fails
-- the held streams without remembering anything; no answer in ASK_TIMEOUT_MS does the same. One question per app:
-- a second stream of the same app waits for the same answer.
-- Such an app makes streams only: a node of its own that says it is something else (a sink, a source, a camera),
-- which other apps' sound or pictures would be routed to, is destroyed, and never a target in the meantime.
--
-- When the permission store cannot be read, the microphone and the cameras stay hidden.
--
-- And for every client, not only sandboxed apps: Settings' Microphone and Camera switches (Security and privacy:
-- org.gnome.desktop.privacy disable-microphone and disable-camera, which the shell mirrors into the permission store
-- as "vibeos-privacy"/"microphone" and "vibeos-privacy"/"camera", app "all" = "blocked"). While one is blocked, the
-- microphones (or the cameras) are hidden from every client but PipeWire's and WirePlumber's own, VibeOS's own apps,
-- the portals and the shell included; no capture stream of theirs is linked to one, and the links one had are taken
-- away at once. A sound capture stream can still record what plays (a sink's monitor), as on Android. PipeWire's
-- and WirePlumber's own nodes keep theirs: they run the devices, and a call's audio, where it is a loopback in the
-- daemon, is theirs. A program outside the sandbox can still open the sound and video devices itself, past
-- PipeWire; a sandboxed app cannot (vibeos-sandbox gives it neither /dev/snd nor /dev/video*).

cutils = require ("common-utils")
log = Log.open_topic ("s-client")

local pps = Plugin.find ("portal-permissionstore")
local MIC_TABLE, MIC_ID = "vibeos-sandbox", "microphone"
-- the shell's answer to a question it did not put to the user: the app's entry says "dismissed"
local ASK_TABLE = "vibeos-sandbox-ask"
-- a question open longer than this (a little over the shell's own 30 s) is answered "no, not now"
local ASK_TIMEOUT_MS = 35000
local ASK_NAME = "VibeOSMicAsk"
local ASK_PRIORITY = 2147483647
local EACCES = -13
local CAM_TABLE, CAM_ID = "devices", "camera"
local cache = {}

local function lookup (tbl, id)
  local key = tbl .. "/" .. id
  if cache[key] == nil then
    local perms = nil
    if pps ~= nil then
      local ok, value = pcall (function () return pps:call ("lookup", tbl, id) end)
      if ok then
        perms = value
      end
    end
    cache[key] = perms or {}
  end
  return cache[key]
end

local function granted (tbl, id, app_id)
  local values = lookup (tbl, id)[app_id]
  if values == nil then
    return false
  end
  for _, v in pairs (values) do
    if v == "yes" then
      return true
    end
  end
  return false
end

-- what the store says of the app's microphone: "yes", "no", or nil (no entry, or "ask": the user is yet to be asked)
local function microphone_answer (app_id)
  local values = lookup (MIC_TABLE, MIC_ID)[app_id]
  local answer = nil
  for _, v in pairs (values or {}) do
    if v == "no" and answer == nil then
      answer = "no"
    elseif v == "yes" then
      answer = "yes"
    end
  end
  return answer
end

-- Settings' Microphone and Camera switches: "microphone" or "camera" blocked for every client
local PRIVACY_TABLE = "vibeos-privacy"

local function blocked (id)
  local values = lookup (PRIVACY_TABLE, id)["all"]
  if values == nil then
    return false
  end
  for _, v in pairs (values) do
    if v == "blocked" then
      return true
    end
  end
  return false
end

-- the app id of a client this script handles, or nil
local function sandboxed_app (client)
  local props = client.properties
  if props == nil then
    return nil
  end
  local app_id = props["pipewire.access.portal.app_id"]
  if app_id == nil or app_id == "" then
    return nil
  end
  local access = cutils.get_client_access (props)
  if access == "flatpak" then
    return app_id
  end
  if access == "portal" and props["pipewire.access.portal.is_portal"] ~= "yes" then
    return app_id
  end
  return nil
end

local function prop (object, key)
  local ok, value = pcall (function () return object.properties[key] end)
  if ok and value ~= nil then
    return value
  end
  ok, value = pcall (function () return object["global-properties"][key] end)
  if ok then
    return value
  end
  return nil
end

local function is_mic (class)
  return class ~= nil and (class:find ("^Audio/Source") ~= nil or class:find ("^Audio/Duplex") ~= nil)
end

local function is_camera (class, role)
  return class == "Video/Source" and role == "Camera"
end

-- a microphone or a camera, by its class and role, that Settings' switches block now
local function device_blocked (class, role)
  return (is_mic (class) and blocked ("microphone")) or (is_camera (class, role) and blocked ("camera"))
end

-- PipeWire's and WirePlumber's own connections: the switches never hide anything from them
local function exempt (client)
  local props = client.properties
  local binary = props ~= nil and props["application.process.binary"] or nil
  return binary == "pipewire" or binary == "wireplumber"
end

local clients_om = ObjectManager { Interest { type = "client" } }
clients_om:activate ()

-- the app id of the client with this id, when it is a sandboxed app's
local function app_of (client_id)
  client_id = tonumber (client_id)
  if client_id == nil then
    return nil
  end
  local client = clients_om:lookup { Constraint { "bound-id", "=", client_id, type = "gobject" } }
  if client == nil then
    return nil
  end
  return sandboxed_app (client)
end

-- what each node is, by its id, as first seen here: its ports and links get what it gets
local nodes = {}

-- the node is the client's own: made by this very connection (PipeWire hands a gone client's id to the next one, so
-- the connection is told by its serial, which is never reused)
local function own (client, n)
  if n.client_id ~= nil and n.client_id == tostring (client["bound-id"]) then
    return true
  end
  return n.client_serial ~= nil and n.client_serial == tostring (prop (client, "object.serial"))
end

local function node_perm (client, app_id, n)
  if own (client, n) or (n.app_id ~= nil and n.app_id == app_id) then
    return Perm.RX
  end
  if is_mic (n.class) then
    return (not blocked ("microphone") and granted (MIC_TABLE, MIC_ID, app_id)) and Perm.RX or Perm.NONE
  end
  if is_camera (n.class, n.role) then
    return (not blocked ("camera") and granted (CAM_TABLE, CAM_ID, app_id)) and Perm.RX or Perm.NONE
  end
  if n.class ~= nil and n.class:find ("^Stream/") ~= nil then
    return Perm.NONE
  end
  return Perm.RX
end

local function known_node_perm (client, app_id, node_id)
  local n = nodes[tonumber (node_id) or -1]
  if n == nil then
    return Perm.RX
  end
  return node_perm (client, app_id, n)
end

-- what a node is (nodes[]), as first seen or after it changed
local function node_info (node)
  local id = tonumber (node["bound-id"])
  local n = id ~= nil and nodes[id] or nil
  if n == nil or n.maker_pending or n.client_id ~= prop (node, "client.id") or
     n.class ~= prop (node, "media.class") then
    local client_id = prop (node, "client.id")
    local maker = tonumber (client_id) ~= nil and
                  clients_om:lookup { Constraint { "bound-id", "=", tonumber (client_id), type = "gobject" } } or nil
    local serial = maker ~= nil and prop (maker, "object.serial") or nil
    n = { client_id = client_id ~= nil and tostring (client_id) or nil, app_id = app_of (client_id),
          client_serial = serial ~= nil and tostring (serial) or nil,
          class = prop (node, "media.class"), role = prop (node, "media.role") }
    -- a node whose maker is not known yet (PipeWire can announce the node before its client) is looked at again the
    -- next time, or it would stay another client's stream, hidden from the app that made it
    n.maker_pending = client_id ~= nil and maker == nil
    if id ~= nil then
      nodes[id] = n
    end
  end
  return n
end

sandbox_pm = PermissionManager ()
sandbox_pm:set_default_permissions (Perm.RX)

sandbox_pm:add_interest_match (
  function (_, client, node)
    local n = node_info (node)
    local app_id = sandboxed_app (client)
    if app_id == nil then
      return Perm.RX
    end
    return node_perm (client, app_id, n)
  end,
  Interest { type = "node" }
)

sandbox_pm:add_interest_match (
  function (_, client, port)
    local app_id = sandboxed_app (client)
    if app_id == nil then
      return Perm.RX
    end
    return known_node_perm (client, app_id, prop (port, "node.id"))
  end,
  Interest { type = "port" }
)

sandbox_pm:add_interest_match (
  function (_, client, link)
    local app_id = sandboxed_app (client)
    if app_id == nil then
      return Perm.RX
    end
    if known_node_perm (client, app_id, prop (link, "link.output.node")) == Perm.NONE or
       known_node_perm (client, app_id, prop (link, "link.input.node")) == Perm.NONE then
      return Perm.NONE
    end
    return Perm.RX
  end,
  Interest { type = "link" }
)

sandbox_pm:add_interest_match (
  function (_, client, other)
    if tostring (other["bound-id"]) == tostring (client["bound-id"]) then
      return Perm.RX
    end
    return Perm.NONE
  end,
  Interest { type = "client" }
)

sandbox_pm:add_interest_match (
  function (_, client, factory)
    if sandboxed_app (client) == nil or prop (factory, "factory.name") == "client-node" then
      return Perm.RX
    end
    return Perm.NONE
  end,
  Interest { type = "factory" }
)

-- the client that made a node (a session item's or a node's properties), when it is a sandboxed app's
local function sandboxed_owner (props)
  local client_id = tonumber (props["client.id"])
  if client_id == nil then
    return nil
  end
  local client = clients_om:lookup { Constraint { "bound-id", "=", client_id, type = "gobject" } }
  if client == nil then
    return nil
  end
  if sandboxed_app (client) == nil then
    return nil
  end
  return client
end

-- a node a sandboxed app made that is not a stream
local function posing (props)
  if props == nil then
    return false
  end
  local class = props["media.class"]
  return class ~= nil and class:find ("^Stream/") == nil and sandboxed_owner (props) ~= nil
end

-- what becomes of a sandboxed app's capture stream: "allow", "ask" (the microphone, the user is yet to be asked) or
-- "refuse" with the reason the app is told
local function capture_verdict (si)
  local props = si.properties
  local class = props["media.class"]
  local audio = class ~= nil and class:find ("^Stream/Input/Audio") ~= nil
  local video = class ~= nil and class:find ("^Stream/Input/Video") ~= nil
  if not audio and not video then
    return "allow"
  end
  local client = sandboxed_owner (props)
  if client == nil then
    return "allow"
  end
  local app_id = sandboxed_app (client)
  if audio then
    if blocked ("microphone") then
      return "refuse", app_id, "the microphone is off"
    end
    local answer = microphone_answer (app_id)
    if answer == "yes" then
      return "allow"
    end
    if answer == "no" then
      return "refuse", app_id, "the microphone is not allowed"
    end
    return "ask", app_id
  end
  -- video through the app's own connection; the portals' remotes (the camera, a screen cast) are theirs to grant
  if cutils.get_client_access (client.properties) == "flatpak" and
     (blocked ("camera") or not granted (CAM_TABLE, CAM_ID, app_id)) then
    return "refuse", app_id, "the camera is not allowed"
  end
  return "allow"
end

-- the stream's key while it waits: its serial, which is never reused
local function stream_key (si)
  local props = si.properties
  return tostring (props["object.serial"] or props["node.id"] or si.id)
end

-- the app's capture stream fails: its client is told (pipewire-pulse fails the PulseAudio stream, as for a stream
-- with no target in WirePlumber's own linking) and the node is destroyed
local function refuse (si, why)
  local ok, err = pcall (function ()
    local node = si:get_associated_proxy ("node")
    local client = sandboxed_owner (si.properties)
    if node ~= nil and client ~= nil then
      client:send_error (node["bound-id"], EACCES, why)
    end
    if node ~= nil then
      node:request_destroy ()
    end
  end)
  if not ok then
    log:warning ("vibeos-sandbox: could not fail a refused capture stream: " .. tostring (err))
  end
end

-- ---- the question for the microphone ----------------------------------------------------------------------------

local reserve = Plugin.find ("reserve-device")
-- an app id that fits in a bus name, as hex
local function ask_name (app_id)
  local hex = app_id:gsub (".", function (c) return string.format ("%02x", c:byte ()) end)
  return ASK_NAME .. hex
end

-- the apps being asked: app id -> { name = the bus name's last part, streams = { key = si }, timer = the timeout }
local asking = {}

local function can_ask ()
  if reserve == nil then
    return false
  end
  local ok, dbus = pcall (function () return reserve:call ("get-dbus") end)
  return ok and dbus ~= nil and dbus["state"] == "connected"
end

-- the question over: the name goes from the bus (the shell takes its dialog away) and the held streams are linked
-- on the rescan ("allow") or fail ("refuse")
local function finish (app_id, verdict, why)
  local q = asking[app_id]
  if q == nil then
    return
  end
  asking[app_id] = nil
  if q.timer ~= nil then
    q.timer:destroy ()
  end
  pcall (function () reserve:call ("destroy-reservation", q.name) end)
  if verdict == "refuse" then
    for _, si in pairs (q.streams) do
      refuse (si, why)
    end
  end
end

-- a stream waits for the user's answer for its app: the question is put once per app
local function hold (si, app_id)
  local q = asking[app_id]
  if q == nil then
    local name = ask_name (app_id)
    if not can_ask () or #name > 220 then
      log:warning ("vibeos-sandbox: " .. app_id .. " cannot be asked for the microphone")
      refuse (si, "the microphone cannot be asked for")
      return
    end
    local ok, rd = pcall (function ()
      return reserve:call ("create-reservation", name, "VibeOS sandbox", app_id, ASK_PRIORITY)
    end)
    if not ok or rd == nil then
      log:warning ("vibeos-sandbox: the question for " .. app_id .. " did not go out: " .. tostring (rd))
      refuse (si, "the microphone cannot be asked for")
      return
    end
    rd:call ("acquire")
    q = { name = name, streams = {} }
    q.timer = Core.timeout_add (ASK_TIMEOUT_MS, function ()
      q.timer = nil
      log:notice ("vibeos-sandbox: no answer for the microphone of " .. app_id)
      finish (app_id, "refuse", "the microphone was not answered")
    end)
    asking[app_id] = q
    log:info (si, "vibeos-sandbox: asking for the microphone of " .. app_id)
  end
  q.streams[stream_key (si)] = si
end

-- the store changed (or the Microphone switch): every question that is answered now, is
local function settle ()
  local apps = {}
  for app_id in pairs (asking) do
    apps[#apps + 1] = app_id
  end
  for _, app_id in ipairs (apps) do
    local answer = microphone_answer (app_id)
    local dismissed = false
    for _, v in pairs (lookup (ASK_TABLE, MIC_ID)[app_id] or {}) do
      dismissed = dismissed or v == "dismissed"
    end
    if blocked ("microphone") then
      finish (app_id, "refuse", "the microphone is off")
    elseif answer == "yes" then
      finish (app_id, "allow")
    elseif answer == "no" then
      finish (app_id, "refuse", "the microphone is not allowed")
    elseif dismissed then
      finish (app_id, "refuse", "the microphone was not allowed this time")
    end
  end
end

SimpleEventHook {
  name = "linking/vibeos-sandbox-capture",
  before = { "linking/find-defined-target", "linking/find-filter-target", "linking/find-audio-group-target",
             "linking/find-media-role-target", "linking/find-media-role-sink-target",
             "linking/find-default-target", "linking/find-best-target", "linking/get-filter-from-target",
             "linking/prepare-link", "linking/link-target" },
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "select-target" },
    },
  },
  execute = function (event)
    -- on an error the stream goes on as usual (the microphone itself is hidden all the same): never hold up
    -- the linking of other streams, a call's among them
    local si = event:get_subject ()
    local ok, verdict, app_id, why = pcall (capture_verdict, si)
    if not ok or verdict == "allow" then
      return
    end
    event:stop_processing ()
    if verdict == "ask" then
      log:info (si, "vibeos-sandbox: capture without an answer for the microphone, not linked yet")
      local held, err = pcall (hold, si, app_id)
      if not held then
        log:warning ("vibeos-sandbox: " .. tostring (err))
        refuse (si, "the microphone cannot be asked for")
      end
    else
      log:info (si, "vibeos-sandbox: capture without the permission, refused: " .. tostring (why))
      refuse (si, why)
    end
  end
}:register ()

-- what a sandboxed app's capture stream may be linked to, whatever its permissions: sound from a microphone,
-- pictures (on the app's own connection) from a camera. Not what plays: a sink's monitor, another app's stream
local function capture_target_refused (si, target)
  local props = si.properties
  local class = props["media.class"]
  local audio = class ~= nil and class:find ("^Stream/Input/Audio") ~= nil
  local video = class ~= nil and class:find ("^Stream/Input/Video") ~= nil
  if not audio and not video then
    return false
  end
  local client = sandboxed_owner (props)
  if client == nil then
    return false
  end
  local tclass = target.properties["media.class"]
  if audio then
    return not is_mic (tclass)
  end
  -- the portals' remotes (a screen cast the user chose) are linked to what the portal granted
  if cutils.get_client_access (client.properties) ~= "flatpak" then
    return false
  end
  return not (tclass == "Video/Source" and target.properties["media.role"] == "Camera")
end

-- whatever found the target, never one of those, and nothing a capture stream may not record
SimpleEventHook {
  name = "linking/vibeos-sandbox-target",
  after = { "linking/find-defined-target", "linking/find-filter-target", "linking/find-audio-group-target",
            "linking/find-media-role-target", "linking/find-media-role-sink-target",
            "linking/find-default-target", "linking/find-best-target", "linking/get-filter-from-target" },
  before = { "linking/prepare-link", "linking/link-target" },
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "select-target" },
    },
  },
  execute = function (event)
    local target = event:get_data ("target")
    if target == nil then
      return
    end
    local ok, bad = pcall (function () return posing (target.properties) end)
    if ok and bad then
      log:info (event:get_subject (), "vibeos-sandbox: a sandboxed app's node is no target")
      event:set_data ("target", nil)
      return
    end
    ok, bad = pcall (capture_target_refused, event:get_subject (), target)
    if ok and bad then
      log:info (event:get_subject (), "vibeos-sandbox: a sandboxed app records a microphone or a camera only")
      event:set_data ("target", nil)
    end
  end
}:register ()

SimpleEventHook {
  name = "node/vibeos-sandbox-streams-only",
  before = "node/create-item",
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "node-added" },
    },
  },
  execute = function (event)
    local node = event:get_subject ()
    local ok, bad = pcall (function () return posing (node.properties) end)
    if ok and bad then
      log:notice (node, "vibeos-sandbox: a sandboxed app made a node that is not a stream (" ..
                  tostring (node.properties["media.class"]) .. "); destroyed")
      node:request_destroy ()
      event:stop_processing ()
    end
  end
}:register ()

-- ---- Settings' Microphone and Camera switches, for every other client ------------------------------------------

-- the client that made a node (its client.id), or nil
local function owner (node_props)
  local client_id = tonumber (node_props["client.id"])
  if client_id == nil then
    return nil
  end
  return clients_om:lookup { Constraint { "bound-id", "=", client_id, type = "gobject" } }
end

local function privacy_node_perm (client, n, full)
  if own (client, n) then
    return full                         -- a node of its own, whatever it says it is
  end
  if device_blocked (n.class, n.role) then
    return Perm.NONE
  end
  return full
end

-- a client's view: everything as WirePlumber would give it (all, or read-only for a "restricted" client) but the
-- blocked microphones or cameras, their ports and links
local function make_privacy_pm (full)
  local pm = PermissionManager ()
  pm:set_default_permissions (full)
  pm:add_interest_match (
    function (_, client, node)
      return privacy_node_perm (client, node_info (node), full)
    end,
    Interest { type = "node" }
  )
  local function known (client, node_id)
    local n = nodes[tonumber (node_id) or -1]
    if n == nil then
      return full
    end
    return privacy_node_perm (client, n, full)
  end
  pm:add_interest_match (
    function (_, client, port)
      return known (client, prop (port, "node.id"))
    end,
    Interest { type = "port" }
  )
  pm:add_interest_match (
    function (_, client, link)
      if known (client, prop (link, "link.output.node")) == Perm.NONE or
         known (client, prop (link, "link.input.node")) == Perm.NONE then
        return Perm.NONE
      end
      return full
    end,
    Interest { type = "link" }
  )
  return pm
end

privacy_pm = make_privacy_pm (Perm.ALL)
privacy_restricted_pm = make_privacy_pm (Perm.RX)

-- every client this script has not given a view already, but PipeWire's and WirePlumber's: the view with the switches
-- in it, instead of WirePlumber's default one (all, or read-only for a restricted client). Clients the flatpak, portal
-- and snap rules handle keep theirs (VibeOS has no Flatpak apps; the camera portal refuses while the camera is off).
SimpleEventHook {
  name = "client/find-vibeos-privacy-access",
  after = { "client/find-config-access", "client/find-vibeos-sandbox-access", "client/find-flatpak-access",
            "client/find-portal-access", "client/find-snap-access" },
  before = { "client/find-default-access", "client/apply-access" },
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "select-access" },
    },
  },
  execute = function (event)
    local client = event:get_subject ()
    if event:get_data ("permission-manager") ~= nil or event:get_data ("default-permissions") ~= nil then
      return
    end
    if exempt (client) then
      return
    end
    local access = cutils.get_client_access (client.properties)
    if event:get_data ("effective-access") == nil and access ~= nil then
      event:set_data ("effective-access", access)
    end
    event:set_data ("permission-manager", access == "restricted" and privacy_restricted_pm or privacy_pm)
  end
}:register ()

-- a capture stream of any client but PipeWire's and WirePlumber's is never linked to a blocked microphone or camera
local function privacy_target_refused (si, target)
  local class = si.properties["media.class"]
  if class == nil or (class:find ("^Stream/Input/Audio") == nil and class:find ("^Stream/Input/Video") == nil) then
    return false
  end
  local tprops = target.properties
  if not device_blocked (tprops["media.class"], tprops["media.role"]) then
    return false
  end
  local client = owner (si.properties)
  return client == nil or not exempt (client)
end

SimpleEventHook {
  name = "linking/vibeos-privacy-target",
  after = { "linking/find-defined-target", "linking/find-filter-target", "linking/find-audio-group-target",
            "linking/find-media-role-target", "linking/find-media-role-sink-target",
            "linking/find-default-target", "linking/find-best-target", "linking/get-filter-from-target" },
  before = { "linking/prepare-link", "linking/link-target" },
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "select-target" },
    },
  },
  execute = function (event)
    local target = event:get_data ("target")
    if target == nil then
      return
    end
    local ok, refused = pcall (privacy_target_refused, event:get_subject (), target)
    if ok and refused then
      log:info (event:get_subject (), "vibeos-privacy: the microphone or the camera is off, not linked")
      event:set_data ("target", nil)
    end
  end
}:register ()

-- the links a blocked microphone or camera has to anyone but PipeWire and WirePlumber: taken away as it is blocked
-- (prepare-link would move a stream off it on the rescan, but not one that asked never to be moved), and as one is
-- made while it is blocked (a host client sees every node for the few milliseconds before its permissions are set, and
-- could link one itself in them)
local nodes_om = ObjectManager { Interest { type = "node" } }
nodes_om:activate ()
local links_om = ObjectManager { Interest { type = "link" } }

local function unlink_if_blocked (link)
  local ok, err = pcall (function ()
    local from = nodes_om:lookup { Constraint { "bound-id", "=", tonumber (prop (link, "link.output.node")) or -1,
                                                type = "gobject" } }
    local to = nodes_om:lookup { Constraint { "bound-id", "=", tonumber (prop (link, "link.input.node")) or -1,
                                              type = "gobject" } }
    if from == nil or to == nil or not device_blocked (prop (from, "media.class"), prop (from, "media.role")) then
      return
    end
    local client = owner (to.properties)
    if client ~= nil and exempt (client) then
      return
    end
    log:notice (link, "vibeos-privacy: " .. tostring (prop (from, "node.name")) .. " is off, unlinked from " ..
                tostring (prop (to, "node.name")))
    link:request_destroy ()
  end)
  if not ok then
    log:warning ("vibeos-privacy: " .. tostring (err))
  end
end

function unlink_blocked ()
  for link in links_om:iterate () do
    unlink_if_blocked (link)
  end
end

links_om:connect ("object-added", function (_, link)
  if blocked ("microphone") or blocked ("camera") then
    unlink_if_blocked (link)
  end
end)
links_om:activate ()

if pps ~= nil then
  pps:connect ("changed", function (_, tbl, id, deleted, permissions)
    local privacy = tbl == PRIVACY_TABLE and (id == "microphone" or id == "camera")
    if privacy or (tbl == MIC_TABLE and id == MIC_ID) or (tbl == CAM_TABLE and id == CAM_ID) or
       (tbl == ASK_TABLE and id == MIC_ID) then
      cache[tbl .. "/" .. id] = (not deleted and permissions) or {}
      sandbox_pm:update_permissions ()
      if privacy then
        log:notice ("vibeos-privacy: " .. id .. (blocked (id) and " blocked" or " allowed"))
        privacy_pm:update_permissions ()
        privacy_restricted_pm:update_permissions ()
        unlink_blocked ()
      end
      settle ()
      local source = Plugin.find ("standard-event-source")
      if source ~= nil then
        source:call ("schedule-rescan", "linking")
      end
    end
  end)
else
  log:warning ("vibeos-sandbox: no permission store; microphones and cameras stay hidden from sandboxed apps")
end

SimpleEventHook {
  name = "client/find-vibeos-sandbox-access",
  before = { "client/find-flatpak-access", "client/find-portal-access", "client/find-snap-access",
             "client/find-default-access" },
  after = "client/find-config-access",
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "select-access" },
    },
  },
  execute = function (event)
    local client = event:get_subject ()
    if event:get_data ("permission-manager") ~= nil or event:get_data ("default-permissions") ~= nil then
      return
    end
    local app_id = sandboxed_app (client)
    if app_id == nil then
      return
    end
    log:info (client, "vibeos-sandbox permissions for " .. app_id)
    if event:get_data ("effective-access") == nil then
      event:set_data ("effective-access", cutils.get_client_access (client.properties))
    end
    event:set_data ("permission-manager", sandbox_pm)
  end
}:register ()

sandbox_pm:activate (Features.ALL, function (_, e)
  if e then
    log:warning ("vibeos-sandbox: the permission manager did not start: " .. tostring (e))
  end
end)

for name, pm in pairs ({ ["vibeos-privacy"] = privacy_pm, ["vibeos-privacy-restricted"] = privacy_restricted_pm }) do
  pm:activate (Features.ALL, function (_, e)
    if e then
      log:warning ("vibeos-privacy: the permission manager " .. name .. " did not start: " .. tostring (e))
    end
  end)
end

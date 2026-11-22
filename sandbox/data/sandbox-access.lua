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
-- Such an app makes streams only: a node of its own that says it is something else (a sink, a source, a camera),
-- which other apps' sound or pictures would be routed to, is destroyed, and never a target in the meantime.
--
-- When the permission store cannot be read, the microphone and the cameras stay hidden.

cutils = require ("common-utils")
log = Log.open_topic ("s-client")

local pps = Plugin.find ("portal-permissionstore")
local MIC_TABLE, MIC_ID = "vibeos-sandbox", "microphone"
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

local function node_perm (client, app_id, n)
  if n.client_id ~= nil and (n.client_id == tostring (client["bound-id"]) or n.app_id == app_id) then
    return Perm.RX
  end
  if is_mic (n.class) then
    return granted (MIC_TABLE, MIC_ID, app_id) and Perm.RX or Perm.NONE
  end
  if n.class == "Video/Source" and n.role == "Camera" then
    return granted (CAM_TABLE, CAM_ID, app_id) and Perm.RX or Perm.NONE
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

sandbox_pm = PermissionManager ()
sandbox_pm:set_default_permissions (Perm.RX)

sandbox_pm:add_interest_match (
  function (_, client, node)
    local id = tonumber (node["bound-id"])
    local n = id ~= nil and nodes[id] or nil
    if n == nil or n.client_id ~= prop (node, "client.id") or n.class ~= prop (node, "media.class") then
      local client_id = prop (node, "client.id")
      n = { client_id = client_id ~= nil and tostring (client_id) or nil, app_id = app_of (client_id),
            class = prop (node, "media.class"), role = prop (node, "media.role") }
      if id ~= nil then
        nodes[id] = n
      end
    end
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

-- capture streams of apps without the microphone permission stay unlinked
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

local function capture_denied (si)
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
  local app_id = sandboxed_app (client)
  if audio then
    return not granted (MIC_TABLE, MIC_ID, app_id)
  end
  -- video through the app's own connection; the portals' remotes (the camera, a screen cast) are theirs to grant
  return cutils.get_client_access (client.properties) == "flatpak" and not granted (CAM_TABLE, CAM_ID, app_id)
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
    local ok, denied = pcall (capture_denied, event:get_subject ())
    if ok and denied then
      log:info (event:get_subject (), "vibeos-sandbox: capture without the permission, not linked")
      event:stop_processing ()
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

if pps ~= nil then
  pps:connect ("changed", function (_, tbl, id, deleted, permissions)
    if (tbl == MIC_TABLE and id == MIC_ID) or (tbl == CAM_TABLE and id == CAM_ID) then
      cache[tbl .. "/" .. id] = permissions or {}
      sandbox_pm:update_permissions ()
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

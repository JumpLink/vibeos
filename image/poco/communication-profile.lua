-- VibeOS: calls on the earpiece, as on Android.
--
-- A sound card whose UCM verb has an earpiece next to its loudspeaker on one PCM gets them as separate profiles
-- (PipeWire's ACP splits devices that share a PCM): "HiFi (Mic1, Speaker)" and "HiFi (Earpiece, Mic1)".
--
-- An app is in a call when it plays a stream with media.role "Communication" (PulseAudio's "phone" arrives as that)
-- AND records at the same time. The role alone is not enough: the dialer rings and plays its keypad tones with it,
-- and those belong on the loudspeaker; recording alone is not enough either: Vibe and Dictate speak while they
-- listen. Modem calls are not ours: WirePlumber's find-calling-profile picks their Voice Call profile first.
--
-- When the first call starts, the profile with the earpiece in place of the loudspeaker is picked, once. While calls
-- last the profile is never chosen again: whatever plays then (the earpiece, or the loudspeaker the user switched
-- to) stays through new streams and route changes. An app's Speaker button plays without the role, which ends the
-- call here, so the stored profile comes back; its Earpiece button plays with it again. When the last call ends,
-- WirePlumber's own choice comes back (the stored profile, else the best one). Nothing is stored (save = false).
--
-- Only devices that ask for it: device property "vibeos.communication-device", the UCM device to play calls on
-- ("Earpiece"), set by the device's monitor.alsa.rules.
--
-- A profile change removes every node of the card and makes new ones. Streams are moved to them, except those that
-- asked never to be: GStreamer's pipewiresrc sets node.dont-reconnect, and its capture then fails ("target not
-- found") the moment a call moves to the earpiece. A call app that records with pipewiresrc gives it
-- stream-properties "props,node.dont-reconnect=false".

cutils = require ("common-utils")
log = Log.open_topic ("s-device")

local ROLES = { communication = true, phone = true }
local calling = false        -- an app is in a call
local starting = {}          -- device id -> true: the call just started, pick the earpiece at the next selection

-- type "pw": a rule's property is in the device's own properties, never in its registry global's (a constraint
-- without a type matches the global's few keys, and so no device at all)
local alsa_devs_om = ObjectManager {
  Interest {
    type = "device",
    Constraint { "device.api", "=", "alsa" },
    Constraint { "vibeos.communication-device", "is-present", type = "pw" },
  }
}

local clients_om = ObjectManager {
  Interest {
    type = "client",
  }
}

local streams_om = ObjectManager {
  Interest {
    type = "node",
    Constraint { "media.class", "matches", "Stream/*" },
  }
}

-- the app a stream belongs to, the same whether it plays over PulseAudio and records over PipeWire (as a sandboxed
-- app does): the two are different clients with different application.names ("python3" and "python3.14"),
-- and a stream made by pipewiresrc has no application.name of its own at all. The sandbox's app id is on both
-- clients (pipewire-pulse sets it for each PulseAudio client); outside the sandbox the process id each reports.
local function app_of (node)
  local id = tonumber (node.properties ["client.id"])
  if not id then
    return nil
  end
  for client in clients_om:iterate () do
    if client ["bound-id"] == id then
      local props = client.properties
      local app_id = props ["pipewire.access.portal.app_id"]
      if app_id ~= nil and app_id ~= "" then
        return app_id
      end
      local pid = props ["application.process.id"]
      return pid and ("process " .. pid) or nil
    end
  end
  return nil
end

local function in_call ()
  local plays, records = {}, {}
  for node in streams_om:iterate () do
    local props = node.properties
    local app = app_of (node)
    local class = props ["media.class"] or ""
    local role = props ["media.role"]
    if app and class:find ("^Stream/Output/Audio") and role and ROLES [role:lower ()] then
      plays [app] = true
    elseif app and class:find ("^Stream/Input/Audio") then
      records [app] = true
    end
  end
  for app in pairs (plays) do
    if records [app] then
      return true, app
    end
  end
  return false, nil
end

local function reselect ()
  local now, app = in_call ()
  if now == calling then
    return
  end
  calling = now
  log:info (string.format ("%s: the profiles are chosen again", now and (app .. " is in a call") or "the calls ended"))
  local source = Plugin.find ("standard-event-source")
  if not source then
    log:warning ("no standard-event-source: the profiles stay as they are")
    return
  end
  for device in alsa_devs_om:iterate () do
    starting [device ["bound-id"]] = now or nil
    source:call ("push-event", "select-profile", device, nil)
  end
end

-- Looked at once the streams have settled: pulsesink makes a short-lived stream of its own before the one it plays
-- on, and an app's Speaker button replaces its stream with one without the role. Taken one by one, each of these
-- would switch the card's profile there and back (and the sink with it) within a tenth of a second.
local generation = 0
local function settle ()
  generation = generation + 1
  local mine = generation
  Core.timeout_add (300, function ()
    if mine == generation then
      reselect ()
    end
    return false
  end)
end

clients_om:connect ("object-added", function (_, client) settle () end)
streams_om:connect ("object-added", function (_, node) settle () end)
streams_om:connect ("object-removed", function (_, node) settle () end)

-- "HiFi (Mic1, Speaker)" -> "HiFi", { Mic1 = true, Speaker = true }
local function parse (name)
  local verb, list = (name or ""):match ("^(.-) %((.*)%)$")
  if not verb then
    return nil
  end
  local set = {}
  for d in list:gmatch ("[^,]+") do
    set [d:match ("^%s*(.-)%s*$")] = true
  end
  return verb, set
end

local function same (a, b)
  for k in pairs (a) do
    if not b [k] then return false end
  end
  for k in pairs (b) do
    if not a [k] then return false end
  end
  return true
end

local function usable (verb)
  return verb ~= nil and not verb:find ("^Voice Call")
end

-- the profile to start a call on: the active one's (or, under a modem call's verb, the best other one's) with `want`
-- in place of the loudspeaker, the microphones as they were
local function call_profile (device, want, profiles, active)
  local outputs = {}
  for p in device:iterate_params ("EnumRoute") do
    local route = cutils.parseParam (p, "EnumRoute")
    if route and route.direction == "Output" then
      local dev = (route.name or ""):match ("^%[Out%] (.+)$")
      if dev then outputs [dev] = true end
    end
  end
  local base_verb, base_set = nil, nil
  if active then
    base_verb, base_set = parse (active.name)
  end
  if not usable (base_verb) then
    base_verb, base_set = nil, nil
    local best = nil
    for _, profile in ipairs (profiles) do
      local verb, set = parse (profile.name)
      if usable (verb) and profile.available ~= "no" and (not best or profile.priority > best.priority) then
        best, base_verb, base_set = profile, verb, set
      end
    end
  end
  if not base_verb then
    return nil
  end
  local target = { [want] = true }
  for dev in pairs (base_set) do
    if not outputs [dev] then target [dev] = true end
  end
  for _, profile in ipairs (profiles) do
    local verb, set = parse (profile.name)
    if verb == base_verb and set and same (set, target) and profile.available ~= "no" then
      return profile
    end
  end
  log:info (device, string.format ("a call plays, but %s has no '%s' profile", base_verb, want))
  return nil
end

SimpleEventHook {
  name = "device/find-communication-profile",
  after = "device/find-calling-profile",
  before = "device/find-stored-profile",
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "select-profile" },
    },
  },
  execute = function (event)
    if not calling or event:get_data ("selected-profile") then
      return
    end
    local device = event:get_subject ()
    local want = device.properties ["vibeos.communication-device"]
    if not want then
      return
    end

    local profiles = {}
    for p in device:iterate_params ("EnumProfile") do
      local profile = cutils.parseParam (p, "EnumProfile")
      if profile then table.insert (profiles, profile) end
    end
    local active = nil
    for p in device:iterate_params ("Profile") do
      local profile = cutils.parseParam (p, "Profile")
      if profile then
        for _, e in ipairs (profiles) do
          if e.index == profile.index then active = e end
        end
      end
    end

    local id = device ["bound-id"]
    if starting [id] then
      starting [id] = nil
      local profile = call_profile (device, want, profiles, active)
      if profile then
        log:info (device, string.format ("a call starts: profile '%s' (%d)", profile.name, profile.index))
        event:set_data ("selected-profile", profile)
        return
      end
    end
    -- during the call: what plays now stays, whoever chose it (this hook at the start, or the user since)
    if active then
      event:set_data ("selected-profile", active)
    end
  end
}:register ()

-- The earpiece's route is new, and WirePlumber would start it at its default volume (0.064, inaudible next to the
-- ear). Until the user has set one of its own (WirePlumber stores that, and it comes with the route from then on),
-- the call's route starts at the volume the loudspeaker has: between the stored props and the route being set.
devinfo = require ("device-info-cache")
local routes_state = nil

local function stored_props (dev_name, route_name)
  routes_state = routes_state or State ("default-routes")
  local value = routes_state:load () [dev_name .. ":output:" .. route_name]
  if value then
    local json = Json.Raw (value)
    if json and json:is_object () then
      return json:parse ()
    end
  end
  return nil
end

SimpleEventHook {
  name = "device/communication-route-volume",
  after = "device/apply-route-props",
  before = "device/apply-routes",
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "select-routes" },
    },
  },
  execute = function (event)
    local device = event:get_subject ()
    local want = device.properties ["vibeos.communication-device"]
    local selected = event:get_data ("selected-routes")
    if not want or not selected then
      return
    end
    local dev_info = devinfo:get_device_info (device)
    if not dev_info then
      return
    end
    local changed = false
    local out = {}
    for device_id, json in pairs (selected) do
      out [device_id] = json
      local route = Json.Raw (json):parse ()
      local ri = devinfo.find_route_info (dev_info, route, false)
      if ri and ri.name == "[Out] " .. want and not (route.props and route.props.channelVolumes) then
        -- the loudspeaker's stored volume (any other output route of this card that has one)
        for _, other in pairs (dev_info.route_infos) do
          if other.direction == "Output" and other.name ~= ri.name then
            local props = stored_props (dev_info.name, other.name)
            if props and props.channelVolumes then
              log:info (device, string.format ("%s starts at the volume of %s", ri.name, other.name))
              out [device_id] = Json.Object {
                index = ri.index,
                props = Json.Object {
                  mute = false,
                  channelVolumes = Json.Array (props.channelVolumes),
                },
              }:to_string ()
              changed = true
              break
            end
          end
        end
      end
    end
    if changed then
      event:set_data ("selected-routes", out)
    end
  end
}:register ()

alsa_devs_om:activate ()
clients_om:activate ()
streams_om:activate ()

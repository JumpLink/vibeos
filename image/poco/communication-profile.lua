-- VibeOS: calls on the earpiece, as on Android.
--
-- A sound card whose UCM verb has an earpiece next to its loudspeaker on one PCM gets them as separate profiles
-- (PipeWire's ACP splits devices that share a PCM): "HiFi (Mic1, Speaker)" and "HiFi (Earpiece, Mic1)". While an app
-- plays a call (an output stream with media.role "Communication", or PulseAudio's "phone"), this picks the profile
-- with the earpiece in place of the loudspeaker, the microphones as they were; when the last one ends, WirePlumber's
-- own choice comes back (the stored profile, else the best one). An app's loudspeaker button plays without the role.
--
-- Only devices that ask for it: device property "vibeos.communication-device", the UCM device to play calls on
-- ("Earpiece"), set by the device's monitor.alsa.rules. A modem call's profile (find-calling-profile) always wins.
-- The profile is applied as WirePlumber applies any it selects (save = false): what the user chose stays stored.

cutils = require ("common-utils")
log = Log.open_topic ("s-device")

local ROLES = { communication = true, phone = true }
local active = false

local alsa_devs_om = ObjectManager {
  Interest {
    type = "device",
    Constraint { "device.api", "=", "alsa" },
    Constraint { "vibeos.communication-device", "is-present" },
  }
}

local streams_om = ObjectManager {
  Interest {
    type = "node",
    Constraint { "media.class", "matches", "Stream/Output/Audio*" },
  }
}

local function is_call (node)
  local role = node.properties ["media.role"]
  return role ~= nil and ROLES [role:lower ()] == true
end

local function reselect ()
  local now = false
  for node in streams_om:iterate () do
    if is_call (node) then
      now = true
      break
    end
  end
  if now == active then
    return
  end
  active = now
  log:info (string.format ("a call stream %s: the profiles are chosen again", active and "plays" or "ended"))
  local source = Plugin.find ("standard-event-source")
  if not source then
    log:warning ("no standard-event-source: the profiles stay as they are")
    return
  end
  for device in alsa_devs_om:iterate () do
    source:call ("push-event", "select-profile", device, nil)
  end
end

streams_om:connect ("object-added", function (_, node) reselect () end)
streams_om:connect ("object-removed", function (_, node) reselect () end)

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
    if not active or event:get_data ("selected-profile") then
      return
    end
    local device = event:get_subject ()
    local want = device.properties ["vibeos.communication-device"]
    if not want then
      return
    end

    -- the UCM devices that play: the loudspeaker among them gives way to `want`
    local outputs = {}
    for p in device:iterate_params ("EnumRoute") do
      local route = cutils.parseParam (p, "EnumRoute")
      if route and route.direction == "Output" then
        local dev = (route.name or ""):match ("^%[Out%] (.+)$")
        if dev then outputs [dev] = true end
      end
    end

    -- what plays now, unless that is a modem call's verb: then the best other one
    local base_verb, base_set
    for p in device:iterate_params ("Profile") do
      local profile = cutils.parseParam (p, "Profile")
      if profile then
        base_verb, base_set = parse (profile.name)
      end
    end
    local profiles = {}
    for p in device:iterate_params ("EnumProfile") do
      local profile = cutils.parseParam (p, "EnumProfile")
      if profile then table.insert (profiles, profile) end
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
      return
    end

    local target = { [want] = true }
    for dev in pairs (base_set) do
      if not outputs [dev] then target [dev] = true end
    end
    for _, profile in ipairs (profiles) do
      local verb, set = parse (profile.name)
      if verb == base_verb and set and same (set, target) and profile.available ~= "no" then
        log:info (device, string.format ("a call plays: profile '%s' (%d)", profile.name, profile.index))
        event:set_data ("selected-profile", profile)
        return
      end
    end
    log:info (device, string.format ("a call plays, but %s has no '%s' profile", base_verb, want))
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
streams_om:activate ()

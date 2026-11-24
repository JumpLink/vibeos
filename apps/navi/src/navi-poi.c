/* navi-poi: the kinds of points of interest. See navi-poi.h. */
#include "navi-poi.h"

#include <string.h>

static const NaviPoiKind kinds[] = {
  { "fuel", NAVI_CATEGORY_FUEL, "fuel", 0x1A73E8, TRUE },
  { "charging", NAVI_CATEGORY_CHARGING, "charging-station", 0x0F9D58, FALSE },
  { "parking", NAVI_CATEGORY_PARKING, "parking", 0x3949AB, FALSE },
  { "food", NAVI_CATEGORY_FOOD, "restaurant", 0xF4511E, FALSE },
  { "fast_food", NAVI_CATEGORY_FOOD, "fast-food", 0xFB8C00, FALSE },
  { "cafe", NAVI_CATEGORY_COFFEE, "cafe", 0x8D6E63, FALSE },
  { "grocery", NAVI_CATEGORY_GROCERY, "grocery", 0x43A047, FALSE },
  { "pharmacy", NAVI_CATEGORY_PHARMACY, "pharmacy", 0xE53935, FALSE },
  { "hospital", NAVI_CATEGORY_HOSPITAL, "hospital", 0xC62828, FALSE },
  { "atm", NAVI_CATEGORY_ATM, "bank", 0x546E7A, FALSE },
  { "bank", NAVI_CATEGORY_ATM, "bank", 0x546E7A, FALSE },
  { "hotel", NAVI_CATEGORY_HOTEL, "lodging", 0x8E24AA, FALSE },
  { "toilets", NAVI_CATEGORY_TOILETS, "toilet", 0x00897B, FALSE },
};

/* (key, value) -> group: a tile's (class, subclass) and an OSM tag (key, value); a NULL value is any */
static const struct {
  const char *key, *value, *group;
} groups[] = {
  /* OpenMapTiles' poi layer */
  { "fuel", "fuel", "fuel" },
  { "fuel", "charging_station", "charging" },
  { "parking", "parking", "parking" },
  { "parking", "multi-storey", "parking" },
  { "parking", "underground", "parking" },
  { "restaurant", NULL, "food" },
  { "fast_food", NULL, "fast_food" },
  { "cafe", NULL, "cafe" },
  { "grocery", "supermarket", "grocery" },
  { "shop", "supermarket", "grocery" },
  { "pharmacy", "pharmacy", "pharmacy" },
  { "hospital", "hospital", "hospital" },
  { "atm", NULL, "atm" },
  { "bank", NULL, "bank" },
  { "lodging", "hotel", "hotel" },
  { "lodging", "motel", "hotel" },
  { "lodging", "hostel", "hotel" },
  { "lodging", "guest_house", "hotel" },
  { "toilets", NULL, "toilets" },
  /* OSM's tags, as Photon has them */
  { "amenity", "fuel", "fuel" },
  { "amenity", "charging_station", "charging" },
  { "amenity", "parking", "parking" },
  { "amenity", "restaurant", "food" },
  { "amenity", "fast_food", "fast_food" },
  { "amenity", "cafe", "cafe" },
  { "amenity", "pharmacy", "pharmacy" },
  { "amenity", "hospital", "hospital" },
  { "amenity", "atm", "atm" },
  { "amenity", "bank", "bank" },
  { "amenity", "toilets", "toilets" },
  { "tourism", "hotel", "hotel" },
  { "tourism", "motel", "hotel" },
  { "tourism", "hostel", "hotel" },
  { "tourism", "guest_house", "hotel" },
};

static const char *const photon[] = {
  [NAVI_CATEGORY_FUEL] = "osm.amenity.fuel",
  [NAVI_CATEGORY_CHARGING] = "osm.amenity.charging_station",
  [NAVI_CATEGORY_PARKING] = "osm.amenity.parking",
  [NAVI_CATEGORY_FOOD] = "osm.amenity.restaurant,osm.amenity.fast_food",
  [NAVI_CATEGORY_COFFEE] = "osm.amenity.cafe",
  [NAVI_CATEGORY_GROCERY] = "osm.shop.supermarket",
  [NAVI_CATEGORY_PHARMACY] = "osm.amenity.pharmacy",
  [NAVI_CATEGORY_HOSPITAL] = "osm.amenity.hospital",
  [NAVI_CATEGORY_ATM] = "osm.amenity.atm,osm.amenity.bank",
  [NAVI_CATEGORY_HOTEL] = "osm.tourism.hotel,osm.tourism.motel,osm.tourism.hostel,osm.tourism.guest_house",
  [NAVI_CATEGORY_TOILETS] = "osm.amenity.toilets",
};

const NaviPoiKind *
navi_poi_kinds (guint *n_kinds)
{
  *n_kinds = G_N_ELEMENTS (kinds);
  return kinds;
}

const NaviPoiKind *
navi_poi_kind (const char *group)
{
  for (guint i = 0; group && i < G_N_ELEMENTS (kinds); i++)
    if (!strcmp (kinds[i].group, group))
      return &kinds[i];
  return NULL;
}

const char *
navi_poi_group (const char *key, const char *value)
{
  if (!key)
    return NULL;
  for (guint i = 0; i < G_N_ELEMENTS (groups); i++)
    if (!strcmp (groups[i].key, key) && (!groups[i].value || !g_strcmp0 (groups[i].value, value)))
      return groups[i].group;
  return NULL;
}

const char *
navi_poi_photon_categories (NaviCategory category)
{
  return category > NAVI_CATEGORY_NONE && category < G_N_ELEMENTS (photon) ? photon[category] : NULL;
}

gboolean
navi_poi_in_category (NaviCategory category, const char *group)
{
  const NaviPoiKind *k = navi_poi_kind (group);
  return k && k->category == category;
}

import json, sys, urllib.parse, urllib.request
UA = {"User-Agent": "Mozilla/5.0 (X11; Linux x86_64) vibeos-assets-build"}
q = sys.argv[1]
url = "https://images-api.nasa.gov/search?" + urllib.parse.urlencode({"q": q, "media_type": "image"})
d = json.load(urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=60))
for it in d["collection"]["items"][:int(sys.argv[2]) if len(sys.argv) > 2 else 12]:
    m = it["data"][0]
    print(m.get("nasa_id"), "|", m.get("title", "")[:70], "|", m.get("center"), "|", (m.get("secondary_creator") or m.get("photographer") or "")[:50], "|", m.get("date_created", "")[:10])

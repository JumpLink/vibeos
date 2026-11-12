# nasa_item.py ID [download-dir]: an NASA Image Library item's title, center, credits and description, and
# with a download dir its original image (~orig) saved there as ID.<ext> plus ID.meta.json
import json, os, sys, urllib.parse, urllib.request
UA = {"User-Agent": "Mozilla/5.0 (X11; Linux x86_64) neo-assets-build"}
def get(url):
    return urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=120)
nid = sys.argv[1]
d = json.load(get("https://images-api.nasa.gov/search?" + urllib.parse.urlencode({"nasa_id": nid})))
items = d["collection"]["items"]
if not items:
    print(nid, "NOT FOUND"); sys.exit(1)
m = items[0]["data"][0]
print("==", nid, "|", m.get("title"), "|", m.get("center"), "|", m.get("date_created", "")[:10])
print("   creator:", m.get("secondary_creator"), "| photographer:", m.get("photographer"), "| location:", m.get("location"))
print("   desc:", " ".join((m.get("description") or "").split())[:700])
assets = [a["href"] for a in json.load(get("https://images-api.nasa.gov/asset/" + urllib.parse.quote(nid)))["collection"]["items"]]
orig = [a for a in assets if "~orig" in a] or [a for a in assets if "~large" in a]
print("   assets:", [os.path.basename(a) for a in assets])
if len(sys.argv) > 2 and orig:
    url = orig[0].replace("http://", "https://")
    ext = os.path.splitext(urllib.parse.urlparse(url).path)[1]
    out = os.path.join(sys.argv[2], nid + ext)
    with get(url.replace(" ", "%20")) as r, open(out, "wb") as f:
        f.write(r.read())
    json.dump({"nasa_id": nid, "data": m, "source": url}, open(os.path.join(sys.argv[2], nid + ".meta.json"), "w"), indent=1)
    print("   saved", out, os.path.getsize(out))

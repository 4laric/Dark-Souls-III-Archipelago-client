import urllib.request, sys
candidates = [
    "https://raw.githubusercontent.com/soulsmods/Paramdex/master/ER/Defs/EquipParamGoods.xml",
    "https://raw.githubusercontent.com/soulsmods/Paramdex/main/ER/Defs/EquipParamGoods.xml",
    "https://raw.githubusercontent.com/vawser/Smithbox/main/src/Smithbox.Data/Assets/PARAM/ER/Defs/EquipParamGoods.xml",
    "https://raw.githubusercontent.com/soulsmods/DSMapStudio/master/src/StudioCore/Assets/Paramdex/ER/Defs/EquipParamGoods.xml",
]
for url in candidates:
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "curl/8"})
        data = urllib.request.urlopen(req, timeout=30).read()
        if b"PARAMDEF" in data and b"vagrant" in data.lower():
            open("EquipParamGoods.xml", "wb").write(data)
            print(f"OK {url}\n  {len(data)} bytes")
            sys.exit(0)
        else:
            print(f"skip (no paramdef/vagrant marker): {url}")
    except Exception as e:
        print(f"fail {url}\n  {type(e).__name__}: {e}")
print("NONE matched"); sys.exit(1)

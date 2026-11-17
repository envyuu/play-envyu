#!/usr/bin/env python3
"""Check that both patchers (patcher/play_patch.py and the browser docs/patcher.js) produce the built .ppf byte for byte.

usage: test_patchers.py STOCK.ppf MANIFEST.json BUILT.ppf
The JavaScript check runs the real apply() from docs/patcher.js in JavaScriptCore (macOS `jsc`) or Node, if available.
"""
import base64, json, os, shutil, subprocess, sys, tempfile, importlib.util

HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)


def fnv(b):
    h = 0x811C9DC5
    for x in b: h = ((h ^ x) * 0x01000193) & 0xFFFFFFFF
    return h


def main():
    stock, man_p, built = sys.argv[1:4]
    m = json.load(open(man_p)); raw = open(stock, "rb").read(); want = open(built, "rb").read()
    spec = importlib.util.spec_from_file_location("pp", os.path.join(ROOT, "patcher", "play_patch.py"))
    pp = importlib.util.module_from_spec(spec); spec.loader.exec_module(pp)
    patches = {}
    for p in m["patches"]:
        a = int(p["addr"], 16)
        for k, b in enumerate(bytes.fromhex(p["bytes"])): patches[a + k] = b
    payload = base64.b64decode(m["payload_base64"]); pa = int(m["payload_addr"], 16)
    out = pp.apply(raw, patches, pa, payload)
    ok_py = out == want
    print(f"{'ok   ' if ok_py else 'FAIL '} play_patch.py == build ({len(out)} bytes, payload {len(payload)} bytes, ends {pa + len(payload):#x})")

    js = open(os.path.join(ROOT, "docs", "patcher.js")).read()
    i = js.index("  const hex2"); j = js.index("  async function run(")
    body = js[i:j]
    eng = shutil.which("node") or ("/System/Library/Frameworks/JavaScriptCore.framework/Versions/Current/Helpers/jsc"
                                   if os.path.exists("/System/Library/Frameworks/JavaScriptCore.framework/Versions/Current/Helpers/jsc") else None)
    if not eng:
        print("skip  no JavaScript engine (node or macOS jsc)"); return 0 if ok_py else 1
    with tempfile.TemporaryDirectory() as t:
        pj = os.path.join(t, "p.json"); json.dump({"patches": [[a, v] for a, v in patches.items()], "pa": pa}, open(pj, "w"))
        pl = os.path.join(t, "payload.bin"); open(pl, "wb").write(payload)
        harness = os.path.join(t, "h.js")
        node = eng.endswith("node")
        rd = ("const fs=require('fs');const RB=f=>new Uint8Array(fs.readFileSync(f));const RT=f=>fs.readFileSync(f,'utf8');const OUT=console.log;"
              if node else "const RB=f=>new Uint8Array(read(f,'binary'));const RT=f=>read(f);const OUT=print;")
        open(harness, "w").write(rd + body + f"""
const P = JSON.parse(RT({json.dumps(pj)}));
const patches = new Map(P.patches);
const res = apply(RB({json.dumps(stock)}), patches, P.pa, RB({json.dumps(pl)}));
let h = 0x811C9DC5 >>> 0; for (let k = 0; k < res.length; k++) {{ h = Math.imul((h ^ res[k]) >>> 0, 0x01000193) >>> 0; }}
OUT(res.length + ' ' + h);
""")
        r = subprocess.run([eng, harness], capture_output=True, text=True)
        got = r.stdout.strip().split()
        ok_js = len(got) == 2 and int(got[0]) == len(want) and int(got[1]) == fnv(want)
        print(f"{'ok   ' if ok_js else 'FAIL '} docs/patcher.js == build ({os.path.basename(eng)}: {r.stdout.strip() or r.stderr.strip()[:200]})")
    return 0 if ok_py and ok_js else 1


if __name__ == "__main__":
    sys.exit(main())

import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "OpenFaceFX", "tools"))
import lip_codec_research as L

# grid curve-slot -> phoneme name (OpenFaceFX SKYRIM_SLOT_MAP, inverted)
SLOT2NAME = {0:"BMP",2:"ChJSh",4:"DST",6:"Eee",8:"Eh",10:"FV",12:"i",14:"k",
             16:"N",18:"Oh",20:"OohQ",22:"Aah",24:"BigAah",26:"R",28:"Th",30:"W"}

def analyze(path, label):
    d = open(path,"rb").read()
    h = L.parse_header(d)
    toks, end = L.parse_payload(d)
    cv = L.decode_curves(d)
    # WEIGHTS only: real phoneme weights are in [0,1]; large/negative floats are
    # Hermite tangents; ~9.6e-16 is the rest sentinel. Keep only even phoneme slots.
    peak = {}
    for curve, frames in cv["grid"].items():
        if curve not in SLOT2NAME:
            continue
        ws = [v for v in frames.values() if v is not None and 1e-6 < v <= 1.0001]
        if ws:
            peak[curve] = max(ws)
    named = [(SLOT2NAME[s], s, p) for s,p in peak.items()]
    named.sort(key=lambda x:-x[2])
    rt,_ = L.roundtrip_curves(d)
    print(f"\n=== {label}  ({os.path.basename(path)}) ===")
    print(f"  frames={h['count12']} num_curves={h['num_curves']} u22={h['u22']} "
          f"end={end}/{len(d)} roundtrip={'EXACT' if rt else 'FAIL'} slots_used={len(peak)}")
    top = ", ".join(f"{n}={p:.2f}" for n,s,p in named[:6] if p>0.01)
    print(f"  top phonemes by peak weight: {top}")

base = os.path.dirname(__file__)
lip = os.path.join(base,"lipres","lip")
analyze(os.path.join(lip,"mmm.lip"), "MMM (expect BMP dominant)")
analyze(os.path.join(lip,"aaa.lip"), "AAA (expect Aah/BigAah)")
analyze(os.path.join(lip,"numbers.lip"), "NUMBERS one..five")
analyze(os.path.join(lip,"sentence.lip"), "SENTENCE quick brown fox")
van = r"C:\Playground\Skyrim\othermods\Fertility Adventures\sound\voice\fertility adventures.esp\dlc1seranavoice\fma_serana_fma_seranaannou_0008a422_1.lip"
analyze(van, "VANILLA Serana line")

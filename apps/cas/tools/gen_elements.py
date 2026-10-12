#!/usr/bin/env python3
"""
Writes apps/cas/main/cas_elements.c, the periodic table of CAS's chemistry.

    python3 apps/cas/tools/gen_elements.py [pubchem.csv]

Two sources:
  - the atomic weights below: IUPAC's standard atomic weights, as textbook
    tables print them; for an element with no stable isotope, the mass
    number of its longest-lived one (shown in brackets);
  - everything else (electron configuration, electronegativity, state,
    melting and boiling points, density, oxidation states, family) from
    PubChem's periodic table (NIH, public domain), downloaded when no CSV
    is given: https://pubchem.ncbi.nlm.nih.gov/rest/pug/periodictable/CSV
PubChem's masses are not used: some are rounded too far (Pb 207, Zn 65.4).
The names are in Spanish, the key of the translations; lang/en and lang/de
get theirs from NAMES too (--lang prints the catalogue lines).
"""
import csv
import io
import sys
import urllib.request

URL = "https://pubchem.ncbi.nlm.nih.gov/rest/pug/periodictable/CSV"

W = dict(H=1.008, He=4.0026, Li=6.94, Be=9.0122, B=10.81, C=12.011, N=14.007, O=15.999, F=18.998, Ne=20.180,
 Na=22.990, Mg=24.305, Al=26.982, Si=28.085, P=30.974, S=32.06, Cl=35.45, Ar=39.948, K=39.098, Ca=40.078,
 Sc=44.956, Ti=47.867, V=50.942, Cr=51.996, Mn=54.938, Fe=55.845, Co=58.933, Ni=58.693, Cu=63.546, Zn=65.38,
 Ga=69.723, Ge=72.630, As=74.922, Se=78.971, Br=79.904, Kr=83.798, Rb=85.468, Sr=87.62, Y=88.906, Zr=91.224,
 Nb=92.906, Mo=95.95, Tc=98, Ru=101.07, Rh=102.91, Pd=106.42, Ag=107.87, Cd=112.41, In=114.82, Sn=118.71,
 Sb=121.76, Te=127.60, I=126.90, Xe=131.29, Cs=132.91, Ba=137.33, La=138.91, Ce=140.12, Pr=140.91, Nd=144.24,
 Pm=145, Sm=150.36, Eu=151.96, Gd=157.25, Tb=158.93, Dy=162.50, Ho=164.93, Er=167.26, Tm=168.93, Yb=173.05,
 Lu=174.97, Hf=178.49, Ta=180.95, W=183.84, Re=186.21, Os=190.23, Ir=192.22, Pt=195.08, Au=196.97, Hg=200.59,
 Tl=204.38, Pb=207.2, Bi=208.98, Po=209, At=210, Rn=222, Fr=223, Ra=226, Ac=227, Th=232.04, Pa=231.04, U=238.03,
 Np=237, Pu=244, Am=243, Cm=247, Bk=247, Cf=251, Es=252, Fm=257, Md=258, No=259, Lr=266, Rf=267, Db=268,
 Sg=269, Bh=270, Hs=269, Mt=278, Ds=281, Rg=282, Cn=285, Nh=286, Fl=289, Mc=290, Lv=293, Ts=294, Og=294)

# Spanish | English | German, by atomic number
NAMES = """Hidrógeno|Hydrogen|Wasserstoff
Helio|Helium|Helium
Litio|Lithium|Lithium
Berilio|Beryllium|Beryllium
Boro|Boron|Bor
Carbono|Carbon|Kohlenstoff
Nitrógeno|Nitrogen|Stickstoff
Oxígeno|Oxygen|Sauerstoff
Flúor|Fluorine|Fluor
Neón|Neon|Neon
Sodio|Sodium|Natrium
Magnesio|Magnesium|Magnesium
Aluminio|Aluminium|Aluminium
Silicio|Silicon|Silicium
Fósforo|Phosphorus|Phosphor
Azufre|Sulfur|Schwefel
Cloro|Chlorine|Chlor
Argón|Argon|Argon
Potasio|Potassium|Kalium
Calcio|Calcium|Calcium
Escandio|Scandium|Scandium
Titanio|Titanium|Titan
Vanadio|Vanadium|Vanadium
Cromo|Chromium|Chrom
Manganeso|Manganese|Mangan
Hierro|Iron|Eisen
Cobalto|Cobalt|Cobalt
Níquel|Nickel|Nickel
Cobre|Copper|Kupfer
Zinc|Zinc|Zink
Galio|Gallium|Gallium
Germanio|Germanium|Germanium
Arsénico|Arsenic|Arsen
Selenio|Selenium|Selen
Bromo|Bromine|Brom
Kriptón|Krypton|Krypton
Rubidio|Rubidium|Rubidium
Estroncio|Strontium|Strontium
Itrio|Yttrium|Yttrium
Circonio|Zirconium|Zirconium
Niobio|Niobium|Niob
Molibdeno|Molybdenum|Molybdän
Tecnecio|Technetium|Technetium
Rutenio|Ruthenium|Ruthenium
Rodio|Rhodium|Rhodium
Paladio|Palladium|Palladium
Plata|Silver|Silber
Cadmio|Cadmium|Cadmium
Indio|Indium|Indium
Estaño|Tin|Zinn
Antimonio|Antimony|Antimon
Telurio|Tellurium|Tellur
Yodo|Iodine|Iod
Xenón|Xenon|Xenon
Cesio|Caesium|Caesium
Bario|Barium|Barium
Lantano|Lanthanum|Lanthan
Cerio|Cerium|Cer
Praseodimio|Praseodymium|Praseodym
Neodimio|Neodymium|Neodym
Prometio|Promethium|Promethium
Samario|Samarium|Samarium
Europio|Europium|Europium
Gadolinio|Gadolinium|Gadolinium
Terbio|Terbium|Terbium
Disprosio|Dysprosium|Dysprosium
Holmio|Holmium|Holmium
Erbio|Erbium|Erbium
Tulio|Thulium|Thulium
Iterbio|Ytterbium|Ytterbium
Lutecio|Lutetium|Lutetium
Hafnio|Hafnium|Hafnium
Tantalio|Tantalum|Tantal
Wolframio|Tungsten|Wolfram
Renio|Rhenium|Rhenium
Osmio|Osmium|Osmium
Iridio|Iridium|Iridium
Platino|Platinum|Platin
Oro|Gold|Gold
Mercurio|Mercury|Quecksilber
Talio|Thallium|Thallium
Plomo|Lead|Blei
Bismuto|Bismuth|Bismut
Polonio|Polonium|Polonium
Astato|Astatine|Astat
Radón|Radon|Radon
Francio|Francium|Francium
Radio|Radium|Radium
Actinio|Actinium|Actinium
Torio|Thorium|Thorium
Protactinio|Protactinium|Protactinium
Uranio|Uranium|Uran
Neptunio|Neptunium|Neptunium
Plutonio|Plutonium|Plutonium
Americio|Americium|Americium
Curio|Curium|Curium
Berkelio|Berkelium|Berkelium
Californio|Californium|Californium
Einstenio|Einsteinium|Einsteinium
Fermio|Fermium|Fermium
Mendelevio|Mendelevium|Mendelevium
Nobelio|Nobelium|Nobelium
Lawrencio|Lawrencium|Lawrencium
Rutherfordio|Rutherfordium|Rutherfordium
Dubnio|Dubnium|Dubnium
Seaborgio|Seaborgium|Seaborgium
Bohrio|Bohrium|Bohrium
Hasio|Hassium|Hassium
Meitnerio|Meitnerium|Meitnerium
Darmstatio|Darmstadtium|Darmstadtium
Roentgenio|Roentgenium|Roentgenium
Copernicio|Copernicium|Copernicium
Nihonio|Nihonium|Nihonium
Flerovio|Flerovium|Flerovium
Moscovio|Moscovium|Moscovium
Livermorio|Livermorium|Livermorium
Teneso|Tennessine|Tenness
Oganesón|Oganesson|Oganesson""".split("\n")

# no stable isotope: the mass is a mass number, in brackets
RADIO = {43, 61} | set(range(84, 119)) - {90, 91, 92}

FAMILY = {
    "Alkali metal": 0, "Alkaline earth metal": 1, "Transition metal": 2,
    "Post-transition metal": 3, "Metalloid": 4, "Nonmetal": 5, "Halogen": 6,
    "Noble gas": 7, "Lanthanide": 8, "Actinide": 9,
}
SUP = str.maketrans("0123456789", "⁰¹²³⁴⁵⁶⁷⁸⁹")


def config(s):
    """[He]2s2 2p2 -> [He] 2s² 2p²"""
    s = s.replace("(predicted)", "").replace("(calculated)", "").strip()
    core, rest = "", s
    if s.startswith("["):
        core, rest = s[:s.index("]") + 1], s[s.index("]") + 1:]
    out = []
    for t in rest.split():
        i = 1
        while i < len(t) and t[i].isdigit():
            i += 1
        # "2s2": n, letter, count
        n, l, k = t[:i], t[i:i + 1], t[i + 1:]
        out.append(n + l + k.translate(SUP))
    return (core + " " + " ".join(out)).strip()


def num(s, default="NAN"):
    try:
        return repr(float(s))
    except ValueError:
        return default


def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    names = [line.split("|") for line in NAMES]
    if "--lang" in sys.argv:
        for code, col in (("en", 1), ("de", 2)):
            print("# " + code)
            for n in names:
                print(n[0] + "\t" + n[col])
        return
    if args:
        text = open(args[0], encoding="utf-8").read()
    else:
        text = urllib.request.urlopen(URL, timeout=30).read().decode("utf-8")
    rows = list(csv.DictReader(io.StringIO(text)))
    assert len(rows) == 118
    out = [
        "/*",
        " * P4OS - CAS: the periodic table. Written by apps/cas/tools/gen_elements.py:",
        " * edit that, not this. Masses: IUPAC; the rest: PubChem (public domain).",
        " */",
        '#include "cas_chem.h"',
        "",
        "#include <math.h>",
        "",
        "const chem_el_t CHEM_EL[118] = {",
    ]
    for r in rows:
        z = int(r["AtomicNumber"])
        sym = r["Symbol"]
        state = r["StandardState"]
        st = 0 if "Solid" in state else 1 if "Liquid" in state else 2 if "Gas" in state else 0
        predicted = "Expected" in state or "predicted" in r["ElectronConfiguration"]
        ox = r["OxidationStates"].replace("-", "−").replace(" ", "")
        out.append(
            "    { %s, N_(%s), %.6g, %d, %d, %d, %s, %sf, %sf, %sf, %sf, %s },"
            % (cstr(sym), cstr(names[z - 1][0]), W[sym], 1 if z in RADIO else 0,
               FAMILY[r["GroupBlock"]], st | (4 if predicted else 0),
               cstr(config(r["ElectronConfiguration"])),
               num(r["Electronegativity"]), num(r["MeltingPoint"]), num(r["BoilingPoint"]),
               num(r["Density"]), cstr(ox)))
    out.append("};")
    out.append("")
    sys.stdout.write("\n".join(out).replace("NANf", "NAN"))


if __name__ == "__main__":
    main()

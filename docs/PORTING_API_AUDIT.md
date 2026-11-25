# Audit M0 — pokeruby (Rubis / Saphir) vers PC natif

Méthode : la skill « nds-decomp-pc-port » et le port Émeraude
(`pokeemerald-master`), dont la couche PC est reprise ici. Chiffres relevés le
2026-11-25 sur pret/pokeruby, commit `5784633ce` (« Sync a lot of player
controller with pokeemerald (#892) »).

## 1. Ce qui est identique à Émeraude

Même console, même moteur : carte mémoire GBA aux vraies adresses, exe à base
fixe 0x10000000, BIOS en C, DMA par macros, IRQ à 228 lignes / 59,7275 Hz,
moteur 2D logiciel, Flash 128 Kio (MX29L010) → fichier, RTC Seiko → horloge du
PC, moteur son MP2K. Le mixeur ARM `src/libs/m4a_1.s` est **le même code** que
celui d'Émeraude (seuls les noms de champs diffèrent) : la transcription C
d'Émeraude (`sound_pc.c`) s'applique telle quelle.

Aucune ROM n'est nécessaire : tout se construit depuis le dépôt (anglais US).

## 2. Ce qui diffère d'Émeraude

| Sujet | pokeemerald | pokeruby | Conséquence pour le port |
|---|---|---|---|
| Versions | une | **Rubis et Saphir** (`GAME_VERSION`) | option CMake `PRS_GAME_VERSION`, un exe par jeu |
| Données graphiques | `INCGFX` vers `build/assets` | `INCBIN` de fichiers générés à côté des PNG (4 992) | `gen_assets.py` réécrit (scaninc sans `-M`) |
| Variables | toutes définies en C | **84 variables EWRAM définies seulement dans le script de liaison** (`sym_ewram.txt`) | générées en C (`gen_ld_syms.py`) |
| Tas | `gHeap` (malloc.c) | pas de tas : **`gSharedMem`** (128 Kio en tête d'EWRAM) + 100 macros d'adresses fixes (`ewram.h`) | `gSharedMem` = vraie EWRAM 0x02000000 (`--defsym`) |
| Tables typées en assembleur | cartes, sons, scripts | idem **+ tables de sprites** (`data/mon_attrs.s`, `btl_attrs.s`, `pokemon_storage_system.s`, 7 petits fichiers) : modèles, images, animations, palettes | macros 64 bits dans `include/macros.inc` (`#ifdef PLATFORM_PC`) + `pp_s.py --wide` |
| Tilesets | en C | en assembleur (`data/tilesets/headers.inc`) | `struct Tileset` en `PC_PTR32` |
| Tables de fonctions asm | `.8byte` sous `#ifdef` | `gScriptCmdTable`, `gSpecials`, Cadeau Mystère | lues en 32 bits (`PC_PTR32`) et converties avant l'appel |
| asm en ligne | rare | **indices de registres pour agbcc** (`register x asm("r4")`, `asm("":::"r5")`), `asm(".space 32")` entre données, 32 fonctions `NAKED` | `pc_game_prefix.h` (`#define asm(...)`), `-DNONMATCHING`, `NAKED` → erreur de compilation si utilisée |
| Fonctions sans version C | — | `sub_80A1334`, `sub_80A1500` (écran de résumé, défilement de la description d'attaque) | traduites en C sous `PLATFORM_PC` |
| Sans-fil | librfu | aucun (câble link seulement) | rien à neutraliser de plus |

## 3. Points d'accès au matériel

- Registres : 2 654 références à 135 `REG_*` distincts.
- BIOS : `LZ77UnCompVram` 117×, `LZ77UnCompWram` 46×, `CpuSet` 43×, `RLUnCompVram` 17×,
  `CpuFastSet` 12×, `ObjAffineSet` 5×, `RLUnCompWram` 5×, `BgAffineSet` 3×,
  `RegisterRamReset` 3×, `Sqrt`, `SoftReset`, `VBlankIntrWait`, `ArcTan2`.
- DMA : 347 appels aux macros `Dma*` (`include/gba/macro.h`), plus les DMA son
  de `m4a.c` écrits directement dans les registres.
- Interruptions : `gIntrTable` (main.c) et `IntrMain` (crt0.s).

## 4. Données en assembleur

25 fichiers `data/*.s` (+ 417 chansons `.mid` → `.s` par mid2agb).
Bytecode (scripts d'événements, de combat, d'IA, d'animations) : pointeurs
`.4byte` gardés tels quels grâce à la base basse. Tables lues comme des
structures C : voir §2. `layout_check_pc.c` vérifie à la compilation chaque
taille supposée.

## 5. Communication

14 fichiers de link (câble, échanges, records, concours en link, Cadeau
Mystère). Stratégie « câble débranché » : le registre série ne répond jamais.

## 6. Outils

11 outils natifs (`gbagfx`, `preproc`, `mapjson`, `jsonproc`, `mid2agb`,
`wav2agb`, `scaninc`, `ramscrgen`, `bin2c`, `gbafix`, `rsfont`), compilés par
`src/platform_pc/tools/CMakeLists.txt` (le même qu'Émeraude).

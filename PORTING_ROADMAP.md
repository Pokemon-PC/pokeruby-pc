# Feuille de route — Pokémon Rubis / Saphir en exécutable PC natif

Objectif : `pokeruby.exe` et `pokesapphire.exe` (Windows, puis Linux) compilés
depuis la décomp pret/pokeruby, sans émulateur, avec CMake + Clang + SDL2, en
gardant la ROM GBA « matching » (`make compare` doit toujours passer).

Méthode : celle du port Émeraude (`pokeemerald-master`), dont la couche PC
(`src/platform_pc/`) est reprise ici (préfixe `PRS_`). Audit détaillé :
[docs/PORTING_API_AUDIT.md](docs/PORTING_API_AUDIT.md).

## Règles

1. **ROM GBA matching** : tout changement hors de `src/platform_pc/` est sous
   `#ifdef PLATFORM_PC` (code d'origine dans `#else`), ou une annotation vide
   sur GBA (`PC_PTR32`, `PC_APCS`). Le `Makefile` GBA ignore `src/platform_pc/`.
   Vérification : `python src/platform_pc/scripts/check_gba_sources.py`.
2. **Aucune sauvegarde ni ROM dans le dépôt.** Toutes les données du jeu
   viennent du dépôt pret (PNG, JSON, .s, .mid…) : pas besoin de ROM.
3. **Le code du jeu reste le code du jeu** : on émule le matériel GBA.
4. **Prouver avant d'affirmer** : capture d'image, journal ou autotest.

## Décisions

| Sujet | Choix | Raison |
|---|---|---|
| Couche PC | celle d'Émeraude, adaptée | même console, même moteur (mixeur `m4a_1.s` identique) |
| Versions | `-DPRS_GAME_VERSION=RUBY\|SAPPHIRE`, un exe et un dossier de build par jeu | comme HeartGold/SoulSilver ; sauvegardes `save_ruby.bin` / `save_sapphire.bin` |
| Variables du script de liaison | générées en C (`gen_ld_syms.py`), taille ×2 | 84 variables EWRAM n'existent que dans `sym_ewram.txt` |
| `gSharedMem` | la vraie EWRAM (0x02000000), symbole absolu | adresses EWRAM fixes du jeu (`EWRAM + 0x9000`) |
| Tables de sprites en asm | macros 64 bits (`include/macros.inc`) + `pp_s.py --wide` | modèles/images/animations lus comme des structs C à pointeurs |
| Tailles de structs | `PC_APCS` (aligné sur 4) là où l'asm le suppose | agbcc arrondit toute struct à 4 octets |
| asm ARM dans le C | `pc_game_prefix.h` (`#define asm(...)`) + `-DNONMATCHING` | indices de registres sans effet sur PC |
| Fonctions sans version C | traduites en C sous `PLATFORM_PC` | 2 tâches de l'écran de résumé |

## Jalons

| Jalon | Contenu | Critère de sortie | État |
|---|---|---|---|
| M0 Audit | différences pokeruby / pokeemerald | `docs/PORTING_API_AUDIT.md` | ✅ |
| M1 Outils + données | outils natifs, `gen_assets.py` | 7 922 fichiers générés | ✅ |
| M2 Compilation + matériel | CMake, couche matériel | autotest CTest vert | ✅ (64/64) |
| M3 Démarrage | copyright, intro, titre | écran titre sans plantage | ✅ |
| M4 Rendu | BG, sprites, fenêtres, effets | intro, titre, monde justes | ✅ |
| M5 Son | MP2K en C | musique de l'intro non muette | ✅ (RMS 1 404 ; écoute à confirmer) |
| M6 Jeu | nouvelle partie, monde, combat, menus, sauvegarde, config, paquet | tests CTest | ✅ Rubis 4/4, Saphir 4/4 |
| M7 Linux | WSL Ubuntu | tests verts sous Linux | ✅ Rubis 5/5, Saphir 5/5 (macOS non testé) |
| M8 Langues | anglais (dépôt) **et** français (ROM FR du joueur, comme Émeraude), `config.ini [game] language = auto\|fr\|en`, `--lang`, `PRS_LANG` | les deux langues testées dans **Rubis et Saphir** | ✅ `game_french` vert ×2 (Windows et Linux) |

## Journal

- **2026-11-25** — Démarrage (session parallèle à Rouge Feu et Noire 2).
  - M0 : audit (`docs/PORTING_API_AUDIT.md`). pokeruby est entièrement en C
    (plus d'`asm/`) mais garde beaucoup de tables typées en assembleur.
  - M1 : couche PC d'Émeraude copiée (`PEM_` → `PRS_`), outils compilés,
    `gen_assets.py` réécrit (le preproc de pokeruby n'a pas `INCGFX`/`-g`,
    scaninc pas `-M`) : 7 922 fichiers en ~1 h (machine partagée).
  - M2 : autotest 62/62 puis 64/64 (division par zéro ajoutée).
    `check_gba_sources.py` : base = commit pret `5784633ce`, `PC_APCS` ignoré.
  - Classes de bugs propres à Rubis (toutes traitées) :
    1. **ordre de la table d'IRQ** : `crt0.s` de Rubis teste Série, Timer3,
       H-Blank, V-Blank, V-Count (Émeraude : V-Count d'abord). Symptôme :
       écran blanc, fondu jamais fini (la H-Blank était appelée à la place de
       la V-Blank). → `irq_pc.c`, autotest.
    2. **variables définies seulement par le script de liaison** → `gen_ld_syms.py`.
    3. **tables de sprites en asm** (`mon_attrs.s`, `btl_attrs.s`…) →
       macros 64 bits + `--wide` ; deux tables mixtes en `#ifdef` (`btl_attrs.s`).
    4. **taille des structs arrondie à 4 sur GBA** (`MonCoords`, `TrainerReward`,
       `AffineAnimCmd`) → `PC_APCS`, vérifié dans `layout_check_pc.c`.
    5. **division par zéro** : la GBA n'a pas d'instruction de division,
       `__divsi3` renvoie 0 ; x86 lève #DE (ex. `Task_IntroSpinAndZoomPokeball`,
       premier tour). Traitée à la racine dans le gestionnaire d'exceptions
       (`pc_emulate_divide_by_zero`, Windows et Linux), signalée une fois par site.
    6. **structs à pointeurs posées à des adresses EWRAM fixes** (`ewram.h`) :
       elles grossissent et débordent sur la zone voisine. Détecteur
       `find_ewram_overlaps.py` (tailles calculées par Clang pour ARM/APCS et
       pour le PC) : 2 cas, les piles de combat (`B_BATTLESCRIPTS_STACK` écrasait
       `B_FUNCTION_STACK` → appel via NULL au 1er tour) → déplacées après
       `gSharedMem` sur PC. Reste à vérifier à la main : `struct EvoInfo`.
    7. constantes C aliasées par `constants/version.inc` (`.set MUS_EVIL_TEAM,
       MUS_ENCOUNTER_MAGMA`) → définies dans `link_pc.c` (COFF).
    8. préprocesseur des données : **pas** de `-DENGLISH` (sinon `ENGLISH` → `1`
       dans le texte asm) ; les versions sont des `--defsym` de l'assembleur.
  - Accès via NULL vus : `InitBackupMapLayoutConnections` (bug documenté du jeu,
    carte du camion sans connexions ; lire 0 = même effet que sur GBA).
  - **Vu en jeu** (captures) : copyright → intro (Latios, Brendan, Poussifeu
    contre Gobou) → écran titre Rubis (Groudon) → menu → discours du Prof. Seko
    → nom → camion → Bourg-en-Vol → maison ; **combat sauvage complet**
    (crochet `PRS_TEST=battle@N` : Gobou N5 contre Medhyèna N3, victoire, retour).
- **2026-11-25 (suite)** — Tests, Saphir, français.
  - Classe 6 bis : une table de textes (`gAbilityDescriptions`) incluse dans un
    fichier élargi (`btl_attrs.s`, `--wide`) ne doit pas être déclarée `PC_PTR32`
    côté C (vérifié : c'est le seul `.inc` de ce genre).
  - Tests CTest (`smoke_test.py` réécrit pour Rubis) : `game_smoke` (copyright →
    intro → titre → menu → Prof. Seko, musique non muette), `game_world` (camion →
    Bourg-en-Vol → maison → combat), `game_save` (sauvegarde, CONTINUE au même
    endroit, équipe, résumé sur 3 pages — dont les deux tâches traduites d'ARM),
    `hal_autotest`. **Rubis 4/4, Saphir 4/4** (`build-sapphire`, même code).
  - **M8 français** (exigence : les deux langues dans les deux versions) :
    outillage `gba_lang` d'Émeraude adapté (`scripts/lang/`) : ROM FR attendues
    Rubis `AXVF` et Saphir `AXPF` rév. 1 (16 Mio, CRC-32 vérifié), ordre de liaison
    tiré de `ld_script.txt`, défisym de la version pour assembler les scripts.
    Nouveau : tables dont la base française est connue (`KNOWN_TABLES`) → les
    778 descriptions du Pokédex placées exactement par les pointeurs des fiches.
    `strings.c` relu à la main (paires décodées, `PRS_LANG_REVIEW`) : phrases
    gardées (≥ 10 octets, mêmes variables, longueur comparable) + 21 libellés
    vérifiés ; échantillon de 30 : 28 justes.
    Résultat : **Rubis 7 253 / 8 170 textes (88,8 %), Saphir 7 268 (89,0 %)**,
    9 tables de noms (Pokémon, attaques, talents, types, classes, objets,
    dresseurs, baies, catégories du Pokédex). Restent anglais : décorations,
    textes non placés avec certitude, textes dessinés dans les images.
  - Choix : `--lang en|fr|auto`, `--rom CHEMIN`, `PRS_LANG`/`PRS_ROM`,
    `config.ini [game] language/rom`. Combat : « Medhyèna sauvage » (nom puis
    suffixe en français, sous `PLATFORM_PC` dans `battle_message.c`).
  - Vu en français (Rubis) : Maman, déménageurs, « GOBOU! Go! », « GOBOU utilise CHARGE ».
- **2026-11-25 (fin)** — Saphir en français, Linux, paquet.
  - Saphir : `fr_axpf.tsv` (7 268 textes, 89,0 %), tables dans
    `lang/fr_axpf_tables.inc`. Vu : titre « SAPPHIRE VERSION » (Kyogre), menu
    « NOUVELLE PARTIE / OPTIONS » contre « NEW GAME / OPTION » en anglais,
    Maman et combat en français.
  - Test `game_french` (si `-DPRS_FR_ROM=…`) : nouvelle partie, Maman, combat en
    français, 9 tables de noms copiées, et le même menu diffère en anglais.
  - **Windows : Rubis 5/5, Saphir 5/5. Linux (WSL Ubuntu, Clang 21) : Rubis 5/5,
    Saphir 5/5** (`src/platform_pc/scripts/build_linux.sh`, construit dans `$HOME`).
  - Linux a révélé : Clang 21 refuse d'initialiser statiquement un champ
    `PC_PTR32` avec une adresse (`gPokemonCrySongTemplate.tone`) → mis à
    l'exécution après la copie du modèle (`m4a.c`, sous `PLATFORM_PC`).
  - Paquet : `package.py` → `dist/pokeruby-pc-<commit>.zip` (les deux exe +
    README), testé dans un dossier vierge : seulement des DLL système.
  - `check_gba_sources.py` : 0 différence ; validé en injectant une ligne non
    protégée dans `main.c` (détectée), puis retirée.
  - Docs : `README_PC.md`, `docs/LAUNCHER.md` (langue/version pilotées par le
    launcher : `--lang`, `--rom`, `PRS_LANG`, `PRS_ROM`, `config.ini`).

- **2026-11-25 (soir)** — Dossier commun des ROM (`rom_dir`).
  - Les ROM de l'utilisateur sont désormais dans `D:\LUC\pokemonall\roms\` (à
    plat). Rubis et Saphir FR y ont été **copiés** depuis
    `D:\LUC\émulateur\VBA\Pokemon PC\Pokemon Rubis|Saphir\` (originaux gardés ;
    SHA-1 identiques) sous les noms `Pokemon - Version Rubis (France) (Rev 1).gba`
    et `… Saphir (France) (Rev 1).gba`.
  - `lang_pc.c` : `rom` → dossier de l'exe → `rom_dir` → dossier courant ; dans un
    dossier, la ROM est choisie par en-tête **et** CRC (une autre révision est
    sautée). `config.ini` : `rom_dir =` (vide, commenté) ; `--rom-dir`, `PRS_ROM_DIR`.
  - Tests : `-DPRS_ROM_DIR=` remplace `-DPRS_FR_ROM=` ; `game_french` vérifie que
    la ROM vient bien de ce dossier. `build_linux.sh [ROM_DIR]` (défaut
    `/mnt/d/LUC/pokemonall/roms`) écrit `rom_dir` dans le `config.ini` de chaque build.
    Configs locales : `build-pc`, `build-sapphire` → `rom_dir = D:\LUC\pokemonall\roms` ;
    Linux : `~/pokeruby-linux/{ruby,sapphire}/config.ini` → `rom_dir = /mnt/d/LUC/pokemonall/roms`.
  - Piège : avec `rom_dir` réglé, `language = auto` passe en français ; les tests
    anglais (touches calées sur le texte anglais) forcent `PRS_LANG=en`.
  - Vérifié : Windows 5/5 et Linux 5/5, Rubis et Saphir ; chaque exe lancé sans
    variable d'environnement trouve sa ROM FR dans le dossier commun par son seul `config.ini`.
  - `.gitattributes` : `*.sh` en LF (sinon `sh` refuse le script sous Linux).

## Reste à faire / limites connues

- Écoute du son, ressenti de la fluidité, manette réelle : à confirmer par l'utilisateur.
- `struct EvoInfo` (scène d'évolution) contient des pointeurs dans l'EWRAM fixe :
  l'évolution n'a pas encore été testée en jeu.
- Français : ~11 % des textes restent anglais (non placés avec certitude), ainsi
  que les noms de décorations et les textes dessinés dans les images ; quelques
  libellés courts de `strings.c` peuvent être décalés (échantillon : 28/30 justes).
- Mosaïque (effet GBA) non émulée (comme Émeraude) ; macOS non testé ; CI non écrite.
- Rien n'est commité : à faire sur demande de l'utilisateur.

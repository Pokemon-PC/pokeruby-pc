# Pokémon Rubis / Saphir — version PC native

Exécutables PC compilés depuis la décompilation pret/pokeruby, sans émulateur :
`pokeruby.exe` (Rubis) et `pokesapphire.exe` (Saphir). La ROM GBA se construit
toujours avec `make` comme avant (sources inchangées : tout le code PC est dans
`src/platform_pc/` ou sous `#ifdef PLATFORM_PC`).

Pas besoin de ROM pour jouer en anglais : toutes les données du jeu sont dans
ce dépôt. Le **français** est lu dans la ROM française du joueur (voir plus bas).

## Prérequis (Windows)

- **Clang** (llvm-mingw), **CMake** ≥ 3.20, **Ninja**
- **GNU binutils + make** (WinLibs : fournit `as` et `mingw32-make`) — l'assembleur GNU
  est nécessaire pour les données du jeu, celui de LLVM ne suffit pas
- **Python 3**

SDL2, zlib et libpng sont téléchargés et compilés automatiquement par CMake.

## Prérequis (Linux)

```
sudo apt install clang cmake ninja-build python3 binutils make libpng-dev zlib1g-dev libasound2-dev libpulse-dev libx11-dev libxext-dev libgl-dev
```

Linux sous WSL (les deux jeux et leurs tests, construits dans `$HOME`) :
`sh src/platform_pc/scripts/build_linux.sh [ROM_RUBIS_FR ROM_SAPHIR_FR]`.

## Construire

```
cmake -S src/platform_pc/tools -B build-tools -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build-tools
python src/platform_pc/scripts/gen_assets.py --make mingw32-make
cmake -B build-pc -G Ninja -DCMAKE_C_COMPILER=clang
cmake --build build-pc
cmake -B build-sapphire -G Ninja -DCMAKE_C_COMPILER=clang -DPRS_GAME_VERSION=SAPPHIRE
cmake --build build-sapphire
```

1. les outils de la décomp (`gbagfx`, `preproc`, `mapjson`…) ;
2. les données (graphismes, cartes, musique : ~7 900 fichiers, longtemps la première fois) ;
3. Rubis : `build-pc/pokeruby.exe` ; Saphir : `build-sapphire/pokesapphire.exe`.

Tests : `cd build-pc && ctest` (couche matériel ; démarrage avec son ; monde et
combat ; sauvegarde, reprise, équipe et résumé). Avec
`-DPRS_ROM_DIR=chemin/du/dossier/des/roms` à la configuration, le test
`game_french` joue aussi une nouvelle partie et un combat en français (la ROM
est cherchée dans ce dossier, comme avec `rom_dir`).

## Jouer

Lancer l'exe depuis un dossier où il peut écrire : la sauvegarde
(`save_ruby.bin` ou `save_sapphire.bin`, 128 Kio, même format que la cartouche)
et l'horloge (`save_ruby.rtc` / `save_sapphire.rtc`) y sont créées. Les deux
jeux peuvent partager un dossier.

| GBA | Clavier |
|---|---|
| A / B | X / Z |
| Start / Select | Entrée / Retour arrière |
| Croix | Flèches |
| L / R | A / S |
| Plein écran | F11 ou Alt+Entrée |

Touches, taille de fenêtre, plein écran, volume et langue se règlent dans
**`config.ini`**, créé à côté de l'exe au premier lancement.

## Langue : anglais ou français

- **Anglais** : intégré à l'exe (celui de la décomp).
- **Français** : lu au lancement dans **votre propre ROM française** du même jeu,
  révision 1 : « Pokémon Version Rubis (France) » (code `AXVF`) pour
  `pokeruby.exe`, « Version Saphir (France) » (`AXPF`) pour `pokesapphire.exe`.
  Elle est vérifiée (CRC-32) ; une autre ROM est refusée et le jeu reste en anglais.
  Ni la ROM ni ses textes ne sont dans le dépôt ou dans l'exe : seulement des
  positions (`src/platform_pc/lang/fr_axvf.tsv`, `fr_axpf.tsv`).

Choix, du plus prioritaire au moins prioritaire :

| Quoi | Comment |
|---|---|
| Ligne de commande | `pokeruby.exe --lang fr --rom-dir "D:\Jeux\roms"` (`--lang en\|fr\|auto`, `--rom FICHIER`, `--rom-dir DOSSIER`) |
| Environnement | `PRS_LANG=en\|fr\|auto`, `PRS_ROM=fichier`, `PRS_ROM_DIR=dossier` |
| `config.ini` | `[game] language = auto\|en\|fr`, `rom = fichier`, `rom_dir = dossier` |

**`rom_dir`** : le dossier commun où sont rangées vos ROM (le launcher le
règle ; vide par défaut). Ordre de recherche de la ROM française : le chemin
donné par `rom` s'il y en a un, puis le dossier de l'exe, puis `rom_dir`, puis
le dossier courant. Dans un dossier, la bonne ROM est reconnue **à son en-tête
et à sa somme de contrôle** (code `AXVF` pour Rubis, `AXPF` pour Saphir, rév. 1),
jamais à son nom : le dossier peut contenir toutes vos autres ROM.

`auto` (défaut) : français si la ROM française est trouvée, anglais sinon. Le
journal (sortie standard) indique ce qui a été appliqué : `lang: French from …`
ou `lang: English`.

Limites : les textes que l'outil n'a pas pu placer avec certitude restent en
anglais (un texte faux serait pire), ainsi que les textes dessinés dans les
images (logo du titre…) et les noms de décorations.

## Variables d'environnement (tests et débogage)

| Variable | Effet |
|---|---|
| `PRS_HEADLESS=1`, `PRS_MUTE=1`, `PRS_FAST=1` | sans fenêtre, sans son, sans cadence |
| `PRS_EXIT_AFTER=N` | quitter après N images |
| `PRS_SHOT=F:x.bmp,...` | capture d'écran à l'image F |
| `PRS_INPUT=F1-F2:A+UP,F3-F4/P:A` | touches scriptées |
| `PRS_TRACE=1` | carte, position, contrôle du joueur à chaque changement |
| `PRS_TEST=givemon@N,battle@N` | donne un Gobou / lance un combat sauvage |
| `PRS_WAV=x.wav` | enregistre le son |
| `PRS_SAVE=fichier`, `PRS_RTC=fichier` | autres fichiers de sauvegarde / horloge |
| `PRS_REGS=F`, `PRS_PEEK=ADDR:LEN`, `PRS_WATCH=ADDR` | registres, mémoire, point d'arrêt matériel |
| `PRS_HANG_EXIT=1` | quitter si le jeu ne produit plus d'image (chien de garde) |

Paquet à distribuer : `python src/platform_pc/scripts/package.py` (exe + README ;
jamais de sauvegarde, de ROM ni de config).

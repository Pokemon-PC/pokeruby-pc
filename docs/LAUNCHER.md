# Intégration avec le launcher

Le choix de la langue (et de la version) se fait **dans le launcher de
l'utilisateur**, pas dans le jeu : comme pour Émeraude et HeartGold/SoulSilver,
le jeu n'a pas de menu de langue, seulement des réglages simples que le
launcher pilote.

## Ce que le jeu offre au launcher

| Quoi | Comment | Remarques |
|---|---|---|
| Version | lancer `pokeruby.exe` (Rubis) ou `pokesapphire.exe` (Saphir) | un exe par jeu ; ils peuvent partager un dossier |
| Langue | `--lang en\|fr\|auto` | ou `PRS_LANG=…`, ou `config.ini [game] language = …` (dans cet ordre de priorité) |
| Dossier des ROM | `--rom-dir DOSSIER` | ou `PRS_ROM_DIR=…`, ou `[game] rom_dir = …` (vide par défaut) : le dossier commun des ROM |
| ROM française (fichier précis) | `--rom CHEMIN` | ou `PRS_ROM=…`, ou `[game] rom = …` |
| Sauvegarde | `PRS_SAVE=fichier` | défaut `save_ruby.bin` / `save_sapphire.bin` ; même sauvegarde pour les deux langues |
| Fenêtre | `[video] scale`, `fullscreen` | |

Ordre de recherche de la ROM française : `rom` (fichier précis) → dossier de
l'exe → `rom_dir` → dossier courant. Dans un dossier, la ROM est reconnue à son
en-tête (`AXVF` / `AXPF`) et à sa somme de contrôle, pas à son nom.

`auto` = français si la ROM française **du même jeu** est trouvée, anglais sinon.

## Comment marche le français

- L'exe contient le jeu anglais complet (construit depuis la décomp).
- Le français est lu **au lancement** dans la ROM française du joueur, révision 1 :
  « Pokémon Version Rubis (France) », code `AXVF`, CRC-32 `9F981F72`, pour
  `pokeruby.exe` ; « Version Saphir (France) », `AXPF`, CRC-32 `2AE49146`,
  pour `pokesapphire.exe`. Une autre ROM est refusée et le jeu reste en anglais.
- Positions des textes : `src/platform_pc/lang/fr_axvf.tsv`, `fr_axpf.tsv`
  (aucun texte de Nintendo) ; ~89 % des textes et 9 tables de noms.
- Pour savoir quelle langue a été appliquée : ligne `[prs] lang: French from …`
  ou `[prs] lang: English…` sur la sortie standard.

## Ce qui ne doit jamais passer par le launcher

La ROM du joueur : il la fournit lui-même. Un launcher qui distribue la ROM
distribue le jeu.

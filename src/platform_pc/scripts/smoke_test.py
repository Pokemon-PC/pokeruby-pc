#!/usr/bin/env python3
"""End-to-end checks of the PC build (run by CTest), on the real game without
a window, fast, with scripted keys (PRS_INPUT) and captures (PRS_SHOT).

  smoke_test.py GAME_EXE WORK_DIR            boot -> copyright -> intro -> title
                                             -> main menu -> new game; sound
  smoke_test.py --world GAME_EXE WORK_DIR    new game -> truck -> Littleroot Town
                                             -> house -> wild battle
  smoke_test.py --save GAME_EXE WORK_DIR     new game -> save; then Continue,
                                             party, summary (3 pages)
  smoke_test.py --french GAME_EXE WORK_DIR ROM_DIR   new game and a battle in
                                             French, text from the player's French
                                             ROM, found in ROM_DIR (rom_dir) by its
                                             header (never part of the repository)

Frame numbers are those of pokeruby / pokesapphire (US English); the save
file is save_ruby.bin or save_sapphire.bin after the executable's name.
Every run fails on a crash or a hang (watchdog).
"""
import array
import math
import os
import subprocess
import sys
import wave

# Title screen from frame ~3220; A at 3700 opens the main menu (NEW GAME);
# Prof. Birch's speech, the name (A x7), the speech's end: A every 25 frames.
NEW_GAME = "3700-3705:A,4000-8000/25:A"
# The truck (25.40) is controllable at ~8840; RIGHT leaves it into Littleroot
# Town (0.9); Mom's text and the house (1.0), controllable at ~11110.
TO_HOUSE = NEW_GAME + ",8900-9200:RIGHT,9300-11100/40:A"
# In the house: Start -> POKEMON, BAG, <name>, SAVE (3 x DOWN) -> A -> YES.
SAVE_KEYS = ",11200-11203:START,11300-11303:DOWN,11340-11343:DOWN,11380-11383:DOWN," \
            "11450-11453:A,11550-11553:A,11800-11803:A,12000-12003:A"


def frame_bytes(path):
    with open(path, "rb") as f:
        return f.read()[54:]   # skip the BMP header


def colors(path):
    data = frame_bytes(path)
    return len({data[i:i + 3] for i in range(0, len(data) - 2, 3)})


def run_game(exe, work, env_extra, timeout=1500):
    env = dict(os.environ, PRS_HEADLESS="1", PRS_FAST="1", PRS_HANG_EXIT="1", PRS_TRACE="1", **env_extra)
    env.setdefault("PRS_MUTE", "1")
    # The scripted keys are timed on the English text: unless a test asks for
    # French, English even if config.ini (rom_dir) would find the French ROM.
    env.setdefault("PRS_LANG", "en")
    r = subprocess.run([exe], cwd=work, env=env, capture_output=True, text=True, timeout=timeout)
    return r.returncode, r.stdout + r.stderr


def trace_lines(log):
    return [l for l in log.splitlines() if l.startswith("[prs] trace@")]


def check_frames(work, names, errors):
    for n in names:
        p = os.path.join(work, n + ".bmp")
        if not os.path.exists(p):
            errors.append(f"no {n} frame")
            continue
        c = colors(p)
        print(f"{n}: {c} colours")
        if c < 16:
            errors.append(f"{n} frame looks empty ({c} colours)")


def report(errors, log, ok):
    for e in errors:
        print("FAIL", e)
    if errors:
        print(log[-3000:])
        return 1
    print(ok)
    return 0


def save_name(exe):
    return "save_sapphire.bin" if "sapphire" in os.path.basename(exe).lower() else "save_ruby.bin"


def main_smoke(exe, work):
    rc, log = run_game(exe, work, dict(
        PRS_MUTE="0", PRS_WAV="smoke.wav", PRS_EXIT_AFTER="4500", PRS_INPUT=NEW_GAME,
        PRS_SHOT="100:copyright.bmp,1200:intro.bmp,3450:title1.bmp,3650:title2.bmp,"
                 "3900:menu.bmp,4400:birch.bmp"))
    errors = []
    if rc != 0 or "[prs:crash]" in log or "[prs:hang]" in log:
        errors.append(f"exit {rc}, crash/hang in log")
    check_frames(work, ("copyright", "intro", "title1", "title2", "menu", "birch"), errors)
    if not errors:
        t1, t2, menu, birch = (frame_bytes(os.path.join(work, n + ".bmp"))
                               for n in ("title1", "title2", "menu", "birch"))
        # The title screen must hold until a key (it needs the music
        # sequencer): no change of screen (main callback) between the two
        # captures. Its background (lava, Groudon's glow) is animated, so
        # the pixels themselves change.
        changes = [int(l.split("@")[1].split()[0]) for l in trace_lines(log)]
        if any(3450 <= f <= 3650 for f in changes) or not any(3150 <= f < 3450 for f in changes):
            errors.append("title screen did not hold until A")
        if menu == t2 or birch == menu:
            errors.append("A did not change screens")
    wav = os.path.join(work, "smoke.wav")
    if os.path.exists(wav):
        w = wave.open(wav, "rb")
        a = array.array("h")
        a.frombytes(w.readframes(w.getnframes()))
        sr = w.getframerate()
        w.close()
        seg = a[2 * 8 * sr:2 * 14 * sr]    # 8 s .. 14 s: intro music
        rms = math.sqrt(sum(x * x for x in seg) / max(1, len(seg)))
        print(f"intro music RMS {rms:.0f}")
        if rms < 200:
            errors.append(f"intro music silent (RMS {rms:.0f})")
    else:
        errors.append("no recording")
    return report(errors, log, "game smoke test passed")


def main_world(exe, work):
    rc, log = run_game(exe, work, dict(
        PRS_EXIT_AFTER="13500", PRS_TEST="battle@11150",
        PRS_INPUT=TO_HOUSE + ",11400-13400/45:A",
        PRS_SHOT="8850:truck.bmp,9700:town.bmp,11000:house.bmp,11900:battle.bmp,12800:battle2.bmp"))
    errors = []
    if rc != 0 or "[prs:crash]" in log or "[prs:hang]" in log:
        errors.append(f"exit {rc}, crash/hang in log")
    trace = trace_lines(log)
    for label, map_id in (("the truck", "map=25.40 "), ("Littleroot Town", "map=0.9 "), ("the house", "map=1.0 ")):
        if not any(map_id in l for l in trace):
            errors.append(f"never reached {label}")
    if "test: wild battle" not in log:
        errors.append("the test battle did not start")
    check_frames(work, ("truck", "town", "house", "battle", "battle2"), errors)
    return report(errors, log, "world smoke test passed")


def main_save(exe, work):
    errors = []
    rc, log = run_game(exe, work, dict(
        PRS_EXIT_AFTER="12400", PRS_TEST="givemon@11150", PRS_INPUT=TO_HOUSE + SAVE_KEYS,
        PRS_SHOT="11250:startmenu.bmp,11700:saving.bmp"))
    if rc != 0 or "[prs:crash]" in log or "[prs:hang]" in log:
        errors.append(f"first run: exit {rc}, crash/hang")
    save = os.path.join(work, save_name(exe))
    if not os.path.exists(save) or os.path.getsize(save) != 0x20000:
        errors.append(f"no 128 KiB {save_name(exe)} after saving")
    trace = trace_lines(log)
    saved_pos = trace[-1].split()[2:4] if trace else None   # map=..., pos=...

    rc, log = run_game(exe, work, dict(
        PRS_EXIT_AFTER="5450",
        PRS_INPUT="3700-3705:A,4000-4003:A,4300-4303:START,4350-4353:A,4450-4453:A,4550-4553:A,"
                  "4750-4753:RIGHT,4900-4903:RIGHT,5200-5203:B",
        PRS_SHOT="3800:continue.bmp,4400:party.bmp,4650:summary.bmp,4800:skills.bmp,4950:moves.bmp"))
    if rc != 0 or "[prs:crash]" in log or "[prs:hang]" in log:
        errors.append(f"second run: exit {rc}, crash/hang")
    if f"flash: loaded {save_name(exe)}" not in log:
        errors.append("the save was not loaded")
    free = [l for l in trace_lines(log) if "free=1" in l]
    if not free:
        errors.append("never got control after Continue")
    elif saved_pos and free[0].split()[2:4] != saved_pos:
        errors.append(f"continued at {free[0].split()[2:4]}, saved at {saved_pos}")
    else:
        print("continued at", " ".join(free[0].split()[2:4]))
    check_frames(work, ("startmenu", "saving", "continue", "party", "summary", "skills", "moves"), errors)
    return report(errors, log, "save/continue/menus test passed")


def main_french(exe, work, rom_dir):
    errors = []
    rc, log = run_game(exe, work, dict(
        PRS_LANG="fr", PRS_ROM_DIR=rom_dir, PRS_EXIT_AFTER="13500", PRS_TEST="battle@11150",
        PRS_INPUT=TO_HOUSE + ",11400-13400/45:A",
        PRS_SHOT="3900:menu_fr.bmp,4400:birch_fr.bmp,9700:town_fr.bmp,11900:battle_fr.bmp,"
                 "12500:battle2_fr.bmp"))
    if rc != 0 or "[prs:crash]" in log or "[prs:hang]" in log:
        errors.append(f"exit {rc}, crash/hang")
    import re
    m = re.search(r"lang: French from (.*) \((\d+) strings", log)
    if not m or int(m.group(2)) < 4000:
        errors.append("French text not loaded" if not m else f"only {m.group(2)} French strings")
    elif os.path.normcase(os.path.dirname(os.path.abspath(m.group(1)))) != os.path.normcase(os.path.abspath(rom_dir)):
        errors.append(f"French ROM not taken from rom_dir: {m.group(1)}")
    names = re.findall(r"lang: (\w+): (\d+)/(\d+) French names", log)
    if len(names) < 8 or any(int(a) < int(b) * 0.9 for _, a, b in names):
        errors.append(f"French name tables: {names}")
    if "test: wild battle" not in log:
        errors.append("the test battle did not start")
    check_frames(work, ("menu_fr", "birch_fr", "town_fr", "battle_fr", "battle2_fr"), errors)
    # The same frame in English must differ (main menu: the text is French).
    rc2, log2 = run_game(exe, work, dict(PRS_LANG="en", PRS_EXIT_AFTER="3950", PRS_INPUT=NEW_GAME,
                                         PRS_SHOT="3900:menu_en.bmp"))
    if "lang: English" not in log2:
        errors.append("English not chosen with PRS_LANG=en")
    en, fr = (os.path.join(work, n + ".bmp") for n in ("menu_en", "menu_fr"))
    if os.path.exists(en) and os.path.exists(fr) and frame_bytes(en) == frame_bytes(fr):
        errors.append("French and English frames are identical")
    return report(errors, log, f"French test passed ({m.group(2) if m else '?'} strings, {m.group(1) if m else ''})")


def main():
    mode = None
    args = sys.argv[1:]
    if args and args[0].startswith("--"):
        mode = args.pop(0)
    exe, work = os.path.abspath(args[0]), os.path.abspath(args[1])
    os.makedirs(work, exist_ok=True)
    for f in os.listdir(work):
        if f.endswith((".bmp", ".wav", ".bin", ".rtc")):
            os.remove(os.path.join(work, f))
    if mode == "--world":
        return main_world(exe, work)
    if mode == "--save":
        return main_save(exe, work)
    if mode == "--french":
        return main_french(exe, work, args[2])
    return main_smoke(exe, work)


if __name__ == "__main__":
    sys.exit(main())

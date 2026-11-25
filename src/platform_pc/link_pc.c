/*
 * link_pc.c - link cable: "cable unplugged", and the few symbols that only
 * ARM code or the linker script used to provide.
 *
 * The C parts of the link code (link.c...) are compiled as-is: with nothing
 * answering on the serial port they report no partner, and the game shows
 * its own "no connection" messages. Only the ARM-only pieces left out of the
 * build are replaced here:
 *   - src/libs/libgcnmultiboot.s (GameCube link for Pokemon Colosseum)
 */
#include <string.h>

#include "platform_pc.h"

#include "global.h"
#include "libgncmultiboot.h"
#include "constants/global.h"
#include "constants/items.h"
#include "constants/opponents.h"
#include "constants/songs.h"
#include "constants/species.h"

/* ---- libgcnmultiboot.s: no GameCube on the other end -------------------- */
void GameCubeMultiBoot_Init(struct GcmbStruct *pStruct) {
    memset(pStruct, 0, sizeof(*pStruct));
}
void GameCubeMultiBoot_Main(struct GcmbStruct *pStruct) { (void)pStruct; }
void GameCubeMultiBoot_ExecuteProgram(struct GcmbStruct *pStruct) { (void)pStruct; }
void GameCubeMultiBoot_HandleSerialInterrupt(struct GcmbStruct *pStruct) { (void)pStruct; }
void GameCubeMultiBoot_Quit(void) {}

/* ---- constants the data files reference without defining ---------------
 * Macro .inc files alias C constants (map.inc: .equiv connection_up,
 * CONNECTION_NORTH; constants/version.inc: .set MUS_EVIL_TEAM,
 * MUS_ENCOUNTER_MAGMA...). Files that include them but not the C header keep
 * those aliases unresolved; ARM ELF gas drops unused ones, PE/COFF gas emits
 * them as external references. Define them, with their real values (none is
 * actually used by those files). Both games' names are listed. */
#define DEFINE_ABS(name, value) __asm__ volatile(".globl " #name "\n.set " #name ", %c0" ::"i"(value))
/* #name before expansion: the symbol is the macro's name, its value the macro */
#define ABS(name) __asm__ volatile(".globl " #name "\n.set " #name ", %c0" ::"i"(name));

__attribute__((used)) static void prs_define_asm_constants(void) {
    /* gSharedMem (an EWRAM_DATA array of main.c on the GBA, the first EWRAM
     * variable: 0x02000000) is the real EWRAM reserved by the HAL, so that
     * the game's fixed EWRAM addresses (EWRAM + 0x9000...) alias it as on
     * hardware. */
    DEFINE_ABS(gSharedMem, 0x02000000);

    ABS(CONNECTION_SOUTH) ABS(CONNECTION_NORTH) ABS(CONNECTION_WEST)
    ABS(CONNECTION_EAST) ABS(CONNECTION_DIVE) ABS(CONNECTION_EMERGE)
    /* trainerbattle_* macros test `music == TRUE` with music=NO_MUSIC, an
     * undefined symbol (0 at assembly time) */
    DEFINE_ABS(NO_MUSIC, 0);

    /* constants/version.inc, right-hand sides of .set */
    ABS(ITEM_BLUE_ORB) ABS(ITEM_RED_ORB) ABS(MUS_ENCOUNTER_AQUA) ABS(MUS_ENCOUNTER_MAGMA)
    ABS(SPECIES_GROUDON) ABS(SPECIES_KYOGRE) ABS(SPECIES_LATIAS) ABS(SPECIES_LATIOS)
    ABS(TRAINER_ARCHIE_2) ABS(TRAINER_ARCHIE_3) ABS(TRAINER_COURTNEY_1) ABS(TRAINER_COURTNEY_2)
    ABS(TRAINER_GRUNT_1) ABS(TRAINER_GRUNT_2) ABS(TRAINER_GRUNT_3) ABS(TRAINER_GRUNT_4)
    ABS(TRAINER_GRUNT_5) ABS(TRAINER_GRUNT_6) ABS(TRAINER_GRUNT_7) ABS(TRAINER_GRUNT_9)
    ABS(TRAINER_GRUNT_13) ABS(TRAINER_GRUNT_15) ABS(TRAINER_GRUNT_16) ABS(TRAINER_GRUNT_17)
    ABS(TRAINER_GRUNT_18) ABS(TRAINER_GRUNT_19) ABS(TRAINER_GRUNT_20) ABS(TRAINER_GRUNT_22)
    ABS(TRAINER_GRUNT_23) ABS(TRAINER_GRUNT_24) ABS(TRAINER_GRUNT_25) ABS(TRAINER_GRUNT_26)
    ABS(TRAINER_GRUNT_27) ABS(TRAINER_GRUNT_28) ABS(TRAINER_GRUNT_29) ABS(TRAINER_GRUNT_30)
    ABS(TRAINER_GRUNT_31) ABS(TRAINER_GRUNT_32) ABS(TRAINER_GRUNT_33) ABS(TRAINER_GRUNT_34)
    ABS(TRAINER_GRUNT_36) ABS(TRAINER_GRUNT_40) ABS(TRAINER_GRUNT_42) ABS(TRAINER_GRUNT_43)
    ABS(TRAINER_GRUNT_44) ABS(TRAINER_GRUNT_45) ABS(TRAINER_GRUNT_46) ABS(TRAINER_GRUNT_47)
    ABS(TRAINER_GRUNT_49) ABS(TRAINER_GRUNT_50) ABS(TRAINER_GRUNT_51) ABS(TRAINER_GRUNT_52)
    ABS(TRAINER_GRUNT_53) ABS(TRAINER_GRUNT_54) ABS(TRAINER_HECTOR_1) ABS(TRAINER_HECTOR_2)
    ABS(TRAINER_MATT_1) ABS(TRAINER_MATT_2) ABS(TRAINER_MAXIE_2) ABS(TRAINER_MAXIE_3)
    ABS(TRAINER_SHELLY_1) ABS(TRAINER_SHELLY_2) ABS(TRAINER_TABITHA_1) ABS(TRAINER_TABITHA_2)
}

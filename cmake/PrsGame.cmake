# The game's own sources (src/**/*.c, data/*.s, songs), built for the host.
#
# Each C file goes through the GBA Makefile's pipeline (cpp | preproc), run by
# src/platform_pc/scripts/pp_c.py, then is compiled as plain C. Files whose
# job is done by the PC HAL instead are left out (PRS_GAME_EXCLUDE).
#
# PRS_GAME_VERSION (RUBY or SAPPHIRE) picks the game, as GAME_VERSION does
# in the Makefile: one executable per game, pokeruby / pokesapphire.

find_package(Python3 REQUIRED COMPONENTS Interpreter)

if(PRS_GAME_VERSION STREQUAL "RUBY")
    set(PRS_EXE_NAME pokeruby)
elseif(PRS_GAME_VERSION STREQUAL "SAPPHIRE")
    set(PRS_EXE_NAME pokesapphire)
else()
    message(FATAL_ERROR "PRS_GAME_VERSION must be RUBY or SAPPHIRE (got ${PRS_GAME_VERSION})")
endif()
message(STATUS "Game: ${PRS_GAME_VERSION} -> ${PRS_EXE_NAME}")

set(PRS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(PRS_EXE_SUFFIX "")
if(WIN32)
    set(PRS_EXE_SUFFIX ".exe")
endif()
set(PRS_PREPROC ${PRS_ROOT}/tools/preproc/preproc${PRS_EXE_SUFFIX})
if(NOT EXISTS ${PRS_PREPROC})
    message(FATAL_ERROR "tools not built: cmake -S src/platform_pc/tools -B build-tools && cmake --build build-tools")
endif()
if(NOT EXISTS ${PRS_ROOT}/data/maps/headers.inc)
    message(WARNING "generated data missing: run python src/platform_pc/scripts/gen_assets.py")
endif()

# Replaced by the HAL (same symbols, see src/platform_pc/*_pc.c).
set(PRS_GAME_EXCLUDE
    src/libs/agb_flash.c src/libs/agb_flash_1m.c src/libs/agb_flash_le.c
    src/libs/agb_flash_mx.c                                 # flash_pc.c
    src/libs/siirtc.c                                       # rtc_pc.c
    src/libs/libisagbprn.c                                  # debug printing to ROM addresses
)

file(GLOB PRS_GAME_SOURCES RELATIVE ${PRS_ROOT}
     ${PRS_ROOT}/src/*.c ${PRS_ROOT}/src/*/*.c ${PRS_ROOT}/src/*/*/*.c)
list(FILTER PRS_GAME_SOURCES EXCLUDE REGEX "^src/platform_pc/")
list(REMOVE_ITEM PRS_GAME_SOURCES ${PRS_GAME_EXCLUDE})

# The Makefile's version/language/revision switches (config.mk).
set(PRS_GAME_DEFS -D${PRS_GAME_VERSION} -DREVISION=0 -DENGLISH -DDEBUG=0 -DDEBUG_FIX=0
                  -DMODERN=1 -DPLATFORM_PC=1)
set(PRS_ASM_DEFSYMS --defsym ${PRS_GAME_VERSION}=1 --defsym REVISION=0 --defsym ENGLISH=1
                    --defsym DEBUG=0 --defsym DEBUG_FIX=0 --defsym MODERN=1 --defsym PLATFORM_PC=1)

# Preprocessor flags, as CPPFLAGS in the Makefile (MODERN=1). NONMATCHING
# selects the C version of the functions the decomp still keeps in ARM
# assembly. pc_game_prefix.h neutralizes the ARM register hints (asm("r4")).
set(PRS_CPP_FLAGS
    -iquote ${PRS_ROOT}/include
    -iquote ${PRS_ROOT}/src/platform_pc
    ${PRS_GAME_DEFS} -DNONMATCHING
    -include ${PRS_ROOT}/src/platform_pc/pc_game_prefix.h
    -fms-extensions -funsigned-char    # same as the compile step (system headers test them)
    -Wno-trigraphs)

# All writable game variables go to one section (snapshot for soft reset,
# see src/platform_pc/main_pc.c). COFF: grouped section between markers.
if(WIN32)
    set(PRS_GAME_DATA_SECTION ".data$prs1")
elseif(APPLE)
    set(PRS_GAME_DATA_SECTION "__DATA,__prsgame")
else()
    set(PRS_GAME_DATA_SECTION "prs_game")
endif()

# French text (src/platform_pc/lang_pc.c): room and registry for each string
# of the map, filled at run time from the player's own French ROM of this
# game. One map per game (positions in its French ROM, no text): fr_axvf.tsv
# (Ruby), fr_axpf.tsv (Sapphire), made by scripts/lang/build_fr_map.py.
if(PRS_GAME_VERSION STREQUAL "SAPPHIRE")
    set(PRS_LANG_MAP ${PRS_ROOT}/src/platform_pc/lang/fr_axpf.tsv)
else()
    set(PRS_LANG_MAP ${PRS_ROOT}/src/platform_pc/lang/fr_axvf.tsv)
endif()
if(WIN32)
    set(PRS_LANG_SECTION ".rdata$prslang1")
elseif(APPLE)
    set(PRS_LANG_SECTION "__DATA,__prslang")
else()
    set(PRS_LANG_SECTION "prs_lang")
endif()
set(PRS_LANG_C_ARGS)
set(PRS_LANG_S_ARGS)
set(PRS_LANG_DEPS)
if(EXISTS ${PRS_LANG_MAP})
    set(PRS_LANG_C_ARGS --lang-map ${PRS_LANG_MAP} --lang-section ${PRS_LANG_SECTION})
    set(PRS_LANG_S_ARGS --lang-map ${PRS_LANG_MAP})
    set(PRS_LANG_DEPS ${PRS_LANG_MAP} ${PRS_ROOT}/src/platform_pc/scripts/lang/lang_build.py)
    message(STATUS "French text map: ${PRS_LANG_MAP}")
else()
    message(STATUS "No French text map (${PRS_LANG_MAP}): English only")
endif()

set(PRS_PP_DIR ${CMAKE_CURRENT_BINARY_DIR}/pp)
set(PRS_GAME_PP_SOURCES)
foreach(src ${PRS_GAME_SOURCES})
    string(REPLACE "/" "__" flat ${src})
    set(out ${PRS_PP_DIR}/${flat})
    add_custom_command(
        OUTPUT ${out}
        COMMAND ${Python3_EXECUTABLE} ${PRS_ROOT}/src/platform_pc/scripts/pp_c.py
                --clang ${CMAKE_C_COMPILER} --preproc ${PRS_PREPROC} --root ${PRS_ROOT}
                --out ${out} --dep ${out}.d --data-section ${PRS_GAME_DATA_SECTION} ${PRS_LANG_C_ARGS}
                -- ${PRS_CPP_FLAGS} ${PRS_ROOT}/${src}
        DEPENDS ${PRS_ROOT}/${src} ${PRS_ROOT}/src/platform_pc/scripts/pp_c.py
                ${PRS_ROOT}/src/platform_pc/pc_game_prefix.h ${PRS_LANG_DEPS}
        DEPFILE ${out}.d
        COMMENT "preproc ${src}"
        VERBATIM)
    list(APPEND PRS_GAME_PP_SOURCES ${out})
endforeach()

# Variables pokeruby defines only in its linker script (sym_ewram.txt).
set(PRS_LD_SYMS_C ${PRS_PP_DIR}/ld_syms.c)
add_custom_command(
    OUTPUT ${PRS_LD_SYMS_C}
    COMMAND ${Python3_EXECUTABLE} ${PRS_ROOT}/src/platform_pc/scripts/gen_ld_syms.py
            --clang ${CMAKE_C_COMPILER} --out ${PRS_LD_SYMS_C} --data-section ${PRS_GAME_DATA_SECTION}
            -- -iquote ${PRS_ROOT}/include ${PRS_GAME_DEFS}
            ${PRS_ROOT}/sym_ewram.txt ${PRS_ROOT}/sym_bss.txt
    DEPENDS ${PRS_ROOT}/sym_ewram.txt ${PRS_ROOT}/sym_bss.txt
            ${PRS_ROOT}/src/platform_pc/scripts/gen_ld_syms.py
    COMMENT "linker-script variables"
    VERBATIM)
list(APPEND PRS_GAME_PP_SOURCES ${PRS_LD_SYMS_C})

add_library(prs_game STATIC ${PRS_GAME_PP_SOURCES})
target_compile_options(prs_game PRIVATE
    -fno-strict-aliasing -fwrapv -funsigned-char
    -fms-extensions                           # PC_PTR32 = __ptr32 __uptr
    -fno-zero-initialized-in-bss              # zeros too go to the game data section
    -fno-common
    -w                                        # decompiled code: thousands of noise warnings
    -Werror=implicit-function-declaration
    -Werror=int-conversion
    -Werror=incompatible-pointer-types
    -Werror=return-type
    -Werror=deprecated-declarations)          # call to an ARM-only (NAKED) function
target_link_libraries(prs_game PUBLIC prs_hal_game prs_hal)

# --- data written in assembly (scripts, maps, sound) -----------------------
# Same pipeline as the Makefile's data rule (cpp gets no version macro:
# RUBY, ENGLISH... are --defsym of the assembler, as in ASFLAGS), assembled for the host by
# GNU as (pp_s.py explains why not Clang's assembler).
# llvm-mingw names its LLVM assembler "x86_64-w64-mingw32-as" too: look for
# every "as" on the PATH and keep the first one that is GNU as.
if(NOT PRS_GNU_AS)
    execute_process(COMMAND where as OUTPUT_VARIABLE _as_list ERROR_QUIET)
    if(NOT WIN32)
        execute_process(COMMAND sh -c "which -a as" OUTPUT_VARIABLE _as_list ERROR_QUIET)
    endif()
    string(REPLACE "\n" ";" _as_list "${_as_list}")
    foreach(_as ${_as_list})
        string(STRIP "${_as}" _as)
        if(_as)
            execute_process(COMMAND "${_as}" --version OUTPUT_VARIABLE _v ERROR_QUIET)
            if(_v MATCHES "GNU assembler")
                set(PRS_GNU_AS "${_as}" CACHE FILEPATH "GNU as for the host")
                break()
            endif()
        endif()
    endforeach()
endif()
if(NOT PRS_GNU_AS)
    message(FATAL_ERROR "GNU as (binutils) not found: install binutils / WinLibs, or set PRS_GNU_AS")
endif()
message(STATUS "GNU as: ${PRS_GNU_AS}")

# Files whose every .4byte is a pointer that C reads as a 64-bit pointer
# (sprite animation/image tables, callbacks, text tables): pp_s.py --wide.
set(PRS_WIDE_ASM
    data/mon_attrs.s data/btl_attrs.s data/pokemon_storage_system.s data/battle_7.s
    data/smokescreen.s data/sparkle.s data/gray_sun.s data/unused_hit.s
    data/unused_orb.s data/unused_orb2.s data/unused_paw.s)

file(GLOB PRS_DATA_ASM RELATIVE ${PRS_ROOT} ${PRS_ROOT}/data/*.s)
file(GLOB PRS_SONG_ASM RELATIVE ${PRS_ROOT} ${PRS_ROOT}/sound/songs/midi/*.s)
set(PRS_PP_S_FLAGS)
if(WIN32)
    set(PRS_PP_S_FLAGS --coff)
endif()
set(PRS_GAME_DATA_OBJS)
foreach(src ${PRS_DATA_ASM})
    string(REPLACE "/" "__" flat ${src})
    set(pps ${PRS_PP_DIR}/${flat})
    set(obj ${PRS_PP_DIR}/${flat}.obj)
    set(wide)
    if(src IN_LIST PRS_WIDE_ASM)
        set(wide --wide)
    endif()
    add_custom_command(
        OUTPUT ${obj}
        COMMAND ${Python3_EXECUTABLE} ${PRS_ROOT}/src/platform_pc/scripts/pp_s.py ${PRS_PP_S_FLAGS} ${wide}
                --clang ${CMAKE_C_COMPILER} --preproc ${PRS_PREPROC} --root ${PRS_ROOT}
                --out ${pps} --dep ${pps}.d ${PRS_LANG_S_ARGS}
                -- -iquote ${PRS_ROOT}/include -I ${PRS_ROOT}/include -DPLATFORM_PC=1 ${PRS_ROOT}/${src}
        COMMAND ${PRS_GNU_AS} ${PRS_ASM_DEFSYMS} -I ${PRS_ROOT} -I ${PRS_ROOT}/include -o ${obj} ${pps}
        DEPENDS ${PRS_ROOT}/${src} ${PRS_ROOT}/src/platform_pc/scripts/pp_s.py ${PRS_LANG_DEPS}
        DEPFILE ${pps}.d
        WORKING_DIRECTORY ${PRS_ROOT}
        COMMENT "asm ${src}"
        VERBATIM)
    list(APPEND PRS_GAME_DATA_OBJS ${obj})
endforeach()
# Songs (mid2agb output): assembled directly with -I sound, as in
# audio_rules.mk, after the ARM -> x86 syntax fixes (pp_s.py --raw). Their
# common include, include/MPlayDef.s, gets the same treatment.
set(PRS_SOUND_INC ${PRS_PP_DIR}/sound_inc)
add_custom_command(
    OUTPUT ${PRS_SOUND_INC}/MPlayDef.s
    COMMAND ${Python3_EXECUTABLE} ${PRS_ROOT}/src/platform_pc/scripts/pp_s.py ${PRS_PP_S_FLAGS} --raw
            --root ${PRS_ROOT} --out ${PRS_SOUND_INC}/MPlayDef.s --dep ${PRS_SOUND_INC}/MPlayDef.s.d
            -- ${PRS_ROOT}/include/MPlayDef.s
    DEPENDS ${PRS_ROOT}/include/MPlayDef.s ${PRS_ROOT}/src/platform_pc/scripts/pp_s.py
    VERBATIM)
foreach(src ${PRS_SONG_ASM})
    string(REPLACE "/" "__" flat ${src})
    set(pps ${PRS_PP_DIR}/${flat})
    set(obj ${pps}.obj)
    add_custom_command(
        OUTPUT ${obj}
        COMMAND ${Python3_EXECUTABLE} ${PRS_ROOT}/src/platform_pc/scripts/pp_s.py ${PRS_PP_S_FLAGS} --raw
                --root ${PRS_ROOT} --out ${pps} --dep ${pps}.d -- ${PRS_ROOT}/${src}
        COMMAND ${PRS_GNU_AS} ${PRS_ASM_DEFSYMS} -I ${PRS_SOUND_INC} -I ${PRS_ROOT}/sound -o ${obj} ${pps}
        DEPENDS ${PRS_ROOT}/${src} ${PRS_SOUND_INC}/MPlayDef.s ${PRS_ROOT}/src/platform_pc/scripts/pp_s.py
        WORKING_DIRECTORY ${PRS_ROOT}
        COMMENT "asm ${src}"
        VERBATIM)
    list(APPEND PRS_GAME_DATA_OBJS ${obj})
endforeach()
set_source_files_properties(${PRS_GAME_DATA_OBJS} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)

# --- the executable -----------------------------------------------------------
add_executable(prs_game_exe
    ${PRS_ROOT}/src/platform_pc/main_pc.c
    ${PRS_ROOT}/src/platform_pc/link_pc.c
    ${PRS_ROOT}/src/platform_pc/sound_pc.c
    ${PRS_ROOT}/src/platform_pc/layout_check_pc.c
    ${PRS_ROOT}/src/platform_pc/debug_pc.c
    ${PRS_ROOT}/src/platform_pc/lang_pc.c
    ${PRS_ROOT}/src/platform_pc/lang_tables_pc.c
    ${PRS_GAME_DATA_OBJS})
set_target_properties(prs_game_exe PROPERTIES OUTPUT_NAME ${PRS_EXE_NAME})
target_compile_definitions(prs_game_exe PRIVATE ${PRS_GAME_VERSION}=1 PRS_EXE_NAME="${PRS_EXE_NAME}")
target_link_libraries(prs_game_exe PRIVATE prs_game prs_hal_game prs_hal prs_game_flags prs::SDL2main prs::SDL2)
prs_low_address_link(prs_game_exe)
if(WIN32 AND CMAKE_C_COMPILER_LINKER_ID STREQUAL "LLD")   # lld-only option (all undefined symbols)
    target_link_options(prs_game_exe PRIVATE -Wl,--error-limit=0)
endif()

# End-to-end: boot to a new game, sound recorded (needs the generated assets).
add_test(NAME game_smoke
         COMMAND ${Python3_EXECUTABLE} ${PRS_ROOT}/src/platform_pc/scripts/smoke_test.py
                 $<TARGET_FILE:prs_game_exe> ${CMAKE_CURRENT_BINARY_DIR}/smoke)
set_tests_properties(game_smoke PROPERTIES TIMEOUT 600)

# Longer: new game -> truck -> Littleroot Town -> inside the house.
add_test(NAME game_world
         COMMAND ${Python3_EXECUTABLE} ${PRS_ROOT}/src/platform_pc/scripts/smoke_test.py --world
                 $<TARGET_FILE:prs_game_exe> ${CMAKE_CURRENT_BINARY_DIR}/world)
set_tests_properties(game_world PROPERTIES TIMEOUT 900)

# Menus, save, then continue the save.
add_test(NAME game_save
         COMMAND ${Python3_EXECUTABLE} ${PRS_ROOT}/src/platform_pc/scripts/smoke_test.py --save
                 $<TARGET_FILE:prs_game_exe> ${CMAKE_CURRENT_BINARY_DIR}/savetest)
set_tests_properties(game_save PROPERTIES TIMEOUT 900)

# Code built with the game's headers outside prs_game (HAL pieces, tests)
# sees the same version switches.
target_compile_definitions(prs_game_flags INTERFACE
    ENGLISH=1 REVISION=0 DEBUG=0 DEBUG_FIX=0)

# French: only with the player's own French ROM of this game (never part of
# the repository): cmake -DPRS_FR_ROM="path/to/Pokemon - Version Rubis (F).gba"
set(PRS_FR_ROM "" CACHE FILEPATH "Your French ROM of this game, for the game_french test")
if(PRS_FR_ROM)
    add_test(NAME game_french
             COMMAND ${Python3_EXECUTABLE} ${PRS_ROOT}/src/platform_pc/scripts/smoke_test.py --french
                     $<TARGET_FILE:prs_game_exe> ${CMAKE_CURRENT_BINARY_DIR}/frenchtest ${PRS_FR_ROM})
    set_tests_properties(game_french PROPERTIES TIMEOUT 1500)
endif()

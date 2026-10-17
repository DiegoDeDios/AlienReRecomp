/*
 * Interpreted builds: no recompiled game code. The module tables are empty;
 * the game's executables are loaded from the player's disc and run by the
 * interpreter (see g_interp_only).
 */
#include "psx.h"

const FuncEntry alien_func_table[1];
const unsigned alien_func_count = 0;
const ResumeEntry alien_resume_table[1];
const unsigned alien_resume_count = 0;
const FuncEntry slus_func_table[1];
const unsigned slus_func_count = 0;
const ResumeEntry slus_resume_table[1];
const unsigned slus_resume_count = 0;

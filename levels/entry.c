#include <ultra64.h>
#include "sm64.h"
#include "segment_symbols.h"
#include "level_commands.h"

#include "levels/intro/header.h"

#include "make_const_nonconst.h"
#include "nds/nds_bottom_ui.h"

const LevelScript level_script_entry[] = {
    INIT_LEVEL(),
    SLEEP( 2),
    BLACKOUT( FALSE),
    SET_REG( 0),

    // EXECUTE( 0x14,  _introSegmentRomStart,  _introSegmentRomEnd,  level_intro_mario_head_regular),
#ifdef NDS_BOTTOM_UI
    EXECUTE(0x14, _introSegmentRomStart, _introSegmentRomEnd, level_intro_nds_title),
#else
    EXECUTE(0x14, _introSegmentRomStart, _introSegmentRomEnd, level_intro_splash_screen),
#endif
    JUMP( level_script_entry),
};

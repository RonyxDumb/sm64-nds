#ifdef TARGET_NDS
#include "nds/nds_goddard.h"
#include "renderer.h"

/* This is the only public entry point of the transient DS module. */
__attribute__((visibility("default")))
const struct NdsGoddardApi gNdsGoddardApi = {
    1, sizeof(struct NdsGoddardApi), gdm_init, gdm_setup, gdm_maketestdl,
    gdm_gettestdl, gd_copy_p1_contpad, gd_vblank, gd_sfx_to_play
};
#endif

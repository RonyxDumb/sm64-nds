#ifndef NDS_MINIMAP_H
#define NDS_MINIMAP_H

/* Bottom-screen minimap. tools/nds_minimap_render.py pre-renders every
 * area from above, with its textures, at build time; the current area's
 * image is streamed from NitroFS into reserved sub BG tile slots and shown
 * on BG1, under the HUD. Mario's HUD head icon is drawn on BG0 in a few dedicated
 * tiles, so no copy of the map is kept in RAM. */

#define NDS_MINIMAP_COL    8
#define NDS_MINIMAP_ROW    5
#define NDS_MINIMAP_SIZE   16      /* tiles per side (128 pixels) */
/* Tile slots, just below the fixed bottom HUD rows (21 * 32). */
#define NDS_MINIMAP_SLOT   (21 * 32 - NDS_MINIMAP_SIZE * NDS_MINIMAP_SIZE)
#define NDS_MINIMAP_MARKER (NDS_MINIMAP_SLOT - 9)
/* BG0 uses map block 0 (slots 0..31), BG1 map block 1 (slots 32..63). */
#define NDS_UI_BLANK_TILE  64

void nds_minimap_init(void);
/* Returns a nonzero generation while the minimap is shown. */
int nds_minimap_update(int allowed);
/* Graphics of the reserved slots were overwritten. */
void nds_minimap_invalidate(void);
/* BG0 map entries were rewritten. */
void nds_minimap_map_dirty(void);
void nds_minimap_present(unsigned short *vram, unsigned short *map);

#endif

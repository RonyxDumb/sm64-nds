#ifndef NDS_MINIMAP_H
#define NDS_MINIMAP_H

/* Bottom-screen minimap, rasterized at runtime from the current area's
 * static floor collision. It needs no ROM assets: a 4bpp copy of the
 * map (6 KiB) and a few bytes of state live in RAM, and it occupies a
 * fixed range of sub BG tile slots, so Mario's marker only rewrites the
 * handful of tiles it crosses. */

#define NDS_MINIMAP_COL   9
#define NDS_MINIMAP_ROW   6
#define NDS_MINIMAP_TW    14
#define NDS_MINIMAP_TH    14
/* Last slot before the fixed bottom rows (21 * 32). */
#define NDS_MINIMAP_SLOT  (21 * 32 - NDS_MINIMAP_TW * NDS_MINIMAP_TH)
/* First palette entry: 232..247 map shades, 248..250 marker and frame. */
#define NDS_MINIMAP_PAL   232

void nds_minimap_init(void);
/* Rebuilds the map if needed and returns a nonzero generation while it
 * is shown. */
int nds_minimap_update(int allowed);
/* Graphics of the reserved slots were overwritten. */
void nds_minimap_invalidate(void);
/* Map entries of the minimap rectangle were overwritten. */
void nds_minimap_map_dirty(void);
void nds_minimap_present(unsigned short *vram, unsigned short *map);

#endif

#ifndef NDS_BOTTOM_UI_H
#define NDS_BOTTOM_UI_H

/* The port's supported asset set is US. Other targets keep their original UI. */
#if defined(__NDS__) && defined(VERSION_US)
#define NDS_BOTTOM_UI 1
void nds_bottom_ui_init(void);
void nds_bottom_ui_camera(int status);
void nds_bottom_ui_present(void);
void nds_bottom_ui_vblank(void);
void nds_bottom_ui_message(const unsigned char *text, int start, int lines,
                           int choice, int more);
#endif
#endif


#include <stdint.h>
#include <sys/types.h>
#include <X11/Xlib.h>

void graphicsinit(Display *disp, Visual *vis, Colormap cm);
void gdrawimagestripe(Drawable buf, uint32_t image_id, int start_col, int end_col, int row,
					  int x_pix, int y_pix, int cw, int ch, int reverse);
int gparsecommand(char *buf, size_t len);
void gpreviewimage(uint32_t image_id, const char *command);

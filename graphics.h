
#include <stdint.h>
#include <sys/types.h>
#include <X11/Xlib.h>

void graphicsinit(Display *disp, Visual *vis, Colormap cm);
void graphicsdeinit();
void gdrawimagestripe(Drawable buf, uint32_t image_id, int start_col, int end_col, int row,
					  int x_pix, int y_pix, int cw, int ch, int reverse);
int gparsecommand(char *buf, size_t len);
void gpreviewimage(uint32_t image_id, const char *command);

int gcheckifstilluploading();

/// The (approximate) number of active uploads. If there are active uploads then
/// it is not recommended to do anything computationally heavy.
extern int graphics_uploading;

extern int graphics_command_needs_redraw;

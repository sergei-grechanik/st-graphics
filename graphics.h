
#include <stdint.h>
#include <sys/types.h>
#include <X11/Xlib.h>

void graphicsinit(Display *disp, Visual *vis, Colormap cm);
void graphicsdeinit();
void gr_appendimagerect(Drawable buf, uint32_t image_id, int start_col, int end_col, int start_row, int end_row,
					  int x_pix, int y_pix, int cw, int ch, int reverse);
void gr_drawimagerects(Drawable buf);
int gparsecommand(char *buf, size_t len);
void gpreviewimage(uint32_t image_id, const char *command);

int gcheckifstilluploading();

/// Print additional information, draw bounding bounding boxes, etc.
extern char graphics_debug_mode;

/// The (approximate) number of active uploads. If there are active uploads then
/// it is not recommended to do anything computationally heavy.
extern char graphics_uploading;

#define MAX_GRAPHICS_RESPONSE_LEN 256

typedef struct {
	char redraw;
	char response[MAX_GRAPHICS_RESPONSE_LEN];
	char error;
} GraphicsCommandResult;

extern GraphicsCommandResult graphics_command_result;

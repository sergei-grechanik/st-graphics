
#include <stdint.h>
#include <sys/types.h>
#include <X11/Xlib.h>

/// Initialize the graphics module.
void gr_init(Display *disp, Visual *vis, Colormap cm);
/// Deinitialize the graphics module.
void gr_deinit();

/// Add an image rectangle to a list if rectangles to draw. This function may
/// actually draw some rectangles, or it may wait till more rectangles are
/// appended. Must be called between `gr_start_drawing` and `gr_finish_drawing`.
/// - `start_col` and `start_row` are zero-based.
/// - `end_col` and `end_row` are exclusive (beyond the last col/row).
/// - `reverse` indicates whether colors should be inverted.
void gr_append_imagerect(Drawable buf, uint32_t image_id, uint32_t placement_id,
			 int start_col, int end_col, int start_row, int end_row,
			 int x_pix, int y_pix, int cw, int ch, int reverse);
/// Prepare for image drawing. `cw` and `ch` are dimensions of the cell.
void gr_start_drawing(Drawable buf, int cw, int ch);
/// Finish image drawing. This functions will draw all the rectangles left to
/// draw.
void gr_finish_drawing(Drawable buf);

/// Parse and execute a graphics command. `buf` must start with 'G' and contain
/// at least `len + 1` characters (including '\0'). Returns 0 on success.
/// Additional informations is returned through `graphics_command_result`.
int gr_parse_command(char *buf, size_t len);

/// Executes `command` with the name of the file corresponding to `image_id` as
/// the argument. Executes xmessage with an error message on failure.
void gr_preview_image(uint32_t image_id, const char *command);

/// Dumps the internal state (images and placements) to stderr.
void gr_dump_state();

/// Unloads images to reduce RAM usage.
void gr_unload_images_to_reduce_ram();

/// Print additional information, draw bounding bounding boxes, etc.
extern char graphics_debug_mode;

/// Whether to display images or just draw bounding boxes.
extern char graphics_display_images;

#define MAX_GRAPHICS_RESPONSE_LEN 256

/// A structure representing the result of a graphics command.
typedef struct {
	/// Indicates if the terminal needs to be redrawn.
	char redraw;
	/// The response of the command that should be sent back to the client
	/// (may be empty if the quiet flag is set).
	char response[MAX_GRAPHICS_RESPONSE_LEN];
	/// Whether there was an error executing this command (not very useful,
	/// the response must be sent back anyway).
	char error;
	/// Whether the terminal has to create a placeholder for a non-virtual
	/// placement.
	char create_placeholder;
	/// The placeholder that needs to be created.
	struct {
		uint32_t rows, columns;
		uint32_t image_id, placement_id;
		char do_not_move_cursor;
	} placeholder;
} GraphicsCommandResult;

/// The result of a graphics command.
extern GraphicsCommandResult graphics_command_result;

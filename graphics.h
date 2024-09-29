
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
/// - `img_start_col..img_end_col` and `img_start_row..img_end_row` define the
///   part of the image to draw (row/col indices are zero-based, ends are
///   excluded).
/// - `x_col` and `y_row` are the coordinates of the top-left corner of the
///   image in the terminal grid.
/// - `x_pix` and `y_pix` are the same but in pixels.
/// - `reverse` indicates whether colors should be inverted.
void gr_append_imagerect(Drawable buf, uint32_t image_id, uint32_t placement_id,
			 int img_start_col, int img_end_col, int img_start_row,
			 int img_end_row, int x_col, int y_row, int x_pix,
			 int y_pix, int cw, int ch, int reverse);
/// Prepare for image drawing. `cw` and `ch` are dimensions of the cell.
void gr_start_drawing(Drawable buf, int cw, int ch);
/// Finish image drawing. This functions will draw all the rectangles left to
/// draw.
void gr_finish_drawing(Drawable buf);
/// Mark rows containing animations as dirty if it's time to redraw them. Must
/// be called right after `gr_start_drawing`.
void gr_mark_dirty_animations(int *dirty, int rows);

/// Parse and execute a graphics command. `buf` must start with 'G' and contain
/// at least `len + 1` characters (including '\0'). Returns 1 on success.
/// Additional informations is returned through `graphics_command_result`.
int gr_parse_command(char *buf, size_t len);

/// Executes `command` with the name of the file corresponding to `image_id` as
/// the argument. Executes xmessage with an error message on failure.
void gr_preview_image(uint32_t image_id, const char *command);

/// Executes `<st> -e less <file>` where <file> is the name of a temporary file
/// containing the information about an image and placement, and <st> is
/// specified with `st_executable`.
void gr_show_image_info(uint32_t image_id, uint32_t placement_id,
			uint32_t imgcol, uint32_t imgrow,
			char is_classic_placeholder, int32_t diacritic_count,
			char *st_executable);

/// Dumps the internal state (images and placements) to stderr.
void gr_dump_state();

/// Unloads images to reduce RAM usage.
void gr_unload_images_to_reduce_ram();

/// Executes `callback` for each image cell. `callback` may return 1 to erase
/// the cell or 0 to keep it. This function is implemented in `st.c`.
void gr_for_each_image_cell(int (*callback)(void *data, uint32_t image_id,
					    uint32_t placement_id, int col,
					    int row, char is_classic),
			    void *data);

/// Marks all the rows containing the image with `image_id` as dirty.
void gr_schedule_image_redraw_by_id(uint32_t image_id);

typedef enum {
	GRAPHICS_DEBUG_NONE = 0,
	GRAPHICS_DEBUG_LOG = 1,
	GRAPHICS_DEBUG_LOG_AND_BOXES = 2,
} GraphicsDebugMode;

/// Print additional information, draw bounding bounding boxes, etc.
extern GraphicsDebugMode graphics_debug_mode;

/// Whether to display images or just draw bounding boxes.
extern char graphics_display_images;

/// The time in milliseconds until the next redraw to update animations.
/// INT_MAX means no redraw is needed. Populated by `gr_finish_drawing`.
extern int graphics_next_redraw_delay;

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

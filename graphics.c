////////////////////////////////////////////////////////////////////////////////
//
// This file implements a subset of the kitty graphics protocol with the unicode
// placeholder extension (placing images using a special unicode symbol with
// diacritics to indicate rows and columns). Actually, unicode placeholder image
// placement is the only supported image placement method right now.
//
////////////////////////////////////////////////////////////////////////////////

#define _POSIX_C_SOURCE 200809L

#include <Imlib2.h>
#include <X11/Xlib.h>
#include <assert.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "graphics.h"

#define MAX_FILENAME_SIZE 256
#define MAX_INFO_LEN 256
#define MAX_CELL_IMAGES 1024
#define MAX_IMAGE_RECTS 20

enum ScaleMode {
	/// Preserve aspect ratio and fit to width or to height so that the
	/// whole image is visible.
	SCALE_MODE_CONTAIN = 0,
	/// Preserve aspect ratio and fit to width or to height so that the
	/// whole rectangle is covered.
	SCALE_MODE_FILL = 1
};

/// The status of an image. Each image uploaded to the terminal is cached on
/// disk. Then it is loaded to ram when needed in the ready to display form
/// (scaled to the requested box given the current cell width and height).
enum ImageStatus {
	STATUS_UNINITIALIZED = 0,
	STATUS_UPLOADING = 1,
	STATUS_UPLOADING_ERROR = 2,
	STATUS_UPLOADING_SUCCESS = 3,
	STATUS_RAM_LOADING_ERROR = 4,
	STATUS_RAM_LOADING_SUCCESS = 5,
};

enum ImageUploadingFailure {
	ERROR_OVER_SIZE_LIMIT = 1,
	ERROR_CANT_CREATE_FILE = 2,
	ERROR_UNEXPECTED_SIZE = 3,
};

/// The structure representing an image (and its "placement" at the same time
/// since we support only one placement per image).
typedef struct {
	/// The client id (the one specified with 'i='). Must be nonzero.
	uint32_t image_id;
	/// Height and width in cells.
	uint16_t rows, cols;
	/// The last time when the image was displayed or otherwise touched.
	time_t atime;
	/// The size of the corresponding file cached on disk.
	unsigned disk_size;
	/// The expected size of the image file (specified with 'S='), used to
	/// check if uploading uploading succeeded.
	unsigned expected_size;
	/// The scaling mode (see `ScaleMode`).
	char scale_mode;
	/// The status (see `ImageStatus`).
	char status;
	/// The reason of uploading failure (see `ImageUploadingFailure`).
	char uploading_failure;
	/// Whether failures and successes should be reported ('q=').
	char quiet;
	/// The file corresponding to the on-disk cache, used when uploading.
	FILE *open_file;
	/// The image appropriately scaled and loaded into RAM.
	Imlib_Image scaled_image;
	/// The dimensions of the cell used to scale the image. If cell
	/// dimensions are changed (font change), the image will be rescaled.
	uint16_t scaled_cw, scaled_ch;
} CellImage;

/// A rectangular piece of an image to be drawn.
typedef struct {
	uint32_t image_id;
	/// The position of the rectangle in pixels.
	int x_pix, y_pix;
	/// The part of the whole image to be drawn, in cells. Starts are
	/// zero-based, ends are exclusive.
	int start_col, end_col, start_row, end_row;
	/// The current cell width and height in pixels.
	int cw, ch;
	/// Whether colors should be inverted.
	int reverse;
} ImageRect;

static CellImage *gr_find_image(uint32_t image_id);
static void gr_get_image_filename(CellImage *img, char *out, size_t max_len);
static void gr_delete_image(CellImage *img);
static void gr_check_limits();
static char *gr_base64dec(const char *src, size_t *size);

/// The array of image rectangles to draw. It is reset each frame.
static ImageRect image_rects[MAX_IMAGE_RECTS] = {{0}};
/// The known images (including the ones being uploaded).
static CellImage cell_images[MAX_CELL_IMAGES] = {{0}};
/// The number of known images.
static int images_count = 0;
/// The total size of all image files stored in the on-disk cache.
static int32_t cell_images_disk_size = 0;
/// The total size of all images loaded into ram.
static int32_t cell_images_ram_size = 0;
/// The id of the last loaded image.
static uint32_t last_image_id = 0;
/// Current cell width and heigh in pixels.
static int current_cw = 0, current_ch = 0;
/// The id of the currently uploaded image (when using direct uploading).
static uint32_t current_upload_image_id = 0;
/// The time when the latest chunk of data was appended to an image.
static time_t last_uploading_time = 0;
/// The time when the current frame drawing started (used for debugging fps).
static clock_t drawing_start_time;

/// The directory where the on-disk cache files are stored.
static char temp_dir[] = "/tmp/st-images-XXXXXX";

/// The max size of a single image file, in bytes.
static size_t max_image_disk_size = 20 * 1024 * 1024;
/// The max size of the on-disk cache, in bytes.
static int max_total_disk_size = 300 * 1024 * 1024;
/// The max total size of all images loaded into RAM.
static int max_total_ram_size = 300 * 1024 * 1024;
/// The internal cache size of imlib2.
static size_t imlib_cache_size = 4 * 1024 * 1024;

/// The table used for color inversion.
static unsigned char reverse_table[256];

// Declared in the header.
char graphics_debug_mode = 0;
char graphics_uploading = 0;
GraphicsCommandResult graphics_command_result = {0};

////////////////////////////////////////////////////////////////////////////////
// Basic image management functions (create, delete, find, etc).
////////////////////////////////////////////////////////////////////////////////

/// Finds the image corresponding to the client id. Returns NULL if cannot find.
static CellImage *gr_find_image(uint32_t image_id) {
	if (!image_id)
		return NULL;
	for (size_t i = 0; i < MAX_CELL_IMAGES; ++i) {
		if (cell_images[i].image_id == image_id)
			return &cell_images[i];
	}
	return NULL;
}

/// Writes the name of the on-disk cache file to `out`. `max_len` should be the
/// size of `out`. The name will be something like "/tmp/st-images-xxx/img-ID".
static void gr_get_image_filename(CellImage *img, char *out, size_t max_len) {
	snprintf(out, max_len, "%s/img-%.3d", temp_dir, img->image_id);
}

/// Returns the (estimation) of the RAM size used by the image when loaded.
static unsigned gr_image_ram_size(CellImage *img) {
	return (unsigned)img->rows * img->cols * img->scaled_ch *
	       img->scaled_cw * 4;
}

/// Unload the image from RAM (i.e. delete the corresponding imlib object). If
/// the on-disk file is preserved, it can be reloaded later.
static void gr_unload_image(CellImage *img) {
	if (!img->scaled_image)
		return;

	imlib_context_set_image(img->scaled_image);
	imlib_free_image();

	cell_images_ram_size -= gr_image_ram_size(img);

	img->scaled_image = NULL;
	img->scaled_ch = img->scaled_cw = 0;
	img->status = STATUS_UPLOADING_SUCCESS;

	if (graphics_debug_mode) {
		fprintf(stderr, "After unloading image %u ram: %d KiB\n",
			img->image_id, cell_images_ram_size / 1024);
	}
}

/// Delete the on-disk cache file corresponding to the image. The in-ram image
/// object (if it exists) is not deleted, so the image may still be displayed
/// with the same cell width/height values.
static void gr_delete_imagefile(CellImage *img) {
	// It may still be being loaded. Close the file in this case.
	if (img->open_file) {
		fclose(img->open_file);
		img->open_file = NULL;
	}

	if (img->disk_size == 0)
		return;

	char filename[MAX_FILENAME_SIZE];
	gr_get_image_filename(img, filename, MAX_FILENAME_SIZE);
	remove(filename);

	cell_images_disk_size -= img->disk_size;
	img->disk_size = 0;
	if (img->status < STATUS_RAM_LOADING_SUCCESS)
		img->status = STATUS_UPLOADING_ERROR;

	if (graphics_debug_mode) {
		fprintf(stderr, "After deleting image file %u disk: %d KiB\n",
			img->image_id, cell_images_disk_size / 1024);
	}
}

/// Deletes the given image (unloads, deletes the file, removes from images).
/// The corresponding CellImage structure is zeroed out.
static void gr_delete_image(CellImage *img) {
	gr_unload_image(img);
	gr_delete_imagefile(img);
	if (img->image_id)
		images_count--;
	memset(img, 0, sizeof(CellImage));
}

/// Returns the oldest image that is profitable to delete to free up disk space
/// (if ram == 0) or RAM (if ram == 0).
static CellImage *gr_get_image_to_delete(int ram) {
	CellImage *oldest_image = NULL;
	for (size_t i = 0; i < MAX_CELL_IMAGES; ++i) {
		CellImage *img = &cell_images[i];
		if (img->image_id == 0)
			continue;
		if (ram && !img->scaled_image)
			continue;
		if (!ram && img->disk_size == 0)
			continue;
		if (!oldest_image ||
		    difftime(img->atime, oldest_image->atime) < 0)
			oldest_image = img;
	}
	if (graphics_debug_mode && oldest_image) {
		fprintf(stderr, "Oldest image id %u\n", oldest_image->image_id);
	}
	return oldest_image;
}

/// Checks RAM and disk cache limits and deletes/unloads some images.
static void gr_check_limits() {
	if (graphics_debug_mode) {
		fprintf(stderr,
			"Checking limits ram: %d KiB disk: %d KiB count: %d\n",
			cell_images_ram_size / 1024,
			cell_images_disk_size / 1024, images_count);
	}
	char changed = 0;
	while (cell_images_disk_size > max_total_disk_size) {
		gr_delete_imagefile(gr_get_image_to_delete(0));
		changed = 1;
	}
	while (cell_images_ram_size > max_total_ram_size) {
		gr_unload_image(gr_get_image_to_delete(1));
		changed = 1;
	}
	if (graphics_debug_mode && changed) {
		fprintf(stderr,
			"After cleaning ram: %d KiB disk: %d KiB count: %d\n",
			cell_images_ram_size / 1024,
			cell_images_disk_size / 1024, images_count);
	}
}

/// Finds a free CellImage structure for a new image. If there are too many
/// images, deletes the oldest image.
static CellImage *gr_allocate_image() {
	CellImage *oldest_image = NULL;
	for (size_t i = 0; i < MAX_CELL_IMAGES; ++i) {
		CellImage *img = &cell_images[i];
		if (img->image_id == 0)
			return img;
		if (!oldest_image ||
		    difftime(img->atime, oldest_image->atime) < 0)
			oldest_image = img;
	}
	gr_delete_image(oldest_image);
	return oldest_image;
}

/// Creates a new image with the given id. If an image with that id already
/// exists, it is deleted first. The id must not be 0.
static CellImage *gr_new_image(uint32_t id, int cols, int rows) {
	if (id == 0)
		return NULL;
	CellImage *image = gr_find_image(id);
	if (image)
		gr_delete_image(image);
	else
		image = gr_allocate_image();
	images_count++;
	image->image_id = id;
	image->cols = cols;
	image->rows = rows;
	return image;
}

/// Update the atime of the image.
static void gr_touch_image(CellImage *img) { time(&img->atime); }

/// Loads the image into RAM by creating an imlib object. The in-ram image is
/// correctly fit to the box defined by the number of rows/columns of the image
/// and the provided cell dimensions in pixels. If the image is already loaded,
/// it will be reloaded only if the cell dimensions have changed.
/// Loading may fail, in which case the status of the image will be set to
/// STATUS_RAM_LOADING_ERROR.
static void gr_load_image(CellImage *img, int cw, int ch) {
	// If it's already loaded with the same cw and ch, do nothing.
	if (img->scaled_image && img->scaled_ch == ch && img->scaled_cw == cw)
		return;
	// Unload the image first.
	gr_unload_image(img);

	// If the image is uninitialized or uploading has failed, we cannot load
	// the image.
	if (img->status < STATUS_UPLOADING_SUCCESS)
		return;

	// Load the original (non-scaled) image.
	char filename[MAX_FILENAME_SIZE];
	gr_get_image_filename(img, filename, MAX_FILENAME_SIZE);
	Imlib_Image image = imlib_load_image(filename);
	if (!image) {
		if (img->status != STATUS_RAM_LOADING_ERROR) {
			fprintf(stderr, "error: could not load image: %s\n",
				filename);
		}
		img->status = STATUS_RAM_LOADING_ERROR;
		return;
	}

	// Free up some ram before marking this image as loaded.
	gr_touch_image(img);
	gr_check_limits();

	imlib_context_set_image(image);
	int orig_w = imlib_image_get_width();
	int orig_h = imlib_image_get_height();

	// Create the scaled image.
	int scaled_w = (int)img->cols * cw;
	int scaled_h = (int)img->rows * ch;
	img->scaled_image = imlib_create_image(scaled_w, scaled_h);
	if (!img->scaled_image) {
		if (img->status != STATUS_RAM_LOADING_ERROR) {
			fprintf(stderr,
				"error: imlib_create_image(%d, %d) returned "
				"null\n",
				scaled_w, scaled_h);
		}
		img->status = STATUS_RAM_LOADING_ERROR;
		return;
	}
	imlib_context_set_image(img->scaled_image);
	imlib_image_set_has_alpha(1);
	// First fill the scaled image with the transparent color.
	imlib_context_set_blend(0);
	imlib_context_set_color(0, 0, 0, 0);
	imlib_image_fill_rectangle(0, 0, (int)img->cols * cw,
				   (int)img->rows * ch);
	imlib_context_set_anti_alias(1);
	imlib_context_set_blend(1);
	// Then blend the original image onto the transparent background.
	if (orig_w == 0 || orig_h == 0) {
		fprintf(stderr, "warning: image of zero size\n");
	} else if (img->scale_mode == SCALE_MODE_FILL) {
		imlib_blend_image_onto_image(image, 1, 0, 0, orig_w, orig_h, 0,
					     0, scaled_w, scaled_h);
	} else {
		if (img->scale_mode != SCALE_MODE_CONTAIN) {
			fprintf(stderr, "warning: unknown scale mode, using "
					"'contain' instead\n");
		}
		int dest_x, dest_y;
		int dest_w, dest_h;
		if (scaled_w * orig_h > orig_w * scaled_h) {
			// If the box is wider than the original image, fit to
			// height.
			dest_h = scaled_h;
			dest_y = 0;
			dest_w = orig_w * scaled_h / orig_h;
			dest_x = (scaled_w - dest_w) / 2;
		} else {
			// Otherwise, fit to width.
			dest_w = scaled_w;
			dest_x = 0;
			dest_h = orig_h * scaled_w / orig_w;
			dest_y = (scaled_h - dest_h) / 2;
		}
		imlib_blend_image_onto_image(image, 1, 0, 0, orig_w, orig_h,
					     dest_x, dest_y, dest_w, dest_h);
	}

	// Delete the object of the original image.
	imlib_context_set_image(image);
	imlib_free_image();

	// Mark the image as loaded.
	img->scaled_ch = ch;
	img->scaled_cw = cw;
	cell_images_ram_size += gr_image_ram_size(img);
	img->status = STATUS_RAM_LOADING_SUCCESS;
}

////////////////////////////////////////////////////////////////////////////////
// Interaction with the terminal (init, deinit, appending rects, etc).
////////////////////////////////////////////////////////////////////////////////

/// Initialize the graphics module.
void gr_init(Display *disp, Visual *vis, Colormap cm) {
	// Create the temporary dir.
	if (!mkdtemp(temp_dir)) {
		fprintf(stderr,
			"Could not create temporary dir from template %s\n",
			temp_dir);
		abort();
	}

	// Initialize imlib.
	imlib_context_set_display(disp);
	imlib_context_set_visual(vis);
	imlib_context_set_colormap(cm);
	imlib_context_set_anti_alias(1);
	imlib_context_set_blend(1);
	imlib_set_cache_size(imlib_cache_size);

	// Prepare for color inversion.
	for (size_t i = 0; i < 256; ++i)
		reverse_table[i] = 255 - i;
}

/// Deinitialize the graphics module.
void gr_deinit() {
	// Delete all images.
	for (size_t i = 0; i < MAX_CELL_IMAGES; ++i)
		gr_delete_image(&cell_images[i]);
	// Remove the cache dir.
	remove(temp_dir);
}

/// Executes `command` with the name of the file corresponding to `image_id` as
/// the argument. Executes xmessage with an error message on failure.
void gr_preview_image(uint32_t image_id, const char *exec) {
	char command[256];
	size_t len;
	CellImage *img = gr_find_image(image_id);
	if (img) {
		char filename[MAX_FILENAME_SIZE];
		gr_get_image_filename(img, filename, MAX_FILENAME_SIZE);
		if (img->disk_size == 0) {
			len = snprintf(command, 255,
				       "xmessage 'Image with id=%u is not "
				       "fully copied to %s'",
				       image_id, filename);
		} else {
			len = snprintf(command, 255, "%s %s", exec, filename);
		}
	} else {
		len = snprintf(command, 255,
			       "xmessage 'Cannot find image with id=%u'",
			       image_id);
	}
	if (len > 255) {
		fprintf(stderr, "error: command too long: %s\n", command);
		snprintf(command, 255, "xmessage 'error: command too long'");
	}
	if (system(command) != 0) {
		fprintf(stderr, "error: could not execute command %s\n",
			command);
	}
}

/// Checks if we are still really uploading something. Returns 1 if we may be
/// and 0 if we aren't. If certain amount of time has passed since the last data
/// transmission command, we assume that all uploads have failed.
int gr_check_if_still_uploading() {
	if (!graphics_uploading)
		return 0;
	time_t cur_time;
	time(&cur_time);
	double dt = difftime(last_uploading_time, cur_time);
	if (difftime(last_uploading_time, cur_time) < -1.0)
		graphics_uploading = 0;
	return graphics_uploading;
}

/// Displays debug information in the rectangle using colors col1 and col2.
static void gr_displayinfo(Drawable buf, ImageRect *rect, int col1, int col2,
			   const char *message) {
	int w_pix = (rect->end_col - rect->start_col) * rect->cw;
	int h_pix = (rect->end_row - rect->start_row) * rect->ch;
	Display *disp = imlib_context_get_display();
	GC gc = XCreateGC(disp, buf, 0, NULL);
	char info[MAX_INFO_LEN];
	snprintf(info, MAX_INFO_LEN, "%s%u [%d:%d)x[%d:%d)", message,
		 rect->image_id, rect->start_col, rect->end_col,
		 rect->start_row, rect->end_row);
	XSetForeground(disp, gc, col1);
	XDrawString(disp, buf, gc, rect->x_pix + 4, rect->y_pix + h_pix - 3,
		    info, strlen(info));
	XSetForeground(disp, gc, col2);
	XDrawString(disp, buf, gc, rect->x_pix + 2, rect->y_pix + h_pix - 5,
		    info, strlen(info));
	XFreeGC(disp, gc);
}

/// Draws a rectangle (bounding box) for debugging.
static void gr_showrect(Drawable buf, ImageRect *rect) {
	int w_pix = (rect->end_col - rect->start_col) * rect->cw;
	int h_pix = (rect->end_row - rect->start_row) * rect->ch;
	Display *disp = imlib_context_get_display();
	GC gc = XCreateGC(disp, buf, 0, NULL);
	XSetForeground(disp, gc, 0x00FF00);
	XDrawRectangle(disp, buf, gc, rect->x_pix, rect->y_pix, w_pix - 1,
		       h_pix - 1);
	XSetForeground(disp, gc, 0xFF0000);
	XDrawRectangle(disp, buf, gc, rect->x_pix + 1, rect->y_pix + 1,
		       w_pix - 3, h_pix - 3);
	XFreeGC(disp, gc);
}

/// Draws the given part of an image.
static void gr_drawimagerect(Drawable buf, ImageRect *rect) {
	// If we are uploading data then we shouldn't do heavy computation (like
	// displaying graphics), mostly because some versions of tmux may drop
	// pass-through commands if the terminal is too slow.
	if (graphics_uploading)
		return;

	CellImage *img = gr_find_image(rect->image_id);
	// If the image does not exist, display the bounding box and some info
	// like the image id.
	if (!img) {
		gr_showrect(buf, rect);
		gr_displayinfo(buf, rect, 0x000000, 0xFFFFFF, "");
		return;
	}

	// Load the image.
	gr_load_image(img, rect->cw, rect->ch);

	// If the image couldn't be loaded, display the bounding box and info.
	if (!img->scaled_image) {
		gr_showrect(buf, rect);
		gr_displayinfo(buf, rect, 0x000000, 0xFFFFFF, "");
		return;
	}

	gr_touch_image(img);

	// Display the image.
	imlib_context_set_anti_alias(0);
	imlib_context_set_image(img->scaled_image);
	imlib_context_set_drawable(buf);
	if (rect->reverse) {
		Imlib_Color_Modifier cm = imlib_create_color_modifier();
		imlib_context_set_color_modifier(cm);
		imlib_set_color_modifier_tables(reverse_table, reverse_table,
						reverse_table, NULL);
	}
	int w_pix = (rect->end_col - rect->start_col) * rect->cw;
	int h_pix = (rect->end_row - rect->start_row) * rect->ch;
	imlib_render_image_part_on_drawable_at_size(
		rect->start_col * rect->cw, rect->start_row * rect->ch, w_pix,
		h_pix, rect->x_pix, rect->y_pix, w_pix, h_pix);
	if (rect->reverse) {
		imlib_free_color_modifier();
		imlib_context_set_color_modifier(NULL);
	}

	// In debug mode always draw bounding boxes and print info.
	if (graphics_debug_mode) {
		gr_showrect(buf, rect);
		gr_displayinfo(buf, rect, 0x000000, 0xFFFFFF, "");
	}
}

/// Removes the given image rectangle.
static void gr_freerect(ImageRect *rect) { memset(rect, 0, sizeof(ImageRect)); }

/// Returns the bottom coordinate of the rect.
static int gr_getrectbottom(ImageRect *rect) {
	return rect->y_pix + (rect->end_row - rect->start_row) * rect->ch;
}

/// Prepare for image drawing. `cw` and `ch` are dimensions of the cell.
void gr_start_drawing(Drawable buf, int cw, int ch) {
	current_cw = cw;
	current_ch = ch;
	drawing_start_time = clock();
}

/// Finish image drawing. This functions will draw all the rectangles left to
/// draw.
void gr_finish_drawing(Drawable buf) {
	// Draw and then delete all known image rectangles.
	for (size_t i = 0; i < MAX_IMAGE_RECTS; ++i) {
		ImageRect *rect = &image_rects[i];
		if (!rect->image_id)
			continue;
		gr_drawimagerect(buf, rect);
		gr_freerect(rect);
	}

	// In debug mode display additional info.
	if (graphics_debug_mode) {
		clock_t drawing_end_time = clock();
		int milliseconds = 1000 *
				   (drawing_end_time - drawing_start_time) /
				   CLOCKS_PER_SEC;

		Display *disp = imlib_context_get_display();
		GC gc = XCreateGC(disp, buf, 0, NULL);
		char info[MAX_INFO_LEN];
		snprintf(info, MAX_INFO_LEN,
			 "Frame rendering time: %d ms  Image storage ram: %d "
			 "KiB disk: %d KiB  count: %d",
			 milliseconds, cell_images_ram_size / 1024,
			 cell_images_disk_size / 1024, images_count);
		XSetForeground(disp, gc, 0x000000);
		XFillRectangle(disp, buf, gc, 0, 0, 600, 16);
		XSetForeground(disp, gc, 0xFFFFFF);
		XDrawString(disp, buf, gc, 0, 14, info, strlen(info));
		XFreeGC(disp, gc);
	}
}

// Add an image rectangle to a list if rectangles to draw.
void gr_append_imagerect(Drawable buf, uint32_t image_id, int start_col,
			 int end_col, int start_row, int end_row, int x_pix,
			 int y_pix, int cw, int ch, int reverse) {
	current_cw = cw;
	current_ch = ch;

	ImageRect new_rect;
	new_rect.image_id = image_id;
	new_rect.start_col = start_col;
	new_rect.end_col = end_col;
	new_rect.start_row = start_row;
	new_rect.end_row = end_row;
	new_rect.x_pix = x_pix;
	new_rect.y_pix = y_pix;
	new_rect.ch = ch;
	new_rect.cw = cw;
	new_rect.reverse = reverse;

	// Display some red text in debug mode.
	if (graphics_debug_mode)
		gr_displayinfo(buf, &new_rect, 0x000000, 0xFF0000, "? ");

	// If it's the empty image (image_id=0) or an empty rectangle, do
	// nothing.
	if (image_id == 0 || end_col - start_col <= 0 ||
	    end_row - start_row <= 0)
		return;
	// Try to find a rect to merge with.
	ImageRect *free_rect = NULL;
	for (size_t i = 0; i < MAX_IMAGE_RECTS; ++i) {
		ImageRect *rect = &image_rects[i];
		if (rect->image_id == 0) {
			if (!free_rect)
				free_rect = rect;
			continue;
		}
		if (rect->image_id != image_id || rect->cw != cw ||
		    rect->ch != ch || rect->reverse != reverse)
			continue;
		// We only support the case when the new stripe is added to the
		// bottom of an existing rectangle and they are perfectly
		// aligned.
		if (rect->end_row == start_row &&
		    gr_getrectbottom(rect) == y_pix) {
			if (rect->start_col == start_col &&
			    rect->end_col == end_col && rect->x_pix == x_pix) {
				rect->end_row = end_row;
				return;
			}
		}
	}
	// If we haven't merged the new rect with any existing rect, and there
	// is no free rect, we have to render one of the existing rects.
	if (!free_rect) {
		for (size_t i = 0; i < MAX_IMAGE_RECTS; ++i) {
			ImageRect *rect = &image_rects[i];
			if (!free_rect || gr_getrectbottom(free_rect) >
						  gr_getrectbottom(rect))
				free_rect = rect;
		}
		gr_drawimagerect(buf, free_rect);
		gr_freerect(free_rect);
	}
	// Start a new rectangle in `free_rect`.
	*free_rect = new_rect;
}

////////////////////////////////////////////////////////////////////////////////
// Command parsing and handling.
////////////////////////////////////////////////////////////////////////////////

/// A parsed kitty graphics protocol command.
typedef struct {
	/// The command itself, without the 'G'.
	char *command;
	/// The payload (after ';').
	char *payload;
	/// 'a=', may be 't', 'T', 'p', 'd'.
	char action;
	/// 'q=', 1 to suppress OK response, 2 to suppress errors too.
	int quiet;
	/// 'f=', ignored.
	int format;
	/// 't=', may be 'f' or 'd'.
	char transmission_medium;
	/// 'd=', may be only 'I' if specified.
	char delete_specifier;
	/// 's=', 'v=', ignored,
	int pix_width, pix_height;
	/// 'r=', 'c='
	int rows, columns;
	/// 'i='
	uint32_t image_id;
	/// 'I=', not supported.
	uint32_t image_number;
	/// 'p=', not supported, must be 0 or 1.
	uint32_t placement_id;
	/// 'm=', may be 0 or 1.
	int more;
	/// True if either 'm=0' or 'm=1' is specified.
	int has_more;
	/// 'S=', used to check the size of uploaded data.
	int size;
} GraphicsCommand;

/// Creates a response to the current command in `graphics_command_result`.
static void gr_createresponse(uint32_t image_id, uint32_t image_number,
			      const char *msg) {
	if (image_id && image_number) {
		snprintf(graphics_command_result.response,
			 MAX_GRAPHICS_RESPONSE_LEN, "\033_Gi=%u,I=%u;%s\033\\",
			 image_id, image_number, msg);
	} else if (!image_id && image_number) {
		snprintf(graphics_command_result.response,
			 MAX_GRAPHICS_RESPONSE_LEN, "\033_GI=%u;%s\033\\",
			 image_number, msg);
	} else {
		snprintf(graphics_command_result.response,
			 MAX_GRAPHICS_RESPONSE_LEN, "\033_Gi=%u;%s\033\\",
			 image_id, msg);
	}
}

/// Creates the 'OK' response to the current command (unless suppressed).
static void gr_reportsuccess_cmd(GraphicsCommand *cmd) {
	if (cmd->quiet < 1)
		gr_createresponse(cmd->image_id, cmd->image_number, "OK");
}

/// Creates the 'OK' response to the current command (unless suppressed).
static void gr_reportsuccess_img(CellImage *img) {
	if (img->quiet < 1)
		gr_createresponse(img->image_id, 0, "OK");
}

/// Creates an error response to the current command (unless suppressed).
static void gr_reporterror_cmd(GraphicsCommand *cmd, const char *format, ...) {
	char errmsg[MAX_GRAPHICS_RESPONSE_LEN];
	graphics_command_result.error = 1;
	va_list args;
	va_start(args, format);
	vsnprintf(errmsg, MAX_GRAPHICS_RESPONSE_LEN, format, args);
	va_end(args);

	fprintf(stderr, "%s  in command: %s\n", errmsg, cmd->command);
	if (cmd->quiet < 2)
		gr_createresponse(cmd->image_id, cmd->image_number, errmsg);
}

/// Creates an error response to the current command (unless suppressed).
static void gr_reporterror_img(CellImage *img, const char *format, ...) {
	char errmsg[MAX_GRAPHICS_RESPONSE_LEN];
	graphics_command_result.error = 1;
	va_list args;
	va_start(args, format);
	vsnprintf(errmsg, MAX_GRAPHICS_RESPONSE_LEN, format, args);
	va_end(args);

	if (!img) {
		fprintf(stderr, "%s\n", errmsg);
		gr_createresponse(0, 0, errmsg);
	} else {
		fprintf(stderr, "%s  id=%u\n", errmsg, img->image_id);
		if (img->quiet < 2)
			gr_createresponse(img->image_id, 0, errmsg);
	}
}

/// Loads an image and creates a success/failure response.
static void gr_loadimage_and_report(CellImage *img) {
	gr_load_image(img, current_cw, current_ch);
	if (!img->scaled_image) {
		gr_reporterror_img(img, "EBADF: could not load image");
	} else {
		gr_reportsuccess_img(img);
	}
}

/// Creates an appropriate uploading failure response to the current command.
static void gr_reportuploaderror(CellImage *img) {
	switch (img->uploading_failure) {
	case 0:
		return;
	case ERROR_CANT_CREATE_FILE:
		gr_reporterror_img(img,
				   "EIO: could not create a file for image");
		break;
	case ERROR_OVER_SIZE_LIMIT:
		gr_reporterror_img(
			img,
			"EFBIG: the size of the uploaded image exceeded "
			"the image size limit %u",
			max_image_disk_size);
		break;
	case ERROR_UNEXPECTED_SIZE:
		gr_reporterror_img(img,
				   "EINVAL: the size of the uploaded image %u "
				   "doesn't match the expected size %u",
				   img->disk_size, img->expected_size);
		break;
	};
}

/// Appends data from `payload` to the image `img` when using direct
/// transmission. Note that we report errors only for the final command
/// (`!more`) to avoid spamming the client.
static void gr_append_data(CellImage *img, const char *payload, int more) {
	if (!img)
		img = gr_find_image(current_upload_image_id);
	if (!more)
		current_upload_image_id = 0;
	if (!img) {
		if (!more)
			gr_reporterror_img(img, "ENOENT: could not find the "
						"image to append data to");
		return;
	}
	if (img->status != STATUS_UPLOADING) {
		if (!more)
			gr_reportuploaderror(img);
		return;
	}

	// Decode the data.
	size_t data_size = 0;
	char *data = gr_base64dec(payload, &data_size);

	if (graphics_debug_mode)
		fprintf(stderr, "appending %u + %zu = %zu bytes\n",
			img->disk_size, data_size, img->disk_size + data_size);

	// Do not append this data if the image exceeds the size limit.
	if (img->disk_size + data_size > max_image_disk_size ||
	    img->expected_size > max_image_disk_size) {
		free(data);
		gr_delete_imagefile(img);
		img->uploading_failure = ERROR_OVER_SIZE_LIMIT;
		if (!more)
			gr_reportuploaderror(img);
		return;
	}

	// If there is no open file corresponding to the image, create it.
	if (!img->open_file) {
		char filename[MAX_FILENAME_SIZE];
		gr_get_image_filename(img, filename, MAX_FILENAME_SIZE);
		FILE *file = fopen(filename, img->disk_size ? "a" : "w");
		if (!file) {
			img->status = STATUS_UPLOADING_ERROR;
			img->uploading_failure = ERROR_CANT_CREATE_FILE;
			if (!more)
				gr_reportuploaderror(img);
			return;
		}
		img->open_file = file;
	}

	// Write date to the file and update disk size variables.
	fwrite(data, 1, data_size, img->open_file);
	free(data);
	img->disk_size += data_size;
	cell_images_disk_size += data_size;
	gr_touch_image(img);

	if (more) {
		current_upload_image_id = img->image_id;
		time(&last_uploading_time);
	} else {
		if (graphics_uploading) {
			graphics_uploading--;
			// Since direct uploading suppresses image
			// drawing (as a workaround), we need to redraw
			// the screen whenever we finish all uploads.
			if (!graphics_uploading)
				graphics_command_result.redraw = 1;
		}
		current_upload_image_id = 0;
		// Close the file.
		if (img->open_file) {
			fclose(img->open_file);
			img->open_file = NULL;
		}
		img->status = STATUS_UPLOADING_SUCCESS;
		if (img->expected_size &&
		    img->expected_size != img->disk_size) {
			// Report failure if the uploaded image size doesn't
			// match the expected size.
			img->status = STATUS_UPLOADING_ERROR;
			img->uploading_failure = ERROR_UNEXPECTED_SIZE;
			gr_reportuploaderror(img);
		} else {
			// Try to load the image into ram and report the result.
			gr_loadimage_and_report(img);
		}
	}

	// Check whether we need to delete old images.
	gr_check_limits();
}

/// Handles a data transmission command.
static CellImage *gr_transmit_data(GraphicsCommand *cmd) {
	if (cmd->image_number != 0) {
		gr_reporterror_cmd(
			cmd, "EINVAL: image numbers (I) are not supported");
		return NULL;
	}

	if (cmd->image_id == 0) {
		gr_reporterror_cmd(cmd,
				   "EINVAL: image id is not specified or zero");
		return NULL;
	}

	CellImage *img = NULL;
	if (cmd->transmission_medium == 'f') { // file
		// Create a new image structure.
		img = gr_new_image(cmd->image_id, cmd->columns, cmd->rows);
		if (!img)
			return NULL;
		img->expected_size = cmd->size;
		last_image_id = cmd->image_id;
		// Decode the filename.
		char *original_filename = gr_base64dec(cmd->payload, NULL);
		// We will create a symlink to the original file, and then copy
		// the file to the temporary cache dir. We do this symlink trick
		// mostly to be able to use cp for copying, and avoid escaping
		// file name characters when calling system at the same time.
		char tmp_filename[MAX_FILENAME_SIZE];
		char tmp_filename_symlink[MAX_FILENAME_SIZE + 4] = {0};
		gr_get_image_filename(img, tmp_filename, MAX_FILENAME_SIZE);
		strcat(tmp_filename_symlink, tmp_filename);
		strcat(tmp_filename_symlink, ".sym");
		if (access(original_filename, R_OK) != 0 ||
		    symlink(original_filename, tmp_filename_symlink)) {
			gr_reporterror_cmd(cmd,
					   "EBADF: could not create a symlink "
					   "from %s to %s",
					   original_filename,
					   tmp_filename_symlink);
			img->status = STATUS_UPLOADING_ERROR;
		} else {
			// We've successfully created a symlink, now call cp.
			char command[MAX_FILENAME_SIZE + 256];
			size_t len =
				snprintf(command, MAX_FILENAME_SIZE + 255,
					 "cp '%s' '%s'", tmp_filename_symlink,
					 tmp_filename);
			if (len > MAX_FILENAME_SIZE + 255 ||
			    system(command) != 0) {
				gr_reporterror_cmd(
					cmd,
					"EBADF: could not copy the image "
					"from %s to %s",
					tmp_filename_symlink, tmp_filename);
				img->status = STATUS_UPLOADING_ERROR;
			} else {
				// Get the file size of the copied file.
				struct stat imgfile_stat;
				stat(tmp_filename, &imgfile_stat);
				img->status = STATUS_UPLOADING_SUCCESS;
				img->disk_size = imgfile_stat.st_size;
				cell_images_disk_size += img->disk_size;
				// Check whether the file is too large.
				// TODO: Check it before copying.
				if (img->disk_size > max_image_disk_size) {
					// The file is too large.
					gr_delete_imagefile(img);
					img->uploading_failure =
						ERROR_OVER_SIZE_LIMIT;
					gr_reportuploaderror(img);
				} else if (img->expected_size &&
					   img->expected_size !=
						   img->disk_size) {
					// The file has unexpected size.
					img->status = STATUS_UPLOADING_ERROR;
					img->uploading_failure =
						ERROR_UNEXPECTED_SIZE;
					gr_reportuploaderror(img);
				} else {
					// Everything seems fine, try to load.
					gr_loadimage_and_report(img);
				}
			}
			// Delete the symlink.
			unlink(tmp_filename_symlink);
		}
		free(original_filename);
		gr_check_limits();
	} else if (cmd->transmission_medium == 'd') { // direct
		// Create a new image structure.
		img = gr_new_image(cmd->image_id, cmd->columns, cmd->rows);
		if (!img)
			return NULL;
		img->expected_size = cmd->size;
		last_image_id = cmd->image_id;
		img->status = STATUS_UPLOADING;
		// We save the quietness information in the image because
		// subsequent transmission command won't contain this info.
		img->quiet = cmd->quiet;
		graphics_uploading++;
		// Start appending data.
		gr_append_data(img, cmd->payload, cmd->more);
	} else {
		gr_reporterror_cmd(
			cmd,
			"EINVAL: transmission medium '%c' is not supported",
			cmd->transmission_medium);
		return NULL;
	}

	return img;
}

/// Handles the 'put' command. Since we support only one placement (which must
/// be virtual), just update rows/columns and make sure that the image is still
/// available and can be correctly loaded.
static void gr_handle_put_command(GraphicsCommand *cmd) {
	if (cmd->image_number != 0) {
		gr_reporterror_cmd(
			cmd, "EINVAL: image numbers (I) are not supported");
		return;
	}

	// If image id is not specified, use last image id.
	uint32_t image_id = cmd->image_id ? cmd->image_id : last_image_id;

	if (image_id == 0) {
		gr_reporterror_cmd(cmd,
				   "EINVAL: image id is not specified or zero");
		return;
	}

	// Find the image with the id.
	CellImage *img = gr_find_image(image_id);
	if (!img) {
		gr_reporterror_cmd(cmd, "ENOENT: image not found");
		return;
	}

	// If rows and columns are specified and are different, update them in
	// the image structure and unload the image to make sure that it will be
	// reloaded with the new dimensions.
	int needs_unloading = 0;
	if (cmd->columns && cmd->columns != img->cols) {
		img->cols = cmd->columns;
		needs_unloading = 1;
	}
	if (cmd->rows && cmd->rows != img->rows) {
		img->rows = cmd->rows;
		needs_unloading = 1;
	}
	if (needs_unloading)
		gr_unload_image(img);

	// Try to load the image into RAM.
	gr_loadimage_and_report(img);
}

/// Handles the delete command.
static void gr_handle_delete_command(GraphicsCommand *cmd) {
	if (!cmd->delete_specifier) {
		// With no delete specifier just delete everything.
		for (size_t i = 0; i < MAX_CELL_IMAGES; ++i) {
			if (cell_images[i].image_id)
				gr_delete_image(&cell_images[i]);
		}
	} else if (cmd->delete_specifier == 'I') {
		// 'd=I' means delete the image with the given id, including the
		// image data.
		if (cmd->image_id == 0)
			gr_reporterror_cmd(cmd,
					   "EINVAL: no image id to delete");
		CellImage *image = gr_find_image(cmd->image_id);
		if (image)
			gr_delete_image(image);
		gr_reportsuccess_cmd(cmd);
	} else {
		gr_reporterror_cmd(
			cmd, "EINVAL: unsupported values of the d key : '%c'",
			cmd->delete_specifier);
	}
}

/// Handles a command.
static void gr_handle_command(GraphicsCommand *cmd) {
	CellImage *img = NULL;
	switch (cmd->action) {
	case 0:
		// If no action is specified, it may be a data transmission
		// command if 'm=' is specified.
		if (cmd->has_more)
			gr_append_data(NULL, cmd->payload, cmd->more);
		else
			gr_reporterror_cmd(cmd, "EINVAL: no action specified");
		break;
	case 't':
	case 'T':
		// Transmit data or transmit and display.
		gr_transmit_data(cmd);
		break;
	case 'p':
		// Display (put) the image.
		gr_handle_put_command(cmd);
		break;
	case 'd':
		gr_handle_delete_command(cmd);
		break;
	case 'q':
		// query
	default:
		gr_reporterror_cmd(cmd, "EINVAL: unsupported action: %c",
				   cmd->action);
		return;
	}
}

/// Parses the value specified by `value_start` and `value_end` and assigns it
/// to the field of `cmd` specified by `key_start` and `key_end`.
static void gr_set_keyvalue(GraphicsCommand *cmd, char *key_start, char *key_end,
			 char *value_start, char *value_end) {
	// Currently all keys are one-character.
	if (key_end - key_start != 1) {
		gr_reporterror_cmd(cmd, "EINVAL: unknown key of length %ld: %s",
				   key_end - key_start, key_start);
		return;
	}
	long num = 0;
	if (*key_start == 'a' || *key_start == 't' || *key_start == 'd') {
		// Some keys have one-character values.
		if (value_end - value_start != 1) {
			gr_reporterror_cmd(
				cmd,
				"EINVAL: value of 'a', 't' or 'd' must be a "
				"single char: %s",
				key_start);
			return;
		}
	} else {
		// All the other keys have integer values.
		char *num_end = NULL;
		num = strtol(value_start, &num_end, 10);
		if (num_end != value_end) {
			gr_reporterror_cmd(
				cmd, "EINVAL: could not parse number value: %s",
				key_start);
			return;
		}
	}
	switch (*key_start) {
	case 'a':
		cmd->action = *value_start;
		break;
	case 't':
		cmd->transmission_medium = *value_start;
		break;
	case 'd':
		cmd->delete_specifier = *value_start;
		break;
	case 'q':
		cmd->quiet = num;
		break;
	case 'f':
		cmd->format = num;
		break;
	case 's':
		cmd->pix_width = num;
		break;
	case 'v':
		cmd->pix_height = num;
		break;
	case 'i':
		cmd->image_id = num;
		break;
	case 'I':
		cmd->image_number = num;
		break;
	case 'p':
		cmd->placement_id = num;
		if (num != 0 && num != 1) {
			gr_reporterror_cmd(cmd,
					   "EINVAL: placement id other than 0 "
					   "or 1 is not supported: %s",
					   key_start);
		}
		break;
	case 'c':
		cmd->columns = num;
		break;
	case 'r':
		cmd->rows = num;
		break;
	case 'm':
		cmd->has_more = 1;
		cmd->more = num;
		break;
	case 'S':
		cmd->size = num;
		break;
	case 'U':
		// Placement using unicode chars. If specified, it must be true
		// since we don't support other forms of placement.
		if (!num) {
			gr_reporterror_cmd(cmd,
					   "EINVAL: 'U' must be non-zero: %s",
					   key_start);
		}
		break;
	default:
		gr_reporterror_cmd(cmd, "EINVAL: unsupported key: %s",
				   key_start);
		return;
	}
}

/// Parse and execute a graphics command. `buf` must start with 'G' and contain
/// at least `len + 1` characters. Returns 0 on success.
int gr_parse_command(char *buf, size_t len) {
	if (buf[0] != 'G')
		return 0;

	memset(&graphics_command_result, 0, sizeof(GraphicsCommandResult));

	static int cmdnum = 0;
	if (graphics_debug_mode)
		fprintf(stderr, "### Command %d: %.50s\n", cmdnum, buf);
	cmdnum++;

	// Eat the 'G'.
	++buf;
	--len;

	GraphicsCommand cmd = {.command = buf};
	// The state of parsing. 'k' to parse key, 'v' to parse value, 'p' to
	// parse the payload.
	char state = 'k';
	char *key_start = buf;
	char *key_end = NULL;
	char *val_start = NULL;
	char *val_end = NULL;
	char *c = buf;
	while (c - buf < len + 1) {
		if (state == 'k') {
			switch (*c) {
			case ',':
			case ';':
			case '\0':
				state = *c == ',' ? 'k' : 'p';
				key_end = c;
				gr_reporterror_cmd(
					&cmd, "EINVAL: key without value: %s ",
					key_start);
				break;
			case '=':
				key_end = c;
				state = 'v';
				val_start = c + 1;
				break;
			default:
				break;
			}
		} else if (state == 'v') {
			switch (*c) {
			case ',':
			case ';':
			case '\0':
				state = *c == ',' ? 'k' : 'p';
				val_end = c;
				gr_set_keyvalue(&cmd, key_start, key_end,
					     val_start, val_end);
				key_start = c + 1;
				break;
			default:
				break;
			}
		} else if (state == 'p') {
			cmd.payload = c;
			// break out of the loop, we don't check the payload
			break;
		}
		++c;
	}

	if (!cmd.payload)
		cmd.payload = buf + len;

	if (!graphics_command_result.error)
		gr_handle_command(&cmd);

	if (graphics_debug_mode) {
		fprintf(stderr, "Response: ");
		for (const char *resp = graphics_command_result.response;
		     *resp != '\0'; ++resp) {
			if (isprint(*resp))
				fprintf(stderr, "%c", *resp);
			else
				fprintf(stderr, "(0x%x)", *resp);
		}
		fprintf(stderr, "\n");
	}

	// Make sure that we suppress response if needed. Usually cmd.quiet is
	// taken into account when creating the response, but it's not very
	// reliable in the current implementation.
	if (cmd.quiet) {
		if (!graphics_command_result.error || cmd.quiet >= 2)
			graphics_command_result.response[0] = '\0';
	}

	return 1;
}

////////////////////////////////////////////////////////////////////////////////
// base64 decoding part is basically copied from st.c
////////////////////////////////////////////////////////////////////////////////

static const char gr_base64_digits[] = {
	0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	0,  0,  0,  0,  0,  0,  0,  0,  0,  62, 0,  0,  0,  63, 52, 53, 54,
	55, 56, 57, 58, 59, 60, 61, 0,  0,  0,  -1, 0,  0,  0,  0,  1,  2,
	3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
	20, 21, 22, 23, 24, 25, 0,  0,  0,  0,  0,  0,  26, 27, 28, 29, 30,
	31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
	48, 49, 50, 51, 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0};

static char gr_base64_getc(const char **src) {
	while (**src && !isprint(**src))
		(*src)++;
	return **src ? *((*src)++) : '='; /* emulate padding if string ends */
}

char *gr_base64dec(const char *src, size_t *size) {
	size_t in_len = strlen(src);
	char *result, *dst;

	result = dst = malloc((in_len + 3) / 4 * 3 + 1);
	while (*src) {
		int a = gr_base64_digits[(unsigned char)gr_base64_getc(&src)];
		int b = gr_base64_digits[(unsigned char)gr_base64_getc(&src)];
		int c = gr_base64_digits[(unsigned char)gr_base64_getc(&src)];
		int d = gr_base64_digits[(unsigned char)gr_base64_getc(&src)];

		if (a == -1 || b == -1)
			break;

		*dst++ = (a << 2) | ((b & 0x30) >> 4);
		if (c == -1)
			break;
		*dst++ = ((b & 0x0f) << 4) | ((c & 0x3c) >> 2);
		if (d == -1)
			break;
		*dst++ = ((c & 0x03) << 6) | d;
	}
	*dst = '\0';
	if (size) {
		*size = dst - result;
	}
	return result;
}

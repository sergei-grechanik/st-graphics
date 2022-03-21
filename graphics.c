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

enum ScaleMode { SCALE_MODE_CONTAIN = 0, SCALE_MODE_FILL = 1 };

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

typedef struct {
	uint32_t image_id;
	uint16_t rows, cols;
	time_t atime;
	unsigned disk_size;
	unsigned expected_size;
	char scale_mode;
	char status;
	char uploading_failure;
	char quiet;
	uint16_t scaled_cw, scaled_ch;
	Imlib_Image scaled_image;
	FILE *open_file;
} CellImage;

typedef struct {
	uint32_t image_id;
	int x_pix, y_pix;
	int start_col, end_col, start_row, end_row;
	int cw, ch;
	int reverse;
} ImageRect;

static CellImage *gfindimage(uint32_t image_id);
static void gimagefilename(CellImage *img, char *out, size_t max_len);
static void gdeleteimage(CellImage *img);
static void gchecklimits();
static char *gbase64dec(const char *src, size_t *size);

static CellImage cell_images[MAX_CELL_IMAGES] = {{0}};
static int images_count = 0;
static int32_t cell_images_disk_size = 0;
static int32_t cell_images_ram_size = 0;
static uint32_t last_image_id = 0;
static int last_cw = 0, last_ch = 0;
static uint32_t current_upload_image_id = 0;
static time_t last_uploading_time = 0;
static char temp_dir[] = "/tmp/st-images-XXXXXX";
static unsigned char reverse_table[256];
static size_t max_image_disk_size = 20 * 1024 * 1024;
static int max_total_disk_size = 300 * 1024 * 1024;
static int max_total_ram_size = 300 * 1024 * 1024;
static size_t imlib_cache_size = 4 * 1024 * 1024;
static clock_t drawing_start_time;

char graphics_debug_mode = 0;
char graphics_uploading = 0;
GraphicsCommandResult graphics_command_result = {0};

static ImageRect image_rects[MAX_IMAGE_RECTS] = {{0}};

void graphicsinit(Display *disp, Visual *vis, Colormap cm) {
	if (!mkdtemp(temp_dir)) {
		fprintf(stderr,
			"Could not create temporary dir from template %s\n",
			temp_dir);
		abort();
	}
	imlib_context_set_display(disp);
	imlib_context_set_visual(vis);
	imlib_context_set_colormap(cm);
	imlib_context_set_anti_alias(1);
	imlib_context_set_blend(1);
	imlib_set_cache_size(imlib_cache_size);

	for (size_t i = 0; i < 256; ++i)
		reverse_table[i] = 255 - i;
}

void graphicsdeinit() {
	for (size_t i = 0; i < MAX_CELL_IMAGES; ++i)
		gdeleteimage(&cell_images[i]);
	remove(temp_dir);
}

static CellImage *gfindimage(uint32_t image_id) {
	if (!image_id)
		return NULL;
	for (size_t i = 0; i < MAX_CELL_IMAGES; ++i) {
		if (cell_images[i].image_id == image_id)
			return &cell_images[i];
	}
	return NULL;
}

static void gimagefilename(CellImage *img, char *out, size_t max_len) {
	snprintf(out, max_len, "%s/img-%.3d", temp_dir, img->image_id);
}

static unsigned gimageramsize(CellImage *img) {
	return (unsigned)img->rows * img->cols * img->scaled_ch *
	       img->scaled_cw;
}

static void gunloadimage(CellImage *img) {
	if (!img->scaled_image)
		return;
	imlib_context_set_image(img->scaled_image);
	imlib_free_image();
	cell_images_ram_size -= gimageramsize(img);
	img->scaled_image = NULL;
	img->scaled_ch = img->scaled_cw = 0;
	img->status = STATUS_UPLOADING_SUCCESS;

	if (graphics_debug_mode) {
		fprintf(stderr, "After unloading image %u ram: %d KiB\n",
			img->image_id, cell_images_ram_size / 1024);
	}
}

static void gr_deleteimagefile(CellImage *img) {
	if (img->open_file) {
		fclose(img->open_file);
		img->open_file = NULL;
	}

	if (img->disk_size == 0)
		return;

	char filename[MAX_FILENAME_SIZE];
	gimagefilename(img, filename, MAX_FILENAME_SIZE);
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

static void gdeleteimage(CellImage *img) {
	gunloadimage(img);
	gr_deleteimagefile(img);
	if (img->image_id)
		images_count--;
	memset(img, 0, sizeof(CellImage));
}

static CellImage *getoldestimagetodelete(int ram) {
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

static void gchecklimits() {
	if (graphics_debug_mode) {
		fprintf(stderr, "Checking limits ram: %d KiB disk: %d KiB count: %d\n",
			cell_images_ram_size / 1024,
			cell_images_disk_size / 1024, images_count);
	}
	char changed = 0;
	while (cell_images_disk_size > max_total_disk_size) {
		gr_deleteimagefile(getoldestimagetodelete(0));
		changed = 1;
	}
	while (cell_images_ram_size > max_total_ram_size) {
		gunloadimage(getoldestimagetodelete(1));
		changed = 1;
	}
	if (graphics_debug_mode && changed) {
		fprintf(stderr, "After cleaning ram: %d KiB disk: %d KiB count: %d\n",
			cell_images_ram_size / 1024,
			cell_images_disk_size / 1024, images_count);
	}
}

static CellImage *gnewimage() {
	CellImage *oldest_image = NULL;
	for (size_t i = 0; i < MAX_CELL_IMAGES; ++i) {
		CellImage *img = &cell_images[i];
		if (img->image_id == 0)
			return img;
		if (!oldest_image ||
		    difftime(img->atime, oldest_image->atime) < 0)
			oldest_image = img;
	}
	gdeleteimage(oldest_image);
	return oldest_image;
}

static CellImage *gnewimagewithid(uint32_t id, int cols, int rows) {
	CellImage *image = gfindimage(id);
	if (image)
		gdeleteimage(image);
	else
		image = gnewimage();
	images_count++;
	image->image_id = id;
	image->cols = cols;
	image->rows = rows;
	return image;
}

static void gtouchimage(CellImage *img) { time(&img->atime); }

static void gloadimage(CellImage *img, int cw, int ch) {
	if (img->scaled_image &&
	    img->scaled_ch == ch && img->scaled_cw == cw)
		return;
	if (img->disk_size == 0)
		return;
	gunloadimage(img);

	char filename[MAX_FILENAME_SIZE];
	gimagefilename(img, filename, MAX_FILENAME_SIZE);
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
	gtouchimage(img);
	gchecklimits();

	imlib_context_set_image(image);
	int orig_w = imlib_image_get_width();
	int orig_h = imlib_image_get_height();

	int scaled_w = (int)img->cols * cw;
	int scaled_h = (int)img->rows * ch;
	img->scaled_image = imlib_create_image(scaled_w, scaled_h);
	if (!img->scaled_image) {
		if (img->status != STATUS_RAM_LOADING_ERROR) {
			fprintf(stderr,
				"error: imlib_create_image(%d, %d) returned null\n", scaled_w, scaled_h);
		}
		img->status = STATUS_RAM_LOADING_ERROR;
		return;
	}
	imlib_context_set_image(img->scaled_image);
	imlib_image_set_has_alpha(1);
	imlib_context_set_blend(0);
	imlib_context_set_color(0, 0, 0, 0);
	imlib_image_fill_rectangle(0, 0, (int)img->cols * cw,
				   (int)img->rows * ch);
	imlib_context_set_anti_alias(1);
	imlib_context_set_blend(1);
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

	imlib_context_set_image(image);
	imlib_free_image();

	img->scaled_ch = ch;
	img->scaled_cw = cw;
	cell_images_ram_size += gimageramsize(img);
	img->status = STATUS_RAM_LOADING_SUCCESS;
}

void gpreviewimage(uint32_t image_id, const char *exec) {
	char command[256];
	size_t len;
	CellImage *img = gfindimage(image_id);
	if (img) {
		char filename[MAX_FILENAME_SIZE];
		gimagefilename(img, filename, MAX_FILENAME_SIZE);
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
/// transmission command, we assume that all all uploads have failed.
int gcheckifstilluploading() {
	if (!graphics_uploading)
		return 0;
	time_t cur_time;
	time(&cur_time);
	double dt = difftime(last_uploading_time, cur_time);
	if (difftime(last_uploading_time, cur_time) < -1.0)
		graphics_uploading = 0;
	return graphics_uploading;
}

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
}

static void gr_showrect(Drawable buf, ImageRect *rect) {
	int w_pix = (rect->end_col - rect->start_col) * rect->cw;
	int h_pix = (rect->end_row - rect->start_row) * rect->ch;
	Display *disp = imlib_context_get_display();
	GC gc = XCreateGC(disp, buf, 0, NULL);
	XSetForeground(disp, gc, 0x00FF00);
	XDrawRectangle(disp, buf, gc, rect->x_pix, rect->y_pix,
		       w_pix - 1, h_pix - 1);
	XSetForeground(disp, gc, 0xFF0000);
	XDrawRectangle(disp, buf, gc, rect->x_pix + 1, rect->y_pix + 1,
		       w_pix - 3, h_pix - 3);
}

static void gr_drawimagerect(Drawable buf, ImageRect *rect) {
	// If we are uploading data then we shouldn't do heavy computation (like
	// displaying graphics), mostly because some versions of tmux may drop
	// pass-through commands if the terminal is too slow.
	if (graphics_uploading)
		return;

	if (rect->image_id == 0) {
		gr_showrect(buf, rect);
		gr_displayinfo(buf, rect, 0x000000, 0xFFFFFF, "id=");
		return;
	}
	CellImage *img = gfindimage(rect->image_id);
	Imlib_Image scaled_image;
	if (!img) {
		gr_showrect(buf, rect);
		gr_displayinfo(buf, rect, 0x000000, 0xFFFFFF, "error id=");
		return;
	}
	gloadimage(img, rect->cw, rect->ch);

	if (!img->scaled_image) {
		gr_showrect(buf, rect);
		gr_displayinfo(buf, rect, 0x000000, 0xFFFFFF, "error id=");
		return;
	}

	gtouchimage(img);

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

	// In debug mode draw bounding boxes and print info.
	if (graphics_debug_mode) {
		gr_showrect(buf, rect);
		gr_displayinfo(buf, rect, 0x000000, 0xFFFFFF, "# ");
	}
}

static void gr_freerect(ImageRect *rect) { memset(rect, 0, sizeof(ImageRect)); }

static int gr_getrectbottom(ImageRect *rect) {
	return rect->y_pix + (rect->end_row - rect->start_row) * rect->ch;
}

void gr_start_drawing(Drawable buf, int cw, int ch) {
	last_cw = cw;
	last_ch = ch;
	drawing_start_time = clock();
}

void gr_finish_drawing(Drawable buf) {
	for (size_t i = 0; i < MAX_IMAGE_RECTS; ++i) {
		ImageRect *rect = &image_rects[i];
		if (!rect->image_id)
			continue;
		gr_drawimagerect(buf, rect);
		gr_freerect(rect);
	}

	if (graphics_debug_mode) {
		clock_t drawing_end_time = clock();
		int milliseconds = 1000 *
				   (drawing_end_time - drawing_start_time) /
				   CLOCKS_PER_SEC;

		Display *disp = imlib_context_get_display();
		/* Window win_unused; */
		/* int x_unused, y_unused; */
		/* unsigned border_unused, depth_unused; */
		/* unsigned buf_width = 0; */
		/* unsigned buf_height = 0; */
		/* XGetGeometry(disp, buf, &win_unused, &x_unused, &y_unused, */
		/*              &buf_width, &buf_height, &border_unused, */
		/*              &depth_unused); */
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
	}
}

void gr_appendimagerect(Drawable buf, uint32_t image_id, int start_col,
			int end_col, int start_row, int end_row, int x_pix,
			int y_pix, int cw, int ch, int reverse) {
	last_cw = cw;
	last_ch = ch;

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

static size_t findchar(char *buf, size_t start, size_t len, char c) {
	while (start < len && buf[start] != c)
		++start;
	return start;
}

typedef struct {
	char *command;
	char *payload;
	char action;
	int quiet;
	int format;
	char transmission_medium;
	char delete_specifier;
	int pix_width, pix_height;
	int rows, columns;
	uint32_t image_id;
	uint32_t image_number;
	uint32_t placement_id;
	int has_more;
	int more;
	int size;
	int offset;
} GraphicsCommand;

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

static void gr_reportsuccess_cmd(GraphicsCommand *cmd) {
	if (cmd->quiet < 1)
		gr_createresponse(cmd->image_id, cmd->image_number, "OK");
}

static void gr_reportsuccess_img(CellImage *img) {
	if (img->quiet < 1)
		gr_createresponse(img->image_id, 0, "OK");
}

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

static void gr_loadimage_and_report(CellImage *img) {
	gloadimage(img, last_cw, last_ch);
	if (!img->scaled_image) {
		gr_reporterror_img(img, "EBADF: could not load image");
	} else {
		gr_reportsuccess_img(img);
	}
}

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

/// Note that we report errors only for the final command (`!more`) to avoid
/// spamming the client.
static void gappenddata(CellImage *img, const char *payload, int more) {
	if (!img)
		img = gfindimage(current_upload_image_id);
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

	size_t data_size = 0;
	char *data = gbase64dec(payload, &data_size);

	if (graphics_debug_mode)
		fprintf(stderr, "appending %u + %zu = %zu bytes\n",
			img->disk_size, data_size, img->disk_size + data_size);

	if (img->disk_size + data_size > max_image_disk_size ||
	    img->expected_size > max_image_disk_size) {
		free(data);
		gr_deleteimagefile(img);
		img->uploading_failure = ERROR_OVER_SIZE_LIMIT;
		if (!more)
			gr_reportuploaderror(img);
		return;
	}

	if (!img->open_file) {
		char filename[MAX_FILENAME_SIZE];
		gimagefilename(img, filename, MAX_FILENAME_SIZE);
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

	fwrite(data, 1, data_size, img->open_file);
	free(data);
	img->disk_size += data_size;
	cell_images_disk_size += data_size;
	gtouchimage(img);

	if (more) {
		current_upload_image_id = img->image_id;
		time(&last_uploading_time);
	} else {
		if (graphics_uploading) {
			graphics_uploading--;
			if (!graphics_uploading)
				graphics_command_result.redraw = 1;
		}
		current_upload_image_id = 0;
		if (img->open_file) {
			fclose(img->open_file);
			img->open_file = NULL;
		}
		img->status = STATUS_UPLOADING_SUCCESS;
		if (img->expected_size &&
		    img->expected_size != img->disk_size) {
			img->status = STATUS_UPLOADING_ERROR;
			img->uploading_failure = ERROR_UNEXPECTED_SIZE;
			gr_reportuploaderror(img);
		} else {
			gr_loadimage_and_report(img);
		}
	}
	gchecklimits();
}

static CellImage *gtransmitdata(GraphicsCommand *cmd) {
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
	if (cmd->transmission_medium == 'f') {
		img = gnewimagewithid(cmd->image_id, cmd->columns, cmd->rows);
		if (!img)
			return NULL;
		img->expected_size = cmd->size;
		last_image_id = cmd->image_id;
		char *original_filename = gbase64dec(cmd->payload, NULL);
		char tmp_filename[MAX_FILENAME_SIZE];
		char tmp_filename_symlink[MAX_FILENAME_SIZE + 4] = {0};
		gimagefilename(img, tmp_filename, MAX_FILENAME_SIZE);
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
				struct stat imgfile_stat;
				stat(tmp_filename, &imgfile_stat);
				img->status = STATUS_UPLOADING_SUCCESS;
				img->disk_size = imgfile_stat.st_size;
				cell_images_disk_size += img->disk_size;
				if (img->disk_size > max_image_disk_size) {
					gr_deleteimagefile(img);
					img->uploading_failure = ERROR_OVER_SIZE_LIMIT;
					gr_reportuploaderror(img);
				}
				else if (img->expected_size &&
				    img->expected_size != img->disk_size) {
					img->status = STATUS_UPLOADING_ERROR;
					img->uploading_failure = ERROR_UNEXPECTED_SIZE;
					gr_reportuploaderror(img);
				} else {
					gr_loadimage_and_report(img);
				}
			}
			unlink(tmp_filename_symlink);
		}
		free(original_filename);
		gchecklimits();
	} else if (cmd->transmission_medium == 'd') {
		img = gnewimagewithid(cmd->image_id, cmd->columns, cmd->rows);
		if (!img)
			return NULL;
		img->expected_size = cmd->size;
		last_image_id = cmd->image_id;
		img->status = STATUS_UPLOADING;
		img->quiet = cmd->quiet;
		graphics_uploading++;
		gappenddata(img, cmd->payload, cmd->more);
	} else {
		gr_reporterror_cmd(
			cmd,
			"EINVAL: transmission medium '%c' is not supported",
			cmd->transmission_medium);
		return NULL;
	}

	return img;
}

static void gr_handle_delete_command(GraphicsCommand *cmd) {
	if (!cmd->delete_specifier) {
		for (size_t i = 0; i < MAX_CELL_IMAGES; ++i) {
			if (cell_images[i].image_id)
				gdeleteimage(&cell_images[i]);
		}
	}
	else if (cmd->delete_specifier == 'I') {
		if (cmd->image_id == 0)
			gr_reporterror_cmd(
			    cmd, "EINVAL: no image id to delete");
		CellImage *image = gfindimage(cmd->image_id);
		if (image)
			gdeleteimage(image);
		gr_reportsuccess_cmd(cmd);
	} else {
		gr_reporterror_cmd(
		    cmd, "EINVAL: unsupported values of the d key : '%c'",
		    cmd->delete_specifier);
	}
}

static void gruncommand(GraphicsCommand *cmd) {
	CellImage *img = NULL;
	switch (cmd->action) {
	case 0:
		if (cmd->has_more) {
			gappenddata(NULL, cmd->payload, cmd->more);
		} else {
			gr_reporterror_cmd(cmd, "EINVAL: no action specified");
		}
		break;
	case 't':
		gtransmitdata(cmd);
		break;
	case 'd':
		gr_handle_delete_command(cmd);
		break;
	case 'p':
		// display (put) the last image
	case 'q':
		// query
	case 'T':
		// transmit and display
	default:
		gr_reporterror_cmd(cmd, "EINVAL: unsupported action: %c",
				   cmd->action);
		return;
	}
}

static void gsetkeyvalue(GraphicsCommand *cmd, char *key_start, char *key_end,
			 char *value_start, char *value_end) {
	if (key_end - key_start != 1) {
		gr_reporterror_cmd(cmd, "EINVAL: unknown key of length %ld: %s",
				   key_end - key_start, key_start);
		return;
	}
	long num = 0;
	if (*key_start == 'a' || *key_start == 't' || *key_start == 'd') {
		if (value_end - value_start != 1) {
			gr_reporterror_cmd(
				cmd,
				"EINVAL: value of 'a', 't' or 'd' must be a "
				"single char: %s",
				key_start);
			return;
		}
	} else {
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
	default:
		gr_reporterror_cmd(cmd, "EINVAL: unsupported key: %s",
				   key_start);
		return;
	}
}

int gparsecommand(char *buf, size_t len) {
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
				gsetkeyvalue(&cmd, key_start, key_end,
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
		gruncommand(&cmd);

	if (graphics_debug_mode) {
		fprintf(stderr, "Response: %s\n",
			graphics_command_result.response);
	}

	if (cmd.quiet) {
		if (!graphics_command_result.error || cmd.quiet >= 2)
			graphics_command_result.response[0] = '\0';
	}

	return 1;
}

static const char g_base64_digits[] = {
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

static char gbase64dec_getc(const char **src) {
	while (**src && !isprint(**src))
		(*src)++;
	return **src ? *((*src)++) : '='; /* emulate padding if string ends */
}

char *gbase64dec(const char *src, size_t *size) {
	size_t in_len = strlen(src);
	char *result, *dst;

	result = dst = malloc((in_len + 3) / 4 * 3 + 1);
	while (*src) {
		int a = g_base64_digits[(unsigned char)gbase64dec_getc(&src)];
		int b = g_base64_digits[(unsigned char)gbase64dec_getc(&src)];
		int c = g_base64_digits[(unsigned char)gbase64dec_getc(&src)];
		int d = g_base64_digits[(unsigned char)gbase64dec_getc(&src)];

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

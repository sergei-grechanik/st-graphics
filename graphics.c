#define  _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <Imlib2.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#include "graphics.h"

// Defined in st.c
char *base64dec(const char *src);

#define MAX_FILENAME_SIZE 256
#define MAX_CELL_IMAGES 256

enum scale_mode {
	SCALE_MODE_CONTAIN = 0,
	SCALE_MODE_FILL = 1
};

typedef struct {
	uint32_t image_id;
	uint16_t rows, cols;
	time_t atime;
	unsigned disk_size;
	Imlib_Image scaled_image;
	uint16_t scaled_cw, scaled_ch;
	char scale_mode;
} CellImage;

static CellImage cell_images[MAX_CELL_IMAGES] = {{0}};
static unsigned cell_images_disk_size = 0;
static unsigned cell_images_ram_size = 0;
static uint32_t last_image_id = 0;
static char temp_dir[] = "/tmp/st-images-XXXXXX";
static unsigned char reverse_table[256];

void graphicsinit(Display *disp, Visual *vis, Colormap cm)
{
	if (!mkdtemp(temp_dir)) {
	    fprintf(stderr, "Could not create temporary dir from template %s\n",
				temp_dir);
	    abort();
	}
	imlib_context_set_display(disp);
	imlib_context_set_visual(vis);
	imlib_context_set_colormap(cm);
	imlib_context_set_anti_alias(1);
	imlib_context_set_blend(1);
	imlib_set_cache_size(16 * 1024 * 1024);

	for (size_t i = 0; i < 256; ++i)
		reverse_table[i] = 255 - i;
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
	snprintf(out, max_len, "%s/img-%d", temp_dir, img->image_id);
}

static unsigned gimageramsize(CellImage *img) {
	return (unsigned)img->rows * img->cols * img->scaled_ch * img->scaled_cw;
}

static void gunloadimage(CellImage *img) {
	if (!img->scaled_image)
		return;
	imlib_context_set_image(img->scaled_image);
	imlib_free_image();
	cell_images_ram_size -= gimageramsize(img);
	img->scaled_image = NULL;
	img->scaled_ch = img->scaled_cw = 0;
}

static void gdeleteimage(CellImage *img) {
	gunloadimage(img);
	char filename[MAX_FILENAME_SIZE];
	gimagefilename(img, filename, MAX_FILENAME_SIZE);
	remove(filename);
	cell_images_disk_size -= img->disk_size;
	memset(img, 0, sizeof(CellImage));
}

static CellImage *gnewimage() {
	CellImage *oldest_image = NULL;
	for (size_t i = 0; i < MAX_CELL_IMAGES; ++i) {
		CellImage *img = &cell_images[i];
		if (img->image_id == 0)
			return img;
		if (!oldest_image || difftime(img->atime, oldest_image->atime) < 0)
			oldest_image = img;
	}
	gdeleteimage(oldest_image);
	return oldest_image;
}

static CellImage *gnewimagewithid(uint32_t id) {
	CellImage *image = gfindimage(id);
	if (image)
		gdeleteimage(image);
	else
		image = gnewimage();
	image->image_id = id;
	return image;
}

static void gtouchimage(CellImage* img) {
	time(&img->atime);
}

static void gloadimage(CellImage *img, int cw, int ch) {
	if (img->scaled_image && img->scaled_ch == ch && img->scaled_cw == cw)
		return;
	gunloadimage(img);

	char filename[MAX_FILENAME_SIZE];
	gimagefilename(img, filename, MAX_FILENAME_SIZE);
	Imlib_Image image = imlib_load_image(filename);
	if (!image) {
		fprintf(stderr, "error: could not load image: %s\n", filename);
		return;
	}
	imlib_context_set_image(image);
	int orig_w = imlib_image_get_width();
	int orig_h = imlib_image_get_height();

	int scaled_w = (int)img->cols*cw;
	int scaled_h = (int)img->rows*ch;
	img->scaled_image = imlib_create_image(scaled_w, scaled_h);
	if (!img->scaled_image) {
		fprintf(stderr, "error: imlib_create_image returned null\n");
		return;
	}
	imlib_context_set_image(img->scaled_image);
	imlib_image_set_has_alpha(1);
	imlib_context_set_blend(0);
	imlib_context_set_color(0, 0, 0, 0);
	imlib_image_fill_rectangle(0, 0, (int)img->cols*cw, (int)img->rows*ch);
	imlib_context_set_blend(1);
	if (img->scale_mode == SCALE_MODE_FILL) {
		imlib_blend_image_onto_image(image, 1, 0, 0, orig_w, orig_h, 0, 0, scaled_w,
									 scaled_h);
	}
	else {
		int dest_x, dest_y;
		int dest_w, dest_h;
		if (scaled_w*orig_h > orig_w*scaled_h) {
			// If the box is wider than the original image, fit to height.
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
		imlib_blend_image_onto_image(image, 1, 0, 0, orig_w, orig_h, dest_x, dest_y, dest_w, dest_h);
	}

	imlib_context_set_image(image);
	imlib_free_image();

	img->scaled_ch = ch;
	img->scaled_cw = cw;
	cell_images_ram_size += gimageramsize(img);
}

void gdrawimagestripe(Drawable buf, uint32_t image_id, int start_col, int end_col, int row,
					  int x_pix, int y_pix, int cw, int ch, int reverse)
{
	fprintf(stderr, "stripe [%d;%d):%d at (%d, %d)\n", start_col, end_col, row, x_pix, y_pix);
	if (image_id == 0)
		image_id = last_image_id;
	CellImage *cell_image = gfindimage(image_id);
	Imlib_Image scaled_image;
	if (!cell_image) {
		return;
	}
	gloadimage(cell_image, cw, ch);
	gtouchimage(cell_image);

	if (!cell_image->scaled_image) {
		fprintf(stderr, "error: could not load image %d\n", image_id);
		return;
	}

	imlib_context_set_image(cell_image->scaled_image);
	imlib_context_set_drawable(buf);
	if (reverse) {
		Imlib_Color_Modifier cm = imlib_create_color_modifier();
		imlib_context_set_color_modifier(cm);
		imlib_set_color_modifier_tables(reverse_table, reverse_table, reverse_table, NULL);
	}
	imlib_render_image_part_on_drawable_at_size(
	    start_col * cw, row * ch, (end_col - start_col) * cw, ch, x_pix,
	    y_pix, (end_col - start_col) * cw, ch);
	if (reverse) {
		imlib_free_color_modifier();
		imlib_context_set_color_modifier(NULL);
	}
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
	int pix_width, pix_height;
	int rows, columns;
	uint32_t image_id;
	uint32_t image_number;
	uint32_t placement_id;
	int more;
	int error;
} GraphicsCommand;

static void gtransmitdata(GraphicsCommand *cmd) {
	if (cmd->image_number != 0) {
		fprintf(stderr, "error: image numbers (I) are not supported: %s\n", cmd->command);
		cmd->error = 1;
		return;
	}

	if (cmd->image_id == 0) {
		fprintf(stderr, "error: image id is not specified or zero: %s\n", cmd->command);
		cmd->error = 1;
		return;
	}

	CellImage *img = NULL;
	if (cmd->transmission_medium == 'f') {
		img = gnewimagewithid(cmd->image_id);
		if (!img)
			return;
		last_image_id = cmd->image_id;
		char *filename = base64dec(cmd->payload);
		char tmp_filename[MAX_FILENAME_SIZE];
		gimagefilename(img, tmp_filename, MAX_FILENAME_SIZE);
		if (symlink(filename, tmp_filename)) {
			fprintf(stderr, "error: could not create a symlink from %s to %s\n", filename, tmp_filename);
		}
		free(filename);
	} else {
		fprintf(stderr, "error: transmission medium '%c' is not supported: %s\n", cmd->transmission_medium, cmd->command);
		cmd->error = 1;
		return;
	}

	img->rows = cmd->rows;
	img->cols = cmd->columns;
}

static void gruncommand(GraphicsCommand *cmd) {
	switch (cmd->action) {
		case 0:
			fprintf(stderr, "error: no action specified: %s\n", cmd->command);
			cmd->error = 1;
			return;
		case 't':
			gtransmitdata(cmd);
			break;
		case 'p':
			// display (put) the last image
		case 'q':
			// query
		case 'T':
			// transmit and display
		default:
			fprintf(stderr, "error: unsupported action: %c\n", cmd->action);
			return;
	}
}

static void gsetkeyvalue(GraphicsCommand *cmd, char *key_start, char *key_end, char *value_start, char *value_end) {
	if (key_end - key_start != 1) {
		fprintf(stderr, "error: unknown key of length %ld: %s\n", key_end - key_start, key_start);
		cmd->error = 1;
		return;
	}
	long num = 0;
	if (*key_start == 'a' || *key_start == 't') {
		if (value_end - value_start != 1) {
			fprintf(stderr, "error: value of 'a' or 't' must be a single char: %s\n", key_start);
			cmd->error = 1;
			return;
		}
	} else {
		char *num_end = NULL;
		num = strtol(value_start, &num_end, 10);
		if (num_end != value_end) {
			fprintf(stderr, "error: could not parse number value: %s\n", key_start);
			cmd->error = 1;
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
		default:
			fprintf(stderr, "error: unsupported key: %s\n", key_start);
			cmd->error = 1;
			return;
	}
}

int gparsecommand(char *buf, size_t len) {
	if (buf[0] != 'G')
		return 0;
	++buf;

	GraphicsCommand cmd = {.command = buf};
	char state = 'k';
	char *key_start = buf;
	char *key_end = NULL;
	char *val_start = NULL;
	char *val_end = NULL;
	char *c = buf;
	while (1) {
		if (state == 'k') {
			switch (*c) {
				case ',':
				case ';':
				case '\0':
					state = *c == ',' ? 'k' : 'p';
					key_end = c;
					fprintf(stderr,
						"error: key without value at "
						"char %ld of %s\n",
						c - buf, buf);
					cmd.error = 1;
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
					gsetkeyvalue(&cmd, key_start, key_end, val_start, val_end);
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

	if (cmd.error)
		return 1;

	gruncommand(&cmd);
	return 1;
}

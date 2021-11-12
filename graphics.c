#define  _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <Imlib2.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "graphics.h"

#define MAX_FILENAME_SIZE 256
#define MAX_CELL_IMAGES 256
static char *temp_dir_template = "/tmp/st-images-XXXXXX";

typedef struct {
	uint32_t image_id;
	uint16_t rows, cols;
	time_t atime;
	unsigned disk_size;
	Imlib_Image loaded_image;
	uint16_t loaded_cw, loaded_ch;
} CellImage;

static CellImage cell_images[MAX_CELL_IMAGES] = {{0}};
static unsigned cell_images_disk_size = 0;
static unsigned cell_images_ram_size = 0;
static char *temp_dir = 0;

void graphicsinit(Display *disp, Visual *vis, Colormap cm)
{
	temp_dir = mkdtemp(temp_dir_template);
	if (!temp_dir) {
	    fprintf(stderr, "Could now create temporary dir from template %s\n",
				temp_dir_template);
	    abort();
	}
	imlib_context_set_display(disp);
	imlib_context_set_visual(vis);
	imlib_context_set_colormap(cm);
	imlib_context_set_anti_alias(1);
	imlib_context_set_blend(1);
	imlib_set_cache_size(16 * 1024 * 1024);
}

static CellImage *gfindimage(image_id) {
	for (size_t i = 0; i < MAX_CELL_IMAGES; ++i) {
		if (cell_images[i].image_id == image_id)
			return &cell_images[i];
	}
	return 0;
}

static void gimagefilename(CellImage *img, char *out, size_t max_len) {
	snprintf(out, max_len, "%s/img-%d", temp_dir, img->image_id);
}

static unsigned gimageramsize(CellImage *img) {
	return (unsigned)img->rows * img->cols * img->loaded_ch * img->loaded_cw;
}

static void gunloadimage(CellImage *img) {
	if (!img->loaded_image)
		return;
	imlib_context_set_image(img->loaded_image);
	imlib_free_image();
	cell_images_ram_size -= gimageramsize(img);
	img->loaded_image = 0;
	img->loaded_ch = img->loaded_cw = 0;
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
	CellImage *oldest_image = 0;
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

static void gloadimage(CellImage *img, int cw, int ch) {
	if (img->loaded_image && img->loaded_ch == ch && img->loaded_cw == cw)
		return;
	gunloadimage(img);

	int loaded_w = (int)img->cols*cw;
	int loaded_h = (int)img->rows*ch;
	img->loaded_image = imlib_create_image(loaded_w, loaded_h);
	if (!img->loaded_image)
		return;

	char filename[MAX_FILENAME_SIZE];
	gimagefilename(img, filename, MAX_FILENAME_SIZE);
	/* Imlib_Image image = imlib_load_image(filename); */
	Imlib_Image image = imlib_load_image("/home/sgrechanik/temp/png/Ducati_side_shadow-fs8.png");
	if (!image)
		return;
	int orig_w = imlib_image_get_width();
	int orig_h = imlib_image_get_height();

	imlib_context_set_image(img->loaded_image);
	imlib_context_set_color(0, 0, 0, 0);
	imlib_image_fill_rectangle(0, 0, (int)img->cols*cw, (int)img->rows*ch);
	imlib_blend_image_onto_image(image, 1, 0, 0, orig_w, orig_h, 0, 0, loaded_w,
								 loaded_h);

	imlib_context_set_image(image);
	imlib_free_image();

	img->loaded_ch = ch;
	img->loaded_cw = cw;
	cell_images_ram_size += gimageramsize(img);
}

Imlib_Image image = 0;

float cropt = 0, rendert = 0, freet = 0;

void gdrawimagestripe(Drawable buf, uint32_t image_id, int start_col, int end_col, int row,
					  int x_pix, int y_pix, int cw, int ch)
{
	if (!image_id)
	CellImage *cell_image = gfindimage(image_id);
	Imlib_Image loaded_image;
	if (!image_id || !cell_image) {
		
	}
	gloadimage(cell_image, cw, ch);

	imlib_context_set_image(loaded_image);
	imlib_context_set_drawable(buf);
	imlib_render_image_part_on_drawable_at_size(start_col*cw, row*ch, (end_col - start_col)*cw, ch, x_pix, y_pix, (end_col - start_col)*cw, ch);
}

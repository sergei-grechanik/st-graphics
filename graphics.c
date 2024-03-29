/* The MIT License

   Copyright (c) 2021-2023 Sergei Grechanik <sergei.grechanik@gmail.com>

   Permission is hereby granted, free of charge, to any person obtaining
   a copy of this software and associated documentation files (the
   "Software"), to deal in the Software without restriction, including
   without limitation the rights to use, copy, modify, merge, publish,
   distribute, sublicense, and/or sell copies of the Software, and to
   permit persons to whom the Software is furnished to do so, subject to
   the following conditions:

   The above copyright notice and this permission notice shall be
   included in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
   NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
   BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
   ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
   SOFTWARE.
*/

////////////////////////////////////////////////////////////////////////////////
//
// This file implements a subset of the kitty graphics protocol.
//
////////////////////////////////////////////////////////////////////////////////

#define _POSIX_C_SOURCE 200809L

#include <zlib.h>
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
#include <errno.h>

#include "graphics.h"
#include "khash.h"

#define MAX_FILENAME_SIZE 256
#define MAX_INFO_LEN 256
#define MAX_IMAGE_RECTS 20

enum ScaleMode {
	SCALE_MODE_UNSET = 0,
	/// Stretch or shrink the image to fill the box, ignoring aspect ratio.
	SCALE_MODE_FILL = 1,
	/// Preserve aspect ratio and fit to width or to height so that the
	/// whole image is visible.
	SCALE_MODE_CONTAIN = 2,
	/// Do not scale. The image may be cropped if the box is too small.
	SCALE_MODE_NONE = 3,
	/// Do not scale, unless the box is too small, in which case the image
	/// will be shrunk like with `SCALE_MODE_CONTAIN`.
	SCALE_MODE_NONE_OR_CONTAIN = 4,
};

/// The status of an image. Each image uploaded to the terminal is cached on
/// disk, then it is loaded to ram when needed.
enum ImageStatus {
	STATUS_UNINITIALIZED = 0,
	STATUS_UPLOADING = 1,
	STATUS_UPLOADING_ERROR = 2,
	STATUS_UPLOADING_SUCCESS = 3,
	STATUS_RAM_LOADING_ERROR = 4,
	STATUS_RAM_LOADING_SUCCESS = 5,
};

const char *image_status_strings[6] = {
	"STATUS_UNINITIALIZED",
	"STATUS_UPLOADING",
	"STATUS_UPLOADING_ERROR",
	"STATUS_UPLOADING_SUCCESS",
	"STATUS_RAM_LOADING_ERROR",
	"STATUS_RAM_LOADING_SUCCESS",
};

enum ImageUploadingFailure {
	ERROR_OVER_SIZE_LIMIT = 1,
	ERROR_CANNOT_OPEN_CACHED_FILE = 2,
	ERROR_UNEXPECTED_SIZE = 3,
	ERROR_CANNOT_COPY_FILE = 4,
};

const char *image_uploading_failure_strings[5] = {
	"NO_ERROR",
	"ERROR_OVER_SIZE_LIMIT",
	"ERROR_CANNOT_OPEN_CACHED_FILE",
	"ERROR_UNEXPECTED_SIZE",
	"ERROR_CANNOT_COPY_FILE",
};

struct Image;
struct ImagePlacement;

KHASH_MAP_INIT_INT(id2image, struct Image *)
KHASH_MAP_INIT_INT(id2placement, struct ImagePlacement *)

/// The structure representing an image. It's the original image, we store it on
/// disk, and then load it to ram when needed, but we don't display it directly.
typedef struct Image {
	/// The client id (the one specified with 'i='). Must be nonzero.
	uint32_t image_id;
	/// The client id specified in the query command (`a=q`). This one must
	/// be used to create the response if it's non-zero.
	uint32_t query_id;
	/// The number specified in the transmission command (`I=`). If
	/// non-zero, it may be used to identify the image instead of the
	/// image_id, and it also should be mentioned in responses.
	uint32_t image_number;
	/// The last time when the image was displayed or otherwise touched.
	struct timespec atime;
	/// The global index of the creation command. Used to decide which image
	/// is newer if they have the same image number.
	uint64_t global_command_index;
	/// The size of the corresponding file cached on disk.
	unsigned disk_size;
	/// The expected size of the image file (specified with 'S='), used to
	/// check if uploading uploading succeeded.
	unsigned expected_size;
	/// Format specification (see the `f=` key).
	int format;
	/// Compression mode (see the `o=` key).
	char compression;
	/// Pixel width and height if format is 32 or 24.
	int pix_width, pix_height;
	/// The status (see `ImageStatus`).
	char status;
	/// The reason of uploading failure (see `ImageUploadingFailure`).
	char uploading_failure;
	/// Whether failures and successes should be reported ('q=').
	char quiet;
	/// The file corresponding to the on-disk cache, used when uploading.
	FILE *open_file;
	/// The original image loaded into RAM.
	Imlib_Image original_image;
	/// Image placements.
	khash_t(id2placement) *placements;
	/// The default placement.
	uint32_t default_placement;
	/// The initial placement id, specified with the transmission command,
	/// used to report success or failure.
	uint32_t initial_placement_id;
} Image;

typedef struct ImagePlacement {
	/// The original image.
	Image *image;
	/// The id of the placement. Must be nonzero.
	uint32_t placement_id;
	/// The last time when the placement was displayed or otherwise touched.
	struct timespec atime;
	/// Whether the placement shouldn't be unloaded from RAM.
	char protected;
	/// Whether the placement is used only for Unicode placeholders.
	char virtual;
	/// The scaling mode (see `ScaleMode`).
	char scale_mode;
	/// Height and width in cells.
	uint16_t rows, cols;
	/// Top-left corner of the source rectangle ('x=' and 'y=').
	int src_pix_x, src_pix_y;
	/// Height and width of the source rectangle (zero if full image).
	int src_pix_width, src_pix_height;
	/// The image appropriately scaled and loaded into RAM.
	Imlib_Image scaled_image;
	/// The dimensions of the cell used to scale the image. If cell
	/// dimensions are changed (font change), the image will be rescaled.
	uint16_t scaled_cw, scaled_ch;
	/// If true, do not move the cursor when displaying this placement
	/// (non-virtual placements only).
	char do_not_move_cursor;
} ImagePlacement;

/// A rectangular piece of an image to be drawn.
typedef struct {
	uint32_t image_id;
	uint32_t placement_id;
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

static Image *gr_find_image(uint32_t image_id);
static void gr_get_image_filename(Image *img, char *out, size_t max_len);
static void gr_delete_image(Image *img);
static void gr_check_limits();
static char *gr_base64dec(const char *src, size_t *size);
static void sanitize_str(char *str, size_t max_len);
static const char *sanitized_filename(const char *str);

/// The array of image rectangles to draw. It is reset each frame.
static ImageRect image_rects[MAX_IMAGE_RECTS] = {{0}};
/// The known images (including the ones being uploaded).
static khash_t(id2image) *images = NULL;
/// The total number of placements in all images.
static unsigned total_placement_count = 0;
/// The total size of all image files stored in the on-disk cache.
static int64_t images_disk_size = 0;
/// The total size of all images and placements loaded into ram.
static int64_t images_ram_size = 0;
/// The id of the last loaded image.
static uint32_t last_image_id = 0;
/// Current cell width and heigh in pixels.
static int current_cw = 0, current_ch = 0;
/// The id of the currently uploaded image (when using direct uploading).
static uint32_t current_upload_image_id = 0;
/// The time when the current frame drawing started (used for debugging fps).
static clock_t drawing_start_time;
/// The global index of the current command.
static uint64_t global_command_counter = 0;

/// The directory where the cache files are stored.
static char cache_dir[MAX_FILENAME_SIZE - 16];

/// The table used for color inversion.
static unsigned char reverse_table[256];

// Declared in the header.
GraphicsDebugMode graphics_debug_mode = GRAPHICS_DEBUG_NONE;
char graphics_display_images = 1;
GraphicsCommandResult graphics_command_result = {0};

// Defined in config.h
extern const char graphics_cache_dir_template[];
extern unsigned graphics_max_single_image_file_size;
extern unsigned graphics_total_file_cache_size;
extern unsigned graphics_max_single_image_ram_size;
extern unsigned graphics_max_total_ram_size;
extern unsigned graphics_max_total_placements;
extern double graphics_excess_tolerance_ratio;


#define MIN(a, b)		((a) < (b) ? (a) : (b))
#define MAX(a, b)		((a) < (b) ? (b) : (a))

////////////////////////////////////////////////////////////////////////////////
// Logging.
////////////////////////////////////////////////////////////////////////////////

#define GR_LOG(...) \
	do { if(graphics_debug_mode) fprintf(stderr, __VA_ARGS__); } while(0)

////////////////////////////////////////////////////////////////////////////////
// Basic image management functions (create, delete, find, etc).
////////////////////////////////////////////////////////////////////////////////

/// Finds the image corresponding to the client id. Returns NULL if cannot find.
static Image *gr_find_image(uint32_t image_id) {
	khiter_t k = kh_get(id2image, images, image_id);
	if (k == kh_end(images))
		return NULL;
	Image *res = kh_value(images, k);
	return res;
}

/// Finds the newest image corresponding to the image number. Returns NULL if
/// cannot find.
static Image *gr_find_image_by_number(uint32_t image_number) {
	if (image_number == 0)
		return NULL;
	Image *newest_img = NULL;
	Image *img = NULL;
	kh_foreach_value(images, img, {
		if (img->image_number == image_number &&
		    (!newest_img || newest_img->global_command_index <
					    img->global_command_index))
			newest_img = img;
	});
	if (!newest_img)
		GR_LOG("Image number %u not found\n", image_number);
	else
		GR_LOG("Found image number %u, its id is %u\n", image_number,
		       img->image_id);
	return newest_img;
}

/// Finds the placement corresponding to the id. If the placement id is 0,
/// returns some default placement.
static ImagePlacement *gr_find_placement(Image *img, uint32_t placement_id) {
	if (!img)
		return NULL;
	if (placement_id == 0) {
		// Try to get the default placement.
		ImagePlacement *dflt = NULL;
		if (img->default_placement != 0)
			dflt = gr_find_placement(img, img->default_placement);
		if (dflt)
			return dflt;
		// If there is no default placement, return the first one and
		// set it as the default.
		kh_foreach_value(img->placements, dflt, {
			img->default_placement = dflt->placement_id;
			return dflt;
		});
		// If there are no placements, return NULL.
		return NULL;
	}
	khiter_t k = kh_get(id2placement, img->placements, placement_id);
	if (k == kh_end(img->placements))
		return NULL;
	ImagePlacement *res = kh_value(img->placements, k);
	return res;
}

/// Finds the placement by image id and placement id.
static ImagePlacement *gr_find_image_and_placement(uint32_t image_id,
						   uint32_t placement_id) {
	return gr_find_placement(gr_find_image(image_id), placement_id);
}

/// Writes the name of the on-disk cache file to `out`. `max_len` should be the
/// size of `out`. The name will be something like "/tmp/st-images-xxx/img-ID".
static void gr_get_image_filename(Image *img, char *out, size_t max_len) {
	snprintf(out, max_len, "%s/img-%.3u", cache_dir, img->image_id);
}

/// Returns the (estimation) of the RAM size used by the image when loaded.
static unsigned gr_image_ram_size(Image *img) {
	return (unsigned)img->pix_width * img->pix_height * 4;
}

/// Returns the (estimation) of the RAM size used by the placemenet when loaded.
static unsigned gr_placement_ram_size(ImagePlacement *placement) {
	return (unsigned)placement->rows * placement->cols *
	       placement->scaled_ch * placement->scaled_cw * 4;
}

/// Unload the image from RAM (i.e. delete the corresponding imlib object).
/// If the on-disk file of the image is preserved, it can be reloaded later.
static void gr_unload_image(Image *img) {
	if (!img->original_image)
		return;

	imlib_context_set_image(img->original_image);
	imlib_free_image_and_decache();

	images_ram_size -= gr_image_ram_size(img);

	img->original_image = NULL;

	GR_LOG("After unloading image %u ram: %ld KiB\n", img->image_id,
	       images_ram_size / 1024);
}

/// Unload the placement from RAM (i.e. delete the corresponding imlib object).
/// If the on-disk file of the corresponding image is preserved, it can be
/// reloaded later.
static void gr_unload_placement(ImagePlacement *placement) {
	if (!placement->scaled_image)
		return;

	imlib_context_set_image(placement->scaled_image);
	imlib_free_image();

	images_ram_size -= gr_placement_ram_size(placement);

	placement->scaled_image = NULL;
	placement->scaled_ch = placement->scaled_cw = 0;

	GR_LOG("After unloading placement %u/%u ram: %ld KiB\n",
	       placement->image->image_id, placement->placement_id,
	       images_ram_size / 1024);
}

/// Delete the on-disk cache file corresponding to the image. The in-ram image
/// object (if it exists) is not deleted, so the image may still be displayed
/// with the same cell width/height values.
static void gr_delete_imagefile(Image *img) {
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

	images_disk_size -= img->disk_size;
	img->disk_size = 0;

	GR_LOG("After deleting image file %u disk: %ld KiB\n", img->image_id,
	       images_disk_size / 1024);
}

/// Deletes the given placement: unloads, frees the object, but doesn't change
/// the `placements` hash table.
static void gr_delete_placement_keep_id(ImagePlacement *placement) {
	if (!placement)
		return;
	GR_LOG("Deleting placement %u/%u\n", placement->image->image_id,
	       placement->placement_id);
	gr_unload_placement(placement);
	free(placement);
	total_placement_count--;
}

/// Deletes all placements of `img`.
static void gr_delete_all_placements(Image *img) {
	ImagePlacement *placement = NULL;
	kh_foreach_value(img->placements, placement, {
		gr_delete_placement_keep_id(placement);
	});
	kh_clear(id2placement, img->placements);
}

/// Deletes the given image: unloads, deletes the file, frees the Image object,
/// but doesn't change the `images` hash table.
static void gr_delete_image_keep_id(Image *img) {
	if (!img)
		return;
	GR_LOG("Deleting image %u\n", img->image_id);
	gr_unload_image(img);
	gr_delete_imagefile(img);
	gr_delete_all_placements(img);
	kh_destroy(id2placement, img->placements);
	free(img);
}

/// Deletes the given image: unloads, deletes the file, frees the Image object,
/// and also removes it from `images`.
static void gr_delete_image(Image *img) {
	if (!img)
		return;
	uint32_t id = img->image_id;
	gr_delete_image_keep_id(img);
	khiter_t k = kh_get(id2image, images, id);
	kh_del(id2image, images, k);
}

/// Deletes the given placement: unloads, frees the object, and also removes it
/// from `placements`.
static void gr_delete_placement(ImagePlacement *placement) {
	if (!placement)
		return;
	uint32_t id = placement->placement_id;
	Image *img = placement->image;
	gr_delete_placement_keep_id(placement);
	khiter_t k = kh_get(id2placement, img->placements, id);
	kh_del(id2placement, img->placements, k);
}

/// Deletes all images and clears `images`.
static void gr_delete_all_images() {
	Image *img = NULL;
	kh_foreach_value(images, img, {
		gr_delete_image_keep_id(img);
	});
	kh_clear(id2image, images);
}

/// A helper to compare timespecs.
int gr_cmp_timespec(const struct timespec *t1, const struct timespec *t2) {
	if (t1->tv_sec < t2->tv_sec)
		return -1;
	if (t1->tv_sec > t2->tv_sec)
		return 1;
	if (t1->tv_nsec < t2->tv_nsec)
		return -1;
	if (t1->tv_nsec > t2->tv_nsec)
		return 1;
	return 0;
}

/// A helper to compare images by atime for qsort.
static int gr_cmp_images_by_atime(const void *a, const void *b) {
	Image *img_a = *(Image *const *)a;
	Image *img_b = *(Image *const *)b;
	return gr_cmp_timespec(&img_a->atime, &img_b->atime);
}

/// A helper to compare placements by atime for qsort.
static int gr_cmp_placements_by_atime(const void *a, const void *b) {
	ImagePlacement *p_a = *(ImagePlacement **)a;
	ImagePlacement *p_b = *(ImagePlacement **)b;
	return gr_cmp_timespec(&p_a->atime, &p_b->atime);
}

/// Returns an array of pointers to all images sorted by atime. The size of the
/// array is `kh_size(images)`. Returns NULL if there are no images.
static Image **gr_get_images_sorted_by_atime() {
	if (kh_size(images) == 0)
		return NULL;
	Image **images_sorted = malloc(sizeof(Image *) * kh_size(images));
	Image *img = NULL;
	int i = 0;
	kh_foreach_value(images, img, {
		images_sorted[i] = img;
		i++;
	});
	qsort(images_sorted, kh_size(images), sizeof(Image *),
	      gr_cmp_images_by_atime);
	return images_sorted;
}

/// Returns an array of pointers to all placements sorted by atime. The size of
/// the array is `total_placement_count`. Returns NULL if there are no
/// placements. The array must be freed with `free()` by the caller.
static ImagePlacement **gr_get_placements_sorted_by_atime() {
	if (total_placement_count == 0)
		return NULL;
	ImagePlacement **placements_sorted =
		malloc(sizeof(ImagePlacement *) * total_placement_count);
	Image *img = NULL;
	ImagePlacement *placement = NULL;
	int i = 0;
	kh_foreach_value(images, img, {
		kh_foreach_value(img->placements, placement, {
			if (i < total_placement_count)
				placements_sorted[i] = placement;
			i++;
		});
	});
	if (i != total_placement_count) {
		// This should never happen, but if it does, report an error,
		// set the correct count, and redo the sorting.
		fprintf(stderr,
			"error: total_placement_count (%d) is wrong, the "
			"correct value is %d\n",
			total_placement_count, i);
		free(placements_sorted);
		total_placement_count = i;
		return gr_get_placements_sorted_by_atime();
	}
	qsort(placements_sorted, total_placement_count,
	      sizeof(ImagePlacement *), gr_cmp_placements_by_atime);
	return placements_sorted;
}

/// Returns the limit adjusted by the excess tolerance ratio.
static inline unsigned apply_tolerance(unsigned limit) {
	return limit + (unsigned)(limit * graphics_excess_tolerance_ratio);
}

/// Checks RAM and disk cache limits and deletes/unloads some images.
static void gr_check_limits() {
	Image **images_sorted = NULL;
	ImagePlacement **placements_sorted = NULL;
	int images_begin = 0;
	int placements_begin = 0;
	// First reduce the number of images if there are too many.
	if (kh_size(images) > apply_tolerance(graphics_max_total_placements)) {
		GR_LOG("Too many images: %d\n", kh_size(images));
		images_sorted = gr_get_images_sorted_by_atime();
		int to_delete = kh_size(images) -
				graphics_max_total_placements;
		for (int i = 0; i < to_delete; i++) {
			gr_delete_image(images_sorted[images_begin]);
			images_begin++;
		}
	}
	// Then reduce the number of placements if there are too many.
	if (total_placement_count >
	    apply_tolerance(graphics_max_total_placements)) {
		GR_LOG("Too many placements: %d\n", total_placement_count);
		placements_sorted = gr_get_placements_sorted_by_atime();
		int to_delete = total_placement_count -
				graphics_max_total_placements;
		for (int i = 0; i < to_delete; i++) {
			if (placements_sorted[placements_begin]->protected)
				break;
			gr_delete_placement(placements_sorted[placements_begin]);
			placements_begin++;
		}
	}
	// Then reduce the size of the image file cache.
	if (images_disk_size >
	    apply_tolerance(graphics_total_file_cache_size)) {
		GR_LOG("Too big disk cache: %ld KiB\n",
		       images_disk_size / 1024);
		if (!images_sorted)
			images_sorted = gr_get_images_sorted_by_atime();
		int i = 0;
		int total_images = kh_size(images);
		while (images_disk_size > graphics_total_file_cache_size &&
		       i < total_images) {
			gr_delete_imagefile(images_sorted[images_begin + i]);
			i++;
		}
	}
	// Then unload images from RAM.
	if (images_ram_size > apply_tolerance(graphics_max_total_ram_size)) {
		GR_LOG("Too much ram: %ld KiB\n", images_ram_size / 1024);
		if (!images_sorted)
			images_sorted = gr_get_images_sorted_by_atime();
		int i = 0;
		unsigned total_images = kh_size(images);
		while (images_ram_size > graphics_max_total_ram_size &&
		       i < total_images) {
			gr_unload_image(images_sorted[images_begin + i]);
			i++;
		}
	}
	// Then unload placements from RAM.
	if (images_ram_size > apply_tolerance(graphics_max_total_ram_size)) {
		GR_LOG("Still too much ram: %ld KiB\n", images_ram_size / 1024);
		if (!placements_sorted)
			placements_sorted = gr_get_placements_sorted_by_atime();
		int i = 0;
		while (images_ram_size > graphics_max_total_ram_size &&
		       i < total_placement_count) {
			if (!placements_sorted[placements_begin + i]->protected)
				gr_unload_placement(
					placements_sorted[placements_begin +
							  i]);
			i++;
		}
	}
	if (images_sorted || placements_sorted) {
		GR_LOG("After cleaning:  ram: %ld KiB  disk: %ld KiB  "
		       "img count: %d  placement count: %d\n",
		       images_ram_size / 1024, images_disk_size / 1024,
		       kh_size(images), total_placement_count);
	}
	free(images_sorted);
	free(placements_sorted);
}

/// Unloads all images by user request.
void gr_unload_images_to_reduce_ram() {
	Image *img = NULL;
	ImagePlacement *placement = NULL;
	kh_foreach_value(images, img, {
		kh_foreach_value(img->placements, placement, {
			if (placement->protected)
				continue;
			gr_unload_placement(placement);
		});
		gr_unload_image(img);
	});
}

/// Update the atime of the image.
static void gr_touch_image(Image *img) {
	clock_gettime(CLOCK_MONOTONIC, &img->atime);
}

/// Update the atime of the placement. Touches the images too.
static void gr_touch_placement(ImagePlacement *placement) {
	clock_gettime(CLOCK_MONOTONIC, &placement->atime);
	placement->image->atime = placement->atime;
}

/// Creates a new image with the given id. If an image with that id already
/// exists, it is deleted first. If the provided id is 0, generates a
/// random id.
static Image *gr_new_image(uint32_t id) {
	if (id == 0) {
		do {
			id = rand();
			// Avoid IDs that don't need full 32 bits.
		} while ((id & 0xFF000000) == 0 || (id & 0x00FFFF00) == 0 ||
			 gr_find_image(id));
		GR_LOG("Generated random image id %u\n", id);
	}
	Image *img = gr_find_image(id);
	gr_delete_image_keep_id(img);
	GR_LOG("Creating image %u\n", id);
	img = malloc(sizeof(Image));
	memset(img, 0, sizeof(Image));
	img->placements = kh_init(id2placement);
	int ret;
	khiter_t k = kh_put(id2image, images, id, &ret);
	kh_value(images, k) = img;
	img->image_id = id;
	gr_touch_image(img);
	img->global_command_index = global_command_counter;
	return img;
}

/// Creates a new placement with the given id. If a placement with that id
/// already exists, it is deleted first. If the provided id is 0, generates a
/// random id.
static ImagePlacement *gr_new_placement(Image *img, uint32_t id) {
	if (id == 0) {
		do {
			// Currently we support only 24-bit IDs.
			id = rand() & 0xFFFFFF;
			// Avoid IDs that need only one byte.
		} while ((id & 0x00FFFF00) == 0 || gr_find_placement(img, id));
	}
	ImagePlacement *placement = gr_find_placement(img, id);
	gr_delete_placement_keep_id(placement);
	GR_LOG("Creating placement %u/%u\n", img->image_id, id);
	placement = malloc(sizeof(ImagePlacement));
	memset(placement, 0, sizeof(ImagePlacement));
	total_placement_count++;
	int ret;
	khiter_t k = kh_put(id2placement, img->placements, id, &ret);
	kh_value(img->placements, k) = placement;
	placement->image = img;
	placement->placement_id = id;
	gr_touch_placement(placement);
	if (img->default_placement == 0)
		img->default_placement = id;
	return placement;
}

static int64_t ceil_div(int64_t a, int64_t b) {
	return (a + b - 1) / b;
}

/// Computes the best number of rows and columns for a placement if it's not
/// specified, and also adjusts the source rectangle size.
static void gr_infer_placement_size_maybe(ImagePlacement *placement) {
	// Negative values are not allowed. Quietly set them to 0.
	if (placement->src_pix_x < 0)
		placement->src_pix_x = 0;
	if (placement->src_pix_y < 0)
		placement->src_pix_y = 0;
	if (placement->src_pix_width < 0)
		placement->src_pix_width = 0;
	if (placement->src_pix_height < 0)
		placement->src_pix_height = 0;
	// If the source rectangle is outside the image, truncate it.
	if (placement->src_pix_x > placement->image->pix_width)
		placement->src_pix_x = placement->image->pix_width;
	if (placement->src_pix_y > placement->image->pix_height)
		placement->src_pix_y = placement->image->pix_height;
	// If the source rectangle is not specified, use the whole image. If
	// it's partially outside the image, truncate it.
	if (placement->src_pix_width == 0 ||
	    placement->src_pix_x + placement->src_pix_width >
		    placement->image->pix_width)
		placement->src_pix_width =
			placement->image->pix_width - placement->src_pix_x;
	if (placement->src_pix_height == 0 ||
	    placement->src_pix_y + placement->src_pix_height >
		    placement->image->pix_height)
		placement->src_pix_height =
			placement->image->pix_height - placement->src_pix_y;

	if (placement->cols != 0 && placement->rows != 0)
		return;
	if (placement->src_pix_width == 0 || placement->src_pix_height == 0)
		return;
	if (current_cw == 0 || current_ch == 0)
		return;

	// If no size is specified, use the image size.
	if (placement->cols == 0 && placement->rows == 0) {
		placement->cols =
			ceil_div(placement->src_pix_width, current_cw);
		placement->rows =
			ceil_div(placement->src_pix_height, current_ch);
		return;
	}

	// It doesn't seem to be documented anywhere, but some applications
	// specify only one of the dimensions.
	if (placement->scale_mode == SCALE_MODE_CONTAIN) {
		// If we preserve aspect ratio and fit to width/height, the most
		// logical thing is to find the minimum size of the
		// non-specified dimension that allows the image to fit the
		// specified dimension.
		if (placement->cols == 0) {
			placement->cols = ceil_div(
				placement->src_pix_width * placement->rows *
					current_ch,
				placement->src_pix_height * current_cw);
			return;
		}
		if (placement->rows == 0) {
			placement->rows =
				ceil_div(placement->src_pix_height *
						 placement->cols * current_cw,
					 placement->src_pix_width * current_ch);
			return;
		}
	} else {
		// Otherwise we stretch the image or preserve the original size.
		// In both cases we compute the best number of columns from the
		// pixel size and cell size to be compatible with kitty.
		if (!placement->cols)
			placement->cols =
				ceil_div(placement->src_pix_width, current_cw);
		if (!placement->rows)
			placement->rows =
				ceil_div(placement->src_pix_height, current_ch);
	}
}

////////////////////////////////////////////////////////////////////////////////
// Image loading.
////////////////////////////////////////////////////////////////////////////////

/// Copies `num_pixels` pixels (not bytes!) from a buffer `from` to an imlib2
/// image data `to`. The format may be 24 (RGBA) or 32 (RGB), and it's converted
/// to imlib2's representation, which depends on the endianness, and on
/// little-endian architectures the memory layout is actually BGRA.
static inline void gr_copy_pixels(DATA32 *to, unsigned char *from, int format,
				  size_t num_pixels) {
	size_t pixel_size = format == 24 ? 3 : 4;
	if (format == 32) {
		for (unsigned i = 0; i < num_pixels; ++i) {
			unsigned byte_i = i * pixel_size;
			to[i] = ((DATA32)from[byte_i + 2]) |
				((DATA32)from[byte_i + 1]) << 8 |
				((DATA32)from[byte_i]) << 16 |
				((DATA32)from[byte_i + 3]) << 24;
		}
	} else {
		for (unsigned i = 0; i < num_pixels; ++i) {
			unsigned byte_i = i * pixel_size;
			to[i] = ((DATA32)from[byte_i + 2]) |
				((DATA32)from[byte_i + 1]) << 8 |
				((DATA32)from[byte_i]) << 16 | 0xFF000000;
		}
	}
}

/// Loads uncompressed RGB or RGBA image data from a file.
static void gr_load_raw_pixel_data_uncompressed(DATA32 *data, FILE *file,
						int format,
						size_t total_pixels) {
	unsigned char chunk[BUFSIZ];
	size_t pixel_size = format == 24 ? 3 : 4;
	size_t chunk_size_pix = BUFSIZ / 4;
	size_t chunk_size_bytes = chunk_size_pix * pixel_size;
	size_t bytes = total_pixels * pixel_size;
	for (size_t chunk_start_pix = 0; chunk_start_pix < total_pixels;
	     chunk_start_pix += chunk_size_pix) {
		size_t read_size = fread(chunk, 1, chunk_size_bytes, file);
		size_t read_pixels = read_size / pixel_size;
		if (chunk_start_pix + read_pixels > total_pixels)
			read_pixels = total_pixels - chunk_start_pix;
		gr_copy_pixels(data + chunk_start_pix, chunk, format,
			       read_pixels);
	}
}

#define COMPRESSED_CHUNK_SIZE BUFSIZ
#define DECOMPRESSED_CHUNK_SIZE (BUFSIZ * 4)

/// Loads compressed RGB or RGBA image data from a file.
static int gr_load_raw_pixel_data_compressed(DATA32 *data, FILE *file,
					     int format, size_t total_pixels) {
	size_t pixel_size = format == 24 ? 3 : 4;
	unsigned char compressed_chunk[COMPRESSED_CHUNK_SIZE];
	unsigned char decompressed_chunk[DECOMPRESSED_CHUNK_SIZE];

	z_stream strm;
	strm.zalloc = Z_NULL;
	strm.zfree = Z_NULL;
	strm.opaque = Z_NULL;
	strm.next_out = decompressed_chunk;
	strm.avail_out = DECOMPRESSED_CHUNK_SIZE;
	strm.avail_in = 0;
	strm.next_in = Z_NULL;
	int ret = inflateInit(&strm);
	if (ret != Z_OK)
	    return 1;

	int error = 0;
	int progress = 0;
	size_t total_copied_pixels = 0;
	while (1) {
		// If we don't have enough data in the input buffer, try to read
		// from the file.
		if (strm.avail_in <= COMPRESSED_CHUNK_SIZE / 4) {
			// Move the existing data to the beginning.
			memmove(compressed_chunk, strm.next_in, strm.avail_in);
			strm.next_in = compressed_chunk;
			// Read more data.
			size_t bytes_read = fread(
				compressed_chunk + strm.avail_in, 1,
				COMPRESSED_CHUNK_SIZE - strm.avail_in, file);
			strm.avail_in += bytes_read;
			if (bytes_read != 0)
				progress = 1;
		}

		// Try to inflate the data.
		int ret = inflate(&strm, Z_SYNC_FLUSH);
		if (ret == Z_MEM_ERROR || ret == Z_DATA_ERROR) {
			error = 1;
			fprintf(stderr,
				"error: could not decompress the image, error "
				"%s\n",
				ret == Z_MEM_ERROR ? "Z_MEM_ERROR"
						   : "Z_DATA_ERROR");
			break;
		}

		// Copy the data from the output buffer to the image.
		size_t full_pixels =
			(DECOMPRESSED_CHUNK_SIZE - strm.avail_out) / pixel_size;
		// Make sure we don't overflow the image.
		if (full_pixels > total_pixels - total_copied_pixels)
			full_pixels = total_pixels - total_copied_pixels;
		if (full_pixels > 0) {
			// Copy pixels.
			gr_copy_pixels(data, decompressed_chunk, format,
				       full_pixels);
			data += full_pixels;
			total_copied_pixels += full_pixels;
			if (total_copied_pixels >= total_pixels) {
				// We filled the whole image, there may be some
				// data left, but we just truncate it.
				break;
			}
			// Move the remaining data to the beginning.
			size_t copied_bytes = full_pixels * pixel_size;
			size_t leftover =
				(DECOMPRESSED_CHUNK_SIZE - strm.avail_out) -
				copied_bytes;
			memmove(decompressed_chunk,
				decompressed_chunk + copied_bytes, leftover);
			strm.next_out -= copied_bytes;
			strm.avail_out += copied_bytes;
			progress = 1;
		}

		// If we haven't made any progress, then we have reached the end
		// of both the file and the inflated data.
		if (!progress)
			break;
		progress = 0;
	}

	inflateEnd(&strm);
	return error;
}

#undef COMPRESSED_CHUNK_SIZE
#undef DECOMPRESSED_CHUNK_SIZE

/// Load the image from a file containing raw pixel data (RGB or RGBA), the data
/// may be compressed.
static Imlib_Image gr_load_raw_pixel_data(Image *img,
					  const char *filename) {
	size_t total_pixels = img->pix_width * img->pix_height;
	if (total_pixels * 4 > graphics_max_single_image_ram_size) {
		fprintf(stderr,
			"error: image %u is too big too load: %zu > %u\n",
			img->image_id, total_pixels * 4,
			graphics_max_single_image_ram_size);
		return NULL;
	}

	FILE* file = fopen(filename, "rb");
	if (!file) {
		fprintf(stderr,
			"error: could not open image file: %s\n",
			sanitized_filename(filename));
		return NULL;
	}

	Imlib_Image image = imlib_create_image(img->pix_width, img->pix_height);
	if (!image) {
		fprintf(stderr,
			"error: could not create an image of size %d x %d\n",
			img->pix_width, img->pix_height);
		fclose(file);
		return NULL;
	}

	imlib_context_set_image(image);
	imlib_image_set_has_alpha(1);
	DATA32* data = imlib_image_get_data();

	if (img->compression == 0) {
		gr_load_raw_pixel_data_uncompressed(data, file, img->format,
						    total_pixels);
	} else {
		int ret = gr_load_raw_pixel_data_compressed(
			data, file, img->format, total_pixels);
		if (ret != 0) {
			imlib_image_put_back_data(data);
			imlib_free_image();
			fclose(file);
			return NULL;
		}
	}

	fclose(file);
	imlib_image_put_back_data(data);
	return image;
}

/// Loads the original image into RAM by creating an imlib object. If the
/// placement is already loaded,  does nothing. Loading may fail, in which case
/// the status of the image will be set to STATUS_RAM_LOADING_ERROR.
static void gr_load_image(Image *img) {
	if (img->original_image)
		return;

	// If the image is uninitialized or uploading has failed, or the file
	// has been deleted, we cannot load the image.
	if (img->status < STATUS_UPLOADING_SUCCESS)
		return;
	if (img->disk_size == 0) {
		if (img->status != STATUS_RAM_LOADING_ERROR) {
			fprintf(stderr,
				"error: cached image was deleted: %u\n",
				img->image_id);
		}
		img->status = STATUS_RAM_LOADING_ERROR;
		return;
	}

	// Load the original image.
	char filename[MAX_FILENAME_SIZE];
	gr_get_image_filename(img, filename, MAX_FILENAME_SIZE);
	GR_LOG("Loading image: %s\n", sanitized_filename(filename));
	if (img->format == 100 || img->format == 0) {
		img->original_image = imlib_load_image(filename);
		if (img->original_image) {
			// If imlib loading succeeded, set the information about
			// the original image size.
			imlib_context_set_image(img->original_image);
			img->pix_width = imlib_image_get_width();
			img->pix_height = imlib_image_get_height();
		}
	}
	if (img->format == 32 || img->format == 24 ||
	    (!img->original_image && img->format == 0)) {
		img->original_image = gr_load_raw_pixel_data(img, filename);
	}
	if (!img->original_image) {
		if (img->status != STATUS_RAM_LOADING_ERROR) {
			fprintf(stderr, "error: could not load image: %s\n",
				sanitized_filename(filename));
		}
		img->status = STATUS_RAM_LOADING_ERROR;
		return;
	}

	images_ram_size += gr_image_ram_size(img);
	img->status = STATUS_RAM_LOADING_SUCCESS;
}

/// Loads the image placement into RAM by creating an imlib object. The in-ram
/// image is correctly fit to the box defined by the number of rows/columns of
/// the image placement and the provided cell dimensions in pixels. If the
/// placement is already loaded, it will be reloaded only if the cell dimensions
/// have changed.
static void gr_load_placement(ImagePlacement *placement, int cw, int ch) {
	// Update the atime uncoditionally.
	gr_touch_placement(placement);

	// If it's already loaded with the same cw and ch, do nothing.
	if (placement->scaled_image && placement->scaled_ch == ch &&
	    placement->scaled_cw == cw)
		return;

	// Unload the placement first.
	gr_unload_placement(placement);

	Image *img = placement->image;
	GR_LOG("Loading placement: %u/%u\n", img->image_id,
	       placement->placement_id);

	// Load the original image.
	gr_load_image(img);
	if (!img->original_image)
		return;

	// Infer the placement size if needed.
	gr_infer_placement_size_maybe(placement);

	// Create the scaled image.
	int scaled_w = (int)placement->cols * cw;
	int scaled_h = (int)placement->rows * ch;
	if (scaled_w * scaled_h * 4 > graphics_max_single_image_ram_size) {
		fprintf(stderr,
			"error: placement %u/%u would be too big to load: %d x "
			"%d x 4 > %u\n",
			img->image_id, placement->placement_id, scaled_w,
			scaled_h, graphics_max_single_image_ram_size);
		return;
	}
	placement->scaled_image = imlib_create_image(scaled_w, scaled_h);
	if (!placement->scaled_image) {
		fprintf(stderr,
			"error: imlib_create_image(%d, %d) returned "
			"null\n",
			scaled_w, scaled_h);
		return;
	}
	imlib_context_set_image(placement->scaled_image);
	imlib_image_set_has_alpha(1);

	// First fill the scaled image with the transparent color.
	imlib_context_set_blend(0);
	imlib_context_set_color(0, 0, 0, 0);
	imlib_image_fill_rectangle(0, 0, (int)placement->cols * cw,
				   (int)placement->rows * ch);
	imlib_context_set_anti_alias(1);
	imlib_context_set_blend(1);

	// The source rectangle.
	int src_x = placement->src_pix_x;
	int src_y = placement->src_pix_y;
	int src_w = placement->src_pix_width;
	int src_h = placement->src_pix_height;
	// Whether the box is too small to use the true size of the image.
	char box_too_small = scaled_w < src_w || scaled_h < src_h;
	char mode = placement->scale_mode;

	// Then blend the original image onto the transparent background.
	if (src_w <= 0 || src_h <= 0) {
		fprintf(stderr, "warning: image of zero size\n");
	} else if (mode == SCALE_MODE_FILL) {
		imlib_blend_image_onto_image(img->original_image, 1, src_x,
					     src_y, src_w, src_h, 0, 0,
					     scaled_w, scaled_h);
	} else if (mode == SCALE_MODE_NONE ||
		   (mode == SCALE_MODE_NONE_OR_CONTAIN && !box_too_small)) {
		imlib_blend_image_onto_image(img->original_image, 1, src_x,
					     src_y, src_w, src_h, 0, 0, src_w,
					     src_h);
	} else {
		if (mode != SCALE_MODE_CONTAIN &&
		    mode != SCALE_MODE_NONE_OR_CONTAIN) {
			fprintf(stderr,
				"warning: unknown scale mode %u, using "
				"'contain' instead\n",
				mode);
		}
		int dest_x, dest_y;
		int dest_w, dest_h;
		if (scaled_w * src_h > src_w * scaled_h) {
			// If the box is wider than the original image, fit to
			// height.
			dest_h = scaled_h;
			dest_y = 0;
			dest_w = src_w * scaled_h / src_h;
			dest_x = (scaled_w - dest_w) / 2;
		} else {
			// Otherwise, fit to width.
			dest_w = scaled_w;
			dest_x = 0;
			dest_h = src_h * scaled_w / src_w;
			dest_y = (scaled_h - dest_h) / 2;
		}
		imlib_blend_image_onto_image(img->original_image, 1, src_x,
					     src_y, src_w, src_h, dest_x,
					     dest_y, dest_w, dest_h);
	}

	// Mark the placement as loaded.
	placement->scaled_ch = ch;
	placement->scaled_cw = cw;
	images_ram_size += gr_placement_ram_size(placement);

	// Free up ram if needed, but keep the placement we've loaded no matter
	// what.
	placement->protected = 1;
	gr_check_limits();
	placement->protected = 0;
}

////////////////////////////////////////////////////////////////////////////////
// Interaction with the terminal (init, deinit, appending rects, etc).
////////////////////////////////////////////////////////////////////////////////

/// Creates a temporary directory.
static int gr_create_cache_dir() {
	strncpy(cache_dir, graphics_cache_dir_template, sizeof(cache_dir));
	if (!mkdtemp(cache_dir)) {
		fprintf(stderr,
			"error: could not create temporary dir from template "
			"%s\n",
			sanitized_filename(cache_dir));
		return 0;
	}
	fprintf(stderr, "Graphics cache directory: %s\n", cache_dir);
	return 1;
}

/// Checks whether `tmp_dir` exists and recreates it if it doesn't.
static void gr_make_sure_tmpdir_exists() {
	struct stat st;
	if (stat(cache_dir, &st) == 0 && S_ISDIR(st.st_mode))
		return;
	fprintf(stderr,
		"error: %s is not a directory, will need to create a new "
		"graphics cache directory\n",
		sanitized_filename(cache_dir));
	gr_create_cache_dir();
}

/// Initialize the graphics module.
void gr_init(Display *disp, Visual *vis, Colormap cm) {
	// Create the temporary dir.
	if (!gr_create_cache_dir())
		abort();

	// Initialize imlib.
	imlib_context_set_display(disp);
	imlib_context_set_visual(vis);
	imlib_context_set_colormap(cm);
	imlib_context_set_anti_alias(1);
	imlib_context_set_blend(1);
	// Imlib2 checks only the file name when caching, which is not enough
	// for us since we reuse file names. Disable caching.
	imlib_set_cache_size(0);

	// Prepare for color inversion.
	for (size_t i = 0; i < 256; ++i)
		reverse_table[i] = 255 - i;

	// Create data structures.
	images = kh_init(id2image);

	atexit(gr_deinit);
}

/// Deinitialize the graphics module.
void gr_deinit() {
	if (!images)
		return;
	// Delete all images.
	gr_delete_all_images();
	// Remove the cache dir.
	remove(cache_dir);
	// Destroy the data structures.
	kh_destroy(id2image, images);
	images = NULL;
}

/// Executes `command` with the name of the file corresponding to `image_id` as
/// the argument. Executes xmessage with an error message on failure.
void gr_preview_image(uint32_t image_id, const char *exec) {
	char command[256];
	size_t len;
	Image *img = gr_find_image(image_id);
	if (img) {
		char filename[MAX_FILENAME_SIZE];
		gr_get_image_filename(img, filename, MAX_FILENAME_SIZE);
		if (img->disk_size == 0) {
			len = snprintf(command, 255,
				       "xmessage 'Image with id=%u is not "
				       "fully copied to %s'",
				       image_id, sanitized_filename(filename));
		} else {
			len = snprintf(command, 255, "%s %s &", exec,
				       sanitized_filename(filename));
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

/// Generates a human-readable description of the image placement.
void gr_get_placement_description(uint32_t image_id, uint32_t placement_id,
				  char *buf, size_t len) {
	Image *img = gr_find_image(image_id);
	if (!img) {
		snprintf(buf, len, "Image with id=%u not found", image_id);
		return;
	}
	ImagePlacement *placement = gr_find_placement(img, placement_id);
	if (!placement) {
		snprintf(buf, len, "Placement %u of image %u not found",
			 placement_id, image_id);
		return;
	}
	snprintf(buf, len,
		 "Image %u, placement %u\n"
		 "%u cols x %u rows\n"
		 "image size: %u x %u\n"
		 "cell size: %u x %u\n"
		 "src rect %u, %u  %u x %u\n"
		 "image uploading status: %s\n"
		 "disk size: %u KiB\n"
		 "placement is %s\n"
		 "original image is %s\n",
		 image_id, placement_id, placement->cols, placement->rows,
		 img->pix_width, img->pix_height,
		 placement->scaled_cw, placement->scaled_ch,
		 placement->src_pix_x, placement->src_pix_y,
		 placement->src_pix_width, placement->src_pix_height,
		 image_uploading_failure_strings[img->uploading_failure],
		 img->disk_size / 1024,
		 placement->scaled_image ? "loaded" : "not loaded",
		 img->original_image ? "loaded" : "not loaded");
}

/// Prints the time difference between now and past in a human-readable format.
static void gr_print_ago(struct timespec *now, struct timespec *past) {
	double seconds = difftime(now->tv_sec, past->tv_sec) +
			 (now->tv_nsec - past->tv_nsec) / 1e9;

	if (seconds < 1)
		fprintf(stderr, "%.2f sec ago\n", seconds);
	else if (seconds < 60)
		fprintf(stderr, "%d sec ago\n", (int)seconds);
	else if (seconds < 3600)
		fprintf(stderr, "%d min %d sec ago\n", (int)(seconds / 60),
			(int)(seconds) % 60);
	else {
		fprintf(stderr, "%d hr %d min %d sec ago\n",
			(int)(seconds / 3600), (int)(seconds) % 3600 / 60,
			(int)(seconds) % 60);
	}
}

/// Dumps the internal state (images and placements) to stderr.
void gr_dump_state() {
	fprintf(stderr, "======== Graphics module state dump ========\n");
	fprintf(stderr, "Image count: %u\n", kh_size(images));
	fprintf(stderr, "Placement count: %u\n", total_placement_count);
	fprintf(stderr, "Estimated RAM usage: %ld KiB\n",
		images_ram_size / 1024);
	fprintf(stderr, "Estimated Disk usage: %ld KiB\n",
		images_disk_size / 1024);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	int64_t images_ram_size_computed = 0;
	int64_t images_disk_size_computed = 0;

	Image *img = NULL;
	ImagePlacement *placement = NULL;
	kh_foreach_value(images, img, {
		fprintf(stderr, "----------------\n");
		fprintf(stderr, "Image %u\n", img->image_id);
		fprintf(stderr, "    number %u\n", img->image_number);
		fprintf(stderr, "    global command index %lu\n",
			img->global_command_index);
		fprintf(stderr, "    accessed ");
		gr_print_ago(&now, &img->atime);
		fprintf(stderr, "    status: %s\n",
			image_status_strings[img->status]);
		if (img->uploading_failure)
			fprintf(stderr, "    uploading failure: %s\n",
				image_uploading_failure_strings
					[img->uploading_failure]);
		fprintf(stderr, "    pix size: %ux%u\n", img->pix_width,
			img->pix_height);
		char filename[MAX_FILENAME_SIZE];
		gr_get_image_filename(img, filename, MAX_FILENAME_SIZE);
		if (access(filename, F_OK) != -1)
			fprintf(stderr, "    file: %s\n",
				sanitized_filename(filename));
		else
			fprintf(stderr, "    not on disk\n");
		fprintf(stderr, "    disk size: %u KiB\n",
			img->disk_size / 1024);
		images_disk_size_computed += img->disk_size;
		if (img->original_image) {
			unsigned ram_size = gr_image_ram_size(img);
			fprintf(stderr, "    loaded into ram, size: %d KiB\n",
				ram_size / 1024);
			images_ram_size_computed += ram_size;
		} else {
			fprintf(stderr, "    not loaded into ram\n");
		}
		fprintf(stderr, "    default_placement = %u\n",
			img->default_placement);
		kh_foreach_value(img->placements, placement, {
			fprintf(stderr, "    Placement %u\n",
				placement->placement_id);
			if (placement->image != img)
				fprintf(stderr,
					"        ERROR: WRONG IMAGE POINTER\n");
			fprintf(stderr, "        accessed ");
			gr_print_ago(&now, &placement->atime);
			fprintf(stderr, "        scale_mode = %u\n",
				placement->scale_mode);
			fprintf(stderr,
				"        cell size: %u cols x %u rows\n",
				placement->cols, placement->rows);
			if (placement->scaled_image) {
				unsigned ram_size =
					gr_placement_ram_size(placement);
				fprintf(stderr,
					"        loaded into ram, size: %d "
					"KiB\n",
					ram_size / 1024);
				images_ram_size_computed += ram_size;
				fprintf(stderr, "        cell size: %ux%u\n",
					placement->scaled_cw,
					placement->scaled_ch);
			} else {
				fprintf(stderr,
					"        not loaded into ram\n");
			}
		});
	});
	if (images_ram_size != images_ram_size_computed) {
		fprintf(stderr,
			"WARNING: images_ram_size is %ld, but computed value "
			"is %ld\n",
			images_ram_size, images_ram_size_computed);
	}
	if (images_disk_size != images_disk_size_computed) {
		fprintf(stderr,
			"WARNING: images_disk_size is %ld, but computed value "
			"is %ld\n",
			images_disk_size, images_disk_size_computed);
	}
	fprintf(stderr, "============================================\n");
}

/// Displays debug information in the rectangle using colors col1 and col2.
static void gr_displayinfo(Drawable buf, ImageRect *rect, int col1, int col2,
			   const char *message) {
	int w_pix = (rect->end_col - rect->start_col) * rect->cw;
	int h_pix = (rect->end_row - rect->start_row) * rect->ch;
	Display *disp = imlib_context_get_display();
	GC gc = XCreateGC(disp, buf, 0, NULL);
	char info[MAX_INFO_LEN];
	if (rect->placement_id)
		snprintf(info, MAX_INFO_LEN, "%s%u/%u [%d:%d)x[%d:%d)", message,
			 rect->image_id, rect->placement_id, rect->start_col,
			 rect->end_col, rect->start_row, rect->end_row);
	else
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
	ImagePlacement *placement =
		gr_find_image_and_placement(rect->image_id, rect->placement_id);
	// If the image does not exist or image display is switched off, draw
	// the bounding box.
	if (!placement || !graphics_display_images) {
		gr_showrect(buf, rect);
		if (graphics_debug_mode == GRAPHICS_DEBUG_LOG_AND_BOXES)
			gr_displayinfo(buf, rect, 0x000000, 0xFFFFFF, "");
		return;
	}

	// Load the image.
	gr_load_placement(placement, rect->cw, rect->ch);

	// If the image couldn't be loaded, display the bounding box.
	if (!placement->scaled_image) {
		gr_showrect(buf, rect);
		if (graphics_debug_mode == GRAPHICS_DEBUG_LOG_AND_BOXES)
			gr_displayinfo(buf, rect, 0x000000, 0xFFFFFF, "");
		return;
	}

	// Display the image.
	imlib_context_set_anti_alias(0);
	imlib_context_set_image(placement->scaled_image);
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
	if (graphics_debug_mode == GRAPHICS_DEBUG_LOG_AND_BOXES) {
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
			 "Frame rendering time: %d ms  Image storage ram: %ld "
			 "KiB disk: %ld KiB  count: %d   cell %dx%d",
			 milliseconds, images_ram_size / 1024,
			 images_disk_size / 1024, kh_size(images),
			 current_cw, current_ch);
		XSetForeground(disp, gc, 0x000000);
		XFillRectangle(disp, buf, gc, 0, 0, 600, 16);
		XSetForeground(disp, gc, 0xFFFFFF);
		XDrawString(disp, buf, gc, 0, 14, info, strlen(info));
		XFreeGC(disp, gc);
	}

	// Check the limits in case we have used too much ram for placements.
	gr_check_limits();
}

// Add an image rectangle to the list of rectangles to draw.
void gr_append_imagerect(Drawable buf, uint32_t image_id, uint32_t placement_id,
			 int start_col, int end_col, int start_row, int end_row,
			 int x_pix, int y_pix, int cw, int ch, int reverse) {
	current_cw = cw;
	current_ch = ch;

	ImageRect new_rect;
	new_rect.image_id = image_id;
	new_rect.placement_id = placement_id;
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
	if (graphics_debug_mode == GRAPHICS_DEBUG_LOG_AND_BOXES)
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
		if (rect->image_id != image_id ||
		    rect->placement_id != placement_id || rect->cw != cw ||
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
	/// 'f=', use 24 or 32 for raw pixel data, 100 to autodetect with
	/// imlib2. If 'f=0', will try to load with imlib2, then fallback to
	/// 32-bit pixel data.
	int format;
	/// 'o=', may be 'z' for RFC 1950 ZLIB.
	int compression;
	/// 't=', may be 'f' or 'd'.
	char transmission_medium;
	/// 'd=', may be only 'I' if specified.
	char delete_specifier;
	/// 's=', 'v=', used only when 'f=24' or 'f=32'.
	int pix_width, pix_height;
	/// 'x=', 'y=' - top-left corner of the source rectangle.
	int src_pix_x, src_pix_y;
	/// 'w=', 'h=' - width and height of the source rectangle.
	int src_pix_width, src_pix_height;
	/// 'r=', 'c='
	int rows, columns;
	/// 'i='
	uint32_t image_id;
	/// 'I=', not supported.
	uint32_t image_number;
	/// 'p='
	uint32_t placement_id;
	/// 'm=', may be 0 or 1.
	int more;
	/// True if either 'm=0' or 'm=1' is specified.
	char is_data_transmission;
	/// True if turns out that this command is a continuation of a data
	/// transmission and not the first one for this image. Populated by
	/// `gr_handle_transmit_command`.
	char is_direct_transmission_continuation;
	/// 'S=', used to check the size of uploaded data.
	int size;
	/// 'U=', whether it's a virtual placement for Unicode placeholders.
	int virtual;
	/// 'C=', if true, do not move the cursor when displaying this placement
	/// (non-virtual placements only).
	char do_not_move_cursor;
} GraphicsCommand;

/// Replaces all non-printed characters in `str` with '?' and truncates the
/// string to `max_size`, maybe inserting ellipsis at the end.
static void sanitize_str(char *str, size_t max_size) {
	assert(max_size >= 4);
	for (size_t i = 0; i < max_size; ++i) {
		unsigned c = str[i];
		if (c == '\0')
			return;
		if (c >= 128 || !isprint(c))
			str[i] = '?';
	}
	str[max_size - 1] = '\0';
	str[max_size - 2] = '.';
	str[max_size - 3] = '.';
	str[max_size - 4] = '.';
}

/// A non-destructive version of `sanitize_str`. Uses a static buffer, so be
/// careful.
static const char *sanitized_filename(const char *str) {
	static char buf[MAX_FILENAME_SIZE];
	strncpy(buf, str, sizeof(buf));
	sanitize_str(buf, sizeof(buf));
	return buf;
}

/// Creates a response to the current command in `graphics_command_result`.
static void gr_createresponse(uint32_t image_id, uint32_t image_number,
			      uint32_t placement_id, const char *msg) {
	if (!image_id && !image_number && !placement_id) {
		// Nobody expects the response in this case, so just print it to
		// stderr.
		fprintf(stderr,
			"error: No image id or image number or placement_id, "
			"but still there is a response: %s\n",
			msg);
		return;
	}
	char *buf = graphics_command_result.response;
	size_t maxlen = MAX_GRAPHICS_RESPONSE_LEN;
	size_t written;
	written = snprintf(buf, maxlen, "\033_G");
	buf += written;
	maxlen -= written;
	if (image_id) {
		written = snprintf(buf, maxlen, "i=%u,", image_id);
		buf += written;
		maxlen -= written;
	}
	if (image_number) {
		written = snprintf(buf, maxlen, "I=%u,", image_number);
		buf += written;
		maxlen -= written;
	}
	if (placement_id) {
		written = snprintf(buf, maxlen, "p=%u,", placement_id);
		buf += written;
		maxlen -= written;
	}
	buf[-1] = ';';
	written = snprintf(buf, maxlen, "%s\033\\", msg);
	buf += written;
	maxlen -= written;
	buf[-2] = '\033';
	buf[-1] = '\\';
}

/// Creates the 'OK' response to the current command, unless suppressed or a
/// non-final data transmission.
static void gr_reportsuccess_cmd(GraphicsCommand *cmd) {
	if (cmd->quiet < 1 && !cmd->more)
		gr_createresponse(cmd->image_id, cmd->image_number,
				  cmd->placement_id, "OK");
}

/// Creates the 'OK' response to the current command (unless suppressed).
static void gr_reportsuccess_img(Image *img) {
	uint32_t id = img->query_id ? img->query_id : img->image_id;
	if (img->quiet < 1)
		gr_createresponse(id, img->image_number,
				  img->initial_placement_id, "OK");
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
		gr_createresponse(cmd->image_id, cmd->image_number,
				  cmd->placement_id, errmsg);
}

/// Creates an error response to the current command (unless suppressed).
static void gr_reporterror_img(Image *img, const char *format, ...) {
	char errmsg[MAX_GRAPHICS_RESPONSE_LEN];
	graphics_command_result.error = 1;
	va_list args;
	va_start(args, format);
	vsnprintf(errmsg, MAX_GRAPHICS_RESPONSE_LEN, format, args);
	va_end(args);

	if (!img) {
		fprintf(stderr, "%s\n", errmsg);
		gr_createresponse(0, 0, 0, errmsg);
	} else {
		uint32_t id = img->query_id ? img->query_id : img->image_id;
		fprintf(stderr, "%s  id=%u\n", errmsg, id);
		if (img->quiet < 2)
			gr_createresponse(id, img->image_number,
					  img->initial_placement_id, errmsg);
	}
}

/// Loads an image and creates a success/failure response. Returns `img`, or
/// NULL if it's a query action and the image was deleted.
static Image *gr_loadimage_and_report(Image *img) {
	gr_load_image(img);
	if (!img->original_image) {
		gr_reporterror_img(img, "EBADF: could not load image");
	} else {
		gr_reportsuccess_img(img);
	}
	// If it was a query action, discard the image.
	if (img->query_id) {
		gr_delete_image(img);
		return NULL;
	}
	return img;
}

/// Creates an appropriate uploading failure response to the current command.
static void gr_reportuploaderror(Image *img) {
	switch (img->uploading_failure) {
	case 0:
		return;
	case ERROR_CANNOT_OPEN_CACHED_FILE:
		gr_reporterror_img(img,
				   "EIO: could not create a file for image");
		break;
	case ERROR_OVER_SIZE_LIMIT:
		gr_reporterror_img(
			img,
			"EFBIG: the size of the uploaded image exceeded "
			"the image size limit %u",
			graphics_max_single_image_file_size);
		break;
	case ERROR_UNEXPECTED_SIZE:
		gr_reporterror_img(img,
				   "EINVAL: the size of the uploaded image %u "
				   "doesn't match the expected size %u",
				   img->disk_size, img->expected_size);
		break;
	};
}

/// Displays a non-virtual placement. This functions records the information in
/// `graphics_command_result`, the placeholder itself is created by the terminal
/// after handling the current command in the graphics module.
static void gr_display_nonvirtual_placement(ImagePlacement *placement) {
	if (placement->virtual)
		return;
	if (placement->image->status < STATUS_RAM_LOADING_SUCCESS)
		return;
	// Infer the placement size if needed.
	gr_infer_placement_size_maybe(placement);
	// Populate the information about the placeholder which will be created
	// by the terminal.
	graphics_command_result.create_placeholder = 1;
	graphics_command_result.placeholder.image_id = placement->image->image_id;
	graphics_command_result.placeholder.placement_id = placement->placement_id;
	graphics_command_result.placeholder.columns = placement->cols;
	graphics_command_result.placeholder.rows = placement->rows;
	graphics_command_result.placeholder.do_not_move_cursor =
		placement->do_not_move_cursor;
	GR_LOG("Creating a placeholder for %u/%u  %d x %d\n",
	       placement->image->image_id, placement->placement_id,
	       placement->cols, placement->rows);
}

/// Appends data from `payload` to the image `img` when using direct
/// transmission. Note that we report errors only for the final command
/// (`!more`) to avoid spamming the client.
static void gr_append_data(Image *img, const char *payload, int more) {
	if (!img) {
		img = gr_find_image(current_upload_image_id);
		GR_LOG("Appending data to image %u\n", current_upload_image_id);
		if (!img)
			GR_LOG("ERROR: this image doesn't exist\n");
	}
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

	GR_LOG("appending %u + %zu = %zu bytes\n", img->disk_size, data_size,
	       img->disk_size + data_size);

	// Do not append this data if the image exceeds the size limit.
	if (img->disk_size + data_size > graphics_max_single_image_file_size ||
	    img->expected_size > graphics_max_single_image_file_size) {
		free(data);
		gr_delete_imagefile(img);
		img->uploading_failure = ERROR_OVER_SIZE_LIMIT;
		if (!more)
			gr_reportuploaderror(img);
		return;
	}

	// If there is no open file corresponding to the image, create it.
	if (!img->open_file) {
		gr_make_sure_tmpdir_exists();
		char filename[MAX_FILENAME_SIZE];
		gr_get_image_filename(img, filename, MAX_FILENAME_SIZE);
		FILE *file = fopen(filename, img->disk_size ? "a" : "w");
		if (!file) {
			img->status = STATUS_UPLOADING_ERROR;
			img->uploading_failure = ERROR_CANNOT_OPEN_CACHED_FILE;
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
	images_disk_size += data_size;
	gr_touch_image(img);

	if (more) {
		current_upload_image_id = img->image_id;
	} else {
		current_upload_image_id = 0;
		// Close the file.
		if (img->open_file) {
			fclose(img->open_file);
			img->open_file = NULL;
		}
		img->status = STATUS_UPLOADING_SUCCESS;
		uint32_t placement_id = img->default_placement;
		if (img->expected_size &&
		    img->expected_size != img->disk_size) {
			// Report failure if the uploaded image size doesn't
			// match the expected size.
			img->status = STATUS_UPLOADING_ERROR;
			img->uploading_failure = ERROR_UNEXPECTED_SIZE;
			gr_reportuploaderror(img);
		} else {
			// Try to load the image into ram and report the result.
			img = gr_loadimage_and_report(img);
			if (img) {
				// If there is a non-virtual image placement, we
				// may need to display it.
				ImagePlacement *placement = NULL;
				kh_foreach_value(img->placements, placement, {
					gr_display_nonvirtual_placement(placement);
				});
			}
		}
	}

	// Check whether we need to delete old images.
	gr_check_limits();
}

/// Finds the image either by id or by number specified in the command and sets
/// the image_id of `cmd` if the image was found.
static Image *gr_find_image_for_command(GraphicsCommand *cmd) {
	if (cmd->image_id)
		return gr_find_image(cmd->image_id);
	Image *img = NULL;
	// If the image number is not specified, we can't find the image, unless
	// it's a put command, in which case we will try the last image.
	if (cmd->image_number == 0 && cmd->action == 'p')
		img = gr_find_image(last_image_id);
	else
		img = gr_find_image_by_number(cmd->image_number);
	if (img)
		cmd->image_id = img->image_id;
	return img;
}

/// Create a new image and initialize its parameters from the command.
static Image *gr_new_image_from_command(GraphicsCommand *cmd) {
	if (cmd->format != 0 && cmd->format != 32 && cmd->format != 24 &&
	    cmd->compression != 0) {
		gr_reporterror_cmd(cmd, "EINVAL: compression is supported only "
					"for raw pixel data (f=32 or f=24)");
		// Even though we report an error, we still create an image.
	}
	// Create an image object. If the action is `q`, we'll use random id
	// instead of the one specified in the command.
	uint32_t image_id = cmd->action == 'q' ? 0 : cmd->image_id;
	Image *img = gr_new_image(image_id);
	if (!img)
		return NULL;
	if (cmd->action == 'q')
		img->query_id = cmd->image_id;
	else if (!cmd->image_id)
		cmd->image_id = img->image_id;
	// Set the image number.
	img->image_number = cmd->image_number;
	// Set parameters.
	img->expected_size = cmd->size;
	img->format = cmd->format;
	img->compression = cmd->compression;
	img->pix_width = cmd->pix_width;
	img->pix_height = cmd->pix_height;
	// We save the quietness information in the image because for direct
	// transmission subsequent transmission command won't contain this info.
	img->quiet = cmd->quiet;
	return img;
}

/// Removes a file if it actually looks like a temporary file.
static void gr_delete_tmp_file(const char *filename) {
	if (strstr(filename, "tty-graphics-protocol") == NULL)
		return;
	if (strstr(filename, "/tmp/") != filename) {
		const char *tmpdir = getenv("TMPDIR");
		if (!tmpdir || !tmpdir[0] ||
		    strstr(filename, tmpdir) != filename)
			return;
	}
	unlink(filename);
}

/// Handles a data transmission command.
static Image *gr_handle_transmit_command(GraphicsCommand *cmd) {
	// The default is direct transmission.
	if (!cmd->transmission_medium)
		cmd->transmission_medium = 'd';

	// If neither id, nor image number is specified, and the transmission
	// medium is 'd' (or unspecified), and there is an active direct upload,
	// this is a continuation of the upload.
	if (current_upload_image_id != 0 && cmd->image_id == 0 &&
	    cmd->image_number == 0 && cmd->transmission_medium == 'd') {
		cmd->image_id = current_upload_image_id;
		GR_LOG("No images id is specified, continuing uploading %u\n",
		       cmd->image_id);
	}

	Image *img = NULL;
	if (cmd->transmission_medium == 'f' ||
	    cmd->transmission_medium == 't') {
		// File transmission.
		// Create a new image structure.
		img = gr_new_image_from_command(cmd);
		if (!img)
			return NULL;
		last_image_id = img->image_id;
		// Decode the filename.
		char *original_filename = gr_base64dec(cmd->payload, NULL);
		GR_LOG("Copying image %s\n",
		       sanitized_filename(original_filename));
		// Stat the file and check that it's a regular file and not too
		// big.
		struct stat st;
		int stat_res = stat(original_filename, &st);
		const char *stat_error = NULL;
		if (stat_res)
			stat_error = strerror(errno);
		else if (!S_ISREG(st.st_mode))
			stat_error = "Not a regular file";
		else if (st.st_size == 0)
			stat_error = "The size of the file is zero";
		else if (st.st_size > graphics_max_single_image_file_size)
			stat_error = "The file is too large";
		if (stat_error) {
			gr_reporterror_cmd(cmd,
					   "EBADF: %s", stat_error);
			fprintf(stderr, "Could not load the file %s\n",
				sanitized_filename(original_filename));
			img->status = STATUS_UPLOADING_ERROR;
			img->uploading_failure = ERROR_CANNOT_COPY_FILE;
		} else {
			gr_make_sure_tmpdir_exists();
			// Build the filename for the cached copy of the file.
			char cache_filename[MAX_FILENAME_SIZE];
			gr_get_image_filename(img, cache_filename,
					      MAX_FILENAME_SIZE);
			// We will create a symlink to the original file, and
			// then copy the file to the temporary cache dir. We do
			// this symlink trick mostly to be able to use cp for
			// copying, and avoid escaping file name characters when
			// calling system at the same time.
			char tmp_filename_symlink[MAX_FILENAME_SIZE + 4] = {0};
			strcat(tmp_filename_symlink, cache_filename);
			strcat(tmp_filename_symlink, ".sym");
			char command[MAX_FILENAME_SIZE + 256];
			size_t len =
				snprintf(command, MAX_FILENAME_SIZE + 255,
					 "cp '%s' '%s'", tmp_filename_symlink,
					 cache_filename);
			if (len > MAX_FILENAME_SIZE + 255 ||
			    symlink(original_filename, tmp_filename_symlink) ||
			    system(command) != 0) {
				gr_reporterror_cmd(cmd,
						   "EBADF: could not copy the "
						   "image to the cache dir");
				fprintf(stderr,
					"Could not copy the image "
					"%s (symlink %s) to %s",
					sanitized_filename(original_filename),
					tmp_filename_symlink, cache_filename);
				img->status = STATUS_UPLOADING_ERROR;
				img->uploading_failure = ERROR_CANNOT_COPY_FILE;
			} else {
				// Get the file size of the copied file.
				img->status = STATUS_UPLOADING_SUCCESS;
				img->disk_size = st.st_size;
				images_disk_size += img->disk_size;
				if (img->expected_size &&
				    img->expected_size != img->disk_size) {
					// The file has unexpected size.
					img->status = STATUS_UPLOADING_ERROR;
					img->uploading_failure =
						ERROR_UNEXPECTED_SIZE;
					gr_reportuploaderror(img);
				} else {
					// Everything seems fine, try to load.
					img = gr_loadimage_and_report(img);
				}
			}
			// Delete the symlink.
			unlink(tmp_filename_symlink);
			// Delete the original file if it's temporary.
			if (cmd->transmission_medium == 't')
				gr_delete_tmp_file(original_filename);
		}
		free(original_filename);
		gr_check_limits();
	} else if (cmd->transmission_medium == 'd') {
		// Direct transmission (default if 't' is not specified).
		img = gr_find_image_for_command(cmd);
		if (img && img->status == STATUS_UPLOADING) {
			// This is a continuation of the previous transmission.
			cmd->is_direct_transmission_continuation = 1;
			gr_append_data(img, cmd->payload, cmd->more);
			return img;
		}
		// If no action is specified, it's not the first transmission
		// command. If we couldn't find the image, something went wrong
		// and we should just drop this command.
		if (cmd->action == 0)
			return NULL;
		// Otherwise create a new image structure.
		img = gr_new_image_from_command(cmd);
		if (!img)
			return NULL;
		last_image_id = img->image_id;
		img->status = STATUS_UPLOADING;
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

/// Handles the 'put' command by creating a placement.
static void gr_handle_put_command(GraphicsCommand *cmd) {
	if (cmd->image_id == 0 && cmd->image_number == 0) {
		gr_reporterror_cmd(cmd,
				   "EINVAL: neither image id nor image number "
				   "are specified or both are zero");
		return;
	}

	// Find the image with the id or number.
	Image *img = gr_find_image_for_command(cmd);
	if (!img) {
		gr_reporterror_cmd(cmd, "ENOENT: image not found");
		return;
	}

	// Create a placement. If a placement with the same id already exists,
	// it will be deleted. If the id is zero, a random id will be generated.
	ImagePlacement *placement = gr_new_placement(img, cmd->placement_id);
	placement->virtual = cmd->virtual;
	placement->src_pix_x = cmd->src_pix_x;
	placement->src_pix_y = cmd->src_pix_y;
	placement->src_pix_width = cmd->src_pix_width;
	placement->src_pix_height = cmd->src_pix_height;
	placement->cols = cmd->columns;
	placement->rows = cmd->rows;
	placement->do_not_move_cursor = cmd->do_not_move_cursor;

	if (placement->virtual)
		placement->scale_mode = SCALE_MODE_CONTAIN;
	else if (placement->cols || placement->rows)
		placement->scale_mode = SCALE_MODE_FILL;
	else
		placement->scale_mode = SCALE_MODE_NONE;

	// Display the placement unless it's virtual.
	gr_display_nonvirtual_placement(placement);

	// Report success.
	gr_reportsuccess_cmd(cmd);
}

/// Information about what to delete.
typedef struct DeletionData {
	uint32_t image_id;
	uint32_t placement_id;
	/// If true, delete the image object if there are no more placements.
	char delete_image_if_no_ref;
} DeletionData;

/// The callback called for each cell to perform deletion.
static int gr_deletion_callback(void *data, uint32_t image_id,
					    uint32_t placement_id, int col,
					    int row, char is_classic) {
	DeletionData *del_data = data;
	// Leave unicode placeholders alone.
	if (!is_classic)
		return 0;
	if (del_data->image_id && del_data->image_id != image_id)
		return 0;
	if (del_data->placement_id && del_data->placement_id != placement_id)
		return 0;
	Image *img = gr_find_image(image_id);
	// If the image is already deleted, just erase the placeholder.
	if (!img)
		return 1;
	// Delete the placement.
	if (placement_id)
		gr_delete_placement(gr_find_placement(img, placement_id));
	// Delete the image if image deletion is requested (uppercase delete
	// specifier) and there are no more placements.
	if (del_data->delete_image_if_no_ref && kh_size(img->placements) == 0)
		gr_delete_image(img);
	return 1;
}

/// Handles the delete command.
static void gr_handle_delete_command(GraphicsCommand *cmd) {
	DeletionData del_data = {0};
	del_data.delete_image_if_no_ref = isupper(cmd->delete_specifier) != 0;
	char d = tolower(cmd->delete_specifier);

	if (d == 'n') {
		d = 'i';
		Image *img = gr_find_image_by_number(cmd->image_number);
		if (!img)
			return;
		del_data.image_id = img->image_id;
	}

	if (!d || d == 'a') {
		// Delete all visible placements.
		gr_for_each_image_cell(gr_deletion_callback, &del_data);
	} else if (d == 'i') {
		// Delete the specified image by image id and maybe placement
		// id.
		if (!del_data.image_id)
			del_data.image_id = cmd->image_id;
		if (!del_data.image_id) {
			fprintf(stderr,
				"ERROR: image id is not specified in the "
				"delete command\n");
			return;
		}
		del_data.placement_id = cmd->placement_id;
		// NOTE: It's not very clear whether we should delete the image
		// even if there are no _visible_ placements to delete. We do
		// this because otherwise there is no way to delete an image
		// with virtual placements in one command.
		if (!del_data.placement_id && del_data.delete_image_if_no_ref)
			gr_delete_image(gr_find_image(cmd->image_id));
		gr_for_each_image_cell(gr_deletion_callback, &del_data);
	} else {
		fprintf(stderr,
			"WARNING: unsupported value of the d key: '%c'. The "
			"command is ignored.\n",
			cmd->delete_specifier);
	}
}

/// Handles a command.
static void gr_handle_command(GraphicsCommand *cmd) {
	if (!cmd->image_id && !cmd->image_number) {
		// If there is no image id or image number, nobody expects a
		// response, so set quiet to 2.
		cmd->quiet = 2;
	}
	Image *img = NULL;
	switch (cmd->action) {
	case 0:
		// If no action is specified, it may be a data transmission
		// command if 'm=' is specified.
		if (cmd->is_data_transmission) {
			gr_handle_transmit_command(cmd);
			break;
		}
		gr_reporterror_cmd(cmd, "EINVAL: no action specified");
		break;
	case 't':
	case 'q':
		// Transmit data. 'q' means query, which is basically the same
		// as transmit, but the image is discarded, and the id is fake.
		gr_handle_transmit_command(cmd);
		break;
	case 'p':
		// Display (put) the image.
		gr_handle_put_command(cmd);
		break;
	case 'T':
		// Transmit and display.
		img = gr_handle_transmit_command(cmd);
		if (img && !cmd->is_direct_transmission_continuation) {
			gr_handle_put_command(cmd);
			if (cmd->placement_id)
				img->initial_placement_id = cmd->placement_id;
		}
		break;
	case 'd':
		gr_handle_delete_command(cmd);
		break;
	default:
		gr_reporterror_cmd(cmd, "EINVAL: unsupported action: %c",
				   cmd->action);
		return;
	}
}

/// Parses the value specified by `value_start` and `value_end` and assigns it
/// to the field of `cmd` specified by `key_start` and `key_end`.
static void gr_set_keyvalue(GraphicsCommand *cmd, char *key_start,
			    char *key_end, char *value_start, char *value_end) {
	// Currently all keys are one-character.
	if (key_end - key_start != 1) {
		gr_reporterror_cmd(cmd, "EINVAL: unknown key of length %ld: %s",
				   key_end - key_start, key_start);
		return;
	}
	long num = 0;
	if (*key_start == 'a' || *key_start == 't' || *key_start == 'd' ||
	    *key_start == 'o') {
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
		if (num != 0 && num != 24 && num != 32 && num != 100) {
			gr_reporterror_cmd(
				cmd,
				"EINVAL: unsupported format specification: %s",
				key_start);
		}
		break;
	case 'o':
		cmd->compression = *value_start;
		if (cmd->compression != 'z') {
			gr_reporterror_cmd(cmd,
					   "EINVAL: unsupported compression "
					   "specification: %s",
					   key_start);
		}
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
	case 'x':
		cmd->src_pix_x = num;
		break;
	case 'y':
		cmd->src_pix_y = num;
		break;
	case 'w':
		cmd->src_pix_width = num;
		break;
	case 'h':
		cmd->src_pix_height = num;
		break;
	case 'c':
		cmd->columns = num;
		break;
	case 'r':
		cmd->rows = num;
		break;
	case 'm':
		cmd->is_data_transmission = 1;
		cmd->more = num;
		break;
	case 'S':
		cmd->size = num;
		break;
	case 'U':
		cmd->virtual = num;
		break;
	case 'X':
	case 'Y':
	case 'z':
		fprintf(stderr,
			"WARNING: the key '%c' is not supported and will be ignored\n",
			*key_start);
		break;
	case 'C':
		cmd->do_not_move_cursor = num;
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

	global_command_counter++;
	GR_LOG("### Command %lu: %.80s\n", global_command_counter, buf);

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

	if (cmd.payload && cmd.payload[0])
		GR_LOG("    payload size: %ld\n", strlen(cmd.payload));

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

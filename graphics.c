/* The MIT License

   Copyright (c) 2021-2024 Sergei Grechanik <sergei.grechanik@gmail.com>

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
#include <X11/extensions/Xrender.h>
#include <assert.h>
#include <ctype.h>
#include <spawn.h>
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
#include "kvec.h"

extern char **environ;

#define MAX_FILENAME_SIZE 256
#define MAX_INFO_LEN 256
#define MAX_IMAGE_RECTS 20

/// The type used in this file to represent time. Used both for time differences
/// and absolute times (as milliseconds since an arbitrary point in time, see
/// `initialization_time`).
typedef int64_t Milliseconds;

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

enum AnimationState {
	ANIMATION_STATE_UNSET = 0,
	/// The animation is stopped. Display the current frame, but don't
	/// advance to the next one.
	ANIMATION_STATE_STOPPED = 1,
	/// Run the animation to then end, then wait for the next frame.
	ANIMATION_STATE_LOADING = 2,
	/// Run the animation in a loop.
	ANIMATION_STATE_LOOPING = 3,
};

/// The status of an image. Each image uploaded to the terminal is cached on
/// disk, then it is loaded to ram when needed.
enum ImageStatus {
	STATUS_UNINITIALIZED = 0,
	STATUS_UPLOADING = 1,
	STATUS_UPLOADING_ERROR = 2,
	STATUS_UPLOADING_SUCCESS = 3,
	STATUS_RAM_LOADING_ERROR = 4,
	STATUS_RAM_LOADING_IN_PROGRESS = 5,
	STATUS_RAM_LOADING_SUCCESS = 6,
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

////////////////////////////////////////////////////////////////////////////////
//
// We use the following structures to represent images and placements:
//
//   - Image: this is the main structure representing an image, usually created
//     by actions 'a=t', 'a=T`. Each image has an id (image id aka client id,
//     specified by 'i='). An image may have multiple frames (ImageFrame) and
//     placements (ImagePlacement).
//
//   - ImageFrame: represents a single frame of an image, usually created by
//     the action 'a=f' (and the first frame is created with the image itself).
//     Each frame has an index and also:
//     - a file containing the frame data (considered to be "on disk", although
//       it's probably in tmpfs),
//     - an imlib object containing the fully composed frame (i.e. the frame
//       data from the file composed onto the background frame or color). It is
//       not ready for display yet, because it needs to be scaled and uploaded
//       to the X server.
//
//   - ImagePlacement: represents a placement of an image, created by 'a=p' and
//     'a=T'. Each placement has an id (placement id, specified by 'p='). Also
//     each placement has an array of pixmaps: one for each frame of the image.
//     Each pixmap is a scaled and uploaded image ready to be displayed.
//
// Images are store in the `images` hash table, mapping image ids to Image
// objects (allocated on the heap).
//
// Placements are stored in the `placements` hash table of each Image object,
// mapping placement ids to ImagePlacement objects (also allocated on the heap).
//
// ImageFrames are stored in the `first_frame` field and in the
// `frames_beyond_the_first` array of each Image object. They are stored by
// value, so ImageFrame pointer may be invalidated when frames are
// added/deleted, be careful.
//
////////////////////////////////////////////////////////////////////////////////

struct Image;
struct ImageFrame;
struct ImagePlacement;

KHASH_MAP_INIT_INT(id2image, struct Image *)
KHASH_MAP_INIT_INT(id2placement, struct ImagePlacement *)

typedef struct ImageFrame {
	/// The image this frame belongs to.
	struct Image *image;
	/// The 1-based index of the frame. Zero if the frame isn't initialized.
	int index;
	/// The last time when the frame was displayed or otherwise touched.
	Milliseconds atime;
	/// The background color of the frame in the 0xRRGGBBAA format.
	uint32_t background_color;
	/// The index of the background frame. Zero to use the color instead.
	int background_frame_index;
	/// The duration of the frame in milliseconds.
	int gap;
	/// The expected size of the frame image file (specified with 'S='),
	/// used to check if uploading succeeded.
	unsigned expected_size;
	/// Format specification (see the `f=` key).
	int format;
	/// Pixel width and height of the non-composed (on-disk) frame data. May
	/// differ from the image (i.e. first frame) dimensions.
	int data_pix_width, data_pix_height;
	/// The offset of the frame relative to the first frame.
	int x, y;
	/// Compression mode (see the `o=` key).
	char compression;
	/// The status (see `ImageStatus`).
	char status;
	/// The reason of uploading failure (see `ImageUploadingFailure`).
	char uploading_failure;
	/// Whether failures and successes should be reported ('q=').
	char quiet;
	/// Whether to blend the frame with the background or replace it.
	char blend;
	/// The file corresponding to the on-disk cache, used when uploading.
	FILE *open_file;
	/// The size of the corresponding file cached on disk.
	unsigned disk_size;
	/// The imlib object containing the fully composed frame. It's not
	/// scaled for screen display yet.
	Imlib_Image imlib_object;
} ImageFrame;

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
	Milliseconds atime;
	/// The total duration of the animation in milliseconds.
	int total_duration;
	/// The total size of cached image files for all frames.
	int total_disk_size;
	/// The global index of the creation command. Used to decide which image
	/// is newer if they have the same image number.
	uint64_t global_command_index;
	/// The 1-based index of the currently displayed frame.
	int current_frame;
	/// The state of the animation, see `AnimationState`.
	char animation_state;
	/// The absolute time that is assumed to be the start of the current
	/// frame (in ms since initialization).
	Milliseconds current_frame_time;
	/// The absolute time of the last redraw (in ms since initialization).
	/// Used to check whether it's the first time we draw the image in the
	/// current redraw cycle.
	Milliseconds last_redraw;
	/// The absolute time of the next redraw (in ms since initialization).
	/// 0 means no redraw is scheduled.
	Milliseconds next_redraw;
	/// The unscaled pixel width and height of the image. Usually inherited
	/// from the first frame.
	int pix_width, pix_height;
	/// The first frame.
	ImageFrame first_frame;
	/// The array of frames beyond the first one.
	kvec_t(ImageFrame) frames_beyond_the_first;
	/// Image placements.
	khash_t(id2placement) *placements;
	/// The default placement.
	uint32_t default_placement;
	/// The initial placement id, specified with the transmission command,
	/// used to report success or failure.
	uint32_t initial_placement_id;
} Image;

typedef struct ImagePlacement {
	/// The image this placement belongs to.
	Image *image;
	/// The id of the placement. Must be nonzero.
	uint32_t placement_id;
	/// The last time when the placement was displayed or otherwise touched.
	Milliseconds atime;
	/// The 1-based index of the protected pixmap. We protect a pixmap in
	/// gr_load_pixmap to avoid unloading it right after it was loaded.
	int protected_frame;
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
	/// The image appropriately scaled and uploaded to the X server. This
	/// pixmap is premultiplied by alpha.
	Pixmap first_pixmap;
	/// The array of pixmaps beyond the first one.
	kvec_t(Pixmap) pixmaps_beyond_the_first;
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
	int screen_x_pix, screen_y_pix;
	/// The starting row on the screen.
	int screen_y_row;
	/// The part of the whole image to be drawn, in cells. Starts are
	/// zero-based, ends are exclusive.
	int img_start_col, img_end_col, img_start_row, img_end_row;
	/// The current cell width and height in pixels.
	int cw, ch;
	/// Whether colors should be inverted.
	int reverse;
} ImageRect;

/// Executes `code` for each frame of an image. Example:
///
///     foreach_frame(image, frame, {
///         printf("Frame %d\n", frame->index);
///     });
///
#define foreach_frame(image, framevar, code) { size_t __i; \
	for (__i = 0; __i <= kv_size((image).frames_beyond_the_first); ++__i) { \
		ImageFrame *framevar = \
			__i == 0 ? &(image).first_frame \
			: &kv_A((image).frames_beyond_the_first, __i - 1); \
		code; \
	} }

/// Executes `code` for each pixmap of a placement. Example:
///
///     foreach_pixmap(placement, pixmap, {
///         ...
///     });
///
#define foreach_pixmap(placement, pixmapvar, code) { size_t __i; \
	for (__i = 0; __i <= kv_size((placement).pixmaps_beyond_the_first); ++__i) { \
		Pixmap pixmapvar = \
			__i == 0 ? (placement).first_pixmap \
			: kv_A((placement).pixmaps_beyond_the_first, __i - 1); \
		code; \
	} }


static Image *gr_find_image(uint32_t image_id);
static void gr_get_frame_filename(ImageFrame *frame, char *out, size_t max_len);
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
/// The index of the frame currently being uploaded.
static int current_upload_frame_index = 0;
/// The time when the graphics module was initialized.
static struct timespec initialization_time = {0};
/// The time when the current frame drawing started, used for debugging fps and
/// to calculate the current frame for animations.
static Milliseconds drawing_start_time;
/// The global index of the current command.
static uint64_t global_command_counter = 0;
/// The next redraw times for each row of the terminal. Used for animations.
/// 0 means no redraw is scheduled.
static kvec_t(Milliseconds) next_redraw_times = {0, 0, NULL};
/// The number of files loaded in the current redraw cycle.
static int this_redraw_cycle_loaded_files = 0;
/// The number of pixmaps loaded in the current redraw cycle.
static int this_redraw_cycle_loaded_pixmaps = 0;

/// The directory where the cache files are stored.
static char cache_dir[MAX_FILENAME_SIZE - 16];

/// The table used for color inversion.
static unsigned char reverse_table[256];

// Declared in the header.
GraphicsDebugMode graphics_debug_mode = GRAPHICS_DEBUG_NONE;
char graphics_display_images = 1;
GraphicsCommandResult graphics_command_result = {0};
int graphics_next_redraw_delay = INT_MAX;

// Defined in config.h
extern const char graphics_cache_dir_template[];
extern unsigned graphics_max_single_image_file_size;
extern unsigned graphics_total_file_cache_size;
extern unsigned graphics_max_single_image_ram_size;
extern unsigned graphics_max_total_ram_size;
extern unsigned graphics_max_total_placements;
extern double graphics_excess_tolerance_ratio;
extern unsigned graphics_animation_min_delay;


////////////////////////////////////////////////////////////////////////////////
// Basic helpers.
////////////////////////////////////////////////////////////////////////////////

#define MIN(a, b)		((a) < (b) ? (a) : (b))
#define MAX(a, b)		((a) < (b) ? (b) : (a))

/// Returns the difference between `end` and `start` in milliseconds.
static int64_t gr_timediff_ms(const struct timespec *end,
			      const struct timespec *start) {
	return (end->tv_sec - start->tv_sec) * 1000 +
	       (end->tv_nsec - start->tv_nsec) / 1000000;
}

/// Returns the current time in milliseconds since the initialization.
static Milliseconds gr_now_ms() {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return gr_timediff_ms(&now, &initialization_time);
}

////////////////////////////////////////////////////////////////////////////////
// Logging.
////////////////////////////////////////////////////////////////////////////////

#define GR_LOG(...) \
	do { if(graphics_debug_mode) fprintf(stderr, __VA_ARGS__); } while(0)

////////////////////////////////////////////////////////////////////////////////
// Basic image management functions (create, delete, find, etc).
////////////////////////////////////////////////////////////////////////////////

/// Returns the 1-based index of the last frame. Note that you may want to use
/// `gr_last_uploaded_frame_index` instead since the last frame may be not
/// fully uploaded yet.
static inline int gr_last_frame_index(Image *img) {
	return kv_size(img->frames_beyond_the_first) + 1;
}

/// Returns the frame with the given index. Returns NULL if the index is out of
/// bounds. The index is 1-based.
static ImageFrame *gr_get_frame(Image *img, int index) {
	if (!img)
		return NULL;
	if (index == 1)
		return &img->first_frame;
	if (2 <= index && index <= gr_last_frame_index(img))
		return &kv_A(img->frames_beyond_the_first, index - 2);
	return NULL;
}

/// Returns the last frame of the image. Returns NULL if `img` is NULL.
static ImageFrame *gr_get_last_frame(Image *img) {
	if (!img)
		return NULL;
	return gr_get_frame(img, gr_last_frame_index(img));
}

/// Returns the 1-based index of the last frame or the second-to-last frame if
/// the last frame is not fully uploaded yet.
static inline int gr_last_uploaded_frame_index(Image *img) {
	int last_index = gr_last_frame_index(img);
	if (last_index > 1 &&
	    gr_get_frame(img, last_index)->status < STATUS_UPLOADING_SUCCESS)
		return last_index - 1;
	return last_index;
}

/// Returns the pixmap for the frame with the given index. Returns 0 if the
/// index is out of bounds. The index is 1-based.
static Pixmap gr_get_frame_pixmap(ImagePlacement *placement, int index) {
	if (index == 1)
		return placement->first_pixmap;
	if (2 <= index &&
	    index <= kv_size(placement->pixmaps_beyond_the_first) + 1)
		return kv_A(placement->pixmaps_beyond_the_first, index - 2);
	return 0;
}

/// Sets the pixmap for the frame with the given index. The index is 1-based.
/// The array of pixmaps is resized if needed.
static void gr_set_frame_pixmap(ImagePlacement *placement, int index,
				Pixmap pixmap) {
	if (index == 1) {
		placement->first_pixmap = pixmap;
		return;
	}
	// Resize the array if needed.
	size_t old_size = kv_size(placement->pixmaps_beyond_the_first);
	if (old_size < index - 1) {
		kv_a(Pixmap, placement->pixmaps_beyond_the_first, index - 2);
		for (size_t i = old_size; i < index - 1; i++)
			kv_A(placement->pixmaps_beyond_the_first, i) = 0;
	}
	kv_A(placement->pixmaps_beyond_the_first, index - 2) = pixmap;
}

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
/// size of `out`. The name will be something like
/// "/tmp/st-images-xxx/img-ID-FRAME".
static void gr_get_frame_filename(ImageFrame *frame, char *out,
				  size_t max_len) {
	snprintf(out, max_len, "%s/img-%.3u-%.3u", cache_dir,
		 frame->image->image_id, frame->index);
}

/// Returns the (estimation) of the RAM size used by the frame right now.
static unsigned gr_frame_current_ram_size(ImageFrame *frame) {
	if (!frame->imlib_object)
		return 0;
	return (unsigned)frame->image->pix_width * frame->image->pix_height * 4;
}

/// Returns the (estimation) of the RAM size used by a single frame pixmap.
static unsigned gr_placement_single_frame_ram_size(ImagePlacement *placement) {
	return (unsigned)placement->rows * placement->cols *
	       placement->scaled_ch * placement->scaled_cw * 4;
}

/// Returns the (estimation) of the RAM size used by the placemenet right now.
static unsigned gr_placement_current_ram_size(ImagePlacement *placement) {
	unsigned single_frame_size =
		gr_placement_single_frame_ram_size(placement);
	unsigned result = 0;
	foreach_pixmap(*placement, pixmap, {
		if (pixmap)
			result += single_frame_size;
	});
	return result;
}

/// Unload the frame from RAM (i.e. delete the corresponding imlib object).
/// If the on-disk file of the frame is preserved, it can be reloaded later.
static void gr_unload_frame(ImageFrame *frame) {
	if (!frame->imlib_object)
		return;

	unsigned frame_ram_size = gr_frame_current_ram_size(frame);
	images_ram_size -= frame_ram_size;

	imlib_context_set_image(frame->imlib_object);
	imlib_free_image_and_decache();
	frame->imlib_object = NULL;

	GR_LOG("After unloading image %u frame %u (atime %ld ms ago) "
	       "ram: %ld KiB  (- %u KiB)\n",
	       frame->image->image_id, frame->index,
	       drawing_start_time - frame->atime, images_ram_size / 1024,
	       frame_ram_size / 1024);
}

/// Unload all frames of the image.
static void gr_unload_all_frames(Image *img) {
	foreach_frame(*img, frame, {
		gr_unload_frame(frame);
	});
}

/// Unload the placement from RAM (i.e. free all of the corresponding pixmaps).
/// If the on-disk files or imlib objects of the corresponding image are
/// preserved, the placement can be reloaded later.
static void gr_unload_placement(ImagePlacement *placement) {
	unsigned placement_ram_size = gr_placement_current_ram_size(placement);
	images_ram_size -= placement_ram_size;

	Display *disp = imlib_context_get_display();
	foreach_pixmap(*placement, pixmap, {
		if (pixmap)
			XFreePixmap(disp, pixmap);
	});

	placement->first_pixmap = 0;
	placement->pixmaps_beyond_the_first.n = 0;
	placement->scaled_ch = placement->scaled_cw = 0;

	GR_LOG("After unloading placement %u/%u (atime %ld ms ago) "
	       "ram: %ld KiB  (- %u KiB)\n",
	       placement->image->image_id, placement->placement_id,
	       drawing_start_time - placement->atime, images_ram_size / 1024,
	       placement_ram_size / 1024);
}

/// Unload a single pixmap of the placement from RAM.
static void gr_unload_pixmap(ImagePlacement *placement, int frameidx) {
	Pixmap pixmap = gr_get_frame_pixmap(placement, frameidx);
	if (!pixmap)
		return;

	Display *disp = imlib_context_get_display();
	XFreePixmap(disp, pixmap);
	gr_set_frame_pixmap(placement, frameidx, 0);
	images_ram_size -= gr_placement_single_frame_ram_size(placement);

	GR_LOG("After unloading pixmap %ld of "
	       "placement %u/%u (atime %ld ms ago) "
	       "frame %u (atime %ld ms ago) "
	       "ram: %ld KiB  (- %u KiB)\n",
	       pixmap, placement->image->image_id, placement->placement_id,
	       drawing_start_time - placement->atime, frameidx,
	       drawing_start_time -
		       gr_get_frame(placement->image, frameidx)->atime,
	       images_ram_size / 1024,
	       gr_placement_single_frame_ram_size(placement) / 1024);
}

/// Deletes the on-disk cache file corresponding to the frame. The in-ram image
/// object (if it exists) is not deleted, placements are not unloaded either.
static void gr_delete_imagefile(ImageFrame *frame) {
	// It may still be being loaded. Close the file in this case.
	if (frame->open_file) {
		fclose(frame->open_file);
		frame->open_file = NULL;
	}

	if (frame->disk_size == 0)
		return;

	char filename[MAX_FILENAME_SIZE];
	gr_get_frame_filename(frame, filename, MAX_FILENAME_SIZE);
	remove(filename);

	unsigned disk_size = frame->disk_size;
	images_disk_size -= disk_size;
	frame->image->total_disk_size -= disk_size;
	frame->disk_size = 0;

	GR_LOG("After deleting image file %u frame %u (atime %ld ms ago) "
	       "disk: %ld KiB  (- %u KiB)\n",
	       frame->image->image_id, frame->index,
	       drawing_start_time - frame->atime, images_disk_size / 1024,
	       disk_size / 1024);
}

/// Deletes all on-disk cache files of the image (for each frame).
static void gr_delete_imagefiles(Image *img) {
	foreach_frame(*img, frame, {
		gr_delete_imagefile(frame);
	});
}

/// Deletes the given placement: unloads, frees the object, but doesn't change
/// the `placements` hash table.
static void gr_delete_placement_keep_id(ImagePlacement *placement) {
	if (!placement)
		return;
	GR_LOG("Deleting placement %u/%u\n", placement->image->image_id,
	       placement->placement_id);
	gr_unload_placement(placement);
	kv_destroy(placement->pixmaps_beyond_the_first);
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
	foreach_frame(*img, frame, {
		gr_delete_imagefile(frame);
		gr_unload_frame(frame);
	});
	kv_destroy(img->frames_beyond_the_first);
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

/// Update the atime of the image.
static void gr_touch_image(Image *img) {
	img->atime = gr_now_ms();
}

/// Update the atime of the frame.
static void gr_touch_frame(ImageFrame *frame) {
	frame->image->atime = frame->atime = gr_now_ms();
}

/// Update the atime of the placement. Touches the images too.
static void gr_touch_placement(ImagePlacement *placement) {
	placement->image->atime = placement->atime = gr_now_ms();
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

/// Creates a new frame at the end of the frame array. It may be the first frame
/// if there are no frames yet.
static ImageFrame *gr_append_new_frame(Image *img) {
	ImageFrame *frame = NULL;
	if (img->first_frame.index == 0 &&
	    kv_size(img->frames_beyond_the_first) == 0) {
		frame = &img->first_frame;
		frame->index = 1;
	} else {
		frame = kv_pushp(ImageFrame, img->frames_beyond_the_first);
		memset(frame, 0, sizeof(ImageFrame));
		frame->index = kv_size(img->frames_beyond_the_first) + 1;
	}
	frame->image = img;
	gr_touch_frame(frame);
	GR_LOG("Appending frame %d to image %u\n", frame->index, img->image_id);
	return frame;
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
	// The size of the image.
	int image_pix_width = placement->image->pix_width;
	int image_pix_height = placement->image->pix_height;
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
	if (placement->src_pix_x > image_pix_width)
		placement->src_pix_x = image_pix_width;
	if (placement->src_pix_y > image_pix_height)
		placement->src_pix_y = image_pix_height;
	// If the source rectangle is not specified, use the whole image. If
	// it's partially outside the image, truncate it.
	if (placement->src_pix_width == 0 ||
	    placement->src_pix_x + placement->src_pix_width > image_pix_width)
		placement->src_pix_width =
			image_pix_width - placement->src_pix_x;
	if (placement->src_pix_height == 0 ||
	    placement->src_pix_y + placement->src_pix_height > image_pix_height)
		placement->src_pix_height =
			image_pix_height - placement->src_pix_y;

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

	// Some applications specify only one of the dimensions.
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
		// pixel size and cell size.
		// TODO: In the case of stretching it's not the most logical
		//       thing to do, may need to revisit in the future.
		//       Currently we switch to SCALE_MODE_CONTAIN when only one
		//       of the dimensions is specified, so this case shouldn't
		//       happen in practice.
		if (!placement->cols)
			placement->cols =
				ceil_div(placement->src_pix_width, current_cw);
		if (!placement->rows)
			placement->rows =
				ceil_div(placement->src_pix_height, current_ch);
	}
}

/// Adjusts the current frame index if enough time has passed since the display
/// of the current frame. Also computes the time of the next redraw of this
/// image (`img->next_redraw`). The current time is passed as an argument so
/// that all animations are in sync.
static void gr_update_frame_index(Image *img, Milliseconds now) {
	if (img->current_frame == 0) {
		img->current_frame_time = now;
		img->current_frame = 1;
		img->next_redraw = now + MAX(1, img->first_frame.gap);
		return;
	}
	// If the animation is stopped, show the current frame.
	if (!img->animation_state ||
	    img->animation_state == ANIMATION_STATE_STOPPED ||
	    img->animation_state == ANIMATION_STATE_UNSET) {
		// The next redraw is never (unless the state is changed).
		img->next_redraw = 0;
		return;
	}
	int last_uploaded_frame_index = gr_last_uploaded_frame_index(img);
	// If we are loading and we reached the last frame, show the last frame.
	if (img->animation_state == ANIMATION_STATE_LOADING &&
	    img->current_frame == last_uploaded_frame_index) {
		// The next redraw is never (unless the state is changed or
		// frames are added).
		img->next_redraw = 0;
		return;
	}

	// Check how many milliseconds passed since the current frame was shown.
	int passed_ms = now - img->current_frame_time;
	// If the animation is looping and too much time has passes, we can
	// make a shortcut.
	if (img->animation_state == ANIMATION_STATE_LOOPING &&
	    img->total_duration > 0 && passed_ms >= img->total_duration) {
		passed_ms %= img->total_duration;
		img->current_frame_time = now - passed_ms;
	}
	// Find the next frame.
	int original_frame_index = img->current_frame;
	while (1) {
		ImageFrame *frame = gr_get_frame(img, img->current_frame);
		if (!frame) {
			// The frame doesn't exist, go to the first frame.
			img->current_frame = 1;
			img->current_frame_time = now;
			img->next_redraw = now + MAX(1, img->first_frame.gap);
			return;
		}
		if (frame->gap >= 0 && passed_ms < frame->gap) {
			// Not enough time has passed, we are still in the same
			// frame, and it's not a gapless frame.
			img->next_redraw =
				img->current_frame_time + MAX(1, frame->gap);
			return;
		}
		// Otherwise go to the next frame.
		passed_ms -= MAX(0, frame->gap);
		if (img->current_frame >= last_uploaded_frame_index) {
			// It's the last frame, if the animation is loading,
			// remain on it.
			if (img->animation_state == ANIMATION_STATE_LOADING) {
				img->next_redraw = 0;
				return;
			}
			// Otherwise the animation is looping.
			img->current_frame = 1;
			// TODO: Support finite number of loops.
		} else {
			img->current_frame++;
		}
		// Make sure we don't get stuck in an infinite loop.
		if (img->current_frame == original_frame_index) {
			// We looped through all frames, but haven't reached the
			// next frame yet. This may happen if too much time has
			// passed since the last redraw or all the frames are
			// gapless. Just move on to the next frame.
			img->current_frame++;
			if (img->current_frame >
			    last_uploaded_frame_index)
				img->current_frame = 1;
			img->current_frame_time = now;
			img->next_redraw = now + MAX(
				1, gr_get_frame(img, img->current_frame)->gap);
			return;
		}
		// Adjust the start time of the frame. The next redraw time will
		// be set in the next iteration.
		img->current_frame_time += MAX(0, frame->gap);
	}
}

////////////////////////////////////////////////////////////////////////////////
// Unloading and deleting images to save resources.
////////////////////////////////////////////////////////////////////////////////

/// A helper to compare frames by atime for qsort.
static int gr_cmp_frames_by_atime(const void *a, const void *b) {
	ImageFrame *frame_a = *(ImageFrame *const *)a;
	ImageFrame *frame_b = *(ImageFrame *const *)b;
	if (frame_a->atime == frame_b->atime)
		return frame_a->image->global_command_index -
		       frame_b->image->global_command_index;
	return frame_a->atime - frame_b->atime;
}

/// A helper to compare images by atime for qsort.
static int gr_cmp_images_by_atime(const void *a, const void *b) {
	Image *img_a = *(Image *const *)a;
	Image *img_b = *(Image *const *)b;
	if (img_a->atime == img_b->atime)
		return img_a->global_command_index -
		       img_b->global_command_index;
	return img_a->atime - img_b->atime;
}

/// A helper to compare placements by atime for qsort.
static int gr_cmp_placements_by_atime(const void *a, const void *b) {
	ImagePlacement *p_a = *(ImagePlacement **)a;
	ImagePlacement *p_b = *(ImagePlacement **)b;
	if (p_a->atime == p_b->atime)
		return p_a->image->global_command_index -
		       p_b->image->global_command_index;
	return p_a->atime - p_b->atime;
}

typedef kvec_t(Image *) ImageVec;
typedef kvec_t(ImagePlacement *) ImagePlacementVec;
typedef kvec_t(ImageFrame *) ImageFrameVec;

/// Returns an array of pointers to all images sorted by atime.
static ImageVec gr_get_images_sorted_by_atime() {
	ImageVec vec;
	kv_init(vec);
	if (kh_size(images) == 0)
		return vec;
	kv_resize(Image *, vec, kh_size(images));
	Image *img = NULL;
	kh_foreach_value(images, img, { kv_push(Image *, vec, img); });
	qsort(vec.a, kv_size(vec), sizeof(Image *), gr_cmp_images_by_atime);
	return vec;
}

/// Returns an array of pointers to all placements sorted by atime.
static ImagePlacementVec gr_get_placements_sorted_by_atime() {
	ImagePlacementVec vec;
	kv_init(vec);
	if (total_placement_count == 0)
		return vec;
	kv_resize(ImagePlacement *, vec, total_placement_count);
	Image *img = NULL;
	ImagePlacement *placement = NULL;
	kh_foreach_value(images, img, {
		kh_foreach_value(img->placements, placement, {
			kv_push(ImagePlacement *, vec, placement);
		});
	});
	qsort(vec.a, kv_size(vec), sizeof(ImagePlacement *),
	      gr_cmp_placements_by_atime);
	return vec;
}

/// Returns an array of pointers to all frames sorted by atime.
static ImageFrameVec gr_get_frames_sorted_by_atime() {
	ImageFrameVec frames;
	kv_init(frames);
	Image *img = NULL;
	kh_foreach_value(images, img, {
		foreach_frame(*img, frame, {
			kv_push(ImageFrame *, frames, frame);
		});
	});
	qsort(frames.a, kv_size(frames), sizeof(ImageFrame *),
	      gr_cmp_frames_by_atime);
	return frames;
}

/// An object that can be unloaded from RAM.
typedef struct {
	/// Some score, probably based on access time. The lower the score, the
	/// more likely that the object should be unloaded.
	int64_t score;
	union {
		ImagePlacement *placement;
		ImageFrame *frame;
	};
	/// If zero, the object is the imlib object of `frame`, if non-zero,
	/// the object is a pixmap of `frameidx`-th frame of `placement`.
	int frameidx;
} UnloadableObject;

typedef kvec_t(UnloadableObject) UnloadableObjectVec;

/// A helper to compare unloadable objects by score for qsort.
static int gr_cmp_unloadable_objects(const void *a, const void *b) {
	UnloadableObject *obj_a = (UnloadableObject *)a;
	UnloadableObject *obj_b = (UnloadableObject *)b;
	return obj_a->score - obj_b->score;
}

/// Unloads an unloadable object from RAM.
static void gr_unload_object(UnloadableObject *obj) {
	if (obj->frameidx) {
		if (obj->placement->protected_frame == obj->frameidx)
			return;
		gr_unload_pixmap(obj->placement, obj->frameidx);
	} else {
		gr_unload_frame(obj->frame);
	}
}

/// Returns the recency threshold for an image. Frames that were accessed within
/// this threshold from now are considered recent and may be handled
/// differently because we may need them again very soon.
static Milliseconds gr_recency_threshold(Image *img) {
	return img->total_duration * 2 + 1000;
}

/// Creates an unloadable object for the imlib object of a frame.
static UnloadableObject gr_unloadable_object_for_frame(Milliseconds now,
						       ImageFrame *frame) {
	UnloadableObject obj = {0};
	obj.frameidx = 0;
	obj.frame = frame;
	Milliseconds atime = frame->atime;
	obj.score = atime;
	if (atime >= now - gr_recency_threshold(frame->image)) {
		// This is a recent frame, probably from an active animation.
		// Score it above `now` to prefer unloading non-active frames.
		// Randomize the score because it's not very clear in which
		// order we want to unload them: reloading a frame may require
		// reloading other frames.
		obj.score = now + 1000 + rand() % 1000;
	}
	return obj;
}

/// Creates an unloadable object for a pixmap.
static UnloadableObject
gr_unloadable_object_for_pixmap(Milliseconds now, ImageFrame *frame,
				ImagePlacement *placement) {
	UnloadableObject obj = {0};
	obj.frameidx = frame->index;
	obj.placement = placement;
	obj.score = placement->atime;
	// Since we don't store pixmap atimes, use the
	// oldest atime of the frame and the placement.
	Milliseconds atime = MIN(placement->atime, frame->atime);
	obj.score = atime;
	if (atime >= now - gr_recency_threshold(frame->image)) {
		// This is a recent pixmap, probably from an active animation.
		// Score it above `now` to prefer unloading non-active frames.
		// Also assign higher scores to frames that are closer to the
		// current frame (more likely to be used soon).
		int num_frames = gr_last_frame_index(frame->image);
		int dist = frame->index - frame->image->current_frame;
		if (dist < 0)
			dist += num_frames;
		obj.score =
			now + 1000 + (num_frames - dist) * 1000 / num_frames;
		// If the pixmap is much larger than the imlib image, prefer to
		// unload the pixmap by adding up to -1000 to the score. If the
		// imlib image is larger, add up to +1000.
		float imlib_size = gr_frame_current_ram_size(frame);
		float pixmap_size =
			gr_placement_single_frame_ram_size(placement);
		obj.score +=
			2000 * (imlib_size / (imlib_size + pixmap_size) - 0.5);
	}
	return obj;
}

/// Returns an array of unloadable objects sorted by score.
static UnloadableObjectVec
gr_get_unloadable_objects_sorted_by_score(Milliseconds now) {
	UnloadableObjectVec objects;
	kv_init(objects);
	Image *img = NULL;
	ImagePlacement *placement = NULL;
	kh_foreach_value(images, img, {
		foreach_frame(*img, frame, {
			if (!frame->imlib_object)
				continue;
			kv_push(UnloadableObject, objects,
				gr_unloadable_object_for_frame(now, frame));
			int frameidx = frame->index;
			kh_foreach_value(img->placements, placement, {
				if (!gr_get_frame_pixmap(placement, frameidx))
					continue;
				kv_push(UnloadableObject, objects,
					gr_unloadable_object_for_pixmap(
						now, frame, placement));
			});
		});
	});
	qsort(objects.a, kv_size(objects), sizeof(UnloadableObject),
	      gr_cmp_unloadable_objects);
	return objects;
}

/// Returns the limit adjusted by the excess tolerance ratio.
static inline unsigned apply_tolerance(unsigned limit) {
	return limit + (unsigned)(limit * graphics_excess_tolerance_ratio);
}

/// Checks RAM and disk cache limits and deletes/unloads some images.
static void gr_check_limits() {
	Milliseconds now = gr_now_ms();
	ImageVec images_sorted = {0};
	ImagePlacementVec placements_sorted = {0};
	ImageFrameVec frames_sorted = {0};
	UnloadableObjectVec objects_sorted = {0};
	int images_begin = 0;
	int placements_begin = 0;
	char changed = 0;
	// First reduce the number of images if there are too many.
	if (kh_size(images) > apply_tolerance(graphics_max_total_placements)) {
		GR_LOG("Too many images: %d\n", kh_size(images));
		changed = 1;
		images_sorted = gr_get_images_sorted_by_atime();
		int to_delete = kv_size(images_sorted) -
				graphics_max_total_placements;
		for (; images_begin < to_delete; images_begin++)
			gr_delete_image(images_sorted.a[images_begin]);
	}
	// Then reduce the number of placements if there are too many.
	if (total_placement_count >
	    apply_tolerance(graphics_max_total_placements)) {
		GR_LOG("Too many placements: %d\n", total_placement_count);
		changed = 1;
		placements_sorted = gr_get_placements_sorted_by_atime();
		int to_delete = kv_size(placements_sorted) -
				graphics_max_total_placements;
		for (; placements_begin < to_delete; placements_begin++) {
			ImagePlacement *placement =
				placements_sorted.a[placements_begin];
			if (placement->protected_frame)
				break;
			gr_delete_placement(placement);
		}
	}
	// Then reduce the size of the image file cache. The files correspond to
	// image frames.
	if (images_disk_size >
	    apply_tolerance(graphics_total_file_cache_size)) {
		GR_LOG("Too big disk cache: %ld KiB\n",
		       images_disk_size / 1024);
		changed = 1;
		frames_sorted = gr_get_frames_sorted_by_atime();
		for (int i = 0; i < kv_size(frames_sorted); i++) {
			if (images_disk_size <= graphics_total_file_cache_size)
				break;
			gr_delete_imagefile(kv_A(frames_sorted, i));
		}
	}
	// Then unload images from RAM.
	if (images_ram_size > apply_tolerance(graphics_max_total_ram_size)) {
		changed = 1;
		int frames_begin = 0;
		GR_LOG("Too much ram: %ld KiB\n", images_ram_size / 1024);
		objects_sorted = gr_get_unloadable_objects_sorted_by_score(now);
		for (int i = 0; i < kv_size(objects_sorted); i++) {
			if (images_ram_size <= graphics_max_total_ram_size)
				break;
			gr_unload_object(&kv_A(objects_sorted, i));
		}
	}
	if (changed) {
		GR_LOG("After cleaning:  ram: %ld KiB  disk: %ld KiB  "
		       "img count: %d  placement count: %d\n",
		       images_ram_size / 1024, images_disk_size / 1024,
		       kh_size(images), total_placement_count);
	}
	kv_destroy(images_sorted);
	kv_destroy(placements_sorted);
	kv_destroy(frames_sorted);
	kv_destroy(objects_sorted);
}

/// Unloads all images by user request.
void gr_unload_images_to_reduce_ram() {
	Image *img = NULL;
	ImagePlacement *placement = NULL;
	kh_foreach_value(images, img, {
		kh_foreach_value(img->placements, placement, {
			if (placement->protected_frame)
				continue;
			gr_unload_placement(placement);
		});
		gr_unload_all_frames(img);
	});
}

////////////////////////////////////////////////////////////////////////////////
// Image loading.
////////////////////////////////////////////////////////////////////////////////

/// Copies `num_pixels` pixels (not bytes!) from a buffer `from` to an imlib2
/// image data `to`. The format may be 24 (RGB) or 32 (RGBA), and it's converted
/// to imlib2's representation, which is 0xAARRGGBB (having BGRA memory layout
/// on little-endian architectures).
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
static Imlib_Image gr_load_raw_pixel_data(ImageFrame *frame,
					  const char *filename) {
	size_t total_pixels = frame->data_pix_width * frame->data_pix_height;
	if (total_pixels * 4 > graphics_max_single_image_ram_size) {
		fprintf(stderr,
			"error: image %u frame %u is too big too load: %zu > %u\n",
			frame->image->image_id, frame->index, total_pixels * 4,
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

	Imlib_Image image = imlib_create_image(frame->data_pix_width,
					       frame->data_pix_height);
	if (!image) {
		fprintf(stderr,
			"error: could not create an image of size %d x %d\n",
			frame->data_pix_width, frame->data_pix_height);
		fclose(file);
		return NULL;
	}

	imlib_context_set_image(image);
	imlib_image_set_has_alpha(1);
	DATA32* data = imlib_image_get_data();

	// The default format is 32.
	int format = frame->format ? frame->format : 32;

	if (frame->compression == 0) {
		gr_load_raw_pixel_data_uncompressed(data, file, format,
						    total_pixels);
	} else {
		int ret = gr_load_raw_pixel_data_compressed(data, file, format,
							    total_pixels);
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

/// Loads the unscaled frame into RAM as an imlib object. The frame imlib object
/// is fully composed on top of the background frame. If the frame is already
/// loaded, does nothing. Loading may fail, in which case the status of the
/// frame will be set to STATUS_RAM_LOADING_ERROR.
static void gr_load_imlib_object(ImageFrame *frame) {
	if (frame->imlib_object)
		return;

	// If the image is uninitialized or uploading has failed, or the file
	// has been deleted, we cannot load the image.
	if (frame->status < STATUS_UPLOADING_SUCCESS)
		return;
	if (frame->disk_size == 0) {
		if (frame->status != STATUS_RAM_LOADING_ERROR) {
			fprintf(stderr,
				"error: cached image was deleted: %u frame %u\n",
				frame->image->image_id, frame->index);
		}
		frame->status = STATUS_RAM_LOADING_ERROR;
		return;
	}

	// Prevent recursive dependences between frames.
	if (frame->status == STATUS_RAM_LOADING_IN_PROGRESS) {
		fprintf(stderr,
			"error: recursive loading of image %u frame %u\n",
			frame->image->image_id, frame->index);
		frame->status = STATUS_RAM_LOADING_ERROR;
		return;
	}
	frame->status = STATUS_RAM_LOADING_IN_PROGRESS;

	// Load the background frame if needed. Hopefully it's not recursive.
	ImageFrame *bg_frame = NULL;
	if (frame->background_frame_index) {
		bg_frame = gr_get_frame(frame->image,
					frame->background_frame_index);
		if (!bg_frame) {
			fprintf(stderr,
				"error: could not find background "
				"frame %d for image %u frame %d\n",
				frame->background_frame_index,
				frame->image->image_id, frame->index);
			frame->status = STATUS_RAM_LOADING_ERROR;
			return;
		}
		gr_load_imlib_object(bg_frame);
		if (!bg_frame->imlib_object) {
			fprintf(stderr,
				"error: could not load background frame %d for "
				"image %u frame %d\n",
				frame->background_frame_index,
				frame->image->image_id, frame->index);
			frame->status = STATUS_RAM_LOADING_ERROR;
			return;
		}
	}

	// Load the frame data image.
	Imlib_Image frame_data_image = NULL;
	char filename[MAX_FILENAME_SIZE];
	gr_get_frame_filename(frame, filename, MAX_FILENAME_SIZE);
	GR_LOG("Loading image: %s\n", sanitized_filename(filename));
	if (frame->format == 100 || frame->format == 0)
		frame_data_image = imlib_load_image(filename);
	if (frame->format == 32 || frame->format == 24 ||
	    (!frame_data_image && frame->format == 0))
		frame_data_image = gr_load_raw_pixel_data(frame, filename);
	this_redraw_cycle_loaded_files++;

	if (!frame_data_image) {
		if (frame->status != STATUS_RAM_LOADING_ERROR) {
			fprintf(stderr, "error: could not load image: %s\n",
				sanitized_filename(filename));
		}
		frame->status = STATUS_RAM_LOADING_ERROR;
		return;
	}

	imlib_context_set_image(frame_data_image);
	int frame_data_width = imlib_image_get_width();
	int frame_data_height = imlib_image_get_height();
	GR_LOG("Successfully loaded, size %d x %d\n", frame_data_width,
	       frame_data_height);
	// If imlib loading succeeded, and it is the first frame, set the
	// information about the original image size, unless it's already set.
	if (frame->index == 1 && frame->image->pix_width == 0 &&
	    frame->image->pix_height == 0) {
		frame->image->pix_width = frame_data_width;
		frame->image->pix_height = frame_data_height;
	}

	int image_width = frame->image->pix_width;
	int image_height = frame->image->pix_height;

	// Compose the image with the background color or frame.
	if (frame->background_color != 0 || bg_frame ||
	    image_width != frame_data_width ||
	    image_height != frame_data_height) {
		GR_LOG("Composing the frame bg = 0x%08X, bgframe = %d\n",
		       frame->background_color, frame->background_frame_index);
		Imlib_Image composed_image = imlib_create_image(
			image_width, image_height);
		imlib_context_set_image(composed_image);
		imlib_image_set_has_alpha(1);
		imlib_context_set_anti_alias(0);

		// Start with the background frame or color.
		imlib_context_set_blend(0);
		if (bg_frame && bg_frame->imlib_object) {
			imlib_blend_image_onto_image(
				bg_frame->imlib_object, 1, 0, 0,
				image_width, image_height, 0, 0,
				image_width, image_height);
		} else {
			int r = (frame->background_color >> 24) & 0xFF;
			int g = (frame->background_color >> 16) & 0xFF;
			int b = (frame->background_color >> 8) & 0xFF;
			int a = frame->background_color & 0xFF;
			imlib_context_set_color(r, g, b, a);
			imlib_image_fill_rectangle(0, 0, image_width,
						   image_height);
		}

		// Blend the frame data image onto the background.
		imlib_context_set_blend(1);
		imlib_blend_image_onto_image(
			frame_data_image, 1, 0, 0, frame->data_pix_width,
			frame->data_pix_height, frame->x, frame->y,
			frame->data_pix_width, frame->data_pix_height);

		// Free the frame data image.
		imlib_context_set_image(frame_data_image);
		imlib_free_image();

		frame_data_image = composed_image;
	}

	frame->imlib_object = frame_data_image;

	images_ram_size += gr_frame_current_ram_size(frame);
	frame->status = STATUS_RAM_LOADING_SUCCESS;

	GR_LOG("After loading image %u frame %d ram: %ld KiB  (+ %u KiB)\n",
	       frame->image->image_id, frame->index,
	       images_ram_size / 1024, gr_frame_current_ram_size(frame) / 1024);
}

/// Premultiplies the alpha channel of the image data. The data is an array of
/// pixels such that each pixel is a 32-bit integer in the format 0xAARRGGBB.
static void gr_premultiply_alpha(DATA32 *data, size_t num_pixels) {
	for (size_t i = 0; i < num_pixels; ++i) {
		DATA32 pixel = data[i];
		unsigned char a = pixel >> 24;
		if (a == 0) {
			data[i] = 0;
		} else if (a != 255) {
			unsigned char b = (pixel & 0xFF) * a / 255;
			unsigned char g = ((pixel >> 8) & 0xFF) * a / 255;
			unsigned char r = ((pixel >> 16) & 0xFF) * a / 255;
			data[i] = (a << 24) | (r << 16) | (g << 8) | b;
		}
	}
}

/// Creates a pixmap for the frame of an image placement. The pixmap contain the
/// image data correctly scaled and fit to the box defined by the number of
/// rows/columns of the image placement and the provided cell dimensions in
/// pixels. If the placement is already loaded, it will be reloaded only if the
/// cell dimensions have changed.
Pixmap gr_load_pixmap(ImagePlacement *placement, int frameidx, int cw, int ch) {
	Image *img = placement->image;
	ImageFrame *frame = gr_get_frame(img, frameidx);

	// Update the atime uncoditionally.
	gr_touch_placement(placement);
	if (frame)
		gr_touch_frame(frame);

	// If cw or ch are different, unload all the pixmaps.
	if (placement->scaled_cw != cw || placement->scaled_ch != ch) {
		gr_unload_placement(placement);
		placement->scaled_cw = cw;
		placement->scaled_ch = ch;
	}

	// If it's already loaded, do nothing.
	Pixmap pixmap = gr_get_frame_pixmap(placement, frameidx);
	if (pixmap)
		return pixmap;

	GR_LOG("Loading placement: %u/%u frame %u\n", img->image_id,
	       placement->placement_id, frameidx);

	// Load the imlib object for the frame.
	if (!frame) {
		fprintf(stderr,
			"error: could not find frame %u for image %u\n",
			frameidx, img->image_id);
		return 0;
	}
	gr_load_imlib_object(frame);
	if (!frame->imlib_object)
		return 0;

	// Infer the placement size if needed.
	gr_infer_placement_size_maybe(placement);

	// Create the scaled image. This is temporary, we will scale it
	// appropriately, upload to the X server, and then delete immediately.
	int scaled_w = (int)placement->cols * cw;
	int scaled_h = (int)placement->rows * ch;
	if (scaled_w * scaled_h * 4 > graphics_max_single_image_ram_size) {
		fprintf(stderr,
			"error: placement %u/%u would be too big to load: %d x "
			"%d x 4 > %u\n",
			img->image_id, placement->placement_id, scaled_w,
			scaled_h, graphics_max_single_image_ram_size);
		return 0;
	}
	Imlib_Image scaled_image = imlib_create_image(scaled_w, scaled_h);
	if (!scaled_image) {
		fprintf(stderr,
			"error: imlib_create_image(%d, %d) returned "
			"null\n",
			scaled_w, scaled_h);
		return 0;
	}
	imlib_context_set_image(scaled_image);
	imlib_image_set_has_alpha(1);

	// First fill the scaled image with the transparent color.
	imlib_context_set_blend(0);
	imlib_context_set_color(0, 0, 0, 0);
	imlib_image_fill_rectangle(0, 0, scaled_w, scaled_h);
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
		imlib_blend_image_onto_image(frame->imlib_object, 1, src_x,
					     src_y, src_w, src_h, 0, 0,
					     scaled_w, scaled_h);
	} else if (mode == SCALE_MODE_NONE ||
		   (mode == SCALE_MODE_NONE_OR_CONTAIN && !box_too_small)) {
		imlib_blend_image_onto_image(frame->imlib_object, 1, src_x,
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
		imlib_blend_image_onto_image(frame->imlib_object, 1, src_x,
					     src_y, src_w, src_h, dest_x,
					     dest_y, dest_w, dest_h);
	}

	// XRender needs the alpha channel premultiplied.
	DATA32 *data = imlib_image_get_data();
	gr_premultiply_alpha(data, scaled_w * scaled_h);

	// Upload the image to the X server.
	Display *disp = imlib_context_get_display();
	Visual *vis = imlib_context_get_visual();
	Colormap cmap = imlib_context_get_colormap();
	Drawable drawable = imlib_context_get_drawable();
	if (!drawable)
		drawable = DefaultRootWindow(disp);
	pixmap = XCreatePixmap(disp, drawable, scaled_w, scaled_h, 32);
	XVisualInfo visinfo;
	XMatchVisualInfo(disp, DefaultScreen(disp), 32, TrueColor, &visinfo);
	XImage *ximage = XCreateImage(disp, visinfo.visual, 32, ZPixmap, 0,
				      (char *)data, scaled_w, scaled_h, 32, 0);
	GC gc = XCreateGC(disp, pixmap, 0, NULL);
	XPutImage(disp, pixmap, gc, ximage, 0, 0, 0, 0, scaled_w,
		  scaled_h);
	XFreeGC(disp, gc);
	// XDestroyImage will free the data as well, but it is managed by imlib,
	// so set it to NULL.
	ximage->data = NULL;
	XDestroyImage(ximage);
	imlib_image_put_back_data(data);
	imlib_free_image();

	// Assign the pixmap to the frame and increase the ram size.
	gr_set_frame_pixmap(placement, frameidx, pixmap);
	images_ram_size += gr_placement_single_frame_ram_size(placement);
	this_redraw_cycle_loaded_pixmaps++;

	GR_LOG("After loading placement %u/%u frame %d ram: %ld KiB  (+ %u "
	       "KiB)\n",
	       frame->image->image_id, placement->placement_id, frame->index,
	       images_ram_size / 1024,
	       gr_placement_single_frame_ram_size(placement) / 1024);

	// Free up ram if needed, but keep the pixmap we've loaded no matter
	// what.
	placement->protected_frame = frameidx;
	gr_check_limits();
	placement->protected_frame = 0;

	return pixmap;
}

////////////////////////////////////////////////////////////////////////////////
// Initialization and deinitialization.
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
	// Set the initialization time.
	clock_gettime(CLOCK_MONOTONIC, &initialization_time);

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
	kv_init(next_redraw_times);

	atexit(gr_deinit);
}

/// Deinitialize the graphics module.
void gr_deinit() {
	// Remove the cache dir.
	remove(cache_dir);
	kv_destroy(next_redraw_times);
	if (images) {
		// Delete all images.
		gr_delete_all_images();
		// Destroy the data structures.
		kh_destroy(id2image, images);
		images = NULL;
	}
}

////////////////////////////////////////////////////////////////////////////////
// Dumping, debugging, and image preview.
////////////////////////////////////////////////////////////////////////////////

/// Returns a string containing a time difference in a human-readable format.
/// Uses a static buffer, so be careful.
static const char *gr_ago(Milliseconds diff) {
	static char result[32];
	double seconds = (double)diff / 1000.0;
	if (seconds < 1)
		snprintf(result, sizeof(result), "%.2f sec ago", seconds);
	else if (seconds < 60)
		snprintf(result, sizeof(result), "%d sec ago", (int)seconds);
	else if (seconds < 3600)
		snprintf(result, sizeof(result), "%d min %d sec ago",
			 (int)(seconds / 60), (int)(seconds) % 60);
	else {
		snprintf(result, sizeof(result), "%d hr %d min %d sec ago",
			 (int)(seconds / 3600), (int)(seconds) % 3600 / 60,
			 (int)(seconds) % 60);
	}
	return result;
}

/// Prints to `file` with an indentation of `ind` spaces.
static void fprintf_ind(FILE *file, int ind, const char *format, ...) {
	fprintf(file, "%*s", ind, "");
	va_list args;
	va_start(args, format);
	vfprintf(file, format, args);
	va_end(args);
}

/// Dumps the image info to `file` with an indentation of `ind` spaces.
static void gr_dump_image_info(FILE *file, Image *img, int ind) {
	if (!img) {
		fprintf_ind(file, ind, "Image is NULL\n");
		return;
	}
	Milliseconds now = gr_now_ms();
	fprintf_ind(file, ind, "Image %u\n", img->image_id);
	ind += 4;
	fprintf_ind(file, ind, "number: %u\n", img->image_number);
	fprintf_ind(file, ind, "global command index: %lu\n",
		img->global_command_index);
	fprintf_ind(file, ind, "accessed: %ld  %s\n", img->atime,
		    gr_ago(now - img->atime));
	fprintf_ind(file, ind, "pix size: %ux%u\n", img->pix_width,
		    img->pix_height);
	fprintf_ind(file, ind, "cur frame start time: %ld  %s\n",
		    img->current_frame_time,
		    gr_ago(now - img->current_frame_time));
	if (img->next_redraw)
		fprintf_ind(file, ind, "next redraw: %ld  in %ld ms\n",
			    img->next_redraw, img->next_redraw - now);
	fprintf_ind(file, ind, "total disk size: %u KiB\n",
		img->total_disk_size / 1024);
	fprintf_ind(file, ind, "total duration: %d\n", img->total_duration);
	fprintf_ind(file, ind, "frames: %d\n", gr_last_frame_index(img));
	fprintf_ind(file, ind, "cur frame: %d\n", img->current_frame);
	fprintf_ind(file, ind, "animation state: %d\n", img->animation_state);
	fprintf_ind(file, ind, "default_placement: %u\n",
		    img->default_placement);
}

/// Dumps the frame info to `file` with an indentation of `ind` spaces.
static void gr_dump_frame_info(FILE *file, ImageFrame *frame, int ind) {
	if (!frame) {
		fprintf_ind(file, ind, "Frame is NULL\n");
		return;
	}
	Milliseconds now = gr_now_ms();
	fprintf_ind(file, ind, "Frame %d\n", frame->index);
	ind += 4;
	if (frame->index == 0) {
		fprintf_ind(file, ind, "NOT INITIALIZED\n");
		return;
	}
	if (frame->uploading_failure)
		fprintf_ind(file, ind, "uploading failure: %s\n",
			    image_uploading_failure_strings
				    [frame->uploading_failure]);
	fprintf_ind(file, ind, "gap: %d\n", frame->gap);
	fprintf_ind(file, ind, "accessed: %ld  %s\n", frame->atime,
		    gr_ago(now - frame->atime));
	fprintf_ind(file, ind, "data pix size: %ux%u\n", frame->data_pix_width,
		    frame->data_pix_height);
	char filename[MAX_FILENAME_SIZE];
	gr_get_frame_filename(frame, filename, MAX_FILENAME_SIZE);
	if (access(filename, F_OK) != -1)
		fprintf_ind(file, ind, "file: %s\n",
			    sanitized_filename(filename));
	else
		fprintf_ind(file, ind, "not on disk\n");
	fprintf_ind(file, ind, "disk size: %u KiB\n", frame->disk_size / 1024);
	if (frame->imlib_object) {
		unsigned ram_size = gr_frame_current_ram_size(frame);
		fprintf_ind(file, ind,
			    "loaded into ram, size: %d "
			    "KiB\n",
			    ram_size / 1024);
	} else {
		fprintf_ind(file, ind, "not loaded into ram\n");
	}
}

/// Dumps the placement info to `file` with an indentation of `ind` spaces.
static void gr_dump_placement_info(FILE *file, ImagePlacement *placement,
				   int ind) {
	if (!placement) {
		fprintf_ind(file, ind, "Placement is NULL\n");
		return;
	}
	Milliseconds now = gr_now_ms();
	fprintf_ind(file, ind, "Placement %u\n", placement->placement_id);
	ind += 4;
	fprintf_ind(file, ind, "accessed: %ld  %s\n", placement->atime,
		    gr_ago(now - placement->atime));
	fprintf_ind(file, ind, "scale_mode: %u\n", placement->scale_mode);
	fprintf_ind(file, ind, "size: %u cols x %u rows\n", placement->cols,
		    placement->rows);
	fprintf_ind(file, ind, "cell size: %ux%u\n", placement->scaled_cw,
		    placement->scaled_ch);
	fprintf_ind(file, ind, "ram per frame: %u KiB\n",
		    gr_placement_single_frame_ram_size(placement) / 1024);
	unsigned ram_size = gr_placement_current_ram_size(placement);
	fprintf_ind(file, ind, "ram size: %d KiB\n", ram_size / 1024);
}

/// Dumps placement pixmaps to `file` with an indentation of `ind` spaces.
static void gr_dump_placement_pixmaps(FILE *file, ImagePlacement *placement,
				      int ind) {
	if (!placement)
		return;
	int frameidx = 1;
	foreach_pixmap(*placement, pixmap, {
		fprintf_ind(file, ind, "Frame %d pixmap %lu\n", frameidx,
			    pixmap);
		++frameidx;
	});
}

/// Dumps the internal state (images and placements) to stderr.
void gr_dump_state() {
	FILE *file = stderr;
	int ind = 0;
	fprintf_ind(file, ind, "======= Graphics module state dump =======\n");
	fprintf_ind(file, ind,
		"sizeof(Image) = %lu  sizeof(ImageFrame) = %lu  "
		"sizeof(ImagePlacement) = %lu\n",
		sizeof(Image), sizeof(ImageFrame), sizeof(ImagePlacement));
	fprintf_ind(file, ind, "Image count: %u\n", kh_size(images));
	fprintf_ind(file, ind, "Placement count: %u\n", total_placement_count);
	fprintf_ind(file, ind, "Estimated RAM usage: %ld KiB\n",
		images_ram_size / 1024);
	fprintf_ind(file, ind, "Estimated Disk usage: %ld KiB\n",
		images_disk_size / 1024);

	Milliseconds now = gr_now_ms();

	int64_t images_ram_size_computed = 0;
	int64_t images_disk_size_computed = 0;

	Image *img = NULL;
	ImagePlacement *placement = NULL;
	kh_foreach_value(images, img, {
		fprintf_ind(file, ind, "----------------\n");
		gr_dump_image_info(file, img, 0);
		int64_t total_disk_size_computed = 0;
		int total_duration_computed = 0;
		foreach_frame(*img, frame, {
			gr_dump_frame_info(file, frame, 4);
			if (frame->image != img)
				fprintf_ind(file, 8,
					    "ERROR: WRONG IMAGE POINTER\n");
			total_duration_computed += frame->gap;
			images_disk_size_computed += frame->disk_size;
			total_disk_size_computed += frame->disk_size;
			if (frame->imlib_object)
				images_ram_size_computed +=
					gr_frame_current_ram_size(frame);
		});
		if (img->total_disk_size != total_disk_size_computed) {
			fprintf_ind(file, ind,
				"    ERROR: total_disk_size is %u, but "
				"computed value is %ld\n",
				img->total_disk_size, total_disk_size_computed);
		}
		if (img->total_duration != total_duration_computed) {
			fprintf_ind(file, ind,
				"    ERROR: total_duration is %d, but computed "
				"value is %d\n",
				img->total_duration, total_duration_computed);
		}
		kh_foreach_value(img->placements, placement, {
			gr_dump_placement_info(file, placement, 4);
			if (placement->image != img)
				fprintf_ind(file, 8,
					    "ERROR: WRONG IMAGE POINTER\n");
			fprintf_ind(file, 8,
				    "Pixmaps:\n");
			gr_dump_placement_pixmaps(file, placement, 12);
			unsigned ram_size =
				gr_placement_current_ram_size(placement);
			images_ram_size_computed += ram_size;
		});
	});
	if (images_ram_size != images_ram_size_computed) {
		fprintf_ind(file, ind,
			"ERROR: images_ram_size is %ld, but computed value "
			"is %ld\n",
			images_ram_size, images_ram_size_computed);
	}
	if (images_disk_size != images_disk_size_computed) {
		fprintf_ind(file, ind,
			"ERROR: images_disk_size is %ld, but computed value "
			"is %ld\n",
			images_disk_size, images_disk_size_computed);
	}
	fprintf_ind(file, ind, "===========================================\n");
}

/// Executes `command` with the name of the file corresponding to `image_id` as
/// the argument. Executes xmessage with an error message on failure.
// TODO: Currently we do this for the first frame only. Not sure what to do with
//       animations.
void gr_preview_image(uint32_t image_id, const char *exec) {
	char command[256];
	size_t len;
	Image *img = gr_find_image(image_id);
	if (img) {
		ImageFrame *frame = &img->first_frame;
		char filename[MAX_FILENAME_SIZE];
		gr_get_frame_filename(frame, filename, MAX_FILENAME_SIZE);
		if (frame->disk_size == 0) {
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

/// Executes `<st> -e less <file>` where <file> is the name of a temporary file
/// containing the information about an image and placement, and <st> is
/// specified with `st_executable`.
void gr_show_image_info(uint32_t image_id, uint32_t placement_id,
			uint32_t imgcol, uint32_t imgrow,
			char is_classic_placeholder, int32_t diacritic_count,
			char *st_executable) {
	char filename[MAX_FILENAME_SIZE];
	snprintf(filename, sizeof(filename), "%s/info-%u", cache_dir, image_id);
	FILE *file = fopen(filename, "w");
	if (!file) {
		perror("fopen");
		return;
	}
	// Basic information about the cell.
	fprintf(file, "image_id = %u = 0x%08X\n", image_id, image_id);
	fprintf(file, "placement_id = %u = 0x%08X\n", placement_id, placement_id);
	fprintf(file, "column = %d, row = %d\n", imgcol, imgrow);
	fprintf(file, "classic/unicode placeholder = %s\n",
		is_classic_placeholder ? "classic" : "unicode");
	fprintf(file, "original diacritic count = %d\n", diacritic_count);
	// Information about the image and the placement.
	Image *img = gr_find_image(image_id);
	ImagePlacement *placement = gr_find_placement(img, placement_id);
	gr_dump_image_info(file, img, 0);
	gr_dump_placement_info(file, placement, 0);
	if (img) {
		fprintf(file, "Frames:\n");
		foreach_frame(*img, frame, {
			gr_dump_frame_info(file, frame, 4);
		});
	}
	if (placement) {
		fprintf(file, "Placement pixmaps:\n");
		gr_dump_placement_pixmaps(file, placement, 4);
	}
	fclose(file);
	char *argv[] = {st_executable, "-e", "less", filename, NULL};
	if (posix_spawnp(NULL, st_executable, NULL, NULL, argv, environ) != 0) {
		perror("posix_spawnp");
		return;
	}
}

////////////////////////////////////////////////////////////////////////////////
// Appending and displaying image rectangles.
////////////////////////////////////////////////////////////////////////////////

/// Displays debug information in the rectangle using colors col1 and col2.
static void gr_displayinfo(Drawable buf, ImageRect *rect, int col1, int col2,
			   const char *message) {
	int w_pix = (rect->img_end_col - rect->img_start_col) * rect->cw;
	int h_pix = (rect->img_end_row - rect->img_start_row) * rect->ch;
	Display *disp = imlib_context_get_display();
	GC gc = XCreateGC(disp, buf, 0, NULL);
	char info[MAX_INFO_LEN];
	if (rect->placement_id)
		snprintf(info, MAX_INFO_LEN, "%s%u/%u [%d:%d)x[%d:%d)", message,
			 rect->image_id, rect->placement_id,
			 rect->img_start_col, rect->img_end_col,
			 rect->img_start_row, rect->img_end_row);
	else
		snprintf(info, MAX_INFO_LEN, "%s%u [%d:%d)x[%d:%d)", message,
			 rect->image_id, rect->img_start_col, rect->img_end_col,
			 rect->img_start_row, rect->img_end_row);
	XSetForeground(disp, gc, col1);
	XDrawString(disp, buf, gc, rect->screen_x_pix + 4,
		    rect->screen_y_pix + h_pix - 3, info, strlen(info));
	XSetForeground(disp, gc, col2);
	XDrawString(disp, buf, gc, rect->screen_x_pix + 2,
		    rect->screen_y_pix + h_pix - 5, info, strlen(info));
	XFreeGC(disp, gc);
}

/// Draws a rectangle (bounding box) for debugging.
static void gr_showrect(Drawable buf, ImageRect *rect) {
	int w_pix = (rect->img_end_col - rect->img_start_col) * rect->cw;
	int h_pix = (rect->img_end_row - rect->img_start_row) * rect->ch;
	Display *disp = imlib_context_get_display();
	GC gc = XCreateGC(disp, buf, 0, NULL);
	XSetForeground(disp, gc, 0xFF00FF00);
	XDrawRectangle(disp, buf, gc, rect->screen_x_pix, rect->screen_y_pix,
		       w_pix - 1, h_pix - 1);
	XSetForeground(disp, gc, 0xFFFF0000);
	XDrawRectangle(disp, buf, gc, rect->screen_x_pix + 1,
		       rect->screen_y_pix + 1, w_pix - 3, h_pix - 3);
	XFreeGC(disp, gc);
}

/// Updates the next redraw time for the given row. Resizes the
/// next_redraw_times array if needed.
static void gr_update_next_redraw_time(int row, Milliseconds next_redraw) {
	if (next_redraw == 0)
		return;
	if (row >= kv_size(next_redraw_times)) {
		size_t old_size = kv_size(next_redraw_times);
		kv_a(Milliseconds, next_redraw_times, row);
		for (size_t i = old_size; i <= row; ++i)
			kv_A(next_redraw_times, i) = 0;
	}
	Milliseconds old_value = kv_A(next_redraw_times, row);
	if (old_value == 0 || old_value > next_redraw)
		kv_A(next_redraw_times, row) = next_redraw;
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
			gr_displayinfo(buf, rect, 0xFF000000, 0xFFFFFFFF, "");
		return;
	}

	Image *img = placement->image;

	if (img->last_redraw < drawing_start_time) {
		// This is the first time we draw this image in this redraw
		// cycle. Update the frame index we are going to display. Note
		// that currently all image placements are synchronized.
		int old_frame = img->current_frame;
		gr_update_frame_index(img, drawing_start_time);
		img->last_redraw = drawing_start_time;
	}

	// Adjust next redraw times for the rows of this image rect.
	if (img->next_redraw) {
		for (int row = rect->screen_y_row;
		     row <= rect->screen_y_row + rect->img_end_row -
					  rect->img_start_row - 1; ++row) {
			gr_update_next_redraw_time(
				row, img->next_redraw);
		}
	}

	// Load the frame.
	Pixmap pixmap = gr_load_pixmap(placement, img->current_frame, rect->cw,
				       rect->ch);

	// If the image couldn't be loaded, display the bounding box.
	if (!pixmap) {
		gr_showrect(buf, rect);
		if (graphics_debug_mode == GRAPHICS_DEBUG_LOG_AND_BOXES)
			gr_displayinfo(buf, rect, 0xFF000000, 0xFFFFFFFF, "");
		return;
	}

	int src_x = rect->img_start_col * rect->cw;
	int src_y = rect->img_start_row * rect->ch;
	int width = (rect->img_end_col - rect->img_start_col) * rect->cw;
	int height = (rect->img_end_row - rect->img_start_row) * rect->ch;
	int dst_x = rect->screen_x_pix;
	int dst_y = rect->screen_y_pix;

	// Display the image.
	Display *disp = imlib_context_get_display();
	Visual *vis = imlib_context_get_visual();

	// Create an xrender picture for the window.
	XRenderPictFormat *win_format =
		XRenderFindVisualFormat(disp, vis);
	Picture window_pic =
		XRenderCreatePicture(disp, buf, win_format, 0, NULL);

	// If needed, invert the image pixmap. Note that this naive approach of
	// inverting the pixmap is not entirely correct, because the pixmap is
	// premultiplied. But the result is good enough to visually indicate
	// selection.
	if (rect->reverse) {
		unsigned pixmap_w =
			(unsigned)placement->cols * placement->scaled_cw;
		unsigned pixmap_h =
			(unsigned)placement->rows * placement->scaled_ch;
		Pixmap invpixmap =
			XCreatePixmap(disp, buf, pixmap_w, pixmap_h, 32);
		XGCValues gcv = {.function = GXcopyInverted};
		GC gc = XCreateGC(disp, invpixmap, GCFunction, &gcv);
		XCopyArea(disp, pixmap, invpixmap, gc, 0, 0, pixmap_w,
			  pixmap_h, 0, 0);
		XFreeGC(disp, gc);
		pixmap = invpixmap;
	}

	// Create a picture for the image pixmap.
	XRenderPictFormat *pic_format =
		XRenderFindStandardFormat(disp, PictStandardARGB32);
	Picture pixmap_pic =
		XRenderCreatePicture(disp, pixmap, pic_format, 0, NULL);

	// Composite the image onto the window. In the reverse mode we ignore
	// the alpha channel of the image because the naive inversion above
	// seems to invert the alpha channel as well.
	int pictop = rect->reverse ? PictOpSrc : PictOpOver;
	XRenderComposite(disp, pictop, pixmap_pic, 0, window_pic,
			 src_x, src_y, src_x, src_y, dst_x, dst_y, width,
			 height);

	// Free resources
	XRenderFreePicture(disp, pixmap_pic);
	XRenderFreePicture(disp, window_pic);
	if (rect->reverse)
		XFreePixmap(disp, pixmap);

	// In debug mode always draw bounding boxes and print info.
	if (graphics_debug_mode == GRAPHICS_DEBUG_LOG_AND_BOXES) {
		gr_showrect(buf, rect);
		gr_displayinfo(buf, rect, 0xFF000000, 0xFFFFFFFF, "");
	}
}

/// Removes the given image rectangle.
static void gr_freerect(ImageRect *rect) { memset(rect, 0, sizeof(ImageRect)); }

/// Returns the bottom coordinate of the rect.
static int gr_getrectbottom(ImageRect *rect) {
	return rect->screen_y_pix +
	       (rect->img_end_row - rect->img_start_row) * rect->ch;
}

/// Prepare for image drawing. `cw` and `ch` are dimensions of the cell.
void gr_start_drawing(Drawable buf, int cw, int ch) {
	current_cw = cw;
	current_ch = ch;
	this_redraw_cycle_loaded_files = 0;
	this_redraw_cycle_loaded_pixmaps = 0;
	drawing_start_time = gr_now_ms();
	imlib_context_set_drawable(buf);
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

	// Compute the delay until the next redraw as the minimum of the next
	// redraw delays for all rows.
	Milliseconds drawing_end_time = gr_now_ms();
	graphics_next_redraw_delay = INT_MAX;
	for (int row = 0; row < kv_size(next_redraw_times); ++row) {
		Milliseconds row_next_redraw = kv_A(next_redraw_times, row);
		if (row_next_redraw > 0) {
			int delay = MAX(graphics_animation_min_delay,
					row_next_redraw - drawing_end_time);
			graphics_next_redraw_delay =
				MIN(graphics_next_redraw_delay, delay);
		}
	}

	// In debug mode display additional info.
	if (graphics_debug_mode) {
		int milliseconds = drawing_end_time - drawing_start_time;

		Display *disp = imlib_context_get_display();
		GC gc = XCreateGC(disp, buf, 0, NULL);
		const char *debug_mode_str =
			graphics_debug_mode == GRAPHICS_DEBUG_LOG_AND_BOXES
				? "(boxes shown) "
				: "";
		int redraw_delay = graphics_next_redraw_delay == INT_MAX
					   ? -1
					   : graphics_next_redraw_delay;
		char info[MAX_INFO_LEN];
		snprintf(info, MAX_INFO_LEN,
			 "%sRender time: %d ms  ram %ld K  disk %ld K  count "
			 "%d  cell %dx%d  delay %d",
			 debug_mode_str, milliseconds, images_ram_size / 1024,
			 images_disk_size / 1024, kh_size(images), current_cw,
			 current_ch, redraw_delay);
		XSetForeground(disp, gc, 0xFF000000);
		XFillRectangle(disp, buf, gc, 0, 0, 600, 16);
		XSetForeground(disp, gc, 0xFFFFFFFF);
		XDrawString(disp, buf, gc, 0, 14, info, strlen(info));
		XFreeGC(disp, gc);

		if (milliseconds > 0) {
			fprintf(stderr, "%s  (loaded %d files, %d pixmaps)\n",
				info, this_redraw_cycle_loaded_files,
				this_redraw_cycle_loaded_pixmaps);
		}
	}

	// Check the limits in case we have used too much ram for placements.
	gr_check_limits();
}

// Add an image rectangle to the list of rectangles to draw.
void gr_append_imagerect(Drawable buf, uint32_t image_id, uint32_t placement_id,
			 int img_start_col, int img_end_col, int img_start_row,
			 int img_end_row, int x_col, int y_row, int x_pix,
			 int y_pix, int cw, int ch, int reverse) {
	current_cw = cw;
	current_ch = ch;

	ImageRect new_rect;
	new_rect.image_id = image_id;
	new_rect.placement_id = placement_id;
	new_rect.img_start_col = img_start_col;
	new_rect.img_end_col = img_end_col;
	new_rect.img_start_row = img_start_row;
	new_rect.img_end_row = img_end_row;
	new_rect.screen_y_row = y_row;
	new_rect.screen_x_pix = x_pix;
	new_rect.screen_y_pix = y_pix;
	new_rect.ch = ch;
	new_rect.cw = cw;
	new_rect.reverse = reverse;

	// Display some red text in debug mode.
	if (graphics_debug_mode == GRAPHICS_DEBUG_LOG_AND_BOXES)
		gr_displayinfo(buf, &new_rect, 0xFF000000, 0xFFFF0000, "? ");

	// If it's the empty image (image_id=0) or an empty rectangle, do
	// nothing.
	if (image_id == 0 || img_end_col - img_start_col <= 0 ||
	    img_end_row - img_start_row <= 0)
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
		if (rect->img_end_row == img_start_row &&
		    gr_getrectbottom(rect) == y_pix) {
			if (rect->img_start_col == img_start_col &&
			    rect->img_end_col == img_end_col &&
			    rect->screen_x_pix == x_pix) {
				rect->img_end_row = img_end_row;
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

/// Mark rows containing animations as dirty if it's time to redraw them. Must
/// be called right after `gr_start_drawing`.
void gr_mark_dirty_animations(int *dirty, int rows) {
	if (rows < kv_size(next_redraw_times))
		kv_size(next_redraw_times) = rows;
	if (rows * 2 < kv_max(next_redraw_times))
		kv_resize(Milliseconds, next_redraw_times, rows);
	for (int i = 0; i < MIN(rows, kv_size(next_redraw_times)); ++i) {
		if (dirty[i]) {
			kv_A(next_redraw_times, i) = 0;
			continue;
		}
		Milliseconds next_update = kv_A(next_redraw_times, i);
		if (next_update > 0 && next_update <= drawing_start_time) {
			dirty[i] = 1;
			kv_A(next_redraw_times, i) = 0;
		}
	}
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
	/// 'a=', may be 't', 'q', 'f', 'T', 'p', 'd', 'a'.
	char action;
	/// 'q=', 1 to suppress OK response, 2 to suppress errors too.
	int quiet;
	/// 'f=', use 24 or 32 for raw pixel data, 100 to autodetect with
	/// imlib2. If 'f=0', will try to load with imlib2, then fallback to
	/// 32-bit pixel data.
	int format;
	/// 'o=', may be 'z' for RFC 1950 ZLIB.
	int compression;
	/// 't=', may be 'f', 't' or 'd'.
	char transmission_medium;
	/// 'd='
	char delete_specifier;
	/// 's=', 'v=', if 'a=t' or 'a=T', used only when 'f=24' or 'f=32'.
	/// When 'a=f', this is the size of the frame rectangle when composed on
	/// top of another frame.
	int frame_pix_width, frame_pix_height;
	/// 'x=', 'y=' - top-left corner of the source rectangle.
	int src_pix_x, src_pix_y;
	/// 'w=', 'h=' - width and height of the source rectangle.
	int src_pix_width, src_pix_height;
	/// 'r=', 'c='
	int rows, columns;
	/// 'i='
	uint32_t image_id;
	/// 'I='
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
	// ---------------------------------------------------------------------
	// Animation-related fields. Their keys often overlap with keys of other
	// commands, so these make sense only if the action is 'a=f' (frame
	// transmission) or 'a=a' (animation control).
	//
	// 'x=' and 'y=', the relative position of the frame image when it's
	// composed on top of another frame.
	int frame_dst_pix_x, frame_dst_pix_y;
	/// 'X=', 'X=1' to replace colors instead of alpha blending on top of
	/// the background color or frame.
	char replace_instead_of_blending;
	/// 'Y=', the background color in the 0xRRGGBBAA format (still
	/// transmitted as a decimal number).
	uint32_t background_color;
	/// (Only for 'a=f'). 'c=', the 1-based index of the background frame.
	int background_frame;
	/// (Only for 'a=a'). 'c=', sets the index of the current frame.
	int current_frame;
	/// 'r=', the 1-based index of the frame to edit.
	int edit_frame;
	/// 'z=', the duration of the frame. Zero if not specified, negative if
	/// the frame is gapless (i.e. skipped).
	int gap;
	/// (Only for 'a=a'). 's=', if non-zero, sets the state of the
	/// animation, 1 to stop, 2 to run in loading mode, 3 to loop.
	int animation_state;
	/// (Only for 'a=a'). 'v=', if non-zero, sets the number of times the
	/// animation will loop. 1 to loop infinitely, N to loop N-1 times.
	int loops;
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
static void gr_reportsuccess_frame(ImageFrame *frame) {
	uint32_t id = frame->image->query_id ? frame->image->query_id
					     : frame->image->image_id;
	if (frame->quiet < 1)
		gr_createresponse(id, frame->image->image_number,
				  frame->image->initial_placement_id, "OK");
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
static void gr_reporterror_frame(ImageFrame *frame, const char *format, ...) {
	char errmsg[MAX_GRAPHICS_RESPONSE_LEN];
	graphics_command_result.error = 1;
	va_list args;
	va_start(args, format);
	vsnprintf(errmsg, MAX_GRAPHICS_RESPONSE_LEN, format, args);
	va_end(args);

	if (!frame) {
		fprintf(stderr, "%s\n", errmsg);
		gr_createresponse(0, 0, 0, errmsg);
	} else {
		uint32_t id = frame->image->query_id ? frame->image->query_id
						     : frame->image->image_id;
		fprintf(stderr, "%s  id=%u\n", errmsg, id);
		if (frame->quiet < 2)
			gr_createresponse(id, frame->image->image_number,
					  frame->image->initial_placement_id,
					  errmsg);
	}
}

/// Loads an image and creates a success/failure response. Returns `frame`, or
/// NULL if it's a query action and the image was deleted.
static ImageFrame *gr_loadimage_and_report(ImageFrame *frame) {
	gr_load_imlib_object(frame);
	if (!frame->imlib_object) {
		gr_reporterror_frame(frame, "EBADF: could not load image");
	} else {
		gr_reportsuccess_frame(frame);
	}
	// If it was a query action, discard the image.
	if (frame->image->query_id) {
		gr_delete_image(frame->image);
		return NULL;
	}
	return frame;
}

/// Creates an appropriate uploading failure response to the current command.
static void gr_reportuploaderror(ImageFrame *frame) {
	switch (frame->uploading_failure) {
	case 0:
		return;
	case ERROR_CANNOT_OPEN_CACHED_FILE:
		gr_reporterror_frame(frame,
				   "EIO: could not create a file for image");
		break;
	case ERROR_OVER_SIZE_LIMIT:
		gr_reporterror_frame(
			frame,
			"EFBIG: the size of the uploaded image exceeded "
			"the image size limit %u",
			graphics_max_single_image_file_size);
		break;
	case ERROR_UNEXPECTED_SIZE:
		gr_reporterror_frame(frame,
				   "EINVAL: the size of the uploaded image %u "
				   "doesn't match the expected size %u",
				   frame->disk_size, frame->expected_size);
		break;
	};
}

/// Displays a non-virtual placement. This functions records the information in
/// `graphics_command_result`, the placeholder itself is created by the terminal
/// after handling the current command in the graphics module.
static void gr_display_nonvirtual_placement(ImagePlacement *placement) {
	if (placement->virtual)
		return;
	if (placement->image->first_frame.status < STATUS_RAM_LOADING_SUCCESS)
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

/// Marks the rows that are occupied by the image as dirty.
static void gr_schedule_image_redraw(Image *img) {
	if (!img)
		return;
	gr_schedule_image_redraw_by_id(img->image_id);
}

/// Appends data from `payload` to the frame `frame` when using direct
/// transmission. Note that we report errors only for the final command
/// (`!more`) to avoid spamming the client. If the frame is not specified, use
/// the image id and frame index we are currently uploading.
static void gr_append_data(ImageFrame *frame, const char *payload, int more) {
	if (!frame) {
		Image *img = gr_find_image(current_upload_image_id);
		frame = gr_get_frame(img, current_upload_frame_index);
		GR_LOG("Appending data to image %u frame %d\n",
		       current_upload_image_id, current_upload_frame_index);
		if (!img)
			GR_LOG("ERROR: this image doesn't exist\n");
		if (!frame)
			GR_LOG("ERROR: this frame doesn't exist\n");
	}
	if (!more) {
		current_upload_image_id = 0;
		current_upload_frame_index = 0;
	}
	if (!frame) {
		if (!more)
			gr_reporterror_frame(NULL, "ENOENT: could not find the "
						   "image to append data to");
		return;
	}
	if (frame->status != STATUS_UPLOADING) {
		if (!more)
			gr_reportuploaderror(frame);
		return;
	}

	// Decode the data.
	size_t data_size = 0;
	char *data = gr_base64dec(payload, &data_size);

	GR_LOG("appending %u + %zu = %zu bytes\n", frame->disk_size, data_size,
	       frame->disk_size + data_size);

	// Do not append this data if the image exceeds the size limit.
	if (frame->disk_size + data_size >
		    graphics_max_single_image_file_size ||
	    frame->expected_size > graphics_max_single_image_file_size) {
		free(data);
		gr_delete_imagefile(frame);
		frame->uploading_failure = ERROR_OVER_SIZE_LIMIT;
		if (!more)
			gr_reportuploaderror(frame);
		return;
	}

	// If there is no open file corresponding to the image, create it.
	if (!frame->open_file) {
		gr_make_sure_tmpdir_exists();
		char filename[MAX_FILENAME_SIZE];
		gr_get_frame_filename(frame, filename, MAX_FILENAME_SIZE);
		FILE *file = fopen(filename, frame->disk_size ? "a" : "w");
		if (!file) {
			frame->status = STATUS_UPLOADING_ERROR;
			frame->uploading_failure = ERROR_CANNOT_OPEN_CACHED_FILE;
			if (!more)
				gr_reportuploaderror(frame);
			return;
		}
		frame->open_file = file;
	}

	// Write data to the file and update disk size variables.
	fwrite(data, 1, data_size, frame->open_file);
	free(data);
	frame->disk_size += data_size;
	frame->image->total_disk_size += data_size;
	images_disk_size += data_size;
	gr_touch_frame(frame);

	if (more) {
		current_upload_image_id = frame->image->image_id;
		current_upload_frame_index = frame->index;
	} else {
		current_upload_image_id = 0;
		current_upload_frame_index = 0;
		// Close the file.
		if (frame->open_file) {
			fclose(frame->open_file);
			frame->open_file = NULL;
		}
		frame->status = STATUS_UPLOADING_SUCCESS;
		uint32_t placement_id = frame->image->default_placement;
		if (frame->expected_size &&
		    frame->expected_size != frame->disk_size) {
			// Report failure if the uploaded image size doesn't
			// match the expected size.
			frame->status = STATUS_UPLOADING_ERROR;
			frame->uploading_failure = ERROR_UNEXPECTED_SIZE;
			gr_reportuploaderror(frame);
		} else {
			// Make sure to redraw all existing image instances.
			gr_schedule_image_redraw(frame->image);
			// Try to load the image into ram and report the result.
			frame = gr_loadimage_and_report(frame);
			// If there is a non-virtual image placement, we may
			// need to display it.
			if (frame && frame->index == 1) {
				Image *img = frame->image;
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

/// Creates a new image or a new frame in an existing image (depending on the
/// command's action) and initializes its parameters from the command.
static ImageFrame *gr_new_image_or_frame_from_command(GraphicsCommand *cmd) {
	if (cmd->format != 0 && cmd->format != 32 && cmd->format != 24 &&
	    cmd->compression != 0) {
		gr_reporterror_cmd(cmd, "EINVAL: compression is supported only "
					"for raw pixel data (f=32 or f=24)");
		// Even though we report an error, we still create an image.
	}

	Image *img = NULL;
	if (cmd->action == 'f') {
		// If it's a frame transmission action, there must be an
		// existing image.
		img = gr_find_image_for_command(cmd);
		if (!img) {
			gr_reporterror_cmd(cmd, "ENOENT: image not found");
			return NULL;
		}
	} else {
		// Otherwise create a new image object. If the action is `q`,
		// we'll use random id instead of the one specified in the
		// command.
		uint32_t image_id = cmd->action == 'q' ? 0 : cmd->image_id;
		img = gr_new_image(image_id);
		if (!img)
			return NULL;
		if (cmd->action == 'q')
			img->query_id = cmd->image_id;
		else if (!cmd->image_id)
			cmd->image_id = img->image_id;
		// Set the image number.
		img->image_number = cmd->image_number;
	}

	ImageFrame *frame = gr_append_new_frame(img);
	// Initialize the frame.
	frame->expected_size = cmd->size;
	frame->format = cmd->format;
	frame->compression = cmd->compression;
	frame->background_color = cmd->background_color;
	frame->background_frame_index = cmd->background_frame;
	frame->gap = cmd->gap;
	img->total_duration += frame->gap;
	frame->blend = !cmd->replace_instead_of_blending;
	frame->data_pix_width = cmd->frame_pix_width;
	frame->data_pix_height = cmd->frame_pix_height;
	if (cmd->action == 'f') {
		frame->x = cmd->frame_dst_pix_x;
		frame->y = cmd->frame_dst_pix_y;
	}
	// We save the quietness information in the frame because for direct
	// transmission subsequent transmission command won't contain this info.
	frame->quiet = cmd->quiet;
	return frame;
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
static ImageFrame *gr_handle_transmit_command(GraphicsCommand *cmd) {
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

	ImageFrame *frame = NULL;
	if (cmd->transmission_medium == 'f' ||
	    cmd->transmission_medium == 't') {
		// File transmission.
		// Create a new image or a new frame of an existing image.
		frame = gr_new_image_or_frame_from_command(cmd);
		if (!frame)
			return NULL;
		last_image_id = frame->image->image_id;
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
			frame->status = STATUS_UPLOADING_ERROR;
			frame->uploading_failure = ERROR_CANNOT_COPY_FILE;
		} else {
			gr_make_sure_tmpdir_exists();
			// Build the filename for the cached copy of the file.
			char cache_filename[MAX_FILENAME_SIZE];
			gr_get_frame_filename(frame, cache_filename,
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
				frame->status = STATUS_UPLOADING_ERROR;
				frame->uploading_failure = ERROR_CANNOT_COPY_FILE;
			} else {
				// Get the file size of the copied file.
				frame->status = STATUS_UPLOADING_SUCCESS;
				frame->disk_size = st.st_size;
				frame->image->total_disk_size += st.st_size;
				images_disk_size += frame->disk_size;
				if (frame->expected_size &&
				    frame->expected_size != frame->disk_size) {
					// The file has unexpected size.
					frame->status = STATUS_UPLOADING_ERROR;
					frame->uploading_failure =
						ERROR_UNEXPECTED_SIZE;
					gr_reportuploaderror(frame);
				} else {
					// Everything seems fine, try to load
					// and redraw existing instances.
					gr_schedule_image_redraw(frame->image);
					frame = gr_loadimage_and_report(frame);
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
		frame = gr_get_last_frame(gr_find_image_for_command(cmd));
		if (frame && frame->status == STATUS_UPLOADING) {
			// This is a continuation of the previous transmission.
			cmd->is_direct_transmission_continuation = 1;
			gr_append_data(frame, cmd->payload, cmd->more);
			return frame;
		}
		// If no action is specified, it's not the first transmission
		// command. If we couldn't find the image, something went wrong
		// and we should just drop this command.
		if (cmd->action == 0)
			return NULL;
		// Otherwise create a new image or frame structure.
		frame = gr_new_image_or_frame_from_command(cmd);
		if (!frame)
			return NULL;
		last_image_id = frame->image->image_id;
		frame->status = STATUS_UPLOADING;
		// Start appending data.
		gr_append_data(frame, cmd->payload, cmd->more);
	} else {
		gr_reporterror_cmd(
			cmd,
			"EINVAL: transmission medium '%c' is not supported",
			cmd->transmission_medium);
		return NULL;
	}

	return frame;
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

	if (placement->virtual) {
		placement->scale_mode = SCALE_MODE_CONTAIN;
	} else if (placement->cols && placement->rows) {
		// For classic placements the default is to stretch the image if
		// both cols and rows are specified.
		placement->scale_mode = SCALE_MODE_FILL;
	} else if (placement->cols || placement->rows) {
		// But if only one of them is specified, the default is to
		// contain.
		placement->scale_mode = SCALE_MODE_CONTAIN;
	} else {
		// If none of them are specified, the default is to use the
		// original size.
		placement->scale_mode = SCALE_MODE_NONE;
	}

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

static void gr_handle_animation_control_command(GraphicsCommand *cmd) {
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

	// Find the frame to edit, if requested.
	ImageFrame *frame = NULL;
	if (cmd->edit_frame)
		frame = gr_get_frame(img, cmd->edit_frame);
	if (cmd->edit_frame || cmd->gap) {
		if (!frame) {
			gr_reporterror_cmd(cmd, "ENOENT: frame %d not found",
					   cmd->edit_frame);
			return;
		}
		if (cmd->gap) {
			img->total_duration -= frame->gap;
			frame->gap = cmd->gap;
			img->total_duration += frame->gap;
		}
	}

	// Set animation-related parameters of the image.
	if (cmd->current_frame)
		img->current_frame = cmd->current_frame;
	if (cmd->animation_state) {
		if (cmd->animation_state == 1) {
			img->animation_state = ANIMATION_STATE_STOPPED;
		} else if (cmd->animation_state == 2) {
			img->animation_state = ANIMATION_STATE_LOADING;
		} else if (cmd->animation_state == 3) {
			img->animation_state = ANIMATION_STATE_LOOPING;
		} else {
			gr_reporterror_cmd(
				cmd, "EINVAL: invalid animation state: %d",
				cmd->animation_state);
		}
	}
	// TODO: Set the number of loops to cmd->loops

	// Make sure we redraw all instances of the image.
	gr_schedule_image_redraw(img);
}

/// Handles a command.
static void gr_handle_command(GraphicsCommand *cmd) {
	if (!cmd->image_id && !cmd->image_number) {
		// If there is no image id or image number, nobody expects a
		// response, so set quiet to 2.
		cmd->quiet = 2;
	}
	ImageFrame *frame = NULL;
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
	case 'f':
		// Transmit data. 'q' means query, which is basically the same
		// as transmit, but the image is discarded, and the id is fake.
		// 'f' appends a frame to an existing image.
		gr_handle_transmit_command(cmd);
		break;
	case 'p':
		// Display (put) the image.
		gr_handle_put_command(cmd);
		break;
	case 'T':
		// Transmit and display.
		frame = gr_handle_transmit_command(cmd);
		if (frame && !cmd->is_direct_transmission_continuation) {
			gr_handle_put_command(cmd);
			if (cmd->placement_id)
				frame->image->initial_placement_id =
					cmd->placement_id;
		}
		break;
	case 'd':
		gr_handle_delete_command(cmd);
		break;
	case 'a':
		gr_handle_animation_control_command(cmd);
		break;
	default:
		gr_reporterror_cmd(cmd, "EINVAL: unsupported action: %c",
				   cmd->action);
		return;
	}
}

/// A partially parsed key-value pair.
typedef struct KeyAndValue {
	char *key_start;
	char *val_start;
	unsigned key_len, val_len;
} KeyAndValue;

/// Parses the value of a key and assigns it to the appropriate field of `cmd`.
static void gr_set_keyvalue(GraphicsCommand *cmd, KeyAndValue *kv) {
	char *key_start = kv->key_start;
	char *key_end = key_start + kv->key_len;
	char *value_start = kv->val_start;
	char *value_end = value_start + kv->val_len;
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
		if (cmd->action == 'a')
			cmd->animation_state = num;
		else
			cmd->frame_pix_width = num;
		break;
	case 'v':
		if (cmd->action == 'a')
			cmd->loops = num;
		else
			cmd->frame_pix_height = num;
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
		cmd->frame_dst_pix_x = num;
		break;
	case 'y':
		if (cmd->action == 'f')
			cmd->frame_dst_pix_y = num;
		else
			cmd->src_pix_y = num;
		break;
	case 'w':
		cmd->src_pix_width = num;
		break;
	case 'h':
		cmd->src_pix_height = num;
		break;
	case 'c':
		if (cmd->action == 'f')
			cmd->background_frame = num;
		else if (cmd->action == 'a')
			cmd->current_frame = num;
		else
			cmd->columns = num;
		break;
	case 'r':
		if (cmd->action == 'f' || cmd->action == 'a')
			cmd->edit_frame = num;
		else
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
		if (cmd->action == 'f')
			cmd->replace_instead_of_blending = num;
		else
			break; /*ignore*/
		break;
	case 'Y':
		if (cmd->action == 'f')
			cmd->background_color = num;
		else
			break; /*ignore*/
		break;
	case 'z':
		if (cmd->action == 'f' || cmd->action == 'a')
			cmd->gap = num;
		else
			break; /*ignore*/
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
/// at least `len + 1` characters. Returns 1 on success.
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
	// An array of partially parsed key-value pairs.
	KeyAndValue key_vals[32];
	unsigned key_vals_count = 0;
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
				if (key_vals_count >=
				    sizeof(key_vals) / sizeof(*key_vals)) {
					gr_reporterror_cmd(&cmd,
							   "EINVAL: too many "
							   "key-value pairs");
					break;
				}
				key_vals[key_vals_count].key_start = key_start;
				key_vals[key_vals_count].val_start = val_start;
				key_vals[key_vals_count].key_len =
					key_end - key_start;
				key_vals[key_vals_count].val_len =
					val_end - val_start;
				++key_vals_count;
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

	// Set the action key ('a=') first because we need it to disambiguate
	// some keys. Also set 'i=' and 'I=' for better error reporting.
	for (unsigned i = 0; i < key_vals_count; ++i) {
		if (key_vals[i].key_len == 1) {
			char *start = key_vals[i].key_start;
			if (*start == 'a' || *start == 'i' || *start == 'I') {
				gr_set_keyvalue(&cmd, &key_vals[i]);
				break;
			}
		}
	}
	// Set the rest of the keys.
	for (unsigned i = 0; i < key_vals_count; ++i)
		gr_set_keyvalue(&cmd, &key_vals[i]);

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

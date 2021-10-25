#include <stdio.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <Imlib2.h>

#include "graphics.h"

void graphicsinit(Display *disp, Visual *vis, Colormap cm)
{
	imlib_context_set_display(disp);
	imlib_context_set_visual(vis);
	imlib_context_set_colormap(cm);
	imlib_context_set_anti_alias(1);
	imlib_context_set_blend(1);
	imlib_set_cache_size(16 * 1024 * 1024);
}

Imlib_Image image = 0;

void gdrawimagestripe(Drawable buf, uint32_t image_id, int start_col, int end_col, int row,
					  int x_pix, int y_pix, int cw, int ch)
{
	fprintf(stderr, "Stripe row %d col [%d; %d]  at (%d, %d)  id %d  cw %d  ch %d\n",
			row, start_col, end_col, x_pix, y_pix, image_id, cw, ch);
	if (!image)
		image = imlib_load_image("/home/sgrechanik/temp/png/Ducati_side_shadow-fs8.png");
	if (!image)
		fprintf(stderr, "Could not load image\n");
	imlib_context_set_image(image);
	int w = imlib_image_get_width();
	int h = imlib_image_get_height();
	Imlib_Image strip = imlib_create_cropped_scaled_image(0, 0, w, h, (end_col - start_col + 1)*cw, ch);
	imlib_context_set_drawable(buf);
	imlib_context_set_image(strip);
	imlib_render_image_on_drawable(x_pix, y_pix);
	imlib_free_image();
}

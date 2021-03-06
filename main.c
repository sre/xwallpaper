/*
 * Copyright (c) 2017 Tobias Stoeckmann <tobias@stoeckmann.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <xcb/xcb.h>
#include <xcb/xcb_image.h>

#include <err.h>
#include <pixman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "functions.h"

#define ATOM_ESETROOT "ESETROOT_PMAP_ID"
#define ATOM_XSETROOT "_XROOTPMAP_ID"

static uint32_t
get_max_rows_per_request(xcb_connection_t *c, xcb_image_t *image, uint32_t n)
{
	uint32_t max_len, max_req_len, row_len, max_height;

	max_req_len = xcb_get_maximum_request_length(c);
	max_len = (max_req_len > n ? n : max_req_len) * 4;
	if (max_len <= sizeof(xcb_put_image_request_t))
		errx(1, "unable to put image on X server");
	max_len -= sizeof(xcb_put_image_request_t);
	row_len = (image->stride + image->scanline_pad - 1) &
	    -image->scanline_pad;
	max_height = max_len / row_len;
	if (max_height < 1)
		errx(1, "unable to put image on X server");
	DBG("put image request parameters:\n"
	    "maximum request length allowed for server (32 bits): %u\n"
	    "maximum length for row data: %u\n"
	    "length of rows in image: %u\n"
	    "maximum height to send: %u\n",
	    max_req_len, max_len, row_len, max_height);
	return max_height;
}

static pixman_image_t *
load_pixman_image(xcb_connection_t *c, xcb_screen_t *screen, FILE *fp)
{
	pixman_image_t *pixman_image;

	pixman_image = NULL;

#ifdef WITH_PNG
	if (pixman_image == NULL) {
		rewind(fp);
		pixman_image = load_png(fp);
	}
#endif /* WITH_PNG */
#ifdef WITH_JPEG
	if (pixman_image == NULL) {
		rewind(fp);
		pixman_image = load_jpeg(fp);
	}
#endif /* WITH_JPEG */
#ifdef WITH_XPM
	if (pixman_image == NULL) {
		rewind(fp);
		pixman_image = load_xpm(c, screen, fp);
	}
#endif /* WITH_XPM */

	return pixman_image;
}

static void
load_pixman_images(xcb_connection_t *c, xcb_screen_t *screen,
    wp_option_t *options)
{
	wp_option_t *option;
	pixman_image_t *img;

	for (option = options; option->filename != NULL; option++)
		if (option->buffer->pixman_image == NULL) {
			DBG("loading %s\n", option->filename);
			img = load_pixman_image(c, screen, option->buffer->fp);
			if (img == NULL)
				errx(1, "failed to parse %s", option->filename);
			option->buffer->pixman_image = img;
			fclose(option->buffer->fp);
			if (pixman_image_get_width(img) > UINT16_MAX ||
			    pixman_image_get_height(img) > UINT16_MAX)
				errx(1, "%s has illegal dimensions",
				    option->filename);
		}
}

static void
tile(pixman_image_t *dest, wp_output_t *output, wp_option_t *option)
{
	pixman_image_t *pixman_image;
	int pixman_width, pixman_height;
	uint16_t off_x, off_y;

	pixman_image = option->buffer->pixman_image;
	pixman_width = pixman_image_get_width(pixman_image);
	pixman_height = pixman_image_get_height(pixman_image);

	/* reset transformation and filter of transform calls */
	pixman_image_set_transform(pixman_image, NULL);
	pixman_image_set_filter(pixman_image, PIXMAN_FILTER_FAST, NULL, 0);

	/*
	 * Manually performs tiling to support separate modes per
	 * screen with RandR. If possible, xwallpaper will let
	 * X do the tiling natively.
         */
	for (off_y = 0; off_y < output->height; off_y += pixman_height) {
		uint16_t h;

		if (off_y + pixman_height > output->height)
			h = output->height - off_y;
		else
			h = pixman_height;

		for (off_x = 0; off_x < output->width; off_x += pixman_width) {
			uint16_t w;

			if (off_x + pixman_width > output->width)
				w = output->width - off_x;
			else
				w = pixman_width;

			DBG("tiling %s for %s (area %dx%d+%d+%d)\n",
			    option->filename, output->name != NULL ?
			    output->name : "screen", w, h, off_x, off_y);
			pixman_image_composite(PIXMAN_OP_CONJOINT_SRC,
			    pixman_image, NULL, dest, 0, 0, 0, 0,
			    off_x, off_y, w, h);
		}
	}
}

static void
transform(pixman_image_t *dest, wp_output_t *output, wp_option_t *option)
{
	pixman_image_t *pixman_image;
	pixman_f_transform_t ftransform;
	pixman_transform_t transform;
	pixman_filter_t filter;
	int pixman_width, pixman_height;
	uint16_t xcb_width, xcb_height;
	float w_scale, h_scale, scale;
	float translate_x, translate_y;

	pixman_image = option->buffer->pixman_image;
	pixman_width = pixman_image_get_width(pixman_image);
	pixman_height = pixman_image_get_height(pixman_image);
	xcb_width = output->width;
	xcb_height = output->height;

	w_scale = (float)pixman_width / xcb_width;
	h_scale = (float)pixman_height / xcb_height;
	filter = PIXMAN_FILTER_BEST;

	switch (option->mode) {
		case MODE_CENTER:
			filter = PIXMAN_FILTER_FAST;
			w_scale = 1;
			h_scale = 1;
			break;
		case MODE_MAXIMIZE:
			scale = w_scale < h_scale ? h_scale : w_scale;
			w_scale = scale;
			h_scale = scale;
			break;
		case MODE_ZOOM:
			scale = w_scale > h_scale ? h_scale : w_scale;
			w_scale = scale;
			h_scale = scale;
		default:
			break;
	}

	translate_x = (pixman_width / w_scale - xcb_width) / 2;
	translate_y = (pixman_height / h_scale - xcb_height) / 2;

	pixman_f_transform_init_translate(&ftransform,
	    translate_x, translate_y);
	if (option->mode != MODE_CENTER)
		pixman_f_transform_scale(&ftransform, NULL, w_scale, h_scale);
	pixman_image_set_filter(pixman_image, filter, NULL, 0);
	pixman_transform_from_pixman_f_transform(&transform, &ftransform);
	pixman_image_set_transform(pixman_image, &transform);

	DBG("composing %s for %s (area %dx%d+%d+%d) (mode %d)\n",
	    option->filename, output->name != NULL ? output->name : "screen",
	    output->width, output->height, 0, 0, option->mode);
	pixman_image_composite(PIXMAN_OP_CONJOINT_SRC, pixman_image, NULL, dest,
	    0, 0, 0, 0, 0, 0, output->width, output->height);
}

static void
put_wallpaper(xcb_connection_t *c, xcb_screen_t *screen, wp_output_t *output,
    xcb_image_t *xcb_image, xcb_pixmap_t pixmap, xcb_gcontext_t gc)
{
	uint8_t *data;
	uint32_t h, max_height, row_len, sub_height;
	xcb_image_t *sub;

	DBG("xcb image (%dx%d) to %s (%dx%d+%d+%d)\n",
	    xcb_image->width, xcb_image->height,
	    output->name != NULL ? output->name : "screen", output->width,
	    output->height, output->x, output->y);

	max_height = get_max_rows_per_request(c, xcb_image, UINT32_MAX / 4);
	if (max_height < xcb_image->height) {
		DBG("image exceeds request size limitations\n");

		/* adjust for better performance */
		max_height = get_max_rows_per_request(c, xcb_image, 65536);

		sub_height = max_height;
		sub = xcb_image_create_native(c, xcb_image->width, sub_height,
		    XCB_IMAGE_FORMAT_Z_PIXMAP, 32, NULL, ~0, NULL);
		if (sub == NULL)
			errx(1, "failed to create xcb image");
	} else {
		sub = xcb_image;
		sub_height = xcb_image->height;
	}

	row_len = (xcb_image->stride + xcb_image->scanline_pad - 1) &
	    -xcb_image->scanline_pad;

	data = xcb_image->data;
	for (h = 0; h < xcb_image->height; h += sub_height) {
		if (sub_height > xcb_image->height - h) {
			sub_height = xcb_image->height - h;
			xcb_image_destroy(sub);
			sub = xcb_image_create_native(c,
			    xcb_image->width, sub_height,
			    XCB_IMAGE_FORMAT_Z_PIXMAP, 32, NULL, ~0, NULL);
			if (sub == NULL)
				errx(1, "failed to create xcb image");
		}

		DBG("put image (%dx%d+0+%d) to %s (%dx%d+%d+%d)\n",
		    sub->width, sub->height, h,
		    output->name != NULL ? output->name : "screen",
		    sub->width, sub_height, output->x,
		    output->y + h);
		xcb_put_image(c, sub->format, pixmap, gc,
		    sub->width, sub->height, output->x,
		    output->y + h, 0, screen->root_depth,
		    sub->size, data);

		data += row_len * sub_height;
	}

	if (sub != xcb_image)
		xcb_image_destroy(sub);
}

static void
process_output(xcb_connection_t *c, xcb_screen_t *screen, wp_output_t *output,
    wp_option_t *option, xcb_pixmap_t pixmap, xcb_gcontext_t gc)
{
	uint32_t *pixels;
	size_t len, stride;
	xcb_image_t *xcb_image;
	pixman_image_t *pixman_image;

	SAFE_MUL(stride, output->width, sizeof(*pixels));
	SAFE_MUL(len, output->height, stride);
	pixels = xmalloc(len);

	pixman_image = pixman_image_create_bits(PIXMAN_a8r8g8b8, output->width,
	    output->height, pixels, stride);
	if (pixman_image == NULL)
		errx(1, "failed to create temporary pixman image");

	if (option->mode == MODE_TILE)
		tile(pixman_image, output, option);
	else
		transform(pixman_image, output, option);

	xcb_image = xcb_image_create_native(c, output->width, output->height,
	    XCB_IMAGE_FORMAT_Z_PIXMAP, 32, NULL, len, (uint8_t *) pixels);
	if (xcb_image == NULL)
		errx(1, "failed to create xcb image");

	put_wallpaper(c, screen, output, xcb_image, pixmap, gc);

	xcb_image_destroy(xcb_image);
	pixman_image_unref(pixman_image);
	free(pixels);
}

static void
update_atoms(xcb_connection_t *c, xcb_screen_t *screen, xcb_pixmap_t pixmap)
{
	int i;
	xcb_intern_atom_cookie_t atom_cookie[2];
	xcb_intern_atom_reply_t *atom_reply[2];
	xcb_get_property_cookie_t property_cookie[2];
	xcb_get_property_reply_t *property_reply[2];
	xcb_pixmap_t *old[2];

	atom_cookie[0] = xcb_intern_atom(c, 0,
	    sizeof(ATOM_ESETROOT) - 1, ATOM_ESETROOT);
	atom_cookie[1] = xcb_intern_atom(c, 0,
	    sizeof(ATOM_XSETROOT) - 1, ATOM_XSETROOT);

	for (i = 0; i < 2; i++)
		atom_reply[i] = xcb_intern_atom_reply(c, atom_cookie[i], NULL);

	for (i = 0; i < 2; i++)
		if (atom_reply[i] != NULL)
			property_cookie[i] = xcb_get_property(c, 0,
			    screen->root, atom_reply[i]->atom, XCB_ATOM_PIXMAP,
			    0, 1);

	for (i = 0; i < 2; i++) {
		if (atom_reply[i] != NULL)
			property_reply[i] =
			    xcb_get_property_reply(c, property_cookie[i], NULL);
		else
			property_reply[i] = NULL;
		if (property_reply[i] != NULL &&
		    property_reply[i]->type == XCB_ATOM_PIXMAP)
			old[i] = xcb_get_property_value(property_reply[i]);
		else
			old[i] = NULL;
	}

	if (old[0] != NULL)
		xcb_kill_client(c, *old[0]);
	if (old[1] != NULL && (old[0] == NULL || *old[0] != *old[1]))
		xcb_kill_client(c, *old[1]);

	for (i = 0; i < 2; i++) {
		if (atom_reply[i] != NULL)
			xcb_change_property(c, XCB_PROP_MODE_REPLACE,
			    screen->root, atom_reply[i]->atom, XCB_ATOM_PIXMAP,
			    32, 1, &pixmap);
		else
			warnx("failed to update atoms");
		free(property_reply[i]);
		free(atom_reply[i]);
	}
}

static void
process_screen(xcb_connection_t *c, xcb_screen_t *screen, int snum,
    wp_option_t *options)
{
	xcb_pixmap_t pixmap;
	xcb_gcontext_t gc;
	wp_output_t *outputs, tile_output;
	wp_option_t *option;
	uint16_t width, height;
	xcb_rectangle_t rectangle;

	/* let X perform non-randr tiling if requested */
	if (options[0].mode == MODE_TILE && options[0].output == NULL &&
	    options[1].filename == NULL) {
		pixman_image_t *pixman_image = options->buffer->pixman_image;

		/* fake an output that fits the picture */
		width = pixman_image_get_width(pixman_image);
		height = pixman_image_get_height(pixman_image);
		tile_output.x = 0;
		tile_output.y = 0;
		tile_output.width = width;
		tile_output.height = height;
		tile_output.name = NULL;
		outputs = &tile_output;
	} else {
		width = screen->width_in_pixels;
		height = screen->height_in_pixels;
		outputs = get_outputs(c, screen);
	}

	DBG("creating pixmap (%dx%d)\n", width, height);
	pixmap = xcb_generate_id(c);
	xcb_create_pixmap(c, screen->root_depth, pixmap, screen->root, width,
	    height);
	gc = xcb_generate_id(c);
	xcb_create_gc(c, gc, pixmap, 0, NULL);
	rectangle.x = 0;
	rectangle.y = 0;
	rectangle.width = width;
	rectangle.height = height;
	xcb_poly_fill_rectangle(c, pixmap, gc, 1, &rectangle);

	for (option = options; option->filename != NULL; option++) {
		wp_output_t *output;

		/* ignore options which are not relevant for this screen */
		if (option->screen != -1 && option->screen != snum)
			continue;

		if (option->output != NULL &&
		    strcmp(option->output, "all") == 0)
			for (output = outputs; output->name != NULL; output++)
				process_output(c, screen, output, option,
				    pixmap, gc);
		else {
			output = get_output(outputs, option->output);
			if (output != NULL)
				process_output(c, screen, output, option,
				    pixmap, gc);
		}
	}

	xcb_change_window_attributes(c, screen->root, XCB_CW_BACK_PIXMAP,
	    &pixmap);
	update_atoms(c, screen, pixmap);
	xcb_clear_area(c, 0, screen->root, 0, 0, 0, 0);

	if (outputs != &tile_output)
		free_outputs(outputs);
}

static void
usage(void)
{
	fprintf(stderr,
"usage: xwallpaper [--screen <screen>] [--no-randr] [--output <output>]\n"
"  [--center <file>] [--maximize <file>]  [--stretch <file>]\n"
"  [--tile <file>] [--zoom <file>] [--version]\n");
	exit(1);
}

int
main(int argc, char *argv[])
{
	wp_option_t *options;
	xcb_connection_t *c;
	xcb_screen_iterator_t it;
	int snum;
#ifdef HAVE_PLEDGE
	if (pledge("dns inet rpath stdio unix", NULL) == -1)
		err(1, "pledge");
#endif /* HAVE_PLEDGE */
	if (argc < 2 || (options = parse_options(++argv)) == NULL)
		usage();

	c = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(c))
		errx(1, "failed to connect to X server");
#ifdef HAVE_PLEDGE
	if (pledge("stdio", NULL) == -1)
		err(1, "pledge");
#endif /* HAVE_PLEDGE */

	it = xcb_setup_roots_iterator(xcb_get_setup(c));
	/*
	 * Needs a screen for possible XPM color parsing.
	 * Technically this means that it has to be repeated for
	 * every screen, but it's unlikely to even encounter a setup
	 * which as multiple ones. Also the exact colors are parsed
	 * on purpose, so I don't expect a difference.
	 */
	if (it.rem == 0)
		errx(1, "no screen found");
	load_pixman_images(c, it.data, options);

	for (snum = 0; it.rem; snum++, xcb_screen_next(&it))
		process_screen(c, it.data, snum, options);

	xcb_request_check(c,
	    xcb_set_close_down_mode(c, XCB_CLOSE_DOWN_RETAIN_PERMANENT));
	if (xcb_connection_has_error(c))
		warnx("error encountered while setting wallpaper");

	xcb_disconnect(c);

	return 0;
}

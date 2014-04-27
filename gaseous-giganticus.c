/*
	Copyright (C) 2014 Stephen M. Cameron
	Author: Stephen M. Cameron

	This file is part of Spacenerds In Space.

	Spacenerds in Space is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	Spacenerds in Space is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with Spacenerds in Space; if not, write to the Free Software
	Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <limits.h>

#include <png.h>

#include "mtwist.h"
#include "mathutils.h"
#include "quat.h"
#include "simplexnoise1234.h"

#define NPARTICLES 8000000

#define DIM 1024
#define FDIM ((float) (DIM - 1))
#define XDIM DIM
#define YDIM DIM

static const int niterations = 1000;
static const float noise_scale = 3000.0;
static const float velocity_factor = 1.0;

static char *start_image;
static int start_image_width, start_image_height, start_image_has_alpha;
static unsigned char *output_image[6];
static int image_save_period = 5;

/* velocity field for 6 faces of a cubemap */
static struct velocity_field {
	union vec3 v[6][XDIM][YDIM];
} vf;

struct color {
	float r, g, b, a;
};

/* face, i, j -- coords on a cube map */
struct fij {
	int f, i, j;
};

/* particles have color, and exist on the surface of a sphere. */
static struct particle {
	union vec3 pos;
	struct color c;
} particle[NPARTICLES];

static float alphablendcolor(float underchannel, float underalpha, float overchannel, float overalpha)
{
	return overchannel * overalpha + underchannel * underalpha * (1.0 - overalpha);
}

static struct color combine_color(struct color *oc, struct color *c)
{
	struct color nc;

	nc.a = (c->a + oc->a * (1.0f - c->a));
	nc.r = alphablendcolor(oc->r, oc->a, c->r, c->a) / nc.a;
	nc.b = alphablendcolor(oc->b, oc->a, c->b, c->a) / nc.a;
	nc.g = alphablendcolor(oc->g, oc->a, c->g, c->a) / nc.a;
	return nc;
}

static void paint_particle(int face, int i, int j, struct color *c)
{
	unsigned char *pixel;
	int p;
	struct color oc, nc;

	if (i < 0 || i > 1023 || j < 0 || j > 1023)
		printf("i, j = %d, %d!!!!!1\n", i, j); 
	p = j * DIM + i;
	pixel = &output_image[face][p * 4];
#if 0
	pixel[0] = (unsigned char) (255.0f * c->r);
	pixel[1] = (unsigned char) (255.0f * c->g);
	pixel[2] = (unsigned char) (255.0f * c->b);

	pixel[3] = 255;
	return;
#else
	/* FIXME, this is inefficient */
	oc.r = (float) pixel[0] / 255.0f;
	oc.g = (float) pixel[1] / 255.0f;
	oc.b = (float) pixel[2] / 255.0f;
	oc.a = 1.0;
	nc = combine_color(&oc, c);
	pixel[0] = (unsigned char) (255.0f * nc.r);
	pixel[1] = (unsigned char) (255.0f * nc.g);
	pixel[2] = (unsigned char) (255.0f * nc.b);
	pixel[3] = 255;
#endif
}

/* convert from cubemap coords to cartesian coords on surface of sphere */
static union vec3 fij_to_xyz(int f, int i, int j)
{
	union vec3 answer;

	switch (f) {
	case 0:
		answer.v.x = (float) (i - XDIM / 2) / (float) XDIM;
		answer.v.y = -(float) (j - YDIM / 2) / (float) YDIM;
		answer.v.z = (float) DIM / 2.0f;
		break;
	case 1:
		answer.v.x = (float) DIM / 2.0f;
		answer.v.y = -(float) (j - YDIM / 2) / (float) YDIM;
		answer.v.z = -(float) (i - XDIM / 2) / (float) XDIM;
		break;
	case 2:
		answer.v.x = -(float) (i - XDIM / 2) / (float) XDIM;
		answer.v.y = -(float) (j - YDIM / 2) / (float) YDIM;
		answer.v.z = -(float) DIM / 2.0f;
		break;
	case 3:
		answer.v.x = -(float) DIM / 2.0f;
		answer.v.y = -(float) (j - YDIM / 2) / (float) YDIM;
		answer.v.z = (float) (i - XDIM / 2) / (float) XDIM;
		break;
	case 4:
		answer.v.x = (float) (i - XDIM / 2) / (float) XDIM;
		answer.v.y = (float) (float) YDIM / 2.0f;
		answer.v.z = (float) (j - YDIM / 2) / (float) YDIM;
		break;
	case 5:
		answer.v.x = (float) (i - XDIM / 2) / (float) XDIM;
		answer.v.y = -(float) (float) YDIM / 2.0f;
		answer.v.z = -(float) (j - YDIM / 2) / (float) YDIM;
		break;
	}
	vec3_normalize_self(&answer);
	return answer;
}

/* convert from cartesian coords on surface of a sphere to cubemap coords */
static struct fij xyz_to_fij(const union vec3 *p)
{
	struct fij answer;
	union vec3 t;
	int f, i, j;
	float d;

	vec3_normalize(&t, p);

	if (fabs(t.v.x) > fabs(t.v.y)) {
		if (fabs(t.v.x) > fabs(t.v.z)) {
			/* x is longest leg */
			d = fabs(t.v.x);
			if (t.v.x < 0) {
				f = 3;
				i = (int) ((t.v.z / d) * FDIM * 0.5 + 0.5 * (float) FDIM);
			} else {
				f = 1;
				i = (int) ((-t.v.z / d)  * FDIM * 0.5 + 0.5 * FDIM);
			}
		} else {
			/* z is longest leg */
			d = fabs(t.v.z);
			if (t.v.z < 0) {
				f = 2;
				i = (int) ((-t.v.x / d) * FDIM * 0.5 + 0.5 * FDIM);
			} else {
				f = 0;
				i = (int) ((t.v.x / d) * FDIM * 0.5 + 0.5 * FDIM);
				if (i < 0 || i > 1023)
					printf("i = %d!!!!!!!!!!!!!!!!\n", i);
			}
		}
		j = (int) ((-t.v.y / d) * FDIM * 0.5 + 0.5 * FDIM);
	} else {
		/* x is not longest leg, y or z must be. */
		if (fabs(t.v.y) > fabs(t.v.z)) {
			/* y is longest leg */
			d = fabs(t.v.y);
			if (t.v.y < 0) {
				f = 5;
				j = (int) ((-t.v.z / d) * FDIM * 0.5 + 0.5 * FDIM);
			} else {
				f = 4;
				j = (int) ((t.v.z / d) * FDIM * 0.5 + 0.5 * FDIM);
			}
			i = (int) ((t.v.x / d) * FDIM * 0.5 + 0.5 * FDIM);
		} else {
			/* z is longest leg */
			d = fabs(t.v.z);
			if (t.v.z < 0) {
				f = 2;
				i = (int) ((-t.v.x / d) * FDIM * 0.5 + 0.5 * FDIM);
			} else {
				f = 0;
				i = (int) ((t.v.x / d) * FDIM * 0.5 + 0.5 * FDIM);
			}
			j = (int) ((-t.v.y / d) * FDIM * 0.5 + 0.5 * FDIM);
		}
	}

	answer.f = f;
	answer.i = i;
	answer.j = j;
	return answer;
}

static const float face_to_xdim_multiplier[] = { 0.25, 0.5, 0.75, 0.0, 0.25, 0.25 };
static const float face_to_ydim_multiplier[] = { 1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0,
						0.0, 2.0 / 3.0 };

/* place particles randomly on the surface of a sphere */
static void init_particles(struct particle p[], const int nparticles)
{
	float x, y, z, xo, yo;
	const int bytes_per_pixel = start_image_has_alpha ? 4 : 3;
	unsigned char *pixel;
	int pn;
	struct fij fij;

	for (int i = 0; i < nparticles; i++) {
		random_point_on_sphere(10000.0f, &x, &y, &z);
		p[i].pos.v.x = x;
		p[i].pos.v.y = y;
		p[i].pos.v.z = z;
		fij = xyz_to_fij(&p[i].pos);
#if 0
		xo = (float) start_image_width * 0.25 * (float) fij.i / (float) DIM;
		yo = (float) start_image_height * (1.0 / 3.0) * (float) fij.j / (float) DIM;
		x = (float) start_image_width * face_to_xdim_multiplier[fij.f] + xo;
		y = (float) start_image_height * face_to_ydim_multiplier[fij.f] + yo;
		pn = (int) (y * (float) start_image_width + x);
#else
		if (fij.i < 0 || fij.i > DIM || fij.j < 0 || fij.j > DIM) {
			printf("BAD fij: %d,%d\n", fij.i, fij.j);
		}
		xo = ((float) start_image_width * (float) fij.i) / (float) DIM;
		yo = ((float) start_image_height * (float) fij.j) / (float) DIM;	
		//pn = (int) ((int) yo * ((int) start_image_width + xo));
		xo = (start_image_width * fij.i) / DIM;
		yo = (start_image_height * fij.j) / DIM;	
		xo = fij.i;
		yo = fij.j;
		//pn = (int) yo * start_image_height * 4 + (int) (xo * 4);
		pn = (int) yo * start_image_width * 3 + (int) (xo * 3);
		//pn = (pn & ~0x03);
#endif
		//pixel = (unsigned char *) &start_image[pn * bytes_per_pixel];
		pixel = (unsigned char *) &start_image[pn];
		p[i].c.r = ((float) pixel[0]) / 255.0f;
		p[i].c.g = ((float) pixel[1]) / 255.0f;
		p[i].c.b = ((float) pixel[2]) / 255.0f;
		if (fij.i == fij.j) {
			p[i].c.r = 0.0f;
			p[i].c.g = 0.0f;
			p[i].c.b = 1.0f;
		}
		p[i].c.a = start_image_has_alpha ? (float) pixel[3] / 255.0 : 1.0;
		p[i].c.a = 1.0; //start_image_has_alpha ? (float) pixel[3] / 255.0 : 1.0;
	}
}

static float fbmnoise4(float x, float y, float z, float w)
{
	return snoise4(x, y, z, w) +
		0.5 * snoise4(2.0f * x, 2.0f * y, 2.0f * z, 2.0f * w) +
		0.25 * snoise4(4.0f * x, 4.0f * y, 4.0f * z, 4.0f * w) +
		0.125 * snoise4(8.0f * x, 8.0f * y, 8.0f * z, 8.0f * w);
}

/* compute the noise gradient at the given point on the surface of a sphere */
static union vec3 noise_gradient(union vec3 position, float w, float noise_scale)
{
	union vec3 g;
	float dx, dy, dz;

	dx = noise_scale * (1.0f / (float) DIM);
	dy = noise_scale * (1.0f / (float) DIM);
	dz = noise_scale * (1.0f / (float) DIM);

	g.v.x = fbmnoise4(position.v.x + dx, position.v.y, position.v.z, w) -
		fbmnoise4(position.v.x - dx, position.v.y, position.v.z, w);
	g.v.y = fbmnoise4(position.v.x, position.v.y + dy, position.v.z, w) -
		fbmnoise4(position.v.x, position.v.y - dy, position.v.z, w);
	g.v.z = fbmnoise4(position.v.x, position.v.y, position.v.z + dz, w) -
		fbmnoise4(position.v.x, position.v.y, position.v.z - dz, w);
	return g;
}

static union vec3 curl2(union vec3 pos, union vec3 noise_gradient)
{
	union vec3 p1, p2, proj_ng, axis, rotated_ng;
	union quat rotation;
	float m1, m2;

	/* project noise gradient onto sphere surface */
	vec3_add(&p1, &pos, &noise_gradient);
	m1 = vec3_magnitude(&pos);
	m2 = vec3_magnitude(&p1);
	vec3_normalize_self(&p1);
	vec3_mul_self(&p1, m1);
	vec3_sub(&proj_ng, &p1, &pos);

	/* rotate projected noise gradient 90 degrees about pos. */
	vec3_normalize(&axis, &pos);
	if (m1 > m2) 
		quat_init_axis_v(&rotation, &axis, M_PI / 2.0);
	else
		quat_init_axis_v(&rotation, &axis, 3.0 * M_PI / 2.0);
	quat_rot_vec(&rotated_ng, &proj_ng, &rotation);
	return rotated_ng;
}

/* compute the curl of the given noise gradient at the given position */
static union vec3 curl(union vec3 pos, union vec3 noise_gradient)
{
	union quat rot, unrot;
	union vec3 unrotated_ng, rotated_ng;
	union vec3 straight_up = { { 0.0f, 1.0f, 0.0f } };

	/* calculate quaternion to rotate from point on sphere to straight up. */
	quat_from_u2v(&rot, &pos, &straight_up, &straight_up);

	/* calculate quaternion to unrotate from straight up to point on sphere */
	quat_from_u2v(&unrot, &straight_up, &pos, &straight_up);

	/* Rotate noise gradient to top of sphere */
	quat_rot_vec(&rotated_ng, &noise_gradient, &rot);

	/* Now we can turn rotated_ng 90 degrees by swapping x and z (using y as tmp) */
	rotated_ng.v.y = rotated_ng.v.z;
	rotated_ng.v.z = rotated_ng.v.x;
	rotated_ng.v.x = rotated_ng.v.y;

	/* Now we can project rotated noise gradient into x-z plane by zeroing y component. */
	rotated_ng.v.y = 0.0f;

	/* Now unrotate projected, 90-degree rotated noise gradient */
	quat_rot_vec(&unrotated_ng, &rotated_ng, &unrot);

	/* and we're done */
	return unrotated_ng;
}

/* compute velocity field for all cells in cubemap.  It is scaled curl of gradient of noise field */
static void update_velocity_field(struct velocity_field *vf, float noise_scale, float w)
{
	int f, i, j;
	union vec3 v, c, ng;

	for (f = 0; f < 6; f++) {
		for (i = 0; i < XDIM; i++) {
			for (j = 0; j < YDIM; j++) {
				v = fij_to_xyz(f, i, j);
				vec3_mul_self(&v, noise_scale);
				ng = noise_gradient(v, w * noise_scale, noise_scale);
				c = curl2(v, ng);
				vec3_mul(&vf->v[f][i][j], &c, velocity_factor);
				//vec3_mul(&vf->v[f][i][j], &c, 0.0f);
			}
		}
	}
}

/* move a particle according to velocity field at its current location */
static void move_particle(struct particle *p, struct velocity_field *vf)
{
	struct fij fij;

	fij = xyz_to_fij(&p->pos);
	vec3_add_self(&p->pos, &vf->v[fij.f][fij.i][fij.j]);
	vec3_normalize_self(&p->pos);
	vec3_mul_self(&p->pos, (float) XDIM / 2.0f);
}

static void move_particles(struct particle p[], const int nparticles,
			struct velocity_field *vf)
{
	for (int i = 0; i < nparticles; i++)
		move_particle(&p[i], vf);
}

static void update_output_images(struct particle p[], const int nparticles)
{
	int i;
	struct fij fij;

	for (i = 0; i < nparticles; i++) {
		fij = xyz_to_fij(&p[i].pos);
		p[i].c.a = 1.0;
		paint_particle(fij.f, fij.i, fij.j, &p[i].c);
	}
}

/* Copied and modified from snis_graph.c sng_load_png_texture(), see snis_graph.c */
char *load_png_image(const char *filename, int flipVertical, int flipHorizontal,
	int pre_multiply_alpha,
	int *w, int *h, int *hasAlpha, char *whynot, int whynotlen)
{
	int i, j, bit_depth, color_type, row_bytes, image_data_row_bytes;
	png_byte header[8];
	png_uint_32 tw, th;
	png_structp png_ptr = NULL;
	png_infop info_ptr = NULL;
	png_infop end_info = NULL;
	png_byte *image_data = NULL;

	FILE *fp = fopen(filename, "rb");
	if (!fp) {
		snprintf(whynot, whynotlen, "Failed to open '%s': %s",
			filename, strerror(errno));
		return 0;
	}

	if (fread(header, 1, 8, fp) != 8) {
		snprintf(whynot, whynotlen, "Failed to read 8 byte header from '%s'\n",
				filename);
		goto cleanup;
	}
	if (png_sig_cmp(header, 0, 8)) {
		snprintf(whynot, whynotlen, "'%s' isn't a png file.",
			filename);
		goto cleanup;
	}

	png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING,
							NULL, NULL, NULL);
	if (!png_ptr) {
		snprintf(whynot, whynotlen,
			"png_create_read_struct() returned NULL");
		goto cleanup;
	}

	info_ptr = png_create_info_struct(png_ptr);
	if (!info_ptr) {
		snprintf(whynot, whynotlen,
			"png_create_info_struct() returned NULL");
		goto cleanup;
	}

	end_info = png_create_info_struct(png_ptr);
	if (!end_info) {
		snprintf(whynot, whynotlen,
			"2nd png_create_info_struct() returned NULL");
		goto cleanup;
	}

	if (setjmp(png_jmpbuf(png_ptr))) {
		snprintf(whynot, whynotlen, "libpng encounted an error");
		goto cleanup;
	}

	png_init_io(png_ptr, fp);
	png_set_sig_bytes(png_ptr, 8);

	/*
	 * PNG_TRANSFORM_STRIP_16 |
	 * PNG_TRANSFORM_PACKING  forces 8 bit
	 * PNG_TRANSFORM_EXPAND forces to expand a palette into RGB
	 */
	png_read_png(png_ptr, info_ptr, PNG_TRANSFORM_STRIP_16 | PNG_TRANSFORM_PACKING | PNG_TRANSFORM_EXPAND, NULL);

	png_get_IHDR(png_ptr, info_ptr, &tw, &th, &bit_depth, &color_type, NULL, NULL, NULL);

	if (bit_depth != 8) {
		snprintf(whynot, whynotlen, "load_png_texture only supports 8-bit image channel depth");
		goto cleanup;
	}

	if (color_type != PNG_COLOR_TYPE_RGB && color_type != PNG_COLOR_TYPE_RGB_ALPHA) {
		snprintf(whynot, whynotlen, "load_png_texture only supports RGB and RGBA");
		goto cleanup;
	}

	if (w)
		*w = tw;
	if (h)
		*h = th;
	int has_alpha = (color_type == PNG_COLOR_TYPE_RGB_ALPHA);
	if (hasAlpha)
		*hasAlpha = has_alpha;

	row_bytes = png_get_rowbytes(png_ptr, info_ptr);
	image_data_row_bytes = row_bytes;

	/* align to 4 byte boundary */
	if (image_data_row_bytes & 0x03)
		image_data_row_bytes += 4 - (image_data_row_bytes & 0x03);

	png_bytepp row_pointers = png_get_rows(png_ptr, info_ptr);

	image_data = malloc(image_data_row_bytes * th * sizeof(png_byte) + 15);
	if (!image_data) {
		snprintf(whynot, whynotlen, "malloc failed in load_png_texture");
		goto cleanup;
	}

	int bytes_per_pixel = (color_type == PNG_COLOR_TYPE_RGB_ALPHA ? 4 : 3);

	for (i = 0; i < th; i++) {
		png_byte *src_row;
		png_byte *dest_row = image_data + i * image_data_row_bytes;

		if (flipVertical)
			src_row = row_pointers[th - i - 1];
		else
			src_row = row_pointers[i];

		if (flipHorizontal) {
			for (j = 0; j < tw; j++) {
				png_byte *src = src_row + bytes_per_pixel * j;
				png_byte *dest = dest_row + bytes_per_pixel * (tw - j - 1);
				memcpy(dest, src, bytes_per_pixel);
			}
		} else {
			memcpy(dest_row, src_row, row_bytes);
		}

		if (has_alpha && pre_multiply_alpha) {
			for (j = 0; j < tw; j++) {
				png_byte *pixel = dest_row + bytes_per_pixel * j;
				float alpha = pixel[3] / 255.0;
				pixel[0] = pixel[0] * alpha;
				pixel[1] = pixel[1] * alpha;
				pixel[2] = pixel[2] * alpha;
			}
		}
	}

	png_destroy_read_struct(&png_ptr, &info_ptr, &end_info);
	fclose(fp);
	return (char *) image_data;

cleanup:
	if (image_data)
		free(image_data);
	png_destroy_read_struct(&png_ptr, &info_ptr, &end_info);
	fclose(fp);
	return 0;
}

static int write_png_image(const char *filename, unsigned char *pixels, int w, int h, int has_alpha)
{
	png_structp png_ptr;
	png_infop info_ptr;
	png_byte **row;
	int x, y, rc, colordepth = 8;
	int bytes_per_pixel = has_alpha ? 4 : 3;
	FILE *f;

	f = fopen(filename, "w");
	if (!f) {
		fprintf(stderr, "fopen: %s:%s\n", filename, strerror(errno));
		return -1;
	}
	png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	if (!png_ptr)
		goto cleanup1;
	info_ptr = png_create_info_struct(png_ptr);
	if (!info_ptr)
		goto cleanup2;
	if (setjmp(png_jmpbuf(png_ptr))) /* oh libpng, you're old as dirt, aren't you. */
		goto cleanup2;

	png_set_IHDR(png_ptr, info_ptr, (size_t) w, (size_t) h, colordepth,
			has_alpha ? PNG_COLOR_TYPE_RGBA : PNG_COLOR_TYPE_RGB,
			PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
			PNG_FILTER_TYPE_DEFAULT);

	row = png_malloc(png_ptr, h * sizeof(*row));
	for (y = 0; y < h; y++) {
		row[y] = png_malloc(png_ptr, w * bytes_per_pixel);
		for (x = 0; x < w; x++) {
			unsigned char *r = (unsigned char *) row[y];
			unsigned char *src = (unsigned char *)
				&pixels[y * w * bytes_per_pixel + x * bytes_per_pixel];
			unsigned char *dest = &r[x * bytes_per_pixel];
			memcpy(dest, src, bytes_per_pixel);
		}
	}

	png_init_io(png_ptr, f);
	png_set_rows(png_ptr, info_ptr, row);
	png_write_png(png_ptr, info_ptr, PNG_TRANSFORM_PACKING, NULL);

	for (y = 0; y < h; y++)
		png_free(png_ptr, row[y]);
	png_free(png_ptr, row);
	rc = 0;
cleanup2:
	png_destroy_write_struct(&png_ptr, &info_ptr);
cleanup1:
	fclose(f);
	return rc;
}

static void save_output_images(void)
{
	int i;
	char fname[PATH_MAX];

	for (i = 0; i < 6; i++) {
		sprintf(fname, "tmpg-%d.png", i);
		if (write_png_image(fname, output_image[i], DIM, DIM, 1))
			fprintf(stderr, "Failed to write %s\n", fname);
	}
}

static char *load_image(const char *filename, int *w, int *h, int *a)
{
	char *i;
	char msg[100];

	i = load_png_image(filename, 0, 0, 0, w, h, a, msg, sizeof(msg));
	if (!i) {
		fprintf(stderr, "%s: cannot load image: %s\n", filename, msg);
		exit(1);
	}
	return i;
}

void allocate_output_images(void)
{
	int i;

	for (i = 0; i < 6; i++) {
		output_image[i] = malloc(4 * DIM * DIM);
		printf("output_image %d = %p\n", i, output_image[i]);
		memset(output_image[i], 0, 4 * DIM * DIM);
	}
}

int main(int argc, char *argv[])
{
	int i;
	union vec3 test = { { 0, 0, 0 } };
	struct fij fij;


	for (i = 0; i < 1000; i++) {
		test.v.x = ((float) i - 500.0f) / 500.0f;
		test.v.y = 0;
		test.v.z = 1.0;
		fij = xyz_to_fij(&test);
		printf("%f,%f,%f -> %d,%d,%d\n", test.v.x, test.v.y, test.v.z, fij.f, fij.i, fij.j);
	}

	fij = xyz_to_fij(&test);
	printf("fij = %d %d %d\n", fij.f, fij.i, fij.j);

	printf("Allocating output image space\n");
	allocate_output_images();
	printf("Loading image\n");
	start_image_has_alpha = 0;
	start_image = load_image("gas.png", &start_image_width, &start_image_height,
					&start_image_has_alpha);
	
	write_png_image("blah.png", start_image, start_image_width,
			start_image_height, start_image_has_alpha);
	printf("w,h = %d,%d\n", start_image_width, start_image_height);
	printf("Initializing particles\n");
	init_particles(particle, NPARTICLES);
	printf("Initializing velocity field\n");
	update_velocity_field(&vf, noise_scale, 0.0);
	printf("Running simulation\n");

	for (i = 0; i < niterations; i++) {
		if ((i % 50) == 0)
			printf("\n%5d / %5d ", i, niterations);
		else
			printf(".");
		fflush(stdout);
		move_particles(particle, NPARTICLES, &vf);
		update_output_images(particle, NPARTICLES);
		if ((i % image_save_period) == 0)
			save_output_images();
	}
	printf("\n%5d / %5d\n", i, niterations);
	return 0;
}

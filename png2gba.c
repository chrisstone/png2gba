/* Program to convert PNG images into C header files storing
 * arrays of data for programming the GBA */

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PNG_DEBUG 3
#include <png.h>

/* the GBA palette has a limit of 256 colors */
#define PALETTE_MAX 256

/* the GBA always uses 8x8 tiles */
#define TILE_SIZE 8

/* configuration arguments */
struct Arguments {
	int palette;
	int tileize;
	char *colorkey;
	char *output_file_name;
	char *input_file_name;
};

/* image data */
struct Image {
	int width, height, channels;
	png_byte color_type;
	png_byte bit_depth;
	png_bytep *rows;
};

struct Palette {
	unsigned short colors[PALETTE_MAX];
	unsigned char used;
	unsigned char max;
};

char *extractFileName(const char *path) {
	const char *lastSlash =
		strrchr(path, '/');	 // Find the last slash in the path (Unix-style)
	const char *lastBackslash = strrchr(
		path, '\\');  // Find the last backslash in the path (Windows-style)
	const char *lastSeparator =
		lastSlash > lastBackslash ? lastSlash : lastBackslash;

	if (lastSeparator == NULL) {
		return strdup(
			path);	// No separator found, return a copy of the original path
	} else {
		return strdup(
			lastSeparator +
			1);	 // Return a copy of the string after the last separator
	}
}

/* load the png image from a file */
struct Image *read_png(FILE *in) {
	/* read the PNG signature */
	unsigned char header[8];
	fread(header, 1, 8, in);
	if (png_sig_cmp(header, 0, 8)) {
		fprintf(stderr, "Error: This does not seem to be a valid PNG file!\n");
		exit(-1);
	}

	/* setup structs for reading */
	png_structp png_reader =
		png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	if (!png_reader) {
		fprintf(stderr, "Error: Could not read PNG file!\n");
		exit(-1);
	}
	png_infop png_info = png_create_info_struct(png_reader);
	if (!png_info) {
		fprintf(stderr, "Error: Could not read PNG file!\n");
		exit(-1);
	}
	if (setjmp(png_jmpbuf(png_reader))) {
		fprintf(stderr, "Error: Could not read PNG file!\n");
		exit(-1);
	}

	/* allocate an image */
	struct Image *image = malloc(sizeof(struct Image));

	/* read in the header information */
	png_init_io(png_reader, in);
	png_set_sig_bytes(png_reader, 8);
	png_read_info(png_reader, png_info);
	image->width = png_get_image_width(png_reader, png_info);
	image->height = png_get_image_height(png_reader, png_info);
	image->color_type = png_get_color_type(png_reader, png_info);
	image->bit_depth = png_get_bit_depth(png_reader, png_info);
	png_set_interlace_handling(png_reader);
	png_read_update_info(png_reader, png_info);

	/* read the actual file */
	if (setjmp(png_jmpbuf(png_reader))) {
		fprintf(stderr, "Error: Could not read PNG file!\n");
		exit(-1);
	}
	image->rows = (png_bytep *)malloc(sizeof(png_bytep) * image->height);
	int r;
	for (r = 0; r < image->height; r++) {
		image->rows[r] = malloc(png_get_rowbytes(png_reader, png_info));
	}
	png_read_image(png_reader, image->rows);

	/* check format */
	if (png_get_color_type(png_reader, png_info) == PNG_COLOR_TYPE_RGB) {
		image->channels = 3;
	} else if (png_get_color_type(png_reader, png_info) ==
			   PNG_COLOR_TYPE_RGBA) {
		image->channels = 4;
	} else {
		fprintf(stderr, "Error: PNG file is not in the RGB or RGBA format!\n");
		exit(-1);
	}

	return image;
}

/* inserts a color into a palette and returns the index, or return
 * the existing index if the color is already there */
unsigned char insert_palette(unsigned short color,
							 struct Palette* palette) {
	/* loop through the palette */
	unsigned char i;
	for (i = 0; i < palette->used; i++) {
		/* if this is it, return it */
		if (palette->colors[i] == color) {
			return i;
		}
	}

	/* if the palette is full, we're in trouble */
	if (palette->used >= (palette->max - 1)) {
		fprintf(stderr, "Error: Too many colors in image for the palette!\n");
		exit(-1);
	}

	/* it was not found, so add it */
	palette->used++;
	palette->colors[palette->used - 1] = color;

	/* return the index */
	return (palette->used - 1);
}

/* returns the next pixel from the image, based on whether we
 * are tile-izing or not, returns NULL when we have done them all */
png_byte *next_byte(struct Image *image, int tileize) {
	/* keeps track of where we are in the "global" image */
	static int r = 0;
	static int c = 0;

	/* keeps track of where we are relative to one tile (0-7) */
	static int tr = 0;
	static int tc = 0;

	/* if we have gone through it all */
	if (r == image->height) {
		return NULL;
	}

	/* get the pixel next */
	png_byte *row = image->rows[r];
	png_byte *ptr = &(row[c * image->channels]);

	/* increment things based on if we are tileizing or not */
	if (!tileize) {
		/* just go sequentially, wrapping to the next row at the end of a column
		 */
		c++;
		if (c >= image->width) {
			r++;
			c = 0;
		}
	} else {
		/* increment the column */
		c++;
		tc++;

		/* if we hit the end of a tile row */
		if (tc >= 8) {
			/* go to the next one */
			r++;
			tr++;
			c -= 8;
			tc = 0;

			/* if we hit the end of the tile altogether */
			if (tr >= 8) {
				r -= 8;
				tr = 0;
				c += 8;
			}

			/* if we are now at the end of the actual row, go to next one */
			if (c >= image->width) {
				tc = 0;
				tr = 0;
				c = 0;
				r += 8;
			}
		}
	}

	/* and return the pixel we found previously */
	return ptr;
}

unsigned short hex24_to_15(char *hex24) {
	/* skip the # sign */
	hex24++;

	/* break off the pieces */
	char rs[3], gs[3], bs[3];
	rs[0] = *hex24++;
	rs[1] = *hex24++;
	rs[2] = '\0';
	gs[0] = *hex24++;
	gs[1] = *hex24++;
	gs[2] = '\0';
	bs[0] = *hex24++;
	bs[1] = *hex24++;
	bs[2] = '\0';

	/* convert from hex string to int */
	int r = strtol(rs, NULL, 16);
	int g = strtol(gs, NULL, 16);
	int b = strtol(bs, NULL, 16);

	/* build the full 15 bit short */
	unsigned short color = (b >> 3) << 10;
	color += (g >> 3) << 5;
	color += (r >> 3);

	return color;
}

/* perform the actual conversion from png to gba formats */
void png2gba(
	FILE *in, FILE *out, char *name, int palette, int tileize, char *colorkey) {


	/* loop through the pixel data */
	unsigned char red, green, blue;
	int colors_this_line = 0;
	png_byte *ptr;

	while ((ptr = next_byte(image, tileize))) {
		red = ptr[0];
		green = ptr[1];
		blue = ptr[2];

		/* convert to 16-bit color */
		unsigned short color = (blue >> 3) << 10;
		color += (green >> 3) << 5;
		color += (red >> 3);

		/* print leading space if first of line */
		if (colors_this_line == 0) {
			fprintf(out, "    ");
		}

		/* print color directly, or palette index */
		if (!palette) {
			fprintf(out, "0x%04X", color);
		} else {
			unsigned char index =
				insert_palette(color, color_palette, &palette_size);
			fprintf(out, "0x%02X", index);
		}

		fprintf(out, ", ");

		/* increment colors on line unless too many */
		colors_this_line++;
		if ((colors_this_line >= TILE_SIZE)) {
			fprintf(out, "\n");
			colors_this_line = 0;
		}
	}

	/* write postamble stuff */
	fprintf(out, "\n};\n\n");

	/* write the palette if needed */
	if (palette) {
		int colors_this_line = 0;
		fprintf(out, "const unsigned short %s_palette [] = {\n", name);
		int i;
		for (i = 0; i < PALETTE_MAX; i++) {
			if (colors_this_line == 0) {
				fprintf(out, "    ");
			}
			fprintf(out, "0x%04x", color_palette[i]);
			if (i != (PALETTE_MAX - 1)) {
				fprintf(out, ", ");
			}
			colors_this_line++;
			if (colors_this_line > 8) {
				fprintf(out, "\n");
				colors_this_line = 0;
			}
		}
		fprintf(out, "\n};\n\n");
	}

	/* close up, we're done */
	fclose(out);
}

int main(int argc, char **argv) {
	/* set up the arguments structure */
	struct Arguments args;

	/* the default values */
	args.output_file_name = NULL;
	args.input_file_name = NULL;
	args.colorkey = "#ff00ff";
	args.palette = 0;
	args.tileize = 0;

	/* parse command line */
	int opt, p;
	while ((opt = getopt(argc, argv, "p::to:i:c:h")) != -1) {
		/* switch on the command line option that was passed in */
		switch (opt) {
			case 'p':
				/* set the palette option */
				p = atoi(optarg);
				if (p) {
					args.palette = p;
				} else {
					args.palette = PALETTE_MAX;
				}
				break;

			case 't':
				/* set the tileize option */
				args.tileize = 1;
				break;

			case 'o':
				/* the output file name is set */
				args.output_file_name = optarg;
				break;

			case 'c':
				/* the colorkey is set */
				args.colorkey = optarg;
				break;

			case 'i':
				args.input_file_name = optarg;
				break;

			case 'h':
				fprintf(stdout, "");
				exit(0);

			case '?':
				fprintf(stderr, "Invalid option: -%c\n", optopt);
				break;
		}
	}

	/* Verify Arguments */
	if (args.input_file_name == NULL) {
		fprintf(stderr, "No Input Specified");
		exit(-1);
	}
	if (args.palette) {
		if ((args.palette != 16) && (args.palette != 256)) {
			fprintf(stderr, "Palette must be 16 or 256 colors");
			exit(-1);
		}
	}

	/* the image path without the extension */
	char *name = strdup(args.input_file_name);
	char *extension = strstr(name, ".png");
	if (!extension) {
		fprintf(stderr, "Error: File name should end in .png!\n");
		exit(-1);
	}
	*extension = '\0'; /* chop name down, less the extension */

	char *disp_name = extractFileName(name);

	/* Input: Open, Read, Close */
	FILE *input = fopen(args.input_file_name, "rb");
	if (!input) {
		fprintf(stderr,
				"Error: Can not open %s for reading!\n",
				args.input_file_name);
		return -1;
	}
	struct Image *image = read_png(input);
	fclose(input);

	/* Output: Determine Name, Open */
	FILE *output;
	char *output_name;
	if (args.output_file_name) {
		output_name = args.output_file_name;
	} else {
		output_name = malloc(sizeof(char) * (strlen(name) + 3));
		sprintf(output_name, "%s.h", name);
	}
	output = fopen(output_name, "w");

	/* write preamble stuff */
	fprintf(output, "/* %s.h\n * generated by png2gba */\n\n", name);
	fprintf(output, "#define %s_width %d\n", name, image->width);
	fprintf(output, "#define %s_height %d\n\n", name, image->height);
	if (args.palette) {
		fprintf(output, "const unsigned char %s_data [] = {\n", name);
	} else {
		fprintf(output, "const unsigned short %s_data [] = {\n", name);
	}

	/* Create Palette and insert Transparent Color */
	struct Palette palette;
	memset(&palette, 0, sizeof(palette));
	palette.max = args.palette;
	insert_palette(hex24_to_15(args.colorkey), &palette);




	png2gba(input, output, name, args.palette, args.tileize, args.colorkey);

	return 0;
}

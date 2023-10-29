/*
zopfli-pnginator: Embbed javascript code in PNG and add custom chunk with a
"html unpacking" script. Image data (= js code) compression done via standard
deflate or zopfli.

clang -lz -lzopfli -std=c17 -Wall -Wextra -pedantic zopfli-pnginator.c

Based on:
https://daeken.dev/blog/2011-08-31_Superpacking_JS_Demos.html
https://gist.github.com/gasman/2560551

Uses:
https://github.com/google/zopfli
https://github.com/madler/zlib
*/

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <winsock.h>
#else
#include <arpa/inet.h>
#endif
#include "zlib.h"
#include "zopfli.h"

#define min(a, b) (((a) < (b)) ? (a) : (b))

typedef struct PNG_IHDR_s {
  unsigned int width;
  unsigned int height;
  unsigned char bit_depth;
  unsigned char color_type;
  unsigned char compression_method;
  unsigned char filter_method;
  unsigned char interlace_method;
} PNG_IHDR;

typedef struct IMAGE_s {
  unsigned char *data;
  size_t size;
  size_t width;
  size_t height;
} IMAGE;

typedef struct USER_OPTIONS_s {
  char *javascript_path;
  char *png_path;
  bool no_zopfli;
  int zopfli_iterations;
  bool no_blocksplitting;
  bool apply_format_hacks;
  bool no_statistics;
} USER_OPTIONS;

typedef struct COMPRESS_STATISTICS_s {
  size_t javascript_size;
  size_t png_size;
  bool multi_row_image;
} COMPRESSION_STATISTICS;

// Command line option names
const char *NO_ZOPFLI = "--no_zopfli";
const char *ZOPFLI_ITERATIONS = "--zopfli_iterations=";
const char *NO_BLOCK_SPLITTING = "--no_blocksplitting";
const char *NO_FORMAT_HACKS = "--no_format_hacks";
const char *NO_STATISTICS = "--no_statistics";

const unsigned char PNG_HEADER[] = {0x89, 0x50, 0x4e, 0x47,
                                    0x0d, 0x0a, 0x1a, 0x0a};

// Javascript code fits on a single row in the PNG
const int SINGLE_ROW_MAX_LENGTH = 4096;

// p01's single-pixel-row bootstrap (requires an 0x00 end marker on the js
// string) (edit by Gasman: move drawImage out of getImageData params (it
// returns undef, which is invalid) and change eval to (1,eval) to force global
// evaluation)
const char *SINGLE_ROW_IMAGE_HTML_UNPACK =
    "<canvas id=c><img "
    "onload=with(c.getContext('2d'))for(p=e='';drawImage(this,p--,0),t="
    "getImageData(0,0,1,1).data[0];)e+=String.fromCharCode(t);(1,eval)(e) "
    "src=#>";

// p01's multiple-pixel-row bootstrap (requires a dummy first byte on the js
// string) (edit by Gasman: set explicit canvas width to support widths above
// 300; move drawImage out of getImageData params; change eval to (1,eval) to
// force global evaluation)
const char *MULTI_ROW_IMAGE_HTML_UNPACK =
    "<canvas id=c><img "
    "onload=for(w=c.width=4096,a=c.getContext('2d'),a.drawImage(this,p=0,0),"
    "e='"
    "',d=a.getImageData(0,0,w,%u).data;t=d[p+=4];)e+=String.fromCharCode(t);"
    "(1,"
    "eval)(e) src=#>";

char *read_text_file(const char *file_path) {
  char *text = NULL;
  FILE *file = fopen(file_path, "rt");

  if (file != NULL) {
    fseek(file, 0, SEEK_END);
    size_t size = ftell(file);
    rewind(file);

    // Add an additional byte for null terminating the string
    text = (char *)calloc(size + 1, sizeof(char));

    if (fread(text, sizeof(char), size, file) != size) {
      printf("Failed to read source file '%s'\n", file_path);
      free(text);
      text = NULL;
    }

    fclose(file);
  } else {
    printf("Failed to open javascript source file '%s'\n", file_path);
  }

  return text;
}

IMAGE *embbed_javascript_in_image(
    char *javascript, COMPRESSION_STATISTICS *compression_statistics) {
  // Get string length of our javascript source
  size_t javascript_length = strlen(javascript);
  compression_statistics->javascript_size = javascript_length;

  // Create our image
  IMAGE *image = malloc(sizeof(IMAGE));

  // Javascript source either fits on a single image row or needs multiple rows
  if (javascript_length < SINGLE_ROW_MAX_LENGTH) {
    // Size of single row in the image is length of javascript + 1 byte for
    // dummy marker added at the end
    image->width = javascript_length + sizeof(unsigned char);
    image->height = 1;

    // Image data size is it's width plus 1 byte for 'no filtering' indicator
    // at the beginning of the row
    image->size = sizeof(unsigned char) + image->width;
    image->data = (unsigned char *)calloc(image->size, sizeof(unsigned char));

    // Copy javascript string into destination buffer, account for 'no
    // filtering' \0 indicator at begin of row
    memcpy(image->data + sizeof(unsigned char), javascript, javascript_length);

    // Dummy marker \0 at end of javascript string (required by unpacking) is
    // already there due to calloc
  } else {
    // Multi row image has maximum row width
    image->width = SINGLE_ROW_MAX_LENGTH;

    // Account for dummy byte required by unpacking to calculate number of rows
    image->height = (size_t)ceil((javascript_length + sizeof(unsigned char)) /
                                 (float)SINGLE_ROW_MAX_LENGTH);

    // Full data size includes 1 byte 'no filtering' indicator per row
    image->size = ((image->width) + sizeof(unsigned char)) * (image->height);
    image->data = (unsigned char *)calloc(image->size, sizeof(unsigned char));

    // Start at byte 2 of first row because of 'no filtering' \0 indicator
    // and dummy byte \0 for unpacking
    unsigned char *image_ptr = image->data + 2 * sizeof(unsigned char);

    // Handle each row separately
    for (size_t i = 0; i < javascript_length;) {
      // Dummy byte for unpacking in first row was already "written", so
      // width of first row is - 1 Length of last row is only the remaining
      // bytes available in source data (the rest is already padded with \0)
      size_t row_length =
          min(javascript_length - i,
              (i == 0 ? SINGLE_ROW_MAX_LENGTH - 1 : SINGLE_ROW_MAX_LENGTH));

      // Copy the full row from source to destination
      memcpy(image_ptr, javascript + i * sizeof(unsigned char),
             row_length * sizeof(unsigned char));

      // Walk source data in row size increments
      i += row_length;

      // Destination data pointer increment accounts for additional byte
      // from 'no filtering' indicator
      image_ptr += sizeof(unsigned char) + row_length * sizeof(unsigned char);
    }
  }

  return image;
}

int write_png_chunk(char *chunk_identifier, unsigned char *data,
                    size_t data_size, FILE *outfile, bool no_crc,
                    bool overflow_data_in_crc) {
  // Write data size
  uint32_t data_size_out = htonl(data_size - (overflow_data_in_crc ? 4 : 0));
  if (fwrite(&data_size_out, sizeof(unsigned char), 4, outfile) !=
      4 * sizeof(unsigned char)) {
    return EXIT_FAILURE;
  }

  // Write chunk identifier
  if (fwrite(chunk_identifier, sizeof(unsigned char), 4, outfile) !=
      4 * sizeof(unsigned char)) {
    return EXIT_FAILURE;
  }

  // Write actual data (can be null)
  if (fwrite(data, sizeof(unsigned char), data_size, outfile) !=
      data_size * sizeof(unsigned char)) {
    return EXIT_FAILURE;
  }

  if (!no_crc) {
    // Allocate our CRC32 buffer which contains chunk identifier + data
    unsigned char *crc_buffer = (unsigned char *)malloc(
        4 * sizeof(unsigned char) + data_size * sizeof(unsigned char));

    // Copy chunk identifier and data into CRC32 buffer
    memcpy(crc_buffer, chunk_identifier, 4 * sizeof(unsigned char));
    memcpy(crc_buffer + 4 * sizeof(unsigned char), data,
           data_size * sizeof(unsigned char));

    // Calculate CRC32
    unsigned long crc = crc32(0L, crc_buffer, 4 + data_size);

    // Write CRC32 to file
    uint32_t crc_out = htonl(crc);
    if (fwrite(&crc_out, sizeof(unsigned char), 4, outfile) !=
        4 * sizeof(unsigned char)) {
      free(crc_buffer);
      return EXIT_FAILURE;
    }

    free(crc_buffer);
  }

  return EXIT_SUCCESS;
}

int write_image_as_png(IMAGE *image, USER_OPTIONS *user_options,
                       COMPRESSION_STATISTICS *compression_statistics) {
  FILE *outfile = fopen(user_options->png_path, "wb+");
  if (outfile == NULL) {
    printf("Failed to open destination png file '%s'\n",
           user_options->png_path);
    return EXIT_FAILURE;
  }

  // Write PNG header
  if (fwrite(PNG_HEADER, sizeof(unsigned char), sizeof(PNG_HEADER), outfile) !=
      sizeof(PNG_HEADER) * sizeof(unsigned char)) {
    printf("Failed to write destination png file '%s' (PNG header)\n",
           user_options->png_path);
    fclose(outfile);
    return EXIT_FAILURE;
  }

  // Write image header (IHDR) chunk
  PNG_IHDR png_ihdr = {
      htonl(image->width), htonl(image->height), 8, 0, 0, 0, 0};
  if (write_png_chunk("IHDR", (unsigned char *)&png_ihdr,
                      2 * sizeof(unsigned int) + 5 * sizeof(unsigned char),
                      outfile, false, false) != EXIT_SUCCESS) {
    printf("Failed to write destination png file '%s' (IHDR)\n",
           user_options->png_path);
    fclose(outfile);
    return EXIT_FAILURE;
  }

  // Prepare unpack code
  char *unpack_code = NULL;
  if (image->height == 1) {
    // Unpack code for single row image can be stored as is
    unpack_code = (char *)SINGLE_ROW_IMAGE_HTML_UNPACK;
    compression_statistics->multi_row_image = false;
  } else {
    // Height of multi row image needs to be substituted in the unpack code
    size_t max_unpack_code_length = strlen(MULTI_ROW_IMAGE_HTML_UNPACK) + 4;
    unpack_code = malloc(max_unpack_code_length * sizeof(char));
    snprintf(unpack_code, max_unpack_code_length, MULTI_ROW_IMAGE_HTML_UNPACK,
             image->height);
    compression_statistics->multi_row_image = true;
  }

  // Write custom chunk with unpack code
  if (write_png_chunk("jawh", (unsigned char *)unpack_code, strlen(unpack_code),
                      outfile, user_options->apply_format_hacks,
                      user_options->apply_format_hacks) != EXIT_SUCCESS) {
    printf("Failed to write destination png file '%s' (custom chunk)\n",
           user_options->png_path);
    fclose(outfile);
    return EXIT_FAILURE;
  }

  if (image->height > 1) {
    free(unpack_code);
  }

  unsigned long compressed_data_size = 0;
  unsigned char *compressed_data = NULL;
  if (!user_options->no_zopfli) {
    // Zopfli
    ZopfliOptions zopfli_options;
    ZopfliInitOptions(&zopfli_options);
    zopfli_options.numiterations = user_options->zopfli_iterations;
    zopfli_options.blocksplitting = !user_options->no_blocksplitting;
    ZopfliCompress(&zopfli_options, ZOPFLI_FORMAT_ZLIB, image->data,
                   image->size, &compressed_data, &compressed_data_size);
  } else {
    // ZLIB deflate
    compressed_data_size = compressBound(image->size);
    compressed_data =
        (unsigned char *)malloc(compressed_data_size * sizeof(unsigned char));
    if (compress2(compressed_data, &compressed_data_size, image->data,
                  image->size, 9 /* level */) != Z_OK) {
      printf("Failed to deflate image data\n");
      free(compressed_data);
      fclose(outfile);
      return EXIT_FAILURE;
    }
  }

  if (write_png_chunk("IDAT", compressed_data, compressed_data_size, outfile,
                      user_options->apply_format_hacks,
                      false) != EXIT_SUCCESS) {
    printf("Failed to write destination png file '%s' (IDAT)\n",
           user_options->png_path);
    free(compressed_data);
    fclose(outfile);
    return EXIT_FAILURE;
  }

  free(compressed_data);

  if (!user_options->apply_format_hacks) {
    // Write end (IEND) chunk
    if (write_png_chunk("IEND", NULL, 0, outfile, false, false) !=
        EXIT_SUCCESS) {
      printf("Failed to write destination png file '%s' (IEND)\n",
             user_options->png_path);
      fclose(outfile);
      return EXIT_FAILURE;
    }
  }

  compression_statistics->png_size = ftell(outfile);

  fclose(outfile);

  return EXIT_SUCCESS;
}

void print_compression_statistics(
    COMPRESSION_STATISTICS *compression_statistics) {
  printf("Embedded image has %s\n", compression_statistics->multi_row_image
                                        ? "multiple rows"
                                        : "single row");
  printf("Input Javascript size: %lu bytes\n",
         compression_statistics->javascript_size);
  printf("Output PNG file size: %li bytes\n", compression_statistics->png_size);
  printf("PNG is %3.2f percent of javascript\n",
         compression_statistics->png_size /
             (float)compression_statistics->javascript_size * 100.0f);
}

void print_usage_information() {
  printf("Usage: zopfli-pnginator [options] infile.js outfile.png.html\n");
  printf("\n");
  printf("Options:\n");
  printf("%s: Use standard zlib deflate instead of zopfli.\n", NO_ZOPFLI);
  printf("%s[number]: Number of zopfli iterations. More ", ZOPFLI_ITERATIONS);
  printf("iterations take\n  more time but can provide slightly better ");
  printf("compression. Default is 10.\n");
  printf("%s: Do not use block splitting.\n", NO_BLOCK_SPLITTING);
  printf("%s: Do not apply PNG format hacks (omit IEND ", NO_FORMAT_HACKS);
  printf("chunk, custom chunk\n  overflowing in CRC32, IDAT chunk w/o ");
  printf("CRC32).\n");
  printf("%s: Do not show statistics.\n", NO_STATISTICS);
}

void process_command_line(USER_OPTIONS *user_options, int argc, char *argv[]) {
  if (argc < 3) {
    print_usage_information();
    return;
  }

  for (int i = 1; i < argc; i++) {
    if (strncmp(argv[i], NO_ZOPFLI, strlen(NO_ZOPFLI)) == 0) {
      user_options->no_zopfli = true;
      continue;
    }

    if (strncmp(argv[i], ZOPFLI_ITERATIONS, strlen(ZOPFLI_ITERATIONS)) == 0) {
      char iterations[strlen(argv[i]) - strlen(ZOPFLI_ITERATIONS)];
      strncpy(iterations, argv[i] + strlen(ZOPFLI_ITERATIONS),
              sizeof(iterations));
      user_options->zopfli_iterations = atoi(iterations);
      continue;
    }

    if (strncmp(argv[i], NO_BLOCK_SPLITTING, strlen(NO_BLOCK_SPLITTING)) == 0) {
      user_options->no_blocksplitting = true;
      continue;
    }

    if (strncmp(argv[i], NO_FORMAT_HACKS, strlen(NO_FORMAT_HACKS)) == 0) {
      user_options->apply_format_hacks = false;
      continue;
    }

    if (strncmp(argv[i], NO_STATISTICS, strlen(NO_STATISTICS)) == 0) {
      user_options->no_statistics = true;
      continue;
    }

    if (user_options->javascript_path == NULL) {
      user_options->javascript_path = argv[i];
    } else {
      user_options->png_path = argv[i];
    }
  }
}

int main(int argc, char *argv[]) {
  printf("zopfli-pnginator\n\n");

  USER_OPTIONS user_options = {NULL, NULL, false, 10, false, true, false};
  process_command_line(&user_options, argc, argv);
  if (user_options.javascript_path == NULL || user_options.png_path == NULL) {
    return EXIT_FAILURE;
  }

  char *javascript = read_text_file(user_options.javascript_path);
  if (javascript == NULL) {
    return EXIT_FAILURE;
  }

  COMPRESSION_STATISTICS compression_statistics;
  IMAGE *image =
      embbed_javascript_in_image(javascript, &compression_statistics);

  free(javascript);

  int status =
      write_image_as_png(image, &user_options, &compression_statistics);

  free(image->data);
  free(image);

  if (status != EXIT_FAILURE && !user_options.no_statistics) {
    print_compression_statistics(&compression_statistics);
  }

  return status;
}

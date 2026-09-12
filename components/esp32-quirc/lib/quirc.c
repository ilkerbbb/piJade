/* quirc -- QR-code recognition library
 * Copyright (C) 2010-2012 Daniel Beer <dlbeer@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
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

#include <stdlib.h>
#include <string.h>
#include "quirc_internal.h"

const char *quirc_version(void)
{
  return "1.0";
}

//static struct quirc _q;
struct quirc *quirc_new(void)
{
  struct quirc *q = d_malloc(sizeof(*q));

  if (!q)
    return NULL;

  memset(q, 0, sizeof(*q));
  return q;
}

void quirc_destroy(struct quirc *q)
{
  if (q->image)
    if (q->image)
      free(q->image);
  if (sizeof(*q->image) != sizeof(*q->pixels))
    if (q->pixels)
      free(q->pixels);
  /* BBB-AIRGAP: released with the buffers it was sized alongside.  It is zeroed at both ends
   * of its life (quirc_resize() below, threshold() in identify.c), so there is nothing left
   * here to wipe first.
   */
  if (q->row_average)
    free(q->row_average);

  if (q)
    free(q);
}

//static quirc_pixel_t img_buf[320*240];
int quirc_resize(struct quirc *q, int w, int h)
{
  if (q->image)
  {
    free(q->image);
  }
  uint8_t *new_image = ps_malloc(w * h);

  if (!new_image)
    return -1;

  if (sizeof(*q->image) != sizeof(*q->pixels))
  { //should gray, 1==1
    size_t new_size = w * h * sizeof(quirc_pixel_t);
    if (q->pixels)
      free(q->pixels);
    quirc_pixel_t *new_pixels = ps_malloc(new_size);
    if (!new_pixels)
    {
      free(new_image);
      return -1;
    }
    q->pixels = new_pixels;
  }
  /* BBB-AIRGAP: threshold() reads and writes one int per column of the row it is on, and that
   * scratch used to be a variable length array in its row loop.  It is allocated here instead,
   * from the same width as the image, and out of internal memory rather than PSRAM because the
   * loop touches it once per pixel.  Taken last so that a failure frees only what this call
   * itself allocated; callers treat the -1 as fatal (main/qrscan.c:36 and :41).
   *
   * It is zeroed here and again by threshold() before that function returns, which together
   * mean it never holds readable data outside a scan: recycled heap can carry the bytes of
   * whatever was freed before it, and it is released without a wipe.
   */
  int *const new_row_average = d_malloc((size_t)w * sizeof(*new_row_average));
  if (!new_row_average)
  {
    free(new_image);
    return -1;
  }
  memset(new_row_average, 0, (size_t)w * sizeof(*new_row_average));
  if (q->row_average)
    free(q->row_average);
  q->row_average = new_row_average;
  q->image = new_image;
  q->w = w;
  q->h = h;
  return 0;
}

int quirc_count(const struct quirc *q)
{
  return q->num_grids;
}

static const char *const error_table[] = {
    [QUIRC_SUCCESS] = "Success",
    [QUIRC_ERROR_INVALID_GRID_SIZE] = "Invalid grid size",
    [QUIRC_ERROR_INVALID_VERSION] = "Invalid version",
    [QUIRC_ERROR_FORMAT_ECC] = "Format data ECC failure",
    [QUIRC_ERROR_DATA_ECC] = "ECC failure",
    [QUIRC_ERROR_UNKNOWN_DATA_TYPE] = "Unknown data type",
    [QUIRC_ERROR_DATA_OVERFLOW] = "Data overflow",
    [QUIRC_ERROR_DATA_UNDERFLOW] = "Data underflow"};

const char *quirc_strerror(quirc_decode_error_t err)
{
  if (err >= 0 && err < sizeof(error_table) / sizeof(error_table[0]))
    return error_table[err];

  return "Unknown error";
}

/* merge.c - Functions which actually combine and manipulate GIF image data.
   Copyright (C) 1997-8 Eddie Kohler, eddietwo@lcs.mit.edu
   This file is part of gifsicle.

   Gifsicle is free software; you can copy, distribute, or alter it at will, as
   long as this notice is kept intact and this source code is made available.
   Hypo(pa)thetical commerical developers are asked to write the author a note,
   which might make his day. There is no warranty, express or implied. */

#include "gifsicle.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <string.h>

/* First merging stage: Mark the used colors in all colormaps. */

void
unmark_colors(Gif_Colormap *gfcm)
{
  int i;
  if (gfcm)
    for (i = 0; i < gfcm->ncol; i++)
      gfcm->col[i].haspixel = 0;
}


void
unmark_colors_2(Gif_Colormap *gfcm)
{
  int i;
  for (i = 0; i < gfcm->ncol; i++)
    gfcm->col[i].pixel = 256;
}


static void
mark_used_colors(Gif_Image *gfi, Gif_Colormap *gfcm)
{
  Gif_Color *col = gfcm->col;
  int ncol = gfcm->ncol;
  byte have[256];
  int i, j, total;
  
  /* Only mark colors until we've seen all of them. The total variable keeps
     track of how many we've seen. have[i] is true if we've seen color i */
  for (i = 0; i < ncol; i++)
    have[i] = 0;
  for (i = ncol; i < 256; i++)
    have[i] = 1;
  total = 256 - ncol;
  
  /* Loop over every pixel (until we've seen all colors) */
  for (j = 0; j < gfi->height && total < 256; j++) {
    byte *data = gfi->img[j];
    for (i = 0; i < gfi->width; i++, data++)
      if (!have[*data]) {
	have[*data] = 1;
	total++;
      }
  }
  
  /* Mark the colors we've found */
  for (i = 0; i < ncol; i++)
    col[i].haspixel = have[i];
  
  /* Mark the transparent color specially. Its `haspixel' value should be 2 if
     transparency was used, or get rid of transparency if it wasn't used */
  if (gfi->transparent >= 0 && gfi->transparent < ncol
      && have[gfi->transparent])
    col[gfi->transparent].haspixel = 2;
  else
    gfi->transparent = -1;
}


int
find_color_index(Gif_Color *c, int nc, Gif_Color *color)
{
  int index;
  for (index = 0; index < nc; index++)
    if (GIF_COLOREQ(&c[index], color))
      return index;
  return -1;
}


int
merge_colormap_if_possible(Gif_Colormap *dest, Gif_Colormap *src)
{
  Gif_Color *srccol = src->col;
  Gif_Color *destcol = dest->col;
  int ndestcol = dest->ncol;
  int i, x;
  int trivial_map = 1;
  
  for (i = 0; i < src->ncol; i++)
    if (srccol[i].haspixel == 1) {
      int mapto = srccol[i].pixel >= ndestcol ? -1 : srccol[i].pixel;
      if (mapto == -1)
	mapto = find_color_index(destcol, ndestcol, &srccol[i]);
      if (mapto == -1 && ndestcol < 256) {
	/* add the color */
	mapto = ndestcol;
	destcol[mapto] = srccol[i];
	ndestcol++;
      }
      if (mapto == -1)
	/* check for a pure-transparent color */
	for (x = 0; x < ndestcol; x++)
	  if (destcol[x].haspixel == 2) {
	    mapto = x;
	    destcol[mapto] = srccol[i];
	    break;
	  }
      if (mapto == -1) {
	/* local colormap required */
	if (warn_local_colormaps == 1) {
	  warning("too many colors, had to use some local colormaps");
	  warning("  (you may want to try `--colors 256')");
	  warn_local_colormaps = 2;
	}
	return 0;
      }
      
      if (mapto != i)
	trivial_map = 0;
      assert(mapto >= 0 && mapto < ndestcol);
      srccol[i].pixel = mapto;
      destcol[mapto].haspixel = 1;
      
    } else if (srccol[i].haspixel == 2)
      /* a dedicated transparent color; if trivial_map & at end of colormap
         insert it with haspixel == 2. (strictly not necessary; we do it to
	 try to keep the map trivial.) */
      if (trivial_map && i == ndestcol) {
	destcol[ndestcol] = srccol[i];
	ndestcol++;
      }
  
  dest->ncol = ndestcol;
  return 1;
}


void
merge_stream(Gif_Stream *dest, Gif_Stream *src, int no_comments)
{
  assert(dest->global);
  
  if (src->global)
    unmark_colors_2(src->global);
  
  if (dest->loopcount < 0)
    dest->loopcount = src->loopcount;
  
  if (src->comment && !no_comments) {
    if (!dest->comment) dest->comment = Gif_NewComment();
    merge_comments(dest->comment, src->comment);
  }
}


void
merge_comments(Gif_Comment *destc, Gif_Comment *srcc)
{
  int i;
  for (i = 0; i < srcc->count; i++)
    Gif_AddComment(destc, srcc->str[i], srcc->len[i]);
}


Gif_Image *
merge_image(Gif_Stream *dest, Gif_Stream *src, Gif_Image *srci)
{
  Gif_Colormap *imagecm;
  Gif_Color *imagecol;
  int delete_imagecm = 0;
  int islocal;
  int i;
  Gif_Colormap *localcm = 0;
  Gif_Colormap *destcm = dest->global;
  
  byte map[256];		/* map[input pixel value] == output pixval */
  int trivial_map = 1;		/* does the map take input pixval --> the same
				   pixel value for all colors in the image? */
  byte used[256];		/* used[output pixval K] == 1 iff K was used
				   in the image */
  
  Gif_Image *desti;
  
  /* mark colors that were actually used in this image */
  islocal = srci->local != 0;
  imagecm = islocal ? srci->local : src->global;
  if (!imagecm)
    fatal_error("no global or local colormap for source image");
  imagecol = imagecm->col;
  
  mark_used_colors(srci, imagecm);
  
  /* Merge the colormap */
  if (!merge_colormap_if_possible(dest->global, imagecm)) {
    /* Need a local colormap. */
    int ncol = 0;
    destcm = localcm = Gif_NewFullColormap(0, 256);
    for (i = 0; i < imagecm->ncol; i++)
      if (imagecol[i].haspixel) {
	imagecol[i].pixel = ncol;
	localcm->col[ ncol++ ] = imagecol[i];
      }
    localcm->ncol = ncol;
  }
  
  /* Create `map' (map[old_pixel_value] == new_pixel_value) */
  for (i = 0; i < 256; i++)
    map[i] = used[i] = 0;
  for (i = 0; i < imagecm->ncol; i++)
    if (imagecol[i].haspixel == 1) {
      map[i] = imagecol[i].pixel;
      if (map[i] != i) trivial_map = 0;
      used[map[i]] = 1;
    }
  
  /* Decide on a transparent index */
  if (srci->transparent >= 0) {
    int found_transparent = -1;
    
    /* try to keep the map trivial -- prefer same transparent index */
    if (trivial_map && !used[srci->transparent])
      found_transparent = srci->transparent;
    else
      for (i = destcm->ncol - 1; i >= 0; i--)
	if (!used[i])
	  found_transparent = i;
    
    if (found_transparent < 0) {
      Gif_Color *c;
      found_transparent = destcm->ncol++;
      c = &destcm->col[found_transparent];
      *c = imagecol[srci->transparent];
      assert(c->haspixel == 2);
    }
    
    map[srci->transparent] = found_transparent;
    if (srci->transparent != found_transparent) trivial_map = 0;
  }
  
  assert(destcm->ncol <= 256);
  /* Make the new image. */
  desti = Gif_NewImage();
  
  desti->identifier = Gif_CopyString(srci->identifier);
  if (srci->transparent > -1)
    desti->transparent = map[srci->transparent];
  desti->delay = srci->delay;
  desti->disposal = srci->disposal;
  desti->left = srci->left;
  desti->top = srci->top;
  desti->interlace = srci->interlace;
  
  desti->width = srci->width;
  desti->height = srci->height;
  desti->local = localcm;
  
  if (srci->comment) {
    desti->comment = Gif_NewComment();
    merge_comments(desti->comment, srci->comment);
  }
  
  Gif_CreateUncompressedImage(desti);
  
  {
    int i, j;
    
    if (trivial_map)
      for (j = 0; j < desti->height; j++)
	memcpy(desti->img[j], srci->img[j], desti->width);
    
    else
      for (j = 0; j < desti->height; j++) {
	byte *srcdata = srci->img[j];
	byte *destdata = desti->img[j];
	for (i = 0; i < desti->width; i++, srcdata++, destdata++)
	  *destdata = map[*srcdata];
      }
  }
  
  Gif_AddImage(dest, desti);
  if (delete_imagecm)
    Gif_DeleteColormap(imagecm);
  return desti;  
}

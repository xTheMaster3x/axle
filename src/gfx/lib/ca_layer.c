#include "ca_layer.h"
#include <std/kheap.h>
#include "gfx.h"
#include "rect.h"
#include <std/math.h>
#include <std/memory.h>

void layer_teardown(ca_layer* layer) {
	if (!layer) return;

	kfree(layer->raw);
	kfree(layer);
}

ca_layer* create_layer(Size size) {
	ca_layer* ret = (ca_layer*)kmalloc(sizeof(ca_layer));
	ret->size = size;
	ret->raw = (uint8_t*)kmalloc(size.width * size.height * gfx_bpp());
	ret->alpha = 1.0;
	return ret;
}

void blit_layer_alpha_fast(ca_layer* dest, ca_layer* src, Rect copy_frame) {
	//for every pixel in dest, calculate what the pixel should be based on 
	//dest's pixel, src's pixel, and the alpha
	
	//offset into dest that we start writing
	uint8_t* dest_row_start = dest->raw + (rect_min_y(copy_frame) * dest->size.width * gfx_bpp()) + (rect_min_x(copy_frame) * gfx_bpp());
	//data from source to write to dest
	uint8_t* row_start = src->raw;
	
	for (int i = 0; i < copy_frame.size.height; i++) {
		uint8_t* dest_px = dest_row_start;
		uint8_t* row_px = row_start;

		for (int j = 0; j < copy_frame.size.width; j++) {
			//R
			*dest_px = (*dest_px + *row_px++) / 2;
			dest_px++;
			//G
			*dest_px = (*dest_px + *row_px++) / 2;
			dest_px++;
			//B
			*dest_px = (*dest_px + *row_px++) / 2;
			dest_px++;
		}

		//next iteration, start at the next row
		dest_row_start += (dest->size.width * gfx_bpp());
		row_start += (src->size.width * gfx_bpp());
	}
}

void blit_layer_alpha(ca_layer* dest, ca_layer* src, Rect copy_frame) {
	//for every pixel in dest, calculate what the pixel should be based on 
	//dest's pixel, src's pixel, and the alpha
	
	if (src->alpha == 0.5) {
		blit_layer_alpha_fast(dest, src, copy_frame);
		return;
	}
		
	//offset into dest that we start writing
	uint8_t* dest_row_start = dest->raw + (rect_min_y(copy_frame) * dest->size.width * gfx_bpp()) + (rect_min_x(copy_frame) * gfx_bpp());
	//data from source to write to dest
	uint8_t* row_start = src->raw;
	
	//multiply by 100 so we can use fixed point math
	int alpha = (1 - src->alpha) * 256;
	//alpha = abs(alpha);
	//precalculate inverse alpha
	//int inv = 100 - alpha;

	for (int i = 0; i < copy_frame.size.height; i++) {
		uint8_t* dest_px = dest_row_start;
		uint8_t* row_px = row_start;

		for (int j = 0; j < copy_frame.size.width; j++) {
			//TODO fix this code
			//yellow shifted
			//maybe we're dropping the first color byte?
			uint32_t* wide_dest = (uint32_t*)dest_px;
			uint32_t* wide_row = (uint32_t*)row_px;

			uint32_t rb = *wide_row & 0xFF00FF;
			uint32_t g = *wide_row & 0x00FF00;
			rb += ((*wide_dest & 0xFF00FF) - rb) * alpha >> 8;
			g += ((*wide_dest & 0x00FF00) - g) * alpha >> 8;
			*wide_dest = (rb & 0xFF00FF)  | (g & 0x00FF00);

			dest_px += 3;
			row_px += 3;

			//below works, but is 10FPS slower than above
			/*
			//R component
			*dest_px = ((*dest_px * alpha) + (*row_px * inv)) / 100;
			dest_px++;
			row_px++;

			//G component
			*dest_px = ((*dest_px * alpha) + (*row_px * inv)) / 100;
			dest_px++;
			row_px++;

			//B component
			*dest_px = ((*dest_px * alpha) + (*row_px * inv)) / 100;
			dest_px++;
			row_px++;
			*/
		}

		//next iteration, start at the next row
		dest_row_start += (dest->size.width * gfx_bpp());
		row_start += (src->size.width * gfx_bpp());
	}
}

void blit_layer(ca_layer* dest, ca_layer* src, Coordinate origin) {
	Rect copy_frame = rect_make(origin, src->size);
	//make sure we don't write outside dest's frame
	rect_min_x(copy_frame) = MAX(0, rect_min_x(copy_frame));
	rect_min_y(copy_frame) = MAX(0, rect_min_y(copy_frame));
	if (rect_max_x(copy_frame) >= dest->size.width) {
		double overhang = rect_max_x(copy_frame) - dest->size.width;
		copy_frame.size.width -= overhang;
	}
	if (rect_max_y(copy_frame) >= dest->size.height) {
		double overhang = rect_max_y(copy_frame) - dest->size.height;
		copy_frame.size.height -= overhang;
	}

	if (src->alpha >= 1.0) {
		//best case, we can just copy rows directly from src to dest
		//copy row by row
		
		//offset into dest that we start writing
		uint8_t* dest_row_start = dest->raw + (rect_min_y(copy_frame) * dest->size.width * gfx_bpp()) + (rect_min_x(copy_frame) * gfx_bpp());
		//data from source to write to dest
		uint8_t* row_start = src->raw;
		for (int i = 0; i < copy_frame.size.height; i++) {
			memcpy(dest_row_start, row_start, copy_frame.size.width * gfx_bpp());

			dest_row_start += (dest->size.width * gfx_bpp());
			row_start += (src->size.width * gfx_bpp());
		}
	}
	else if (src->alpha <= 0) {
		//do nothing
		return;
	}
	else {
		blit_layer_alpha(dest, src, copy_frame);
	}
}


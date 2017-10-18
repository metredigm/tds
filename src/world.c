#include "world.h"
#include "log.h"
#include "memory.h"
#include "engine.h"

#include <GLXW/glxw.h>
#include <string.h>

/*
 * TODO : world data is currently loaded into a huge buffer in engine.c and then later copied through tds_world_load
 *
 * this is a colossal waste of heap memory and probably brutal on smaller machines especially those lacking in RAM
 * also, with the new world representation we cannot have dynamically sized worlds and they will always gigantic
 * unless we want to develop the engine to only support computers with ~4*(2^28)^2/(1024^3) ~= 262144 terabytes of RAM we'll
 * need to take a different approach --
 *
 * fortunately it's simple; we need to pass the FD from the loader into the world as a part of the segment generation process
 * and segment generation will be as simple as it was before if not even better. also we won't be using a 
 * stupid amount of memory anymore. the yxml loader in engine.c however is an incredible hack right now and difficult to read.
 */

static void _tds_world_generate_hblocks(struct tds_world* ptr);
static void _tds_world_generate_segments(struct tds_world* ptr);

struct tds_world* tds_world_create(void) {
	struct tds_world* output = tds_malloc(sizeof *output);

	output->block_list_head = output->block_list_tail = 0;
	output->buffer = 0;

	output->quadtree = NULL;

	return output;
}

void tds_world_free(struct tds_world* ptr) {
	if (ptr->block_list_head) {
		struct tds_world_hblock* cur = ptr->block_list_head, *tmp = 0;

		while (cur) {
			tmp = cur->next;
			tds_vertex_buffer_free(cur->vb);
			tds_free(cur);
			cur = tmp;
		}
	}

	struct tds_world_segment* head = ptr->segment_list, *cur = NULL;

	while (head) {
		cur = head->next;
		tds_free(head);
		head = cur;
	}

	if (ptr->segment_vb) {
		tds_vertex_buffer_free(ptr->segment_vb);
	}

	if (ptr->quadtree) {
		tds_quadtree_free(ptr->quadtree);
	}

	tds_free(ptr);
}

void tds_world_init(struct tds_world* ptr, int width, int height) {
	if (ptr->buffer) {
		for (int i = 0; i < height; ++i) {
			tds_free(ptr->buffer[i]);
		}

		tds_free(ptr->buffer);
	}

	ptr->buffer = tds_malloc(sizeof ptr->buffer[0] * height);

	ptr->width = width;
	ptr->height = height;

	for (int y = 0; y < height; ++y) {
		ptr->buffer[y] = tds_malloc(sizeof ptr->buffer[0][0] * width);

		for (int x = 0; x < width; ++x) {
			ptr->buffer[y][x] = 0;
		}
	}
}

void tds_world_load(struct tds_world* ptr, const uint8_t* block_buffer, int width, int height) {
	tds_logf(TDS_LOG_DEBUG, "Initializing world structure with size %d by %d\n", width, height);

	tds_world_init(ptr, width, height);

	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			ptr->buffer[y][x] = block_buffer[y * width + x];
		}
	}

	_tds_world_generate_hblocks(ptr);
	_tds_world_generate_segments(ptr);
}

void tds_world_save(struct tds_world* ptr, uint8_t* block_buffer, int width, int height) {
	/* Simply copying the buffer. The buffer will _always_ be up to date. */

	if (width != ptr->width || height != ptr->height) {
		tds_logf(TDS_LOG_CRITICAL, "World size mismatch.\n");
		return;
	}

	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			block_buffer[y * width + x] = ptr->buffer[y][x];
		}
	}
}

void tds_world_set_block(struct tds_world* ptr, int x, int y, uint8_t block) {
	if (x >= ptr->width || x < 0 || y >= ptr->height || y < 0) {
		tds_logf(TDS_LOG_WARNING, "World index out of bounds.\n");
		return;
	}

	if (ptr->buffer[y][x] != block) {
		ptr->buffer[y][x] = block;

		_tds_world_generate_hblocks(ptr); /* This is not the most efficient way to do this, but it really shouldn't matter with small worlds. */
		_tds_world_generate_segments(ptr); /* This is the worst. Likely a huge bottleneck. */
	}

	/* If worlds end up not being small for some reason, we can regenerate only the block which the target resided in along with it's neighbors. */
	/* This shouldn't be called often anyway, so.. not a big deal. */
}

uint8_t tds_world_get_block(struct tds_world* ptr, int x, int y) {
	return ptr->buffer[y][x];
}

static void _tds_world_generate_hblocks(struct tds_world* ptr) {
	/* Perhaps one of the more important functions : regenerate the horizontally reduced blocks */

	if (ptr->block_list_head) {
		/* There are already blocks here. Into the trash! */
		struct tds_world_hblock* cur = ptr->block_list_head, *tmp = 0;

		while (cur) {
			tds_vertex_buffer_free(cur->vb);
			tmp = cur->next;
			tds_free(cur);
			cur = tmp;
		}

		ptr->block_list_head = ptr->block_list_tail = NULL;
	}

	for (int y = 0; y < ptr->height; ++y) {
		uint8_t cur_type = 0;
		int block_length = 0, block_x = -1;
		struct tds_world_hblock* tmp_block = NULL;

		for (int x = 0; x < ptr->width; ++x) {
			if (ptr->buffer[y][x] != cur_type) {
				if (block_length > 0 && cur_type) {
					/* Extract a block. */
					tmp_block = tds_malloc(sizeof *tmp_block);

					tmp_block->next = 0;
					tmp_block->x = block_x;
					tmp_block->y = y;
					tmp_block->w = block_length;
					tmp_block->id = cur_type;

					if (ptr->block_list_tail) {
						ptr->block_list_tail->next = tmp_block;
					} else {
						ptr->block_list_head = tmp_block;
					}

					ptr->block_list_tail = tmp_block;
				}

				cur_type = ptr->buffer[y][x];
				block_x = x;
				block_length = 1;
			} else {
				block_length++;
			}
		}

		if (!cur_type) {
			continue; /* The last block is air, no reason to generate it */
		}

		/* We also extract a block afterwards, or we'll miss some */
		/* Extract a block. */
		tmp_block = tds_malloc(sizeof *tmp_block);

		tmp_block->next = 0;
		tmp_block->x = block_x;
		tmp_block->y = y;
		tmp_block->w = block_length;
		tmp_block->id = cur_type;

		if (ptr->block_list_tail) {
			ptr->block_list_tail->next = tmp_block;
		} else {
			ptr->block_list_head = tmp_block;
		}

		ptr->block_list_tail = tmp_block;
	}

	/* At this time we will also generate VBOs for each hblock, so that tds_render doesn't have to. (and insert the quadtree) */

	if (ptr->quadtree) {
		tds_quadtree_free(ptr->quadtree);
	}

	ptr->quadtree = tds_quadtree_create(-(ptr->width + 1.0f) * TDS_WORLD_BLOCK_SIZE / 2.0f, (ptr->width + 1.0f) * TDS_WORLD_BLOCK_SIZE / 2.0f, (ptr->height + 0.5f) * TDS_WORLD_BLOCK_SIZE / 2.0f, -(ptr->height + 1.0f) * TDS_WORLD_BLOCK_SIZE / 2.0f); 

	struct tds_world_hblock* hb_cur = ptr->block_list_head;
	while (hb_cur) {
		struct tds_vertex vert_list[] = {
			{ -hb_cur->w * TDS_WORLD_BLOCK_SIZE / 2.0f, TDS_WORLD_BLOCK_SIZE / 2.0f, 0.0f, 0.0f, 1.0f },
			{ hb_cur->w * TDS_WORLD_BLOCK_SIZE / 2.0f, -TDS_WORLD_BLOCK_SIZE / 2.0f, 0.0f, hb_cur->w, 0.0f },
			{ hb_cur->w * TDS_WORLD_BLOCK_SIZE / 2.0f, TDS_WORLD_BLOCK_SIZE / 2.0f, 0.0f, hb_cur->w, 1.0f },
			{ -hb_cur->w * TDS_WORLD_BLOCK_SIZE / 2.0f, TDS_WORLD_BLOCK_SIZE / 2.0f, 0.0f, 0.0f, 1.0f },
			{ hb_cur->w * TDS_WORLD_BLOCK_SIZE / 2.0f, -TDS_WORLD_BLOCK_SIZE / 2.0f, 0.0f, hb_cur->w, 0.0f },
			{ -hb_cur->w * TDS_WORLD_BLOCK_SIZE / 2.0f, -TDS_WORLD_BLOCK_SIZE / 2.0f, 0.0f, 0.0f, 0.0f },
		};

		float render_x = TDS_WORLD_BLOCK_SIZE * (hb_cur->x - ptr->width / 2.0f + (hb_cur->w) / 2.0f);
		float render_y = TDS_WORLD_BLOCK_SIZE * (hb_cur->y - ptr->height / 2.0f + 0.5f);

		float block_left = render_x - hb_cur->w / 2.0f * TDS_WORLD_BLOCK_SIZE;
		float block_right = render_x + hb_cur->w / 2.0f * TDS_WORLD_BLOCK_SIZE;
		float block_top = render_y + TDS_WORLD_BLOCK_SIZE / 2.0f;
		float block_bottom = render_y - TDS_WORLD_BLOCK_SIZE / 2.0f;

		tds_quadtree_insert(ptr->quadtree, block_left, block_right, block_top, block_bottom, hb_cur);

		hb_cur->vb = tds_vertex_buffer_create(vert_list, 6, GL_TRIANGLES);

		hb_cur = hb_cur->next;
	}
}

int tds_world_get_overlap_fast(struct tds_world* ptr, struct tds_object* obj, float* x, float* y, float* w, float* h, int flag_req, int flag_or, int flag_not) {
	/* Another important function. Intersection testing with axis-aligned objects. */

	float obj_left = obj->x - obj->cbox_width / 2.0f;
	float obj_right = obj->x + obj->cbox_width / 2.0f;
	float obj_bottom = obj->y - obj->cbox_height / 2.0f;
	float obj_top = obj->y + obj->cbox_height / 2.0f;

	if (obj->angle) {
		tds_logf(TDS_LOG_WARNING, "The target object is not axis-aligned. Using a wider bounding box than normal to accommadate.\n");

		float diagonal = sqrtf(pow(obj->cbox_width, 2) + pow(obj->cbox_height, 2)) / 2.0f;

		obj_left = obj->x - diagonal;
		obj_right = obj->x + diagonal;
		obj_bottom = obj->y - diagonal;
		obj_top = obj->y + diagonal;
	}

	/* The world block coordinates will be treated as centers. The block at [0, 0] will be centered on the origin. */
	struct tds_world_hblock* cblock = ptr->block_list_head;

	while (cblock) {
		float cblock_left = (cblock->x - ptr->width / 2.0f) * TDS_WORLD_BLOCK_SIZE;
		float cblock_right = (cblock->x + cblock->w - ptr->width / 2.0f) * TDS_WORLD_BLOCK_SIZE;
		float cblock_top = (cblock->y + 1.0f - ptr->height / 2.0f) * TDS_WORLD_BLOCK_SIZE;
		float cblock_bottom = (cblock->y - ptr->height / 2.0f) * TDS_WORLD_BLOCK_SIZE;

		if (obj_left > cblock_right) {
			cblock = cblock->next;
			continue;
		}

		if (obj_right < cblock_left) {
			cblock = cblock->next;
			continue;
		}

		if (obj_top < cblock_bottom) {
			cblock = cblock->next;
			continue;
		}

		if (obj_bottom > cblock_top) {
			cblock = cblock->next;
			continue;
		}

		int flags = tds_block_map_get(tds_engine_global->block_map_handle, cblock->id).flags;

		if ((flags & flag_req) != flag_req) {
			cblock = cblock->next;
			continue;
		}

		if (!(flags & flag_or)) {
			cblock = cblock->next;
			continue;
		}

		if (flags & flag_not) {
			cblock = cblock->next;
			continue;
		}

		if (x) {
			*x = (cblock_left + cblock_right) / 2.0f;
		}

		if (y) {
			*y = (cblock_top + cblock_bottom) / 2.0f;
		}

		if (w) {
			*w = cblock_right - cblock_left;
		}

		if (h) {
			*h = cblock_top - cblock_bottom;
		}

		return flags;
	}

	return 0;
}

void _tds_world_generate_segments(struct tds_world* ptr) {
	struct tds_world_segment* head = ptr->segment_list, *cur = NULL;
	struct tds_world_hblock* cur_block = ptr->block_list_head;

	while (head) {
		cur = head->next;
		tds_free(head);
		head = cur;
	}

	if (ptr->segment_vb) {
		tds_vertex_buffer_free(ptr->segment_vb);
	}

	ptr->segment_list = NULL;
	ptr->segment_vb = NULL;

	tds_logf(TDS_LOG_DEBUG, "Starting redundant segment generation phase.\n");

	while (cur_block) {
		/* we can't add segments solely based on hblocks as they can tend to "overlap" in strange ways which can affect the lighting engine */
		int flags = tds_block_map_get(tds_engine_global->block_map_handle, cur_block->id).flags;

		if (flags & TDS_BLOCK_TYPE_NOLIGHT || !(flags & TDS_BLOCK_TYPE_SOLID)) {
			cur_block = cur_block->next;
			continue;
		}

		for (int i = 0; i < cur_block->dim.x; ++i) {
			float block_left = (x  - ptr->width / 2.0f) * TDS_WORLD_BLOCK_SIZE;
			float block_right = (x + 1.0f - ptr->width / 2.0f) * TDS_WORLD_BLOCK_SIZE;
			float block_top = (y + 1.0f - ptr->height / 2.0f) * TDS_WORLD_BLOCK_SIZE;
			float block_bottom = (y - ptr->height / 2.0f) * TDS_WORLD_BLOCK_SIZE;

			int flags_right = (x < ptr->width - 1) ? tds_block_map_get(tds_engine_global->block_map_handle, ptr->buffer[y][x + 1]).flags : 0;
			int flags_left = (x > 0) ? tds_block_map_get(tds_engine_global->block_map_handle, ptr->buffer[y][x - 1]).flags : 0;
			int flags_top = (y < ptr->height - 1) ? tds_block_map_get(tds_engine_global->block_map_handle, ptr->buffer[y + 1][x]).flags : 0;
			int flags_bottom = (y > 0) ? tds_block_map_get(tds_engine_global->block_map_handle, ptr->buffer[y - 1][x]).flags : 0;

			if ((flags_right & TDS_BLOCK_TYPE_NOLIGHT || !(flags_right & TDS_BLOCK_TYPE_SOLID)) && !(flags & (TDS_BLOCK_TYPE_RTSLOPE | TDS_BLOCK_TYPE_RBSLOPE))) {
				/* Out-facing right segment. */
				cur = tds_malloc(sizeof *cur);
				cur->x1 = block_right;
				cur->y1 = block_bottom;
				cur->x2 = block_right;
				cur->y2 = block_top;
				cur->nx = 1.0f;
				cur->ny = 0.0f;
				cur->next = ptr->segment_list;
				if (ptr->segment_list) {
					ptr->segment_list->prev = cur;
				}
				ptr->segment_list = cur;
			}

			if ((flags_left & TDS_BLOCK_TYPE_NOLIGHT || !(flags_left & TDS_BLOCK_TYPE_SOLID)) && !(flags & (TDS_BLOCK_TYPE_LTSLOPE | TDS_BLOCK_TYPE_LBSLOPE))) {
				/* Out-facing left segment. */
				cur = tds_malloc(sizeof *cur);
				cur->x1 = block_left;
				cur->y1 = block_top;
				cur->x2 = block_left;
				cur->y2 = block_bottom;
				cur->nx = -1.0f;
				cur->ny = 0.0f;
				cur->next = ptr->segment_list;
				if (ptr->segment_list) {
					ptr->segment_list->prev = cur;
				}
				ptr->segment_list = cur;
			}

			if (((flags_top & TDS_BLOCK_TYPE_NOLIGHT) || !(flags_top & TDS_BLOCK_TYPE_SOLID)) && !(flags & (TDS_BLOCK_TYPE_LTSLOPE | TDS_BLOCK_TYPE_RTSLOPE))) {
				/* Out-facing up segment. */
				cur = tds_malloc(sizeof *cur);
				cur->x1 = block_right;
				cur->y1 = block_top;
				cur->x2 = block_left;
				cur->y2 = block_top;
				cur->nx = 0.0f;
				cur->ny = 1.0f;
				cur->next = ptr->segment_list;
				if (ptr->segment_list) {
					ptr->segment_list->prev = cur;
				}
				ptr->segment_list = cur;
			}

			if ((flags_bottom & TDS_BLOCK_TYPE_NOLIGHT || !(flags_bottom & TDS_BLOCK_TYPE_SOLID)) && !(flags & (TDS_BLOCK_TYPE_LBSLOPE | TDS_BLOCK_TYPE_RBSLOPE))) {
				/* Out-facing down segment. */
				cur = tds_malloc(sizeof *cur);
				cur->x1 = block_left;
				cur->y1 = block_bottom;
				cur->x2 = block_right;
				cur->y2 = block_bottom;
				cur->nx = 0.0f;
				cur->ny = -1.0f;
				cur->next = ptr->segment_list;
				if (ptr->segment_list) {
					ptr->segment_list->prev = cur;
				}
				ptr->segment_list = cur;
			}

			if (flags & TDS_BLOCK_TYPE_LTSLOPE) {
				cur = tds_malloc(sizeof *cur);
				cur->x1 = block_left;
				cur->y1 = block_bottom;
				cur->x2 = block_right;
				cur->y2 = block_top;
				cur->nx = -1.0f;
				cur->ny = 1.0f;
				cur->next = ptr->segment_list;
				if (ptr->segment_list) {
					ptr->segment_list->prev = cur;
				}
				ptr->segment_list = cur;
			}

			if (flags & TDS_BLOCK_TYPE_RTSLOPE) {
				cur = tds_malloc(sizeof *cur);
				cur->x1 = block_right;
				cur->y1 = block_bottom;
				cur->x2 = block_left;
				cur->y2 = block_top;
				cur->nx = 1.0f;
				cur->ny = 1.0f;
				cur->next = ptr->segment_list;
				if (ptr->segment_list) {
					ptr->segment_list->prev = cur;
				}
				ptr->segment_list = cur;
			}

			if (flags & TDS_BLOCK_TYPE_RBSLOPE) {
				cur = tds_malloc(sizeof *cur);
				cur->x1 = block_left;
				cur->y1 = block_bottom;
				cur->x2 = block_right;
				cur->y2 = block_top;
				cur->nx = 1.0f;
				cur->ny = -1.0f;
				cur->next = ptr->segment_list;
				if (ptr->segment_list) {
					ptr->segment_list->prev = cur;
				}
				ptr->segment_list = cur;
			}

			if (flags & TDS_BLOCK_TYPE_LBSLOPE) {
				cur = tds_malloc(sizeof *cur);
				cur->x1 = block_left;
				cur->y1 = block_top;
				cur->x2 = block_right;
				cur->y2 = block_bottom;
				cur->nx = -1.0f;
				cur->ny = -1.0f;
				cur->next = ptr->segment_list;
				if (ptr->segment_list) {
					ptr->segment_list->prev = cur;
				}
				ptr->segment_list = cur;
			}
		}

		cur_block = cur_block->next;
	}

	tds_logf(TDS_LOG_DEBUG, "Starting linear reduction phase.\n");

	/* We will use a brute-force approach to keep the code small as this is not going to be called very often. */
	int complete = 1, iterations = 0;

	while (complete) {
		complete = 0;

		struct tds_world_segment* reduction_cur = NULL, *reduction_target = ptr->segment_list, *tmp = NULL;

		/* In this pass, we consider all segments for reduction with other segments. If any actual reduction is done, we set the flag for another pass. */
		tds_logf(TDS_LOG_DEBUG, "Starting linear reduction subphase iteration %d\n", iterations);

		while (reduction_target) {
			reduction_cur = ptr->segment_list;

			while (reduction_cur) {
				/* Three nested while loops for linear reduction and n^2 traversal of a linked list. Crazy. */

				if (reduction_cur == reduction_target) {
					reduction_cur = reduction_cur->next;
					continue;
				}

				if (!tds_vec2_cmpi(reduction_cur->n, 0, 1) && !tds_vec2_cmpi(reduction_target, 0, 1)) {
					/* Both of these lines are horizontal, and each segment's x2 is less than the x1. */

					if (reduction_cur->a.y != reduction_target->a.y) {
						reduction_cur = reduction_cur->next;
						continue;
					}

					if (reduction_cur->b.x == reduction_target->a.x) {
						/* reduction_cur is on the right and we can reduce. */
						reduction_target->a.x = reduction_cur->a.x;

						if (reduction_cur->prev) {
							reduction_cur->prev->next = reduction_cur->next;
						} else {
							ptr->segment_list = reduction_cur->next;
						}

						if (reduction_cur->next) {
							reduction_cur->next->prev = reduction_cur->prev;
						}

						tmp = reduction_cur->next;
						tds_free(reduction_cur);
						reduction_cur = tmp;
						complete = 1;
						continue;
					}

					if (reduction_target->b.x == reduction_cur->a.x) {
						/* reduction_target is on the right and we can reduce. */
						reduction_target->b.x = reduction_cur->b.x;

						if (reduction_cur->prev) {
							reduction_cur->prev->next = reduction_cur->next;
						} else {
							ptr->segment_list = reduction_cur->next;
						}

						if (reduction_cur->next) {
							reduction_cur->next->prev = reduction_cur->prev;
						}

						tmp = reduction_cur->next;
						tds_free(reduction_cur);
						reduction_cur = tmp;
						complete = 1;
						continue;
					}
				}

				if (!tds_vec2_cmpi(reduction_cur->n, 0, -1) && !tds_vec2_cmpi(reduction_target, 0, -1)) {
					/* Both of these lines are horizontal, and each segment's x2 is greater than the x1. */

					if (reduction_cur->a.y != reduction_target->a.y) {
						reduction_cur = reduction_cur->next;
						continue;
					}

					if (reduction_cur->a.x == reduction_target->b.x) {
						/* reduction_cur is on the right and we can reduce. */
						reduction_target->b.x = reduction_cur->b.x;

						if (reduction_cur->prev) {
							reduction_cur->prev->next = reduction_cur->next;
						} else {
							ptr->segment_list = reduction_cur->next;
						}

						if (reduction_cur->next) {
							reduction_cur->next->prev = reduction_cur->prev;
						}

						tmp = reduction_cur->next;
						tds_free(reduction_cur);
						reduction_cur = tmp;
						complete = 1;
						continue;
					}

					if (reduction_target->a.x == reduction_cur->b.x) {
						/* reduction_target is on the right and we can reduce. */
						reduction_target->a.x = reduction_cur->a.x;

						if (reduction_cur->prev) {
							reduction_cur->prev->next = reduction_cur->next;
						} else {
							ptr->segment_list = reduction_cur->next;
						}

						if (reduction_cur->next) {
							reduction_cur->next->prev = reduction_cur->prev;
						}

						tmp = reduction_cur->next;
						tds_free(reduction_cur);
						reduction_cur = tmp;
						complete = 1;
						continue;
					}
				}

				if (!tds_vec2_cmpi(reduction_cur->n, 1, 0) && !tds_vec2_cmpi(reduction_target, 1, 0)) {
					/* Both of these lines are vertical, and each segment's y2 is greater than the y1. */

					if (reduction_cur->a.x != reduction_target->a.x) {
						reduction_cur = reduction_cur->next;
						continue;
					}

					if (reduction_cur->b.y == reduction_target->a.y) {
						/* reduction_cur is on the bottom and we can reduce. */
						reduction_target->a.y = reduction_cur->a.y;

						if (reduction_cur->prev) {
							reduction_cur->prev->next = reduction_cur->next;
						} else {
							ptr->segment_list = reduction_cur->next;
						}

						if (reduction_cur->next) {
							reduction_cur->next->prev = reduction_cur->prev;
						}

						tmp = reduction_cur->next;
						tds_free(reduction_cur);
						reduction_cur = tmp;
						complete = 1;
						continue;
					}

					if (reduction_target->b.y == reduction_cur->a.y) {
						/* reduction_target is on the bottom and we can reduce. */
						reduction_target->b.y = reduction_cur->b.y;

						if (reduction_cur->prev) {
							reduction_cur->prev->next = reduction_cur->next;
						} else {
							ptr->segment_list = reduction_cur->next;
						}

						if (reduction_cur->next) {
							reduction_cur->next->prev = reduction_cur->prev;
						}

						tds_free(reduction_cur);
						tmp = reduction_cur->next;
						tds_free(reduction_cur);
						reduction_cur = tmp;
						complete = 1;
						continue;
					}
				}

				if (!tds_vec2_cmpi(reduction_cur->n, -1, 0) && !tds_vec2_cmpi(reduction_target, -1, 0)) {
					/* Both of these lines are vertical, and each segment's y2 is less than the y1. */

					if (reduction_cur->a.x != reduction_target->a.x) {
						reduction_cur = reduction_cur->next;
						continue;
					}

					if (reduction_cur->b.y == reduction_target->a.y) {
						/* reduction_cur is on the top and we can reduce. */
						reduction_target->a.y = reduction_cur->a.y;

						if (reduction_cur->prev) {
							reduction_cur->prev->next = reduction_cur->next;
						} else {
							ptr->segment_list = reduction_cur->next;
						}

						if (reduction_cur->next) {
							reduction_cur->next->prev = reduction_cur->prev;
						}

						tmp = reduction_cur->next;
						tds_free(reduction_cur);
						reduction_cur = tmp;
						complete = 1;
						continue;
					}

					if (reduction_target->b.y == reduction_cur->a.y) {
						/* reduction_target is on the top and we can reduce. */
						reduction_target->b.y = reduction_cur->b.y;

						if (reduction_cur->prev) {
							reduction_cur->prev->next = reduction_cur->next;
						} else {
							ptr->segment_list = reduction_cur->next;
						}

						if (reduction_cur->next) {
							reduction_cur->next->prev = reduction_cur->prev;
						}

						tmp = reduction_cur->next;
						tds_free(reduction_cur);
						reduction_cur = tmp;
						complete = 1;
						continue;
					}
				}

				reduction_cur = reduction_cur->next;
			}

			reduction_target = reduction_target->next;
		}

		++iterations;
	}

	tds_logf(TDS_LOG_DEBUG, "Finished linear reduction phase in %d passes.\n", iterations);

	cur = ptr->segment_list;

	/* We iterate through the segment list twice. Once to grab the size of the list, and another to copy the segment data to the temporary buffer. */

	int segment_count = 0;

	while (cur) {
		segment_count++;
		cur = cur->next;
	}

	struct tds_vertex* segment_verts = tds_malloc(segment_count * sizeof(struct tds_vertex) * 2);
	cur = ptr->segment_list;

	int i = 0;

	while (cur) {
		/* the vertex buffer should be appropriately sized by floats, we will deal with translating it later */
		/* TODO : segment vbos still rely on floating-point niceness, consider moving VBOs to be relative and translating them later in the render process */

		struct tds_vertex verts[] = {
			{(float) cur->a.x / 16.0f, (float) cur->a.y / 16.0f, 0.0f, cur->n.x, cur->n.y}, /* We hide the normal in the texcoords, saving some time. */
			{(float) cur->b.x / 16.0f, (float) cur->b.y, 0.0f, cur->n.x, cur->n.y},
		};

		if (i >= segment_count) {
			break;
		}

		segment_verts[2 * i] = verts[0];
		segment_verts[2 * i + 1] = verts[1];

		cur = cur->next;

		++i;
	}

	ptr->segment_vb = tds_vertex_buffer_create(segment_verts, segment_count * 2, GL_LINES);
	tds_free(segment_verts);
}

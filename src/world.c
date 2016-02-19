#include "world.h"
#include "log.h"
#include "memory.h"
#include "engine.h"

#include <GLXW/glxw.h>
#include <string.h>

static void _tds_world_generate_hblocks(struct tds_world* ptr);
static void _tds_world_generate_segments(struct tds_world* ptr);

struct tds_world* tds_world_create(void) {
	struct tds_world* output = tds_malloc(sizeof *output);

	output->width = output->height = 0;
	output->block_list_head = output->block_list_tail = 0;
	output->buffer = 0;

	return output;
}

void tds_world_free(struct tds_world* ptr) {
	if (ptr->buffer) {
		for (int i = 0; i < ptr->height; ++i) {
			tds_free(ptr->buffer[i]);
		}

		tds_free(ptr->buffer);
	}

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
		tds_vertex_buffer_free(head->vb);
		tds_free(head);
		head = cur;
	}

	struct tds_world_block_info* cur_info = ptr->block_info, *tmp = NULL;

	while (cur_info) {
		tmp = cur_info->next;
		tds_free(cur_info);
		cur_info = tmp;
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

void tds_world_push_info(struct tds_world* ptr, struct tds_world_block_info info) {
	struct tds_world_block_info* new = tds_malloc(sizeof *new);
	memcpy(new, &info, sizeof *new);

	new->next = ptr->block_info;
	ptr->block_info = new;
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

	/* At this time we will also generate VBOs for each hblock, so that tds_render doesn't have to. */

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

		hb_cur->vb = tds_vertex_buffer_create(vert_list, 6, GL_TRIANGLES);
		hb_cur = hb_cur->next;
	}
}

int tds_world_get_overlap_fast(struct tds_world* ptr, struct tds_object* obj, float* x, float* y, float* w, float* h) {
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
		if (!tds_block_map_get(tds_engine_global->block_map_handle, cblock->id).solid) {
			cblock = cblock->next;
			continue;
		}

		float cblock_left = (cblock->x - 0.5f - ptr->width / 2.0f) * TDS_WORLD_BLOCK_SIZE;
		float cblock_right = (cblock->x + cblock->w - 0.5f - ptr->width / 2.0f) * TDS_WORLD_BLOCK_SIZE;
		float cblock_top = (cblock->y + 0.5f - ptr->height / 2.0f) * TDS_WORLD_BLOCK_SIZE;
		float cblock_bottom = (cblock->y - ptr->height / 2.0f - 0.5f) * TDS_WORLD_BLOCK_SIZE;

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

		if (x) {
			*x = (cblock->x - 0.5f - ptr->width / 2.0f) * TDS_WORLD_BLOCK_SIZE;
		}

		if (y) {
			*y = (cblock->y + 0.5f - ptr->height / 2.0f) * TDS_WORLD_BLOCK_SIZE;
		}

		if (w) {
			*w = cblock->w * TDS_WORLD_BLOCK_SIZE;
		}

		if (h) {
			*h = TDS_WORLD_BLOCK_SIZE;
		}

		return 1;
	}

	return 0;
}

void _tds_world_generate_segments(struct tds_world* ptr) {
	struct tds_world_segment* head = ptr->segment_list, *cur = NULL;

	while (head) {
		cur = head->next;
		tds_vertex_buffer_free(head->vb);
		tds_free(head);
		head = cur;
	}

	ptr->segment_list = NULL;

	tds_logf(TDS_LOG_DEBUG, "Starting redundant segment generation phase.\n");

	/* We start by generating a large list of segments without any reduction. */
	for (int x = 0; x < ptr->width; ++x) {
		for (int y = 0; y < ptr->height; ++y) {
			/* For each index, we only consider out-facing edges at the current location. */
			if (!ptr->buffer[y][x]) {
				continue;
			}

			struct tds_world_block_info* cur_info = ptr->block_info;
			int flags = 0;

			while (cur_info) {
				if (cur_info->id == ptr->buffer[y][x]) {
					flags = cur_info->flags;
					break;
				}

				cur_info = cur_info->next;
			}

			if (flags & TDS_WORLD_NOLIGHT) {
				continue;
			}

			float block_left = (x - 0.5f - ptr->width / 2.0f) * TDS_WORLD_BLOCK_SIZE;
			float block_right = (x + 0.5f - ptr->width / 2.0f) * TDS_WORLD_BLOCK_SIZE;
			float block_top = (y + 0.5f - ptr->height / 2.0f) * TDS_WORLD_BLOCK_SIZE;
			float block_bottom = (y - ptr->height / 2.0f - 0.5f) * TDS_WORLD_BLOCK_SIZE;

			if (x < ptr->width - 1 && !ptr->buffer[y][x + 1] && !(flags & TDS_WORLD_RTSLOPE) && !(flags & TDS_WORLD_RBSLOPE)) {
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

			if (x > 0 && !ptr->buffer[y][x - 1] && !(flags & TDS_WORLD_LTSLOPE) && !(flags & TDS_WORLD_LBSLOPE)) {
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

			if (y < ptr->height - 1 && !ptr->buffer[y + 1][x] && !(flags & TDS_WORLD_RTSLOPE) && !(flags & TDS_WORLD_LTSLOPE)) {
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

			if (y > 0 && !ptr->buffer[y - 1][x] && !(flags & TDS_WORLD_RBSLOPE) && !(flags & TDS_WORLD_LBSLOPE)) {
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

			if (flags & TDS_WORLD_LTSLOPE) {
				cur = tds_malloc(sizeof *cur);
				cur->x1 = block_left;
				cur->y1 = block_bottom;
				cur->x2 = block_right;
				cur->y2 = block_top;
				cur->nx = 0.0f;
				cur->ny = -1.0f;
				cur->next = ptr->segment_list;
				if (ptr->segment_list) {
					ptr->segment_list->prev = cur;
				}
				ptr->segment_list = cur;
			}

			if (flags & TDS_WORLD_RTSLOPE) {
				cur = tds_malloc(sizeof *cur);
				cur->x1 = block_right;
				cur->y1 = block_bottom;
				cur->x2 = block_left;
				cur->y2 = block_top;
				cur->nx = 0.0f;
				cur->ny = -1.0f;
				cur->next = ptr->segment_list;
				if (ptr->segment_list) {
					ptr->segment_list->prev = cur;
				}
				ptr->segment_list = cur;
			}

			if (flags & TDS_WORLD_RBSLOPE) {
				cur = tds_malloc(sizeof *cur);
				cur->x1 = block_left;
				cur->y1 = block_bottom;
				cur->x2 = block_right;
				cur->y2 = block_top;
				cur->nx = 0.0f;
				cur->ny = -1.0f;
				cur->next = ptr->segment_list;
				if (ptr->segment_list) {
					ptr->segment_list->prev = cur;
				}
				ptr->segment_list = cur;
			}

			if (flags & TDS_WORLD_LBSLOPE) {
				cur = tds_malloc(sizeof *cur);
				cur->x1 = block_left;
				cur->y1 = block_top;
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
		}
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

				if (reduction_cur->nx == 0.0f && reduction_target->nx == 0.0f && reduction_cur->ny == 1.0f && reduction_target->ny == 1.0f) {
					/* Both of these lines are horizontal, and each segment's x2 is less than the x1. */

					if (reduction_cur->y1 != reduction_target->y1) {
						reduction_cur = reduction_cur->next;
						continue;
					}

					if (reduction_cur->x2 == reduction_target->x1) {
						/* reduction_cur is on the right and we can reduce. */
						reduction_target->x1 = reduction_cur->x1;

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

					if (reduction_target->x2 == reduction_cur->x1) {
						/* reduction_target is on the right and we can reduce. */
						reduction_target->x2 = reduction_cur->x2;

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

				if (reduction_cur->nx == 0.0f && reduction_target->nx == 0.0f && reduction_cur->ny == -1.0f && reduction_target->ny == -1.0f) {
					/* Both of these lines are horizontal, and each segment's x2 is greater than the x1. */

					if (reduction_cur->y1 != reduction_target->y1) {
						reduction_cur = reduction_cur->next;
						continue;
					}

					if (reduction_cur->x1 == reduction_target->x2) {
						/* reduction_cur is on the right and we can reduce. */
						reduction_target->x2 = reduction_cur->x2;

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

					if (reduction_target->x1 == reduction_cur->x2) {
						/* reduction_target is on the right and we can reduce. */
						reduction_target->x1 = reduction_cur->x1;

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

				if (reduction_cur->nx == 1.0f && reduction_target->nx == 1.0f && reduction_cur->ny == 0.0f && reduction_target->ny == 0.0f) {
					/* Both of these lines are vertical, and each segment's y2 is greater than the y1. */

					if (reduction_cur->x1 != reduction_target->x1) {
						reduction_cur = reduction_cur->next;
						continue;
					}

					if (reduction_cur->y2 == reduction_target->y1) {
						/* reduction_cur is on the bottom and we can reduce. */
						reduction_target->y1 = reduction_cur->y1;

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

					if (reduction_target->y2 == reduction_cur->y1) {
						/* reduction_target is on the bottom and we can reduce. */
						reduction_target->y2 = reduction_cur->y2;

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

				if (reduction_cur->nx == -1.0f && reduction_target->nx == -1.0f && reduction_cur->ny == 0.0f && reduction_target->ny == 0.0f) {
					/* Both of these lines are vertical, and each segment's y2 is less than the y1. */

					if (reduction_cur->x1 != reduction_target->x1) {
						reduction_cur = reduction_cur->next;
						continue;
					}

					if (reduction_cur->y2 == reduction_target->y1) {
						/* reduction_cur is on the top and we can reduce. */
						reduction_target->y1 = reduction_cur->y1;

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

					if (reduction_target->y2 == reduction_cur->y1) {
						/* reduction_target is on the top and we can reduce. */
						reduction_target->y2 = reduction_cur->y2;

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

	while (cur) {
		struct tds_vertex verts[] = {
			{cur->x1, cur->y1, 0.0f, cur->nx, cur->ny}, /* We hide the normal in the texcoords, saving some time. */
			{cur->x2, cur->y2, 0.0f, cur->nx, cur->ny},
		};

		cur->vb = tds_vertex_buffer_create(verts, sizeof verts / sizeof verts[0], GL_LINES);
		cur = cur->next;
	}
}

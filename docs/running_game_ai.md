# Running Game AI on Procedural World (Data-Oriented)

This document outlines a minimal, data-oriented implementation plan for a running game where many characters move with basic AI on a procedurally generated world. It aligns with the mu data structures you referenced:
- `mu_bulk_storage` for entity lifetime/handles
- `mu_multi_index` for fast key -> many lookup
- `mu_chunked_u32_array` for per-entity dynamic lists

## Goals

- Spawn a procedural world (grid/tiles or chunks).
- Spawn many characters that run forward with basic AI avoidance.
- Keep all simulation data in SoA, update in linear loops.
- Use indices for spatial queries and chunk membership.

## Core Data Layout (SoA)

All hot data stays in arrays-of-struct fields (SoA) for cache-friendly iteration.

```
typedef struct SimWorld
{
	// Entity storage
	mu_bulk_storage entities;      // slot = per-entity metadata

	// Transform + motion
	float* pos_x;
	float* pos_y;
	float* pos_z;
	float* vel_x;
	float* vel_y;
	float* vel_z;
	float* heading_yaw;

	// Gameplay tags
	uint8_t* is_runner;
	uint8_t* alive;

	// Animation state
	uint32_t* anim_index;
	float*    anim_time;
	float*    anim_speed;

	// Spatial partitioning
	mu_multi_index cell_to_entity; // key=cell_id -> values=entity_id
	uint32_t* cell_node_index;     // per-entity: node index in cell_to_entity

	// Chunk membership (optional)
	mu_chunked_u32_pool  chunk_pool;
	mu_chunked_u32_array* chunk_entities; // one array per chunk

	uint32_t capacity;
} SimWorld;
```

Notes:
- `entities` holds slot ownership and weak-handle validation.
- `cell_to_entity` provides O(1) insert/remove per entity when cell changes.
- `chunk_entities` is optional if you want a higher-level chunk system.

## Procedural World (Simple + Expandable)

Start with a tiled grid and extend to chunks.

1) **Tile function** (height/solid):
```
float tile_height(int x, int z)
{
	// Use noise or a simple height function
	return 0.0f;
}
```

2) **Chunk generation**:
- Chunk size: 32x32 tiles
- Cache generated chunks in a hash map keyed by chunk coords.
- When camera/players move, ensure nearby chunks are generated.

3) **Collision/ground**:
- Characters stick to `tile_height` or a simple plane.

## Spatial Indexing with mu_multi_index

Use `mu_multi_index` as a fast cell lookup.

### Cell key encoding
```
static uint64_t cell_key(int cx, int cz)
{
	return ((uint64_t)(uint32_t)cx << 32) | (uint32_t)cz;
}
```

### Update entity cell membership
```
void update_entity_cell(SimWorld* w, uint32_t id, int cx, int cz)
{
	uint32_t node = w->cell_node_index[id];
	if(node != MU_MULTI_INDEX_NONE)
		mu_multi_index_remove(&w->cell_to_entity, node);

	uint64_t key = cell_key(cx, cz);
	w->cell_node_index[id] = mu_multi_index_add(&w->cell_to_entity, key, id);
}
```

### Query neighbors in same cell
```
void visit_cell(const SimWorld* w, int cx, int cz, mu_multi_index_visit_fn fn, void* user)
{
	uint64_t key = cell_key(cx, cz);
	mu_multi_index_visit_key(&w->cell_to_entity, key, fn, user);
}
```

This is enough for basic separation/avoidance.

## Character AI (Basic Running)

Minimal AI loop:

1) **Desired direction**: forward vector (e.g., +Z)
2) **Avoidance**: steer away from nearby runners in same cell
3) **Integrate**: velocity -> position

```
void update_ai(SimWorld* w, float dt)
{
	for(uint32_t id = 0; id < w->capacity; ++id)
	{
		if(!w->alive[id] || !w->is_runner[id])
			continue;

		float fx = 0.0f;
		float fz = 1.0f; // forward

		// Basic avoidance: sample same cell
		// Use mu_multi_index to scan neighbors, accumulate separation

		float vx = fx * 3.0f; // run speed
		float vz = fz * 3.0f;

		w->vel_x[id] = vx;
		w->vel_z[id] = vz;
	}
}
```

## Movement + Grounding

```
void integrate_motion(SimWorld* w, float dt)
{
	for(uint32_t id = 0; id < w->capacity; ++id)
	{
		if(!w->alive[id])
			continue;

		w->pos_x[id] += w->vel_x[id] * dt;
		w->pos_z[id] += w->vel_z[id] * dt;
		w->pos_y[id] = 0.0f; // flat ground for now
	}
}
```

## Animation Update (Run/Idle)

Map animation clips by name once at load time, then per entity:

```
if(speed > 0.1f)
	anim_index = run_clip;
else
	anim_index = idle_clip;

anim_time += dt * anim_speed;
```

## Entity Lifecycle (mu_bulk_storage)

### Spawn
```
uint32_t id = mu_bulk_storage_alloc(&w->entities);
w->alive[id] = 1;
w->is_runner[id] = 1;
```

### Destroy
```
mu_bulk_storage_free(&w->entities, id);
w->alive[id] = 0;
```

### Weak handles
Use `mu_weak_handle` to reference entities safely from gameplay and UI.

## Putting It Together (Frame Order)

```
// 1) input / camera
// 2) procedural chunk update
// 3) spatial index update (cell changes)
// 4) AI update (runs + avoidance)
// 5) movement integration
// 6) animation update
// 7) render
```

## Minimal First Step (Practical)

1) Create `SimWorld` with SoA arrays and a fixed capacity.
2) Spawn N runners with random XZ.
3) Simple AI: everyone runs toward +Z.
4) Integrate movement + clamp Y.
5) Use `mu_multi_index` to keep a cell index for future avoidance.

## Scaling Notes

- If you have thousands of runners, use larger cells to reduce neighbor scans.
- With `mu_chunked_u32_array`, you can store per-chunk entity lists for faster traversal.
- `mu_multi_index` is ideal for O(1) removal when you already know the node index.

## Suggested Next Enhancements

- Add avoidance force with neighbor sampling.
- Add obstacle tiles from procedural world.
- Add LOD or culling by chunk for rendering.
- Add per-entity randomization (speed variance, lane shifting).

#ifndef SEEKDB_CUVS_BRIDGE_H
#define SEEKDB_CUVS_BRIDGE_H
#ifdef __cplusplus
extern "C" {
#endif
/* One-shot: build a CAGRA index over base (n x dim, row-major f32) on the GPU and
 * search queries (nq x dim), writing top-k uint32 neighbor ids to out_ids
 * (length nq*topk). Returns 0 on success, non-zero on error. */
int seekdb_cuvs_cagra_knn(const float* base, long n, long dim,
                           const float* query, long nq, long topk,
                           unsigned int* out_ids);

/* Build-once / search-many (used by seekdb's ob_vsag_adaptor GPU path).
 *  - seekdb_cuvs_build: builds a CAGRA index on the GPU from base (n x dim,
 *    row-major f32) and returns an opaque handle (NULL on error). cuVS copies
 *    the dataset to the device, so `base` may be freed after this returns.
 *  - seekdb_cuvs_search: runs top-k search for nq queries (row-major f32, dim
 *    cols), writing uint32 row offsets to out_ids[nq*topk] and (if non-NULL) L2
 *    distances to out_dist[nq*topk]. Returns 0 on success.
 *  - seekdb_cuvs_free: releases the handle.
 * All ROCm/cuVS state lives inside the .so; seekdb only ever sees these C symbols. */
void* seekdb_cuvs_build(const float* base, long n, long dim);
int   seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                         unsigned int* out_ids, float* out_dist);
void  seekdb_cuvs_free(void* handle);
int seekdb_cuvs_serialize(void* handle, const char* filename);
void* seekdb_cuvs_deserialize(const char* filename, long n, long dim);

/* The compact snapshot.
 *
 * The cuVS snapshot contains the graph AND the dataset, but the dataset is
 * written into the seekdb index stream anyway (the raw vectors), so with the
 * cuVS format every vector of the index is written twice.  The compact
 * snapshot contains the graph only, and the vectors are taken from the
 * index stream, which halves the size of the index stream
 * (612 MB -> 368 MB for 250K x 256) and the time it takes to write it.
 *
 *   seekdb_cuvs_deserialize_ex: reads the compact snapshot (the graph) and
 *   rebuilds the CAGRA index from it and the vectors that the caller already
 *   has in memory.
 *
 * The format of the file is
 *   magic  "OBGPU03" (8 bytes)
 *   n      int64
 *   dim    int32
 *   degree int32
 *   graph  n * degree uint32 (row major)
 */
void* seekdb_cuvs_deserialize_ex(const char* filename, long n, long dim,
                                 const float* vectors);
#ifdef __cplusplus
}
#endif
#endif

#ifndef __CASTLE_EXTENT_H__
#define __CASTLE_EXTENT_H__

#include "castle.h"

/**
 * Extent dirtylist structure.
 */
typedef struct c_ext_dirtylist {
    spinlock_t                  lock;           /**< Dirtylist lock.                    */
    int                         count;          /**< Elements on dirtylist.             */
    struct rb_root              rb_root;        /**< Dirtylist RB root.                 */
    struct list_head            list;           /**< castle_cache_extent_dirtylist pos. */
} c_ext_dirtylist_t;

c_ext_id_t          castle_extent_alloc                     (c_rda_type_t   rda_type,
                                                             da_id_t        da_id,
                                                             c_ext_type_t   ext_type, 
                                                             c_chk_cnt_t    chk_cnt);
void                castle_extent_free                      (c_ext_id_t     ext_id);
int                 castle_extent_exists                    (c_ext_id_t     ext_id);
void                castle_extent_mark_live                 (c_ext_id_t     ext_id, 
                                                             da_id_t        da_id);
void*               castle_extent_get                       (c_ext_id_t     ext_id);
void                castle_extent_put                       (c_ext_id_t     ext_id);
uint32_t            castle_extent_kfactor_get               (c_ext_id_t     ext_id);
c_chk_cnt_t         castle_extent_size_get                  (c_ext_id_t     ext_id);
/* Sets @chunks to all physical chunks holding the logical chunks from offset */
uint32_t            castle_extent_map_get                   (void*          ext_p,
                                                             c_chk_t        offset,
                                                             c_disk_chk_t  *chk_maps,
                                                             int            rw);
c_ext_dirtylist_t  *castle_extent_dirtylist_get             (c_ext_id_t     ext_id);
void                castle_extent_dirtylist_put             (c_ext_id_t     ext_id);


struct castle_extents_superblock* castle_extents_super_block_get (void);
void                              castle_extents_super_block_put (int dirty);
c_ext_id_t                        castle_extent_sup_ext_init     (struct castle_slave *cs);
void                              castle_extent_sup_ext_close    (struct castle_slave *cs);

void                castle_extents_stats_writeback (c_mstore_t *stats_mstore);
void                castle_extents_stat_read       (struct castle_slist_entry *mstore_entry);

int                 castle_extents_create                   (void);
int                 castle_extents_read                     (void);
int                 castle_extents_read_complete            (void);
int                 castle_extents_writeback                (void);
int                 castle_extents_restore                  (void);
int                 castle_extents_init                     (void);
void                castle_extents_fini                     (void);
int                 castle_extents_rebuild_init             (void);
void                castle_extents_rebuild_fini             (void);
void                castle_extents_rebuild_wake             (void);
void                castle_extents_rebuild_startup_check    (int need_rebuild);
int                 castle_extents_slave_scan               (uint32_t uuid);
signed int          castle_extent_ref_cnt_get               (c_ext_id_t);

#endif /* __CASTLE_EXTENT_H__ */

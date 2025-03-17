/*
 * Generic hashmap manipulation functions
 *
 * Originally by Elliot C Back - http://elliottback.com/wp/hashmap-implementation-in-c/
 *
 * Modified by Pete Warden to fix a serious performance problem, support strings as keys
 * and removed thread synchronization - http://petewarden.typepad.com
 */
#ifndef HASHMAP_H
#define HASHMAP_H

#include "migration/meow_hash_x64_aesni.h"
#define MAP_MISSING -3  /* No such element */
#define MAP_FULL -2 	/* Hashmap is full */
#define MAP_OMEM -1 	/* Out of Memory */
#define MAP_OK 0 	/* OK */

/*
 * any_t is a pointer.  This allows you to put arbitrary structures in
 * the hashmap.
 */
typedef void *any_t;

/*
 * PFany is a pointer to a function that can take two any_t arguments
 * and return an integer. Returns status code..
 */
typedef int (*PFany)(any_t, any_t);

/*
 * map_t is a pointer to an internally maintained data structure.
 * Clients of this package do not need to know how hashmaps are
 * represented.  They see and manipulate only map_t's.
 */
typedef any_t map_t;

/*
 * Return an empty hashmap. Returns NULL if empty.
*/
//extern map_t hashmap_new(unsigned int initial_size);
//extern map_t hashmap_new(void);
//extern map_t hashmap_new(void *ptr, unsigned long n_entries);
extern map_t hashmap_new(void *ptr, void *data_ptr, unsigned long size, int init);

/*
 * Add an element to the hashmap. Return MAP_OK or MAP_OMEM.
 */
extern int hashmap_put(map_t in, meow_u128 *key, any_t value);

/*
 * Get an element from the hashmap. Return MAP_OK or MAP_MISSING.
 */
extern int hashmap_get(map_t in, meow_u128 *key, any_t *arg);
int hashmap_get_and_put(map_t in, meow_u128 *key, any_t *arg_get, any_t arg_put);
int hashmap_get_and_put_static(map_t in, meow_u128 *key, unsigned int *idx_get, unsigned int **idx_put);

/*
 * Remove an element from the hashmap. Return MAP_OK or MAP_MISSING.
 */
extern int hashmap_remove(map_t in, meow_u128 *key);

/*
 * Free the hashmap
 */
extern void hashmap_free(map_t in);

/*
 * Get the current size of a hashmap
 */
extern int hashmap_length(map_t in);
extern unsigned long hashmap_size(map_t in);
unsigned long hashmap_fixed_size(map_t in);

unsigned int hashmap_element_size(void);
typedef struct page_meta {
   unsigned int refcount;
   unsigned int index;
   meow_u128 key;
} page_meta;


#endif

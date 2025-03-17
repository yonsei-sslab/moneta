/*
 * Generic map implementation.
 */
#include "hashmap_custom.h"
#include "migration/meow_hash_x64_aesni.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <sys/mman.h>
#include <sys/time.h>

#define USE_LIST
#define WARN_LIST
#define WARN_LIST_TH 8
//#undef USE_LIST
//#define INITIAL_SIZE (67108864UL) //2**26
#define MAX_CHAIN_LENGTH (8)

/* We need to keep keys and values */
typedef struct _hashmap_element{
	short in_use;
   struct _hashmap_element *next;
   meow_u128 key;
   unsigned int idx;
   //page_meta data;
	//any_t ptr;
} hashmap_element;

/* A hashmap has some maximum size and current size,
 * as well as the data to hold. */
typedef struct _hashmap_map{
	unsigned long table_size;
	unsigned long n_elements;
} hashmap_map;

typedef struct _hashmap_map_info{
   hashmap_map *m;
	hashmap_element *data;
} hashmap_map_info;

unsigned int hashmap_element_size(void) {
   return sizeof(hashmap_element);
}

/*
 * Return an empty hashmap, or NULL on failure.
 */
map_t hashmap_new(void *ptr, void *data_ptr, unsigned long size, int init) {
   hashmap_map_info *mi = malloc(sizeof(hashmap_map_info));
   assert(mi);
   if(ptr) {
      printf("Sharing hashmap %p\n", ptr);
      mi->m = (hashmap_map*)ptr;
   } else {
      printf("Allocating hashmap\n");
      mi->m = (hashmap_map*)malloc(sizeof(hashmap_map));
   }
   assert(mi->m);

   unsigned long n_entries = size / sizeof(hashmap_element);
   if(data_ptr) {
      printf("Sharing hashmap data %p\n", data_ptr);
      mi->data = (hashmap_element*)data_ptr;
      if(init) {
         printf("Init table size %ld\n", n_entries);
         mi->m->table_size = n_entries;
         memset(mi->data, 0, size);
      }
   } else {
      printf("Allocating\n");
      mi->data = (hashmap_element*) calloc(n_entries, sizeof(hashmap_element));
      //madvise(mi->data, n_entries * sizeof(hashmap_element), MADV_WILLNEED);
      mi->m->table_size = n_entries;
   }
   assert(mi->data);

   printf("hashmap %p, data %p, size %ld, entries %ld\n", mi->m, mi->data, size, size / sizeof(hashmap_element));
   if(init) {
      mi->m->n_elements = 0;
   }

	return mi;
}

#if 0
static hashmap_element *hashmap_hash_list(map_t in, meow_u128* key){
	int curr;
	//int i;

	/* Cast the hashmap */
	hashmap_map_info* mi = (hashmap_map_info *) in;

	/* If full, return immediately */
	//if(mi->m->n_elements >= (mi->m->table_size/2)) return MAP_FULL;

	/* Find the best index */
	curr = MeowU32From(*key, 0) %  mi->m->table_size;

   //if(mi->data[curr].in_use == 0)
   //   return &mi->data[curr];

   hashmap_element *next = &mi->data[curr];
   //if(next->in_use = 0 && next->next == NULL)
   //   return next;
   hashmap_element *prev = next;
   while(next != NULL) {
      if(next->in_use == 0) {
         return next;
      }
      prev = next;
      next = next->next;
   }
   next = malloc(sizeof(hashmap_element));
   //madvise(next, sizeof(hashmap_element), MADV_WILLNEED);
   next->next = NULL;
   prev->next = next;
   return next;
}

static int hashmap_hash(map_t in, meow_u128* key){
	int curr;
	int i;

	/* Cast the hashmap */
	hashmap_map_info* mi = (hashmap_map_info *) in;

	/* If full, return immediately */
	if(mi->m->n_elements >= (mi->m->table_size/2)) return MAP_FULL;

	/* Find the best index */
	curr = MeowU32From(*key, 0) %  mi->m->table_size;


	/* Linear probing */
	for(i = 0; i< MAX_CHAIN_LENGTH; i++){
		if(mi->data[curr].in_use == 0)
			return curr;

		if(mi->data[curr].in_use == 1 &&
            MeowHashesAreEqual(mi->data[curr].key, *key))
			return curr;

		curr = (curr + 1) % mi->m->table_size;
	}

	return MAP_FULL;

}
#endif


//unsigned long hgptime_found = 0;
//unsigned long hgptime_miss = 0;
int hashmap_get_and_put_static(map_t in, meow_u128 *key, unsigned int *idx_get, unsigned int **idx_put){
	unsigned int curr;
	hashmap_map_info* mi;


   //unsigned long long *h = (unsigned long long*)key;
	/* Cast the hashmap */
	mi = (hashmap_map_info *) in;

	/* Find data location */
	curr = MeowU32From(*key, 0) % mi->m->table_size;

   hashmap_element *next = &mi->data[curr];

#ifdef WARN_LIST
   unsigned int clen = 0;
#endif
   hashmap_element *prev = next;
   while(next != NULL) {
      if (next->in_use == 1 && MeowHashesAreEqual(next->key, *key)){
         *idx_get = next->idx;
         return MAP_OK;
      }
      prev = next;
      next = next->next;
#ifdef WARN_LIST
      clen++;
#endif
   }
   if(clen == 1 && prev->in_use == 0) {
      prev->key = *key;
      prev->in_use = 1;
      *idx_put = &prev->idx;
      return MAP_MISSING;
   }

   next = malloc(sizeof(hashmap_element));
   next->next = NULL;
   *idx_put = &next->idx;
   next->key = *key;
   next->in_use = 1;
   prev->next = next;

#ifdef WARN_LIST
   if(clen > WARN_LIST_TH) {
      printf("Warning: chain len %d\n", clen);
   }
#endif
	/* Not found */
	return MAP_MISSING;
}
#if 0
/*
 * Add a pointer to the hashmap with some key
 */
unsigned long hputtime = 0;
int hashmap_put(map_t in, meow_u128 *key, any_t value){
	unsigned int index;
	hashmap_map_info* mi;

   struct timeval begin, end, elapsed;
   gettimeofday(&begin, NULL);

	/* Cast the hashmap */
	mi = (hashmap_map_info *) in;
#ifdef USE_LIST
   hashmap_element *curr = hashmap_hash_list(mi, key);

    curr->ptr = value;
	//memcpy(&curr->data, value, sizeof(page_meta));
	//memcpy(&curr->data.key, key, sizeof(meow_u128));
	curr->in_use = 1;
	mi->m->n_elements++;
#else
	/* If full, return immediately */
	if(mi->m->n_elements >= mi->m->table_size) return MAP_FULL;
	/* Find a place to put our value */
   index = hashmap_hash(in, key);
   if(index == MAP_FULL) {
      return MAP_OMEM;
   }

	/* Set the data */
	//mi->data[index].data = value;
	memcpy(&mi->data[index].data, value, sizeof(page_meta));
	//memcpy(&mi->data[index].data.key, key, sizeof(meow_u128));
	mi->data[index].in_use = 1;
	mi->m->n_elements++;
#endif

   gettimeofday(&end, NULL);
   timersub(&end, &begin, &elapsed);
   hputtime += elapsed.tv_sec * 1000L * 1000L + elapsed.tv_usec;
	return MAP_OK;
}
int hashmap_get_and_put(map_t in, meow_u128 *key, any_t *arg_get, any_t arg_put){
	unsigned int curr;
	unsigned int i;
	hashmap_map_info* mi;


   unsigned long long *h = (unsigned long long*)key;
	/* Cast the hashmap */
	mi = (hashmap_map_info *) in;

	/* Find data location */
	curr = MeowU32From(*key, 0) % mi->m->table_size;

   //struct timeval begin, end, elapsed;
   //gettimeofday(&begin, NULL);
   hashmap_element *next = &mi->data[curr];
   if(next->in_use == 0) {
      next->ptr = arg_put;
      next->key = *key;
      next->in_use = 1;
      *arg_get = NULL;
      //gettimeofday(&end, NULL);
      //timersub(&end, &begin, &elapsed);
      //hgptime_miss += elapsed.tv_sec * 1000L * 1000L + elapsed.tv_usec;
      return MAP_MISSING;
   }

#ifdef WARN_LIST
   unsigned int clen = 0;
#endif
   hashmap_element *prev = next;
   while(next != NULL) {
      if (next->in_use == 1 && MeowHashesAreEqual(next->key, *key)){
         *arg_get = next->ptr;
         //gettimeofday(&end, NULL);
         //timersub(&end, &begin, &elapsed);
         //hgptime_found += elapsed.tv_sec * 1000L * 1000L + elapsed.tv_usec;
         return MAP_OK;
      }
      prev = next;
      next = next->next;
#ifdef WARN_LIST
      clen++;
#endif
   }
	*arg_get = NULL;
   next = malloc(sizeof(hashmap_element));
   //madvise(next, sizeof(hashmap_element), MADV_WILLNEED);
   next->next = NULL;
   next->ptr = arg_put;
   next->key = *key;
   next->in_use = 1;
   prev->next = next;

#ifdef WARN_LIST
   if(clen > WARN_LIST_TH) {
      printf("Warning: chain len %d\n", clen);
   }
#endif
   //gettimeofday(&end, NULL);
   //timersub(&end, &begin, &elapsed);
   //hgptime_miss += elapsed.tv_sec * 1000L * 1000L + elapsed.tv_usec;
	/* Not found */
	return MAP_MISSING;
}
/*
 * Get your pointer out of the hashmap with a key
 */
unsigned long hgettime_miss = 0;
unsigned long hgettime_found = 0;
int hashmap_get(map_t in, meow_u128 *key, any_t *arg){
	unsigned int curr;
	unsigned int i;
	hashmap_map_info* mi;


   unsigned long long *h = (unsigned long long*)key;
	/* Cast the hashmap */
	mi = (hashmap_map_info *) in;

	/* Find data location */
	curr = MeowU32From(*key, 0) % mi->m->table_size;


   struct timeval begin, end, elapsed;
   gettimeofday(&begin, NULL);
#ifdef USE_LIST
   hashmap_element *next = &mi->data[curr];
   if(next->in_use == 0 && next->next == NULL) {
      *arg = NULL;
      return MAP_MISSING;
   }

   while(next != NULL) {
      if (next->in_use == 1 && MeowHashesAreEqual(next->key, *key)){
         *arg = next->ptr;
         gettimeofday(&end, NULL);
         timersub(&end, &begin, &elapsed);
         hgettime_found += elapsed.tv_sec * 1000L * 1000L + elapsed.tv_usec;
         return MAP_OK;
      }
      next = next->next;
   }
#else
	/* Linear probing, if necessary */
	for(i = 0; i<MAX_CHAIN_LENGTH; i++){

        int in_use = mi->data[curr].in_use;
        if (in_use == 1){
            if (MeowHashesAreEqual(mi->data[curr].data.key, *key)){
                //*arg = (mi->data[curr].data);
                *arg = &(mi->data[curr].data);
                //memcpy(arg, &(mi->data[curr].data), sizeof(page_meta));
                return MAP_OK;
            }
		}

		curr = (curr + 1) % mi->m->table_size;
	}
#endif

	*arg = NULL;

   gettimeofday(&end, NULL);
   timersub(&end, &begin, &elapsed);
   hgettime_miss += elapsed.tv_sec * 1000L * 1000L + elapsed.tv_usec;
	/* Not found */
	return MAP_MISSING;
}
#endif

/*
 * Remove an element with that key from the map
 */
int hashmap_remove(map_t in, meow_u128 *key){
	//int i;
	int curr;

	hashmap_map_info* mi;

	/* Cast the hashmap */
	mi = (hashmap_map_info *) in;

	/* Find key */
	curr = MeowU32From(*key, 0) % mi->m->table_size;

#ifdef USE_LIST
   hashmap_element *next = &mi->data[curr];
   hashmap_element *prev = &mi->data[curr];
   while(next != NULL) {
      if (next->in_use == 1 && MeowHashesAreEqual(next->key, *key)){
         if(prev == next) {
            next->in_use = 0;
            next->idx = 0;
            //next->key = _mm_setzero_si128();
         } else {
            prev->next = next->next;
            free(next);
         }
         /* Reduce the n_elements */
         mi->m->n_elements--;
         return MAP_OK;
      }
      prev = next;
      next = next->next;
   }

#else
	/* Linear probing, if necessary */
	for(i = 0; i<MAX_CHAIN_LENGTH; i++){

        int in_use = mi->data[curr].in_use;
        if (in_use == 1){
            if (MeowHashesAreEqual(mi->data[curr].data.key, *key)){
                /* Blank out the fields */
                mi->data[curr].in_use = 0;
                //mi->data[curr].data = NULL;
                mi->data[curr].data.key = _mm_setzero_si128();
                /* Reduce the n_elements */
                mi->m->n_elements--;
                return MAP_OK;
            }
		}
		curr = (curr + 1) % mi->m->table_size;
	}
#endif

	/* Data not found */
	return MAP_MISSING;
}

/* Deallocate the hashmap */
void hashmap_free(map_t in){
	hashmap_map_info* mi = (hashmap_map_info*) in;
	free(mi->data);
	free(mi->m);
	free(mi);
}

/* Return the length of the hashmap */
int hashmap_length(map_t in){
	hashmap_map_info* mi = (hashmap_map_info *) in;
	if(mi->m != NULL) return mi->m->n_elements;
	else return 0;
}

unsigned long hashmap_fixed_size(map_t in) {
	hashmap_map_info* mi = (hashmap_map_info *) in;
	if(mi->m != NULL) return mi->m->table_size * sizeof(hashmap_element);
	else return 0;
}

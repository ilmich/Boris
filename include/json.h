/*
 * json.h - Thin wrapper around cJSON for uniform error handling.
 *
 * Uses cJSON_ParseWithLength for bounded parsing and provides safe
 * accessors that return defaults on missing or wrong-typed keys.
 *
 * All production code must use these wrappers exclusively; direct
 * cJSON calls are confined to this translation unit.
 */

#ifndef JSON_H
#define JSON_H

#include "cJSON.h"

/*
 * Macros: lifecycle and iteration
 *
 * json_delete(p)             - free a cJSON tree (wraps cJSON_Delete)
 * json_array_foreach(i, arr) - iterate array items (wraps cJSON_ArrayForEach)
 */
#define json_delete(p)              cJSON_Delete(p)
#define json_array_foreach(i, arr)  cJSON_ArrayForEach(i, arr)

/*
 * Parse a NUL-terminated JSON string.
 * On failure, logs an error and returns NULL.
 * The caller must json_delete() the result when done.
 */
cJSON *json_parse(const char *str);

/*
 * Parse a JSON string with a known length.
 * Safer than json_parse() when you know the buffer size.
 */
cJSON *json_parse_with_len(const char *str, size_t len);

/*
 * Return the string value of key in obj, or NULL.
 * The returned pointer is owned by obj - do not free it.
 */
const char *json_get_str(const cJSON *obj, const char *key);

/*
 * Return the integer value of key, or def on failure.
 * Also coerces numeric strings ("42") to int.
 */
int json_get_int(const cJSON *obj, const char *key, int def);

/*
 * Return the double value of key, or def on failure.
 */
double json_get_double(const cJSON *obj, const char *key, double def);

/*
 * Return 1/0 for a boolean key, or def on failure.
 */
int json_get_bool(const cJSON *obj, const char *key, int def);

/*
 * Return the sub-object or array at key, or NULL.
 * The returned pointer is owned by obj - do not free it.
 */
cJSON *json_get_obj(const cJSON *obj, const char *key);

/*
 * Return the item at index idx in array arr, or NULL.
 */
cJSON *json_get_array_item(const cJSON *arr, int idx);

/*
 * Return the number of items in array arr, or 0 on NULL.
 */
int json_get_array_size(const cJSON *arr);

/*
 * Type predicates - return non-zero if the node matches the type.
 */
int json_is_array(const cJSON *v);
int json_is_object(const cJSON *v);
int json_is_null(const cJSON *v);
int json_is_string(const cJSON *v);
int json_is_number(const cJSON *v);

/*
 * Return the raw string pointer of a string node, or NULL.
 * The returned pointer is owned by the node - do not free it.
 */
const char *json_str_value(const cJSON *v);

/*
 * Serialise obj to a compact (unformatted) JSON string.
 * The caller must free() the returned string.
 */
char *json_to_str(const cJSON *obj);

/*
 * Serialise obj to a pretty-printed JSON string.
 * The caller must free() the returned string.
 */
char *json_to_str_pretty(const cJSON *obj);

/*
 * Builders - construct JSON trees.
 * All return NULL on OOM.  Ownership follows cJSON conventions:
 * items passed to json_add_item / json_array_add are consumed.
 */
cJSON *json_create_object(void);
cJSON *json_create_array(void);

cJSON *json_add_str(cJSON *obj, const char *key, const char *val);
cJSON *json_add_int(cJSON *obj, const char *key, int val);
cJSON *json_add_double(cJSON *obj, const char *key, double val);
cJSON *json_add_bool(cJSON *obj, const char *key, int val);
cJSON *json_add_null(cJSON *obj, const char *key);
cJSON *json_add_item(cJSON *obj, const char *key, cJSON *item);
cJSON *json_array_add(cJSON *arr, cJSON *item);

/*
 * Deep-copy a node.  recurse=1 copies the entire sub-tree.
 */
cJSON *json_duplicate(const cJSON *item, int recurse);

/*
 * Remove and free the entry at key from obj.
 */
void json_delete_key(cJSON *obj, const char *key);

/*
 * Replace the string value of an existing string node in-place.
 * Returns the new string pointer on success, NULL on failure.
 */
char *json_set_str_value(cJSON *item, const char *val);

/*
 * Read an entire file into a heap-allocated, NUL-terminated buffer.
 * Returns the buffer on success (caller must free()), or NULL on failure.
 * Files larger than max_bytes are rejected.
 */
char *file_read_all(const char *path, size_t max_bytes, size_t *out_len);

#endif /* JSON_H */

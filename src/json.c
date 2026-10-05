/*
 * json.c - cJSON convenience wrappers.
 *
 * Enforces project conventions: parse errors are logged with position
 * context; missing-key accessors return typed defaults rather than NULL.
 * All direct cJSON calls in the project are funnelled through this file.
 */

#define _POSIX_C_SOURCE 200809L

#include "json.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

cJSON *json_parse(const char *str)
{
	if (!str) {
		log_error("json_parse: NULL input");
		return NULL;
	}

	const char *err_ptr = NULL;
	cJSON *root = cJSON_ParseWithOpts(str, &err_ptr, 0);
	if (!root) {
		size_t pos = err_ptr ? (size_t)(err_ptr - str) : 0;
		log_error("json_parse: parse error near position %zu: \"%.40s\"",
			  pos, err_ptr ? err_ptr : "(unknown)");
		return NULL;
	}
	return root;
}

cJSON *json_parse_with_len(const char *str, size_t len)
{
	if (!str || len == 0) {
		log_error("json_parse_with_len: NULL input or zero length");
		return NULL;
	}

	const char *err_ptr = NULL;
	cJSON *root = cJSON_ParseWithLengthOpts(str, len, &err_ptr, 0);
	if (!root) {
		size_t pos = err_ptr ? (size_t)(err_ptr - str) : 0;
		log_error("json_parse_with_len: parse error near position %zu",
			  pos);
		return NULL;
	}
	return root;
}

const char *json_get_str(const cJSON *obj, const char *key)
{
	if (!obj || !key)
		return NULL;
	const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
	if (!item)
		return NULL;
	return cJSON_GetStringValue(item);
}

int json_get_int(const cJSON *obj, const char *key, int def)
{
	if (!obj || !key)
		return def;
	const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
	if (!item)
		return def;
	if (cJSON_IsNumber(item))
		return (int)item->valuedouble;
	if (cJSON_IsString(item) && item->valuestring) {
		char *end = NULL;
		errno = 0;
		long val = strtol(item->valuestring, &end, 10);
		if (end == item->valuestring || *end != '\0' || errno != 0)
			return def;
		return (int)val;
	}
	return def;
}

double json_get_double(const cJSON *obj, const char *key, double def)
{
	if (!obj || !key)
		return def;
	const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
	if (!item)
		return def;
	if (cJSON_IsNumber(item))
		return item->valuedouble;
	return def;
}

int json_get_bool(const cJSON *obj, const char *key, int def)
{
	if (!obj || !key)
		return def;
	const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
	if (!item)
		return def;
	if (cJSON_IsTrue(item))
		return 1;
	if (cJSON_IsFalse(item))
		return 0;
	if (cJSON_IsNumber(item))
		return item->valuedouble != 0.0;
	return def;
}

cJSON *json_get_obj(const cJSON *obj, const char *key)
{
	if (!obj || !key)
		return NULL;
	return cJSON_GetObjectItemCaseSensitive(obj, key);
}

cJSON *json_get_array_item(const cJSON *arr, int idx)
{
	if (!arr)
		return NULL;
	return cJSON_GetArrayItem(arr, idx);
}

int json_get_array_size(const cJSON *arr)
{
	if (!arr)
		return 0;
	return cJSON_GetArraySize(arr);
}

int json_is_array(const cJSON *v)
{
	return cJSON_IsArray(v);
}

int json_is_object(const cJSON *v)
{
	return cJSON_IsObject(v);
}

int json_is_null(const cJSON *v)
{
	return cJSON_IsNull(v);
}

int json_is_string(const cJSON *v)
{
	return cJSON_IsString(v);
}

int json_is_number(const cJSON *v)
{
	return cJSON_IsNumber(v);
}

const char *json_str_value(const cJSON *v)
{
	if (!v)
		return NULL;
	return cJSON_GetStringValue(v);
}

char *json_to_str(const cJSON *obj)
{
	if (!obj)
		return NULL;
	return cJSON_PrintUnformatted(obj);
}

char *json_to_str_pretty(const cJSON *obj)
{
	if (!obj)
		return NULL;
	return cJSON_Print(obj);
}

cJSON *json_create_object(void)
{
	return cJSON_CreateObject();
}

cJSON *json_create_array(void)
{
	return cJSON_CreateArray();
}

cJSON *json_add_str(cJSON *obj, const char *key, const char *val)
{
	if (!obj || !key)
		return NULL;
	return cJSON_AddStringToObject(obj, key, val ? val : "");
}

cJSON *json_add_int(cJSON *obj, const char *key, int val)
{
	if (!obj || !key)
		return NULL;
	return cJSON_AddNumberToObject(obj, key, (double)val);
}

cJSON *json_add_double(cJSON *obj, const char *key, double val)
{
	if (!obj || !key)
		return NULL;
	return cJSON_AddNumberToObject(obj, key, val);
}

cJSON *json_add_bool(cJSON *obj, const char *key, int val)
{
	if (!obj || !key)
		return NULL;
	return cJSON_AddBoolToObject(obj, key, val);
}

cJSON *json_add_null(cJSON *obj, const char *key)
{
	if (!obj || !key)
		return NULL;
	return cJSON_AddNullToObject(obj, key);
}

cJSON *json_add_item(cJSON *obj, const char *key, cJSON *item)
{
	if (!obj || !key || !item)
		return NULL;
	if (!cJSON_AddItemToObject(obj, key, item))
		return NULL;
	return item;
}

cJSON *json_array_add(cJSON *arr, cJSON *item)
{
	if (!arr || !item)
		return NULL;
	if (!cJSON_AddItemToArray(arr, item))
		return NULL;
	return item;
}

cJSON *json_duplicate(const cJSON *item, int recurse)
{
	if (!item)
		return NULL;
	return cJSON_Duplicate(item, recurse);
}

void json_delete_key(cJSON *obj, const char *key)
{
	if (!obj || !key)
		return;
	cJSON_DeleteItemFromObject(obj, key);
}

char *json_set_str_value(cJSON *item, const char *val)
{
	if (!item || !val)
		return NULL;
	return cJSON_SetValuestring(item, val);
}

char *file_read_all(const char *path, size_t max_bytes, size_t *out_len)
{
	if (out_len)
		*out_len = 0;

	if (!path)
		return NULL;

	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;

	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (sz <= 0 || (size_t)sz > max_bytes) {
		fclose(f);
		return NULL;
	}

	char *buf = malloc((size_t)sz + 1);
	if (!buf) {
		fclose(f);
		return NULL;
	}

	size_t n = fread(buf, 1, (size_t)sz, f);
	buf[n] = '\0';
	fclose(f);

	if (out_len)
		*out_len = n;
	return buf;
}

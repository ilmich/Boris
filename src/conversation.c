/*
 * conversation.c - Conversation history and persistence.
 *
 * Messages are stored in a doubling dynamic array. All content strings are
 * copied on insertion; callers do not need to manage string lifetimes.
 *
 * Supports tool_calls on assistant messages and ROLE_TOOL result messages
 * as required by the OpenAI chat completions API. Serialises to/from JSON
 * for both API requests and session persistence.
 */

#define _POSIX_C_SOURCE 200809L

#include "conversation.h"
#include "boris_errors.h"
#include "logger.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <libgen.h>

/* Start with room for 8 messages. We'll grow as needed. */
#define INITIAL_CAPACITY 8

/*
 * Create a new, empty conversation.
 */
struct conversation_history *conversation_create(void)
{
	struct conversation_history *conv;

	conv = calloc(1, sizeof(struct conversation_history));
	if (!conv)
		return NULL;

	conv->messages = calloc(INITIAL_CAPACITY,
				sizeof(struct conversation_message));
	if (!conv->messages) {
		free(conv);
		return NULL;
	}

	conv->capacity = INITIAL_CAPACITY;
	conv->count = 0;
	conv->total_characters = 0;

	return conv;
}

/*
 * Free a single message's content and tool_calls.
 */
static void message_free_content(struct conversation_message *msg)
{
	free(msg->content);
	free(msg->tool_call_id);
	free(msg->tool_name);

	if (msg->tool_calls) {
		for (size_t i = 0; i < msg->tool_call_count; i++)
			free(msg->tool_calls[i].argument_json);
		free(msg->tool_calls);
	}

	msg->content = NULL;
	msg->tool_call_id = NULL;
	msg->tool_name = NULL;
	msg->tool_calls = NULL;
	msg->tool_call_count = 0;
}

static size_t message_character_count(const struct conversation_message *msg)
{
	size_t n = 0;
	if (msg->content)
		n += strlen(msg->content);
	if (msg->tool_call_id)
		n += strlen(msg->tool_call_id);
	if (msg->tool_name)
		n += strlen(msg->tool_name);
	for (size_t i = 0; i < msg->tool_call_count; i++)
		if (msg->tool_calls[i].argument_json)
			n += strlen(msg->tool_calls[i].argument_json);
	return n;
}

static void tool_call_copy(struct tool_call *dst, const struct tool_call *src)
{
	snprintf(dst->id, sizeof(dst->id), "%s", src->id);
	snprintf(dst->name, sizeof(dst->name), "%s", src->name);
	dst->argument_json = src->argument_json ? strdup(src->argument_json) : NULL;
}

/*
 * Free a conversation and all its messages.
 */
void conversation_destroy(struct conversation_history *conv)
{
	size_t i;

	if (!conv)
		return;

	for (i = 0; i < conv->count; i++)
		message_free_content(&conv->messages[i]);

	free(conv->messages);
	free(conv);
}

/* Double the message array capacity. Returns BORIS_OK or BORIS_ERROR_OUT_OF_MEMORY. */
static enum boris_error conversation_grow(struct conversation_history *conv)
{
	size_t new_capacity;
	struct conversation_message *new_messages;

	new_capacity = conv->capacity * 2;
	new_messages = realloc(conv->messages,
			       new_capacity * sizeof(struct conversation_message));
	if (!new_messages)
		return BORIS_ERROR_OUT_OF_MEMORY;

	/* Zero out the new slots */
	memset(new_messages + conv->capacity, 0,
	       (new_capacity - conv->capacity) *
		       sizeof(struct conversation_message));

	conv->messages = new_messages;
	conv->capacity = new_capacity;

	return BORIS_OK;
}

/*
 * Add a message to the conversation.
 */
enum boris_error conversation_add(struct conversation_history *conv,
				  enum message_role role,
				  const char *content)
{
	struct conversation_message *msg;

	if (!conv || !content)
		return BORIS_ERROR_BAD_ARGUMENT;

	/* Grow if needed */
	if (conv->count >= conv->capacity) {
		enum boris_error err = conversation_grow(conv);
		if (err != BORIS_OK)
			return err;
	}

	msg = &conv->messages[conv->count];
	msg->role = role;
	msg->content = strdup(content);
	if (!msg->content)
		return BORIS_ERROR_OUT_OF_MEMORY;

	msg->tool_call_id = NULL;
	msg->tool_name = NULL;
	msg->tool_calls = NULL;
	msg->tool_call_count = 0;

	conv->count++;
	conv->total_characters += message_character_count(msg);

	return BORIS_OK;
}

/*
 * Convenience wrappers.
 */
enum boris_error conversation_add_system(struct conversation_history *conv,
					 const char *content)
{
	return conversation_add(conv, MESSAGE_ROLE_SYSTEM, content);
}

enum boris_error conversation_add_user(struct conversation_history *conv,
				       const char *content)
{
	return conversation_add(conv, MESSAGE_ROLE_USER, content);
}

enum boris_error conversation_add_assistant(struct conversation_history *conv,
					    const char *content)
{
	return conversation_add(conv, MESSAGE_ROLE_ASSISTANT, content);
}

/*
 * Append an assistant message that may include tool_calls.
 */
enum boris_error conversation_add_assistant_raw(
	struct conversation_history *conv,
	const char *content,
	const struct tool_call *tool_calls,
	size_t tool_call_count)
{
	if (!conv)
		return BORIS_ERROR_BAD_ARGUMENT;

	if (conv->count >= conv->capacity) {
		enum boris_error err = conversation_grow(conv);
		if (err != BORIS_OK)
			return err;
	}

	struct conversation_message msg;
	memset(&msg, 0, sizeof(msg));
	msg.role = MESSAGE_ROLE_ASSISTANT;
	msg.content = content ? strdup(content) : NULL;
	msg.tool_call_id = NULL;
	msg.tool_name = NULL;

	if (tool_calls && tool_call_count > 0) {
		msg.tool_calls = calloc(tool_call_count,
					sizeof(struct tool_call));
		if (!msg.tool_calls) {
			free(msg.content);
			return BORIS_ERROR_OUT_OF_MEMORY;
		}
		msg.tool_call_count = tool_call_count;

		for (size_t i = 0; i < tool_call_count; i++)
			tool_call_copy(&msg.tool_calls[i], &tool_calls[i]);
	}

	conv->messages[conv->count] = msg;
	conv->count++;
	conv->total_characters += message_character_count(&conv->messages[conv->count - 1]);
	return BORIS_OK;
}

/*
 * Append an assistant message from an llm_response.
 */
enum boris_error conversation_add_assistant_response(
	struct conversation_history *conv,
	const struct llm_response *resp)
{
	if (!conv || !resp)
		return BORIS_ERROR_BAD_ARGUMENT;

	return conversation_add_assistant_raw(conv, resp->content,
					      resp->tool_calls.calls,
					      resp->tool_calls.count);
}

/*
 * Add a tool result message.
 */
enum boris_error conversation_add_tool_result(
	struct conversation_history *conv,
	const char *tool_call_id,
	const char *tool_name,
	const char *content)
{
	struct conversation_message *msg;

	if (!conv || !content)
		return BORIS_ERROR_BAD_ARGUMENT;

	if (conv->count >= conv->capacity) {
		enum boris_error err = conversation_grow(conv);
		if (err != BORIS_OK)
			return err;
	}

	msg = &conv->messages[conv->count];
	msg->role = MESSAGE_ROLE_TOOL;
	msg->content = strdup(content);
	if (!msg->content)
		return BORIS_ERROR_OUT_OF_MEMORY;

	msg->tool_call_id = tool_call_id ? strdup(tool_call_id) : NULL;
	msg->tool_name = tool_name ? strdup(tool_name) : NULL;
	msg->tool_calls = NULL;
	msg->tool_call_count = 0;

	conv->count++;
	conv->total_characters += message_character_count(msg);

	return BORIS_OK;
}

/* Rough token estimate: chars/4 + 4 per message (API role/separator overhead). */
size_t conversation_estimate_tokens(const struct conversation_history *conv)
{
	if (!conv)
		return 0;

	return (conv->total_characters / 4) + (conv->count * 4);
}

/*
 * Convert a role enum to the string the OpenAI API expects.
 */
static const char *role_to_string(enum message_role role)
{
	switch (role) {
	case MESSAGE_ROLE_SYSTEM:
		return "system";
	case MESSAGE_ROLE_USER:
		return "user";
	case MESSAGE_ROLE_ASSISTANT:
		return "assistant";
	case MESSAGE_ROLE_TOOL:
		return "tool";
	default:
		return "user";
	}
}

/*
 * Serialise the conversation to a cJSON array.
 *
 * The output format matches the OpenAI chat completions API.
 */
struct cJSON *conversation_to_json(const struct conversation_history *conv)
{
	struct cJSON *arr;
	size_t i;

	if (!conv)
		return NULL;

	arr = json_create_array();
	if (!arr)
		return NULL;

	for (i = 0; i < conv->count; i++) {
		const struct conversation_message *msg = &conv->messages[i];
		struct cJSON *msg_obj = json_create_object();
		if (!msg_obj) {
			json_delete(arr);
			return NULL;
		}

		json_add_str(msg_obj, "role", role_to_string(msg->role));

		if (msg->content) {
			json_add_str(msg_obj, "content", msg->content);
		} else {
			json_add_null(msg_obj, "content");
		}

		if (msg->role == MESSAGE_ROLE_ASSISTANT &&
		    msg->tool_calls && msg->tool_call_count > 0) {
			struct cJSON *tc_arr = json_create_array();
			if (!tc_arr) {
				json_delete(msg_obj);
				json_delete(arr);
				return NULL;
			}

			for (size_t ti = 0; ti < msg->tool_call_count; ti++) {
				const struct tool_call *tc =
					&msg->tool_calls[ti];
				struct cJSON *tc_obj = json_create_object();
				if (!tc_obj) {
					json_delete(tc_arr);
					json_delete(msg_obj);
					json_delete(arr);
					return NULL;
				}

				json_add_str(tc_obj, "id",
					     tc->id[0] ? tc->id : "");
				json_add_str(tc_obj, "type", "function");

				struct cJSON *fn = json_create_object();
				if (!fn) {
					json_delete(tc_obj);
					json_delete(tc_arr);
					json_delete(msg_obj);
					json_delete(arr);
					return NULL;
				}
				json_add_str(fn, "name",
					     tc->name[0] ? tc->name : "");
				const char *args_val =
					tc->argument_json ? tc->argument_json : "{}";
				json_add_str(fn, "arguments", args_val);
				json_add_item(tc_obj, "function", fn);
				json_array_add(tc_arr, tc_obj);
			}

			json_add_item(msg_obj, "tool_calls", tc_arr);
		}

		if (msg->role == MESSAGE_ROLE_TOOL) {
			if (msg->tool_call_id)
				json_add_str(msg_obj, "tool_call_id",
					     msg->tool_call_id);
			if (msg->tool_name)
				json_add_str(msg_obj, "name", msg->tool_name);
		}

		json_array_add(arr, msg_obj);
	}

	return arr;
}

/*
 * Remove the oldest exchanges until the estimated token count falls below
 * max_tokens. Removes in structural units (one user turn + all following
 * assistant/tool messages) to keep the history API-valid. The system message
 * at index 0 and the most recent user turn are never removed.
 *
 * Returns the number of messages removed.
 */
int conversation_truncate(struct conversation_history *conv, size_t max_tokens)
{
	size_t start_index;
	int removed = 0;

	if (!conv || conv->count == 0)
		return 0;

	/* Always keep the system message (index 0) if it exists */
	start_index = (conv->count > 0 &&
		       conv->messages[0].role == MESSAGE_ROLE_SYSTEM)
			      ? 1
			      : 0;

	/* Remove units from the start until we're under the limit */
	while (conversation_estimate_tokens(conv) > max_tokens &&
	       conv->count > start_index + 1) {
		/*
		 * Find the end of the deletion unit starting at start_index.
		 * A unit = one message + all immediately following assistant/
		 * tool messages until the next user message or end of array.
		 */
		size_t unit_end = start_index + 1;
		while (unit_end < conv->count &&
		       conv->messages[unit_end].role != MESSAGE_ROLE_USER) {
			unit_end++;
		}

		/* Don't remove the last user turn */
		int remaining_user = 0;
		for (size_t i = unit_end; i < conv->count; i++) {
			if (conv->messages[i].role == MESSAGE_ROLE_USER)
				remaining_user++;
		}
		if (remaining_user == 0)
			break;

		/* Free messages in the unit */
		size_t unit_size = unit_end - start_index;
		size_t chars_removed = 0;

		for (size_t i = start_index; i < unit_end; i++) {
			chars_removed += message_character_count(&conv->messages[i]);
			message_free_content(&conv->messages[i]);
		}

		/* Shift remaining messages down */
		memmove(&conv->messages[start_index],
			&conv->messages[unit_end],
			(conv->count - unit_end) *
				sizeof(struct conversation_message));

		conv->count -= unit_size;
		conv->total_characters -= chars_removed;
		removed += (int)unit_size;
	}

	if (removed > 0)
		log_info("Truncated conversation: removed %d messages", removed);

	return removed;
}

/*
 * Return the last message, or NULL if empty.
 */
const struct conversation_message *conversation_last(
	const struct conversation_history *conv)
{
	if (!conv || conv->count == 0)
		return NULL;
	return &conv->messages[conv->count - 1];
}

/*
 * Clear all messages except the system prompt at index 0.
 */
void conversation_clear(struct conversation_history *conv)
{
	if (!conv)
		return;

	size_t start = (conv->count > 0 &&
			conv->messages[0].role == MESSAGE_ROLE_SYSTEM)
			       ? 1
			       : 0;

	for (size_t i = start; i < conv->count; i++)
		message_free_content(&conv->messages[i]);

	conv->count = start;

	conv->total_characters = 0;
	for (size_t i = 0; i < conv->count; i++)
		conv->total_characters += message_character_count(&conv->messages[i]);
}

enum boris_error conversation_replace_system_prompt(
	struct conversation_history *conv, const char *new_prompt)
{
	if (!conv || !new_prompt)
		return BORIS_ERROR_BAD_ARGUMENT;

	if (conv->count > 0 && conv->messages[0].role == MESSAGE_ROLE_SYSTEM) {
		conv->total_characters -= message_character_count(&conv->messages[0]);
		free(conv->messages[0].content);
		conv->messages[0].content = strdup(new_prompt);
		if (!conv->messages[0].content)
			return BORIS_ERROR_OUT_OF_MEMORY;
		conv->total_characters += message_character_count(&conv->messages[0]);
		return BORIS_OK;
	}

	return conversation_add_system(conv, new_prompt);
}

/*
 * Deep-copy src into dst, replacing dst's entire contents.
 */
enum boris_error conversation_clone(struct conversation_history *dst,
				    const struct conversation_history *src)
{
	if (!dst || !src)
		return BORIS_ERROR_BAD_ARGUMENT;

	/* Free existing dst contents */
	for (size_t i = 0; i < dst->count; i++)
		message_free_content(&dst->messages[i]);
	dst->count = 0;
	dst->total_characters = 0;

	/* Grow dst to fit all of src */
	while (dst->capacity < src->count) {
		if (conversation_grow(dst) != BORIS_OK)
			return BORIS_ERROR_OUT_OF_MEMORY;
	}

	/* Deep-copy each message */
	for (size_t i = 0; i < src->count; i++) {
		const struct conversation_message *s = &src->messages[i];
		struct conversation_message *d = &dst->messages[i];

		d->role = s->role;
		d->content = s->content ? strdup(s->content) : NULL;
		d->tool_call_id = s->tool_call_id ? strdup(s->tool_call_id) : NULL;
		d->tool_name = s->tool_name ? strdup(s->tool_name) : NULL;
		d->tool_calls = NULL;
		d->tool_call_count = 0;

		if (s->tool_calls && s->tool_call_count > 0) {
			d->tool_calls = calloc(s->tool_call_count,
					       sizeof(struct tool_call));
			if (!d->tool_calls) {
				free(d->content);
				free(d->tool_call_id);
				free(d->tool_name);
				d->content = NULL;
				d->tool_call_id = NULL;
				d->tool_name = NULL;
				dst->count = i;
				return BORIS_ERROR_OUT_OF_MEMORY;
			}
			d->tool_call_count = s->tool_call_count;
			for (size_t t = 0; t < s->tool_call_count; t++)
				tool_call_copy(&d->tool_calls[t], &s->tool_calls[t]);
		}

		dst->total_characters += message_character_count(d);
	}

	dst->count = src->count;
	return BORIS_OK;
}

/*
 * Count messages with a given role.
 */
int conversation_count_role(const struct conversation_history *conv,
			    enum message_role role)
{
	if (!conv)
		return 0;
	int n = 0;
	for (size_t i = 0; i < conv->count; i++) {
		if (conv->messages[i].role == role)
			n++;
	}
	return n;
}

/* -------------------------------------------------------------------------
 * Persistence - save and load conversations between sessions
 * ---------------------------------------------------------------------- */

/*
 * Build the full save document: metadata + messages array.
 */
static struct cJSON *build_save_doc(const struct conversation_history *conv)
{
	struct cJSON *doc = json_create_object();
	if (!doc)
		return NULL;

	json_add_str(doc, "format", "boris_session_v1");
	json_add_double(doc, "message_count", (double)conv->count);

	time_t now = time(NULL);
	struct tm *tm = gmtime(&now);
	if (tm) {
		char ts[64];
		strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", tm);
		json_add_str(doc, "saved_at", ts);
	}

	struct cJSON *msgs = conversation_to_json(conv);
	if (!msgs) {
		json_delete(doc);
		return NULL;
	}
	json_add_item(doc, "messages", msgs);

	return doc;
}

/*
 * Parse a single tool_call JSON object into a struct tool_call.
 */
static int parse_tool_call(const struct cJSON *tc_obj, struct tool_call *tc)
{
	memset(tc, 0, sizeof(*tc));

	const cJSON *id_item = json_get_obj(tc_obj, "id");
	if (id_item && json_is_string(id_item))
		snprintf(tc->id, sizeof(tc->id), "%s", id_item->valuestring);

	const cJSON *fn = json_get_obj(tc_obj, "function");
	if (!fn)
		return -1;

	const cJSON *name = json_get_obj(fn, "name");
	if (name && json_is_string(name))
		snprintf(tc->name, sizeof(tc->name), "%s", name->valuestring);

	const cJSON *args = json_get_obj(fn, "arguments");
	if (args && json_is_string(args))
		tc->argument_json = strdup(args->valuestring);
	else
		tc->argument_json = strdup("{}");

	if (!tc->argument_json)
		return -1;
	return 0;
}

/*
 * Parse a single message JSON object into a conversation_message.
 */
static int parse_message(const struct cJSON *msg_obj,
			 struct conversation_message *msg)
{
	memset(msg, 0, sizeof(*msg));

	const char *role = json_get_str(msg_obj, "role");
	if (!role)
		return -1;

	if (strcmp(role, "system") == 0)
		msg->role = MESSAGE_ROLE_SYSTEM;
	else if (strcmp(role, "user") == 0)
		msg->role = MESSAGE_ROLE_USER;
	else if (strcmp(role, "assistant") == 0)
		msg->role = MESSAGE_ROLE_ASSISTANT;
	else if (strcmp(role, "tool") == 0)
		msg->role = MESSAGE_ROLE_TOOL;
	else
		return -1;

	const cJSON *content = json_get_obj(msg_obj, "content");
	if (content && !json_is_null(content) && json_is_string(content))
		msg->content = strdup(content->valuestring);

	if (msg->role == MESSAGE_ROLE_ASSISTANT) {
		const cJSON *tc_arr = json_get_obj(msg_obj, "tool_calls");
		if (tc_arr && json_is_array(tc_arr)) {
			int n = json_get_array_size(tc_arr);
			if (n > 0) {
				msg->tool_calls = calloc((size_t)n,
							 sizeof(struct tool_call));
				if (!msg->tool_calls)
					return -1;
				msg->tool_call_count = 0;

				struct cJSON *tc_obj = NULL;
				json_array_foreach(tc_obj, tc_arr)
				{
					if (parse_tool_call(tc_obj,
							    &msg->tool_calls[msg->tool_call_count]) == 0)
						msg->tool_call_count++;
				}
			}
		}
	}

	if (msg->role == MESSAGE_ROLE_TOOL) {
		const cJSON *tci = json_get_obj(msg_obj, "tool_call_id");
		if (tci && json_is_string(tci))
			msg->tool_call_id = strdup(tci->valuestring);

		const cJSON *tn = json_get_obj(msg_obj, "name");
		if (tn && json_is_string(tn))
			msg->tool_name = strdup(tn->valuestring);
	}

	return 0;
}

enum boris_error conversation_save(const struct conversation_history *conv,
				   const char *file_path)
{
	if (!conv || !file_path)
		return BORIS_ERROR_BAD_ARGUMENT;

	/* Create parent directory if needed */
	char path_copy[1024];
	size_t path_len;
	snprintf(path_copy, sizeof(path_copy), "%s", file_path);
	path_len = strlen(path_copy);
	if (path_len >= sizeof(path_copy))
		return BORIS_ERROR_BAD_ARGUMENT;
	char *dir = dirname(path_copy);
	if (dir && dir[0] && dir[0] != '.') {
		size_t dir_len = strlen(dir);
		if (dir_len >= sizeof(path_copy))
			return BORIS_ERROR_BAD_ARGUMENT;
		struct stat st;
		if (stat(dir, &st) != 0) {
			/* Create directories */
			char work[1024];
			snprintf(work, sizeof(work), "%s", dir);
			for (char *p = work + 1; *p; p++) {
				if (*p == '/') {
					*p = '\0';
					if (mkdir(work, 0755) != 0 &&
					    errno != EEXIST)
						return BORIS_ERROR_IO;
					*p = '/';
				}
			}
			if (mkdir(work, 0755) != 0 && errno != EEXIST)
				return BORIS_ERROR_IO;
		}
	}

	struct cJSON *doc = build_save_doc(conv);
	if (!doc)
		return BORIS_ERROR_OUT_OF_MEMORY;

	char *json = json_to_str(doc);
	json_delete(doc);
	if (!json)
		return BORIS_ERROR_OUT_OF_MEMORY;

	/* Write atomically: write to .tmp then rename */
	char tmp_path[1024];
	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", file_path);

	FILE *f = fopen(tmp_path, "w");
	if (!f) {
		free(json);
		return BORIS_ERROR_IO;
	}

	size_t len = strlen(json);
	size_t written = fwrite(json, 1, len, f);
	fclose(f);
	free(json);

	if (written != len) {
		unlink(tmp_path);
		return BORIS_ERROR_IO;
	}

	if (rename(tmp_path, file_path) != 0) {
		unlink(tmp_path);
		return BORIS_ERROR_IO;
	}

	log_info("Conversation saved to %s (%zu messages)",
		 file_path, conv->count);
	return BORIS_OK;
}

struct conversation_history *conversation_load(const char *file_path)
{
	if (!file_path)
		return NULL;

	size_t file_len;
	char *buf = file_read_all(file_path, 10 * 1024 * 1024, &file_len);
	if (!buf) {
		log_warning("Cannot load conversation: %s not found", file_path);
		return NULL;
	}

	struct cJSON *doc = json_parse(buf);
	free(buf);
	if (!doc) {
		log_warning("Failed to parse session file");
		return NULL;
	}

	const char *fmt = json_get_str(doc, "format");
	if (!fmt || strcmp(fmt, "boris_session_v1") != 0) {
		log_warning("Unknown session format: %s", fmt ? fmt : "(null)");
		json_delete(doc);
		return NULL;
	}

	struct cJSON *msgs = json_get_obj(doc, "messages");
	if (!msgs || !json_is_array(msgs)) {
		log_warning("Session file missing messages array");
		json_delete(doc);
		return NULL;
	}

	int msg_count = json_get_array_size(msgs);
	if (msg_count <= 0) {
		json_delete(doc);
		return conversation_create();
	}

	struct conversation_history *conv = conversation_create();
	if (!conv) {
		json_delete(doc);
		return NULL;
	}

	while (conv->capacity < (size_t)msg_count) {
		if (conversation_grow(conv) != BORIS_OK) {
			conversation_destroy(conv);
			json_delete(doc);
			return NULL;
		}
	}

	struct cJSON *msg_obj = NULL;
	json_array_foreach(msg_obj, msgs)
	{
		struct conversation_message msg;
		if (parse_message(msg_obj, &msg) != 0) {
			message_free_content(&msg);
			continue;
		}

		struct conversation_message *slot = &conv->messages[conv->count];
		*slot = msg;
		conv->count++;
		conv->total_characters += message_character_count(slot);
	}

	json_delete(doc);

	log_info("Loaded conversation from %s (%zu messages)",
		 file_path, conv->count);
	return conv;
}

char *conversation_load_saved_at(const char *file_path)
{
	if (!file_path)
		return NULL;

	FILE *f = fopen(file_path, "r");
	if (!f)
		return NULL;

	/* saved_at is always within the first ~100 bytes; read a small peek */
	char peek[512];
	size_t n = fread(peek, 1, sizeof(peek) - 1, f);
	fclose(f);
	peek[n] = '\0';

	/* Extract value with a string search - avoids JSON parse errors from
	 * the truncated buffer being structurally invalid JSON */
	const char *key = "\"saved_at\":\"";
	char *start = strstr(peek, key);
	if (!start)
		return NULL;
	start += strlen(key);
	char *end = strchr(start, '"');
	if (!end)
		return NULL;

	size_t len = (size_t)(end - start);
	char *result = malloc(len + 1);
	if (!result)
		return NULL;
	memcpy(result, start, len);
	result[len] = '\0';
	return result;
}

char *conversation_session_path(void)
{
	const char *home = getenv("HOME");
	char buf[1024];

	if (!home || !home[0])
		return NULL;

	snprintf(buf, sizeof(buf), "%s/.boris/sessions/session.json", home);
	return strdup(buf);
}

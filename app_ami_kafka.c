/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright 2026 VSGroup (Virtual Sistemas e Tecnologia Ltda)  (see the AUTHORS file)
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \brief AMI Events to Kafka
 *
 * Captures all AMI events via manager_custom_hook and publishes them
 * to a Kafka topic via res_kafka. Supports raw AMI text or JSON format.
 *
 * \author Wazo Communication Inc.
 */

/*** MODULEINFO
	<depend>res_kafka</depend>
	<support_level>core</support_level>
 ***/

/*** DOCUMENTATION
	<configInfo name="app_ami_kafka" language="en_US">
		<synopsis>AMI Events to Kafka</synopsis>
		<configFile name="ami_kafka.conf">
			<configObject name="general">
				<synopsis>General configuration settings</synopsis>
				<configOption name="enabled">
					<synopsis>Enable or disable the module</synopsis>
					<description>
						<para>Default is yes.</para>
					</description>
				</configOption>
				<configOption name="format">
					<synopsis>Output format: json or ami</synopsis>
					<description>
						<para>When set to <literal>json</literal>, AMI events are
						parsed into JSON key-value objects. When set to
						<literal>ami</literal>, the raw AMI text is published
						as-is. Default is <literal>json</literal>.</para>
					</description>
				</configOption>
				<configOption name="eventfilter" regex="true">
					<synopsis>Filter AMI events before publishing to Kafka</synopsis>
					<description>
						<para>Same syntax as Asterisk manager.conf eventfilter.
						Multiple lines allowed. Without filters, all events are
						published.</para>
					</description>
				</configOption>
			</configObject>
			<configObject name="kafka">
				<synopsis>Kafka configuration settings</synopsis>
				<configOption name="connection">
					<synopsis>Name of the connection from kafka.conf to use</synopsis>
					<description>
						<para>Specifies the name of the connection from kafka.conf to use.</para>
					</description>
				</configOption>
				<configOption name="topic">
					<synopsis>Name of the Kafka topic to publish to</synopsis>
					<description>
						<para>Defaults to asterisk_ami.</para>
					</description>
				</configOption>
			</configObject>
		</configFile>
	</configInfo>
 ***/

#include "asterisk.h"

#include <regex.h>

#include "asterisk/config_options.h"
#include "asterisk/json.h"
#include "asterisk/kafka.h"
#include "asterisk/manager.h"
#include "asterisk/module.h"
#include "asterisk/paths.h"
#include "asterisk/stringfields.h"
#include "asterisk/strings.h"
#include "asterisk/utils.h"

#define CONF_FILENAME "ami_kafka.conf"

/*! \brief Output format for AMI events */
enum ami_kafka_format {
	AMI_KAFKA_FORMAT_JSON = 0,
	AMI_KAFKA_FORMAT_AMI,
};

/*! \brief Event filter match types (compatible with Asterisk manager.c) */
enum event_filter_match_type {
	FILTER_MATCH_REGEX = 0,
	FILTER_MATCH_EXACT,
	FILTER_MATCH_STARTS_WITH,
	FILTER_MATCH_ENDS_WITH,
	FILTER_MATCH_CONTAINS,
	FILTER_MATCH_NONE,
};

static const char *match_type_names[] = {
	[FILTER_MATCH_REGEX] = "regex",
	[FILTER_MATCH_EXACT] = "exact",
	[FILTER_MATCH_STARTS_WITH] = "starts_with",
	[FILTER_MATCH_ENDS_WITH] = "ends_with",
	[FILTER_MATCH_CONTAINS] = "contains",
	[FILTER_MATCH_NONE] = "none",
};

/*! \brief Event filter entry — one per eventfilter= line */
struct event_filter_entry {
	enum event_filter_match_type match_type;
	regex_t *regex_filter;       /*!< compiled regex (FILTER_MATCH_REGEX only) */
	char *string_filter;         /*!< pattern string (non-REGEX match types) */
	char *event_name;            /*!< NULL = any event */
	char *header_name;           /*!< NULL = full body, "Header:" = specific header */
};

/* Forward declarations for exported (non-static) test-accessible functions */
int add_filter(const char *criteria, const char *filter_pattern,
	struct ao2_container *includefilters, struct ao2_container *excludefilters);
int match_eventdata(struct event_filter_entry *entry, const char *eventdata);
int should_send_event(struct ao2_container *includefilters,
	struct ao2_container *excludefilters, const char *event, const char *body);
struct ast_json *ami_body_to_json(const char *event, char *body);

/*! \brief General configuration */
struct ami_kafka_conf_general {
	/*! \brief whether the module is enabled */
	int enabled;
	/*! \brief output format (json or ami) */
	enum ami_kafka_format format;
	/*! \brief include event filters */
	struct ao2_container *includefilters;
	/*! \brief exclude event filters */
	struct ao2_container *excludefilters;
};

/*! \brief Kafka configuration */
struct ami_kafka_conf_kafka {
	AST_DECLARE_STRING_FIELDS(
		/*! \brief connection name from kafka.conf */
		AST_STRING_FIELD(connection);
		/*! \brief Kafka topic name */
		AST_STRING_FIELD(topic);
	);
};

/*! \brief Module configuration */
struct ami_kafka_conf {
	struct ami_kafka_conf_general *general;
	struct ami_kafka_conf_kafka *kafka;
};

/*! \brief Locking container for safe configuration access. */
static AO2_GLOBAL_OBJ_STATIC(confs);

/*! \brief Cached Kafka producer for fast access. */
static AO2_GLOBAL_OBJ_STATIC(cached_producer);

static int ami_hook_callback(int category, const char *event, char *body);

/*! \brief AMI custom hook for capturing all manager events. */
static struct manager_custom_hook ami_kafka_hook = {
	.file = __FILE__,
	.helper = ami_hook_callback,
};

/* ACO type definitions */

static struct aco_type general_option = {
	.type = ACO_GLOBAL,
	.name = "general",
	.item_offset = offsetof(struct ami_kafka_conf, general),
	.category = "^general$",
	.category_match = ACO_WHITELIST,
};

static struct aco_type *general_options[] = ACO_TYPES(&general_option);

static struct aco_type kafka_option = {
	.type = ACO_GLOBAL,
	.name = "kafka",
	.item_offset = offsetof(struct ami_kafka_conf, kafka),
	.category = "^kafka$",
	.category_match = ACO_WHITELIST,
};

static struct aco_type *kafka_options[] = ACO_TYPES(&kafka_option);

static void conf_general_dtor(void *obj)
{
	struct ami_kafka_conf_general *general = obj;

	ao2_cleanup(general->includefilters);
	ao2_cleanup(general->excludefilters);
}

static struct ami_kafka_conf_general *conf_general_create(void)
{
	struct ami_kafka_conf_general *general;

	general = ao2_alloc(sizeof(*general), conf_general_dtor);
	if (!general) {
		return NULL;
	}

	general->includefilters = ao2_container_alloc_list(
		AO2_ALLOC_OPT_LOCK_MUTEX, 0, NULL, NULL);
	general->excludefilters = ao2_container_alloc_list(
		AO2_ALLOC_OPT_LOCK_MUTEX, 0, NULL, NULL);
	if (!general->includefilters || !general->excludefilters) {
		ao2_ref(general, -1);
		return NULL;
	}

	aco_set_defaults(&general_option, "general", general);

	return general;
}

static void conf_kafka_dtor(void *obj)
{
	struct ami_kafka_conf_kafka *kafka = obj;
	ast_string_field_free_memory(kafka);
}

static struct ami_kafka_conf_kafka *conf_kafka_create(void)
{
	struct ami_kafka_conf_kafka *kafka;

	kafka = ao2_alloc(sizeof(*kafka), conf_kafka_dtor);
	if (!kafka) {
		return NULL;
	}

	if (ast_string_field_init(kafka, 64) != 0) {
		ao2_ref(kafka, -1);
		return NULL;
	}

	aco_set_defaults(&kafka_option, "kafka", kafka);

	return kafka;
}

static struct aco_file conf_file = {
	.filename = CONF_FILENAME,
	.types = ACO_TYPES(&general_option, &kafka_option),
};

static void conf_dtor(void *obj)
{
	struct ami_kafka_conf *conf = obj;

	ao2_cleanup(conf->general);
	ao2_cleanup(conf->kafka);
}

static void *conf_alloc(void)
{
	struct ami_kafka_conf *conf;

	conf = ao2_alloc_options(sizeof(*conf), conf_dtor,
		AO2_ALLOC_OPT_LOCK_NOLOCK);
	if (!conf) {
		return NULL;
	}

	conf->general = conf_general_create();
	if (!conf->general) {
		ao2_ref(conf, -1);
		return NULL;
	}

	conf->kafka = conf_kafka_create();
	if (!conf->kafka) {
		ao2_ref(conf, -1);
		return NULL;
	}

	return conf;
}

static int setup_kafka(void);

CONFIG_INFO_STANDARD(cfg_info, confs, conf_alloc,
	.files = ACO_FILES(&conf_file),
	.pre_apply_config = setup_kafka,
);

static int setup_kafka(void)
{
	struct ami_kafka_conf *conf = aco_pending_config(&cfg_info);

	if (!conf) {
		return 0;
	}

	if (!conf->general || !conf->kafka) {
		ast_log(LOG_ERROR, "Invalid ami_kafka.conf\n");
		return -1;
	}

	return 0;
}

static int setup_cached_producer(void)
{
	RAII_VAR(struct ami_kafka_conf *, conf, ao2_global_obj_ref(confs), ao2_cleanup);

	if (!conf || !conf->kafka || ast_strlen_zero(conf->kafka->connection)) {
		ast_log(LOG_WARNING, "No Kafka connection configured for ami_kafka\n");
		return -1;
	}

	struct ast_kafka_producer *producer = ast_kafka_get_producer(conf->kafka->connection);
	if (!producer) {
		ast_log(LOG_ERROR, "Failed to get Kafka producer for connection '%s'\n",
			conf->kafka->connection);
		return -1;
	}

	ao2_global_obj_replace_unref(cached_producer, producer);
	ao2_cleanup(producer);
	return 0;
}

/*! \brief Destructor for event_filter_entry ao2 objects */
static void event_filter_dtor(void *obj)
{
	struct event_filter_entry *entry = obj;

	if (entry->regex_filter) {
		regfree(entry->regex_filter);
		ast_free(entry->regex_filter);
	}
	ast_free(entry->event_name);
	ast_free(entry->header_name);
	ast_free(entry->string_filter);
}

/*!
 * \brief Parse and add an event filter entry.
 *
 * Supports both legacy and advanced syntax, compatible with Asterisk manager.conf.
 *
 * Legacy:
 *   eventfilter = Event: Newchannel        (include, regex on body)
 *   eventfilter = !Channel: Local/          (exclude, regex on body)
 *
 * Advanced:
 *   eventfilter(action(include),name(Newchannel)) =
 *   eventfilter(action(exclude),header(Channel),method(starts_with)) = Local/
 *
 * \param criteria The option name (e.g. "eventfilter" or "eventfilter(...)")
 * \param filter_pattern The option value
 * \param includefilters Container for include filters
 * \param excludefilters Container for exclude filters
 * \retval 0 on success
 * \retval -1 on failure
 */
int add_filter(const char *criteria, const char *filter_pattern,
	struct ao2_container *includefilters, struct ao2_container *excludefilters)
{
	struct event_filter_entry *filter_entry;
	char *options_start = NULL;
	int is_exclude = 0;

	filter_entry = ao2_alloc(sizeof(*filter_entry), event_filter_dtor);
	if (!filter_entry) {
		ast_log(LOG_WARNING, "Unable to allocate event filter entry\n");
		return -1;
	}

	if (ast_strlen_zero(criteria)) {
		ast_log(LOG_WARNING, "Missing filter criteria\n");
		ao2_ref(filter_entry, -1);
		return -1;
	}

	if (!filter_pattern) {
		ast_log(LOG_WARNING, "Filter pattern was NULL\n");
		ao2_ref(filter_entry, -1);
		return -1;
	}

	/* Leading '!' means exclude (legacy or alternative to action(exclude)) */
	if (filter_pattern[0] == '!') {
		is_exclude = 1;
		filter_pattern++;
	}

	filter_entry->match_type = FILTER_MATCH_REGEX;

	options_start = strstr(criteria, "(");

	/* Legacy filter requires a pattern */
	if (!options_start && ast_strlen_zero(filter_pattern)) {
		ast_log(LOG_WARNING, "'%s = %s': Legacy filter with no filter pattern\n",
			criteria, filter_pattern);
		ao2_ref(filter_entry, -1);
		return -1;
	}

	if (options_start) {
		/* Advanced filter syntax */
		char *temp = ast_strdupa(options_start + 1);
		char *saveptr = NULL;
		char *option = NULL;
		enum {
			ACTION_FOUND = (1 << 0),
			NAME_FOUND   = (1 << 1),
			HEADER_FOUND = (1 << 2),
			METHOD_FOUND = (1 << 3),
		};
		int options_found = 0;

		filter_entry->match_type = FILTER_MATCH_NONE;

		ast_strip(temp);
		if (ast_strlen_zero(temp) || !ast_ends_with(temp, ")")) {
			ast_log(LOG_WARNING, "'%s = %s': Filter options not formatted correctly\n",
				criteria, filter_pattern);
			ao2_ref(filter_entry, -1);
			return -1;
		}

		while ((option = strtok_r(temp, " ,)", &saveptr))) {
			if (!strncmp(option, "action", 6)) {
				char *val = strstr(option, "(");
				if (ast_strlen_zero(val)) {
					ast_log(LOG_WARNING, "'%s = %s': 'action' parameter not formatted correctly\n",
						criteria, filter_pattern);
					ao2_ref(filter_entry, -1);
					return -1;
				}
				val++;
				ast_strip(val);
				if (!strcmp(val, "include")) {
					is_exclude = 0;
				} else if (!strcmp(val, "exclude")) {
					is_exclude = 1;
				} else {
					ast_log(LOG_WARNING, "'%s = %s': 'action' option '%s' is unknown\n",
						criteria, filter_pattern, val);
					ao2_ref(filter_entry, -1);
					return -1;
				}
				options_found |= ACTION_FOUND;
			} else if (!strncmp(option, "name", 4)) {
				char *val = strstr(option, "(");
				if (ast_strlen_zero(val)) {
					ast_log(LOG_WARNING, "'%s = %s': 'name' parameter not formatted correctly\n",
						criteria, filter_pattern);
					ao2_ref(filter_entry, -1);
					return -1;
				}
				val++;
				ast_strip(val);
				if (ast_strlen_zero(val)) {
					ast_log(LOG_WARNING, "'%s = %s': 'name' parameter is empty\n",
						criteria, filter_pattern);
					ao2_ref(filter_entry, -1);
					return -1;
				}
				filter_entry->event_name = ast_strdup(val);
				options_found |= NAME_FOUND;
			} else if (!strncmp(option, "header", 6)) {
				char *val = strstr(option, "(");
				if (ast_strlen_zero(val)) {
					ast_log(LOG_WARNING, "'%s = %s': 'header' parameter not formatted correctly\n",
						criteria, filter_pattern);
					ao2_ref(filter_entry, -1);
					return -1;
				}
				val++;
				ast_strip(val);
				if (ast_strlen_zero(val)) {
					ast_log(LOG_WARNING, "'%s = %s': 'header' parameter is empty\n",
						criteria, filter_pattern);
					ao2_ref(filter_entry, -1);
					return -1;
				}
				if (!ast_ends_with(val, ":")) {
					filter_entry->header_name = ast_malloc(strlen(val) + 2);
					if (!filter_entry->header_name) {
						ao2_ref(filter_entry, -1);
						return -1;
					}
					sprintf(filter_entry->header_name, "%s:", val); /* Safe */
				} else {
					filter_entry->header_name = ast_strdup(val);
				}
				options_found |= HEADER_FOUND;
			} else if (!strncmp(option, "method", 6)) {
				char *val = strstr(option, "(");
				if (ast_strlen_zero(val)) {
					ast_log(LOG_WARNING, "'%s = %s': 'method' parameter not formatted correctly\n",
						criteria, filter_pattern);
					ao2_ref(filter_entry, -1);
					return -1;
				}
				val++;
				ast_strip(val);
				if (!strcmp(val, "regex")) {
					filter_entry->match_type = FILTER_MATCH_REGEX;
				} else if (!strcmp(val, "exact")) {
					filter_entry->match_type = FILTER_MATCH_EXACT;
				} else if (!strcmp(val, "starts_with")) {
					filter_entry->match_type = FILTER_MATCH_STARTS_WITH;
				} else if (!strcmp(val, "ends_with")) {
					filter_entry->match_type = FILTER_MATCH_ENDS_WITH;
				} else if (!strcmp(val, "contains")) {
					filter_entry->match_type = FILTER_MATCH_CONTAINS;
				} else if (!strcmp(val, "none")) {
					filter_entry->match_type = FILTER_MATCH_NONE;
				} else {
					ast_log(LOG_WARNING, "'%s = %s': 'method' option '%s' is unknown\n",
						criteria, filter_pattern, val);
					ao2_ref(filter_entry, -1);
					return -1;
				}
				options_found |= METHOD_FOUND;
			} else {
				ast_log(LOG_WARNING, "'%s = %s': Filter option '%s' is unknown\n",
					criteria, filter_pattern, option);
				ao2_ref(filter_entry, -1);
				return -1;
			}
			temp = NULL;
		}

		if (!options_found) {
			ast_log(LOG_WARNING, "'%s = %s': No action, name, header, or method option found\n",
				criteria, filter_pattern);
			ao2_ref(filter_entry, -1);
			return -1;
		}
		if (ast_strlen_zero(filter_pattern) && filter_entry->match_type != FILTER_MATCH_NONE) {
			ast_log(LOG_WARNING, "'%s = %s': method can't be '%s' with no filter pattern\n",
				criteria, filter_pattern, match_type_names[filter_entry->match_type]);
			ao2_ref(filter_entry, -1);
			return -1;
		}
		if (!ast_strlen_zero(filter_pattern) && filter_entry->match_type == FILTER_MATCH_NONE) {
			ast_log(LOG_WARNING, "'%s = %s': method can't be 'none' with a filter pattern\n",
				criteria, filter_pattern);
			ao2_ref(filter_entry, -1);
			return -1;
		}
		if (!(options_found & NAME_FOUND) && !(options_found & HEADER_FOUND)
			&& filter_entry->match_type == FILTER_MATCH_NONE) {
			ast_log(LOG_WARNING, "'%s = %s': No name or header and no filter pattern\n",
				criteria, filter_pattern);
			ao2_ref(filter_entry, -1);
			return -1;
		}
	}

	/* Compile the filter pattern */
	if (!ast_strlen_zero(filter_pattern)) {
		if (filter_entry->match_type == FILTER_MATCH_REGEX) {
			filter_entry->regex_filter = ast_calloc(1, sizeof(regex_t));
			if (!filter_entry->regex_filter) {
				ao2_ref(filter_entry, -1);
				return -1;
			}
			if (regcomp(filter_entry->regex_filter, filter_pattern, REG_EXTENDED | REG_NOSUB)) {
				ast_log(LOG_WARNING, "Unable to compile regex filter for '%s'\n",
					filter_pattern);
				ao2_ref(filter_entry, -1);
				return -1;
			}
		} else {
			filter_entry->string_filter = ast_strdup(filter_pattern);
		}
	}

	ast_debug(2, "Event filter: %s = %s (event_name=%s, header=%s, match=%s, exclude=%d)\n",
		criteria, S_OR(filter_pattern, ""),
		S_OR(filter_entry->event_name, "<any>"),
		S_OR(filter_entry->header_name, "<body>"),
		match_type_names[filter_entry->match_type],
		is_exclude);

	if (is_exclude) {
		ao2_link(excludefilters, filter_entry);
	} else {
		ao2_link(includefilters, filter_entry);
	}

	ao2_ref(filter_entry, -1);
	return 0;
}

/*!
 * \brief Test event data against a filter entry.
 *
 * \param entry The filter entry to match with
 * \param eventdata The string data to match against
 * \retval 0 no match
 * \retval 1 match
 */
int match_eventdata(struct event_filter_entry *entry, const char *eventdata)
{
	switch (entry->match_type) {
	case FILTER_MATCH_REGEX:
		return regexec(entry->regex_filter, eventdata, 0, NULL, 0) == 0;
	case FILTER_MATCH_STARTS_WITH:
		return ast_begins_with(eventdata, entry->string_filter);
	case FILTER_MATCH_ENDS_WITH:
		return ast_ends_with(eventdata, entry->string_filter);
	case FILTER_MATCH_CONTAINS:
		return strstr(eventdata, entry->string_filter) != NULL;
	case FILTER_MATCH_EXACT:
		return strcmp(eventdata, entry->string_filter) == 0;
	case FILTER_MATCH_NONE:
		return 1;
	}

	return 0;
}

/*! \brief Auxiliary struct for passing event+body to ao2_callback */
struct filter_cmp_args {
	const char *event;
	const char *body;
};

/*!
 * \brief ao2_callback function: check if a filter entry matches an event.
 *
 * Adapted from Asterisk manager.c filter_cmp_fn — uses event name string
 * comparison (no hash) and searches headers in the raw AMI body.
 */
static int filter_cmp_fn(void *obj, void *arg, void *data, int flags)
{
	struct event_filter_entry *filter_entry = obj;
	struct filter_cmp_args *args = arg;
	int *result = data;
	int match = 0;

	/* Check event name filter first */
	if (filter_entry->event_name) {
		if (strcmp(args->event, filter_entry->event_name) != 0) {
			goto done;
		}
	}

	/* No header_name → match against full body */
	if (!filter_entry->header_name) {
		if (!ast_strlen_zero(args->body)) {
			match = match_eventdata(filter_entry, args->body);
		} else {
			/* No body, but if match_type is NONE we still match */
			match = (filter_entry->match_type == FILTER_MATCH_NONE) ? 1 : 0;
		}
		goto done;
	}

	/* Search for specific header in body (format: "Header: Value\r\n") */
	{
		char *copy = ast_strdupa(args->body);
		char *line;
		char *saveptr = NULL;

		for (line = strtok_r(copy, "\r\n", &saveptr); line;
		     line = strtok_r(NULL, "\r\n", &saveptr)) {
			if (ast_strlen_zero(line)) {
				continue;
			}
			if (ast_begins_with(line, filter_entry->header_name)) {
				char *value = line + strlen(filter_entry->header_name);
				value = ast_skip_blanks(value);
				if (ast_strlen_zero(value)) {
					continue;
				}
				match = match_eventdata(filter_entry, value);
				if (match) {
					break;
				}
			}
		}
	}

done:
	*result = match;
	return match ? CMP_MATCH | CMP_STOP : 0;
}

/*!
 * \brief Determine if an event should be sent based on include/exclude filters.
 *
 * Logic (identical to Asterisk manager.c):
 *   - No filters → send everything
 *   - Include only → send only what matches an include
 *   - Exclude only → send everything except what matches an exclude
 *   - Both → match include first, then reject if matches an exclude
 *
 * \param includefilters Container of include filter entries
 * \param excludefilters Container of exclude filter entries
 * \param event AMI event name
 * \param body AMI event body
 * \retval 1 event should be sent
 * \retval 0 event should be filtered out
 */
int should_send_event(struct ao2_container *includefilters,
	struct ao2_container *excludefilters, const char *event, const char *body)
{
	struct filter_cmp_args args = {
		.event = event,
		.body = body,
	};
	int result = 0;
	int num_include = ao2_container_count(includefilters);
	int num_exclude = ao2_container_count(excludefilters);

	if (!num_include && !num_exclude) {
		return 1; /* no filters = send all */
	}

	if (num_include && !num_exclude) {
		/* include only: implied exclude all, then include */
		ao2_callback_data(includefilters, OBJ_NODATA, filter_cmp_fn, &args, &result);
		return result;
	}

	if (!num_include && num_exclude) {
		/* exclude only: implied include all, then exclude */
		ao2_callback_data(excludefilters, OBJ_NODATA, filter_cmp_fn, &args, &result);
		return !result;
	}

	/* Both: include first, then exclude */
	ao2_callback_data(includefilters, OBJ_NODATA, filter_cmp_fn, &args, &result);
	if (result) {
		result = 0;
		ao2_callback_data(excludefilters, OBJ_NODATA, filter_cmp_fn, &args, &result);
		return !result;
	}

	return 0;
}

/*!
 * \brief Custom ACO handler for the 'format' option.
 *
 * Converts the string value "json" or "ami" to the enum.
 */
static int format_handler(const struct aco_option *opt, struct ast_variable *var,
	void *obj)
{
	struct ami_kafka_conf_general *general = obj;

	if (!strcasecmp(var->value, "json")) {
		general->format = AMI_KAFKA_FORMAT_JSON;
	} else if (!strcasecmp(var->value, "ami")) {
		general->format = AMI_KAFKA_FORMAT_AMI;
	} else {
		ast_log(LOG_WARNING, "Invalid format '%s', must be 'json' or 'ami'\n",
			var->value);
		return -1;
	}

	return 0;
}

/*!
 * \brief Custom ACO handler for 'eventfilter' option.
 *
 * Registered as ACO_REGEX to match both "eventfilter" and "eventfilter(...)".
 * Parses legacy and advanced syntax via add_filter().
 */
static int eventfilter_handler(const struct aco_option *opt,
	struct ast_variable *var, void *obj)
{
	struct ami_kafka_conf_general *general = obj;

	return add_filter(var->name, var->value,
		general->includefilters, general->excludefilters);
}

/*!
 * \brief Parse AMI body text into a JSON object.
 *
 * AMI body format is "Key: Value\r\n" pairs. This function parses each
 * line and builds an ast_json object. The event name is also added to
 * the JSON object.
 *
 * \param event The AMI event name.
 * \param body The AMI body text.
 * \return A new ast_json object on success.
 * \return NULL on failure.
 */
struct ast_json *ami_body_to_json(const char *event, char *body)
{
	struct ast_json *json;
	char *copy;
	char *line;
	char *saveptr = NULL;

	json = ast_json_object_create();
	if (!json) {
		return NULL;
	}

	ast_json_object_set(json, "Event", ast_json_string_create(event));

	/* Inject system identification (same fields the AMI manager exposes) */
	{
		char eid_str[20];
		ast_eid_to_str(eid_str, sizeof(eid_str), &ast_eid_default);
		ast_json_object_set(json, "EntityID", ast_json_string_create(eid_str));
	}
	if (!ast_strlen_zero(ast_config_AST_SYSTEM_NAME)) {
		ast_json_object_set(json, "SystemName",
			ast_json_string_create(ast_config_AST_SYSTEM_NAME));
	}

	copy = ast_strdupa(body);

	for (line = strtok_r(copy, "\r\n", &saveptr); line;
	     line = strtok_r(NULL, "\r\n", &saveptr)) {
		char *sep;

		if (ast_strlen_zero(line)) {
			continue;
		}

		sep = strstr(line, ": ");
		if (!sep) {
			continue;
		}

		*sep = '\0';
		ast_json_object_set(json, line, ast_json_string_create(sep + 2));
	}

	return json;
}

/*!
 * \brief AMI hook callback — hot path.
 *
 * Called synchronously for every AMI event under a read-lock in manager.c.
 * ast_kafka_produce() only copies data into librdkafka's internal buffer,
 * so this is effectively non-blocking.
 *
 * \param category AMI event category bitmask (EVENT_FLAG_*).
 * \param event AMI event name (e.g., "Newchannel", "Hangup").
 * \param body Full AMI event body text ("Key: Value\r\n...").
 * \return Always 0 (never blocks the manager event dispatch).
 */
static int ami_hook_callback(int category, const char *event, char *body)
{
	RAII_VAR(struct ami_kafka_conf *, conf, ao2_global_obj_ref(confs), ao2_cleanup);
	struct ast_kafka_producer *producer;
	const char *payload;
	size_t payload_len;
	RAII_VAR(struct ast_json *, json, NULL, ast_json_unref);
	RAII_VAR(char *, json_str, NULL, ast_json_free);
	RAII_VAR(struct ast_str *, ami_buf, NULL, ast_free);

	if (!conf || !conf->general || !conf->general->enabled) {
		return 0;
	}

	if (!should_send_event(conf->general->includefilters,
		conf->general->excludefilters, event, body)) {
		return 0;
	}

	if (!conf->kafka || ast_strlen_zero(conf->kafka->topic)) {
		return 0;
	}

	producer = ao2_global_obj_ref(cached_producer);
	if (!producer) {
		return 0;
	}

	if (conf->general->format == AMI_KAFKA_FORMAT_JSON) {
		json = ami_body_to_json(event, body);
		if (!json) {
			ao2_cleanup(producer);
			return 0;
		}

		json_str = ast_json_dump_string(json);
		if (!json_str) {
			ao2_cleanup(producer);
			return 0;
		}

		payload = json_str;
		payload_len = strlen(json_str);
	} else {
		/* AMI format: prepend system identification headers */
		char eid_str[20];
		ast_eid_to_str(eid_str, sizeof(eid_str), &ast_eid_default);

		ami_buf = ast_str_create(strlen(body) + 128);
		if (!ami_buf) {
			ao2_cleanup(producer);
			return 0;
		}

		ast_str_set(&ami_buf, 0, "EntityID: %s\r\n", eid_str);
		if (!ast_strlen_zero(ast_config_AST_SYSTEM_NAME)) {
			ast_str_append(&ami_buf, 0, "SystemName: %s\r\n",
				ast_config_AST_SYSTEM_NAME);
		}
		ast_str_append(&ami_buf, 0, "%s", body);

		payload = ast_str_buffer(ami_buf);
		payload_len = ast_str_strlen(ami_buf);
	}

	ast_kafka_produce(producer, conf->kafka->topic, event, payload, payload_len);

	ao2_cleanup(producer);
	return 0;
}

static int load_config(int reload)
{
	switch (aco_process_config(&cfg_info, reload)) {
	case ACO_PROCESS_ERROR:
		return -1;
	case ACO_PROCESS_OK:
	case ACO_PROCESS_UNCHANGED:
		break;
	}

	{
		RAII_VAR(struct ami_kafka_conf *, conf, ao2_global_obj_ref(confs), ao2_cleanup);
		if (!conf || !conf->general || !conf->kafka) {
			ast_log(LOG_ERROR, "Error obtaining config from %s\n", CONF_FILENAME);
			return -1;
		}
	}

	return 0;
}

static int load_module(void)
{
	RAII_VAR(struct ami_kafka_conf *, conf, NULL, ao2_cleanup);

	if (aco_info_init(&cfg_info) != 0) {
		ast_log(LOG_ERROR, "Failed to initialize config\n");
		aco_info_destroy(&cfg_info);
		return AST_MODULE_LOAD_DECLINE;
	}

	/* Register general options */
	aco_option_register(&cfg_info, "enabled", ACO_EXACT,
		general_options, "yes", OPT_BOOL_T, 1,
		FLDSET(struct ami_kafka_conf_general, enabled));
	aco_option_register_custom(&cfg_info, "format", ACO_EXACT,
		general_options, "json", format_handler, 0);
	aco_option_register_custom(&cfg_info, "^eventfilter", ACO_REGEX,
		general_options, "", eventfilter_handler, 0);

	/* Register kafka options */
	aco_option_register(&cfg_info, "connection", ACO_EXACT,
		kafka_options, "", OPT_STRINGFIELD_T, 0,
		STRFLDSET(struct ami_kafka_conf_kafka, connection));
	aco_option_register(&cfg_info, "topic", ACO_EXACT,
		kafka_options, "asterisk_ami", OPT_STRINGFIELD_T, 0,
		STRFLDSET(struct ami_kafka_conf_kafka, topic));

	if (load_config(0) != 0) {
		ast_log(LOG_WARNING, "Configuration failed to load\n");
		aco_info_destroy(&cfg_info);
		return AST_MODULE_LOAD_DECLINE;
	}

	conf = ao2_global_obj_ref(confs);
	if (!conf || !conf->general || !conf->general->enabled) {
		ast_log(LOG_NOTICE, "app_ami_kafka is disabled\n");
		aco_info_destroy(&cfg_info);
		ao2_global_obj_release(confs);
		return AST_MODULE_LOAD_DECLINE;
	}

	if (setup_cached_producer() != 0) {
		ast_log(LOG_ERROR, "Failed to setup Kafka producer\n");
		aco_info_destroy(&cfg_info);
		ao2_global_obj_release(confs);
		return AST_MODULE_LOAD_DECLINE;
	}

	ast_manager_register_hook(&ami_kafka_hook);

	ast_log(LOG_NOTICE, "AMI Kafka publishing enabled (format=%s)\n",
		conf->general->format == AMI_KAFKA_FORMAT_JSON ? "json" : "ami");
	return AST_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	/* Unregister hook first — write-lock guarantees no callback is executing */
	ast_manager_unregister_hook(&ami_kafka_hook);

	ao2_global_obj_release(cached_producer);
	aco_info_destroy(&cfg_info);
	ao2_global_obj_release(confs);

	return 0;
}

static int reload_module(void)
{
	int res = load_config(1);
	if (res == 0) {
		setup_cached_producer();
	}
	return res;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER, "AMI Events Kafka Publisher",
	.support_level = AST_MODULE_SUPPORT_CORE,
	.load = load_module,
	.unload = unload_module,
	.reload = reload_module,
	.load_pri = AST_MODPRI_CDR_DRIVER,
);

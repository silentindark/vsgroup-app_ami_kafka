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

#include "asterisk/config_options.h"
#include "asterisk/json.h"
#include "asterisk/kafka.h"
#include "asterisk/manager.h"
#include "asterisk/module.h"
#include "asterisk/stringfields.h"
#include "asterisk/strings.h"

#define CONF_FILENAME "ami_kafka.conf"

/*! \brief Output format for AMI events */
enum ami_kafka_format {
	AMI_KAFKA_FORMAT_JSON = 0,
	AMI_KAFKA_FORMAT_AMI,
};

/*! \brief General configuration */
struct ami_kafka_conf_general {
	/*! \brief whether the module is enabled */
	int enabled;
	/*! \brief output format (json or ami) */
	enum ami_kafka_format format;
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
	/* No string fields to free in general conf */
}

static struct ami_kafka_conf_general *conf_general_create(void)
{
	struct ami_kafka_conf_general *general;

	general = ao2_alloc(sizeof(*general), conf_general_dtor);
	if (!general) {
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
static struct ast_json *ami_body_to_json(const char *event, char *body)
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

	if (!conf || !conf->general || !conf->general->enabled) {
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
		/* AMI format: publish raw body as-is (zero-copy) */
		payload = body;
		payload_len = strlen(body);
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

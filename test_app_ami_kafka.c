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
 * \brief Tests for app_ami_kafka
 *
 * \author VSGroup
 */

/*** MODULEINFO
	<depend>TEST_FRAMEWORK</depend>
	<depend>app_ami_kafka</depend>
	<depend>res_kafka</depend>
	<support_level>core</support_level>
 ***/

#include "asterisk.h"

#include <regex.h>

#include "asterisk/module.h"
#include "asterisk/test.h"
#include "asterisk/json.h"
#include "asterisk/utils.h"
#include "asterisk/astobj2.h"
#include "asterisk/strings.h"

#define TEST_CATEGORY "/app/ami_kafka/"

/* ---- Imported from app_ami_kafka.c ---- */

/*! \brief Event filter match types */
enum event_filter_match_type {
	FILTER_MATCH_REGEX = 0,
	FILTER_MATCH_EXACT,
	FILTER_MATCH_STARTS_WITH,
	FILTER_MATCH_ENDS_WITH,
	FILTER_MATCH_CONTAINS,
	FILTER_MATCH_NONE,
};

/*! \brief Event filter entry */
struct event_filter_entry {
	enum event_filter_match_type match_type;
	regex_t *regex_filter;
	char *string_filter;
	char *event_name;
	char *header_name;
};

extern struct ast_json *ami_body_to_json(const char *event, char *body);

extern int add_filter(const char *criteria, const char *filter_pattern,
	struct ao2_container *includefilters, struct ao2_container *excludefilters);

extern int match_eventdata(struct event_filter_entry *entry,
	const char *eventdata);

extern int should_send_event(struct ao2_container *includefilters,
	struct ao2_container *excludefilters, const char *event, const char *body);

/* ---- Helpers ---- */

#define SAMPLE_BODY \
	"Privilege: call,all\r\n" \
	"Channel: PJSIP/100-00000001\r\n" \
	"ChannelState: 6\r\n" \
	"CallerIDNum: 100\r\n" \
	"Context: from-internal\r\n"

/*!
 * \brief Create a pair of include/exclude filter containers for testing.
 *
 * Both containers are ao2 list containers with mutex lock.
 * Caller must ao2_cleanup both when done.
 */
static void create_filter_containers(struct ao2_container **include,
	struct ao2_container **exclude)
{
	*include = ao2_container_alloc_list(
		AO2_ALLOC_OPT_LOCK_MUTEX, 0, NULL, NULL);
	*exclude = ao2_container_alloc_list(
		AO2_ALLOC_OPT_LOCK_MUTEX, 0, NULL, NULL);
}

/* ---- JSON conversion tests ---- */

AST_TEST_DEFINE(json_basic_parsing)
{
	struct ast_json *json;
	char body[] =
		"Privilege: call,all\r\n"
		"Channel: PJSIP/100-00000001\r\n"
		"ChannelState: 6\r\n";

	switch (cmd) {
	case TEST_INIT:
		info->name = "json_basic_parsing";
		info->category = TEST_CATEGORY;
		info->summary = "Parse AMI body into JSON";
		info->description =
			"Verifies ami_body_to_json() correctly parses AMI "
			"'Key: Value' lines into a JSON object.";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	json = ami_body_to_json("Newchannel", body);
	if (!json) {
		ast_test_status_update(test, "ami_body_to_json returned NULL\n");
		return AST_TEST_FAIL;
	}

	/* Event name is injected */
	if (!ast_json_object_get(json, "Event") ||
	    strcmp(ast_json_string_get(ast_json_object_get(json, "Event")),
		"Newchannel") != 0) {
		ast_test_status_update(test, "Event field mismatch\n");
		ast_json_unref(json);
		return AST_TEST_FAIL;
	}

	/* Parsed headers */
	if (!ast_json_object_get(json, "Channel") ||
	    strcmp(ast_json_string_get(ast_json_object_get(json, "Channel")),
		"PJSIP/100-00000001") != 0) {
		ast_test_status_update(test, "Channel field mismatch\n");
		ast_json_unref(json);
		return AST_TEST_FAIL;
	}

	if (!ast_json_object_get(json, "ChannelState") ||
	    strcmp(ast_json_string_get(ast_json_object_get(json, "ChannelState")),
		"6") != 0) {
		ast_test_status_update(test, "ChannelState field mismatch\n");
		ast_json_unref(json);
		return AST_TEST_FAIL;
	}

	if (!ast_json_object_get(json, "Privilege") ||
	    strcmp(ast_json_string_get(ast_json_object_get(json, "Privilege")),
		"call,all") != 0) {
		ast_test_status_update(test, "Privilege field mismatch\n");
		ast_json_unref(json);
		return AST_TEST_FAIL;
	}

	ast_json_unref(json);
	return AST_TEST_PASS;
}

AST_TEST_DEFINE(json_entity_id)
{
	struct ast_json *json;
	const char *eid;
	char body[] = "Channel: test\r\n";

	switch (cmd) {
	case TEST_INIT:
		info->name = "json_entity_id";
		info->category = TEST_CATEGORY;
		info->summary = "JSON output includes EntityID";
		info->description =
			"Verifies that ami_body_to_json() injects the "
			"EntityID field from ast_eid_default.";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	json = ami_body_to_json("Test", body);
	if (!json) {
		ast_test_status_update(test, "ami_body_to_json returned NULL\n");
		return AST_TEST_FAIL;
	}

	eid = ast_json_string_get(ast_json_object_get(json, "EntityID"));
	if (ast_strlen_zero(eid)) {
		ast_test_status_update(test, "EntityID not present or empty\n");
		ast_json_unref(json);
		return AST_TEST_FAIL;
	}

	/* EntityID must be in MAC format: XX:XX:XX:XX:XX:XX (17 chars) */
	if (strlen(eid) != 17 || eid[2] != ':' || eid[5] != ':') {
		ast_test_status_update(test,
			"EntityID format invalid: '%s'\n", eid);
		ast_json_unref(json);
		return AST_TEST_FAIL;
	}

	ast_json_unref(json);
	return AST_TEST_PASS;
}

AST_TEST_DEFINE(json_empty_body)
{
	struct ast_json *json;
	char body[] = "";

	switch (cmd) {
	case TEST_INIT:
		info->name = "json_empty_body";
		info->category = TEST_CATEGORY;
		info->summary = "Parse empty AMI body";
		info->description =
			"Verifies ami_body_to_json() handles an empty body "
			"gracefully, still producing Event and EntityID.";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	json = ami_body_to_json("EmptyEvent", body);
	if (!json) {
		ast_test_status_update(test, "ami_body_to_json returned NULL\n");
		return AST_TEST_FAIL;
	}

	if (!ast_json_object_get(json, "Event")) {
		ast_test_status_update(test, "Event field missing\n");
		ast_json_unref(json);
		return AST_TEST_FAIL;
	}

	if (!ast_json_object_get(json, "EntityID")) {
		ast_test_status_update(test, "EntityID field missing\n");
		ast_json_unref(json);
		return AST_TEST_FAIL;
	}

	ast_json_unref(json);
	return AST_TEST_PASS;
}

AST_TEST_DEFINE(json_malformed_lines)
{
	struct ast_json *json;
	char body[] =
		"ValidHeader: value1\r\n"
		"no-separator-here\r\n"
		"AnotherHeader: value2\r\n"
		"\r\n";

	switch (cmd) {
	case TEST_INIT:
		info->name = "json_malformed_lines";
		info->category = TEST_CATEGORY;
		info->summary = "Malformed lines are skipped in JSON parsing";
		info->description =
			"Verifies ami_body_to_json() skips lines without "
			"': ' separator and parses valid lines.";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	json = ami_body_to_json("Test", body);
	if (!json) {
		ast_test_status_update(test, "ami_body_to_json returned NULL\n");
		return AST_TEST_FAIL;
	}

	if (!ast_json_object_get(json, "ValidHeader") ||
	    strcmp(ast_json_string_get(ast_json_object_get(json, "ValidHeader")),
		"value1") != 0) {
		ast_test_status_update(test, "ValidHeader not parsed correctly\n");
		ast_json_unref(json);
		return AST_TEST_FAIL;
	}

	if (!ast_json_object_get(json, "AnotherHeader") ||
	    strcmp(ast_json_string_get(ast_json_object_get(json, "AnotherHeader")),
		"value2") != 0) {
		ast_test_status_update(test, "AnotherHeader not parsed correctly\n");
		ast_json_unref(json);
		return AST_TEST_FAIL;
	}

	/* Malformed line should not appear */
	if (ast_json_object_get(json, "no-separator-here")) {
		ast_test_status_update(test, "Malformed line should be skipped\n");
		ast_json_unref(json);
		return AST_TEST_FAIL;
	}

	ast_json_unref(json);
	return AST_TEST_PASS;
}

/* ---- Filter: add_filter tests ---- */

AST_TEST_DEFINE(filter_legacy_include)
{
	struct ao2_container *include = NULL;
	struct ao2_container *exclude = NULL;
	int res;

	switch (cmd) {
	case TEST_INIT:
		info->name = "filter_legacy_include";
		info->category = TEST_CATEGORY;
		info->summary = "Legacy include filter parsing";
		info->description =
			"Verifies add_filter() with legacy syntax adds an "
			"include regex filter.";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	create_filter_containers(&include, &exclude);

	res = add_filter("eventfilter", "Event: Newchannel",
		include, exclude);
	if (res != 0) {
		ast_test_status_update(test, "add_filter failed\n");
		ao2_cleanup(include);
		ao2_cleanup(exclude);
		return AST_TEST_FAIL;
	}

	if (ao2_container_count(include) != 1) {
		ast_test_status_update(test,
			"Expected 1 include filter, got %d\n",
			ao2_container_count(include));
		ao2_cleanup(include);
		ao2_cleanup(exclude);
		return AST_TEST_FAIL;
	}

	if (ao2_container_count(exclude) != 0) {
		ast_test_status_update(test,
			"Expected 0 exclude filters, got %d\n",
			ao2_container_count(exclude));
		ao2_cleanup(include);
		ao2_cleanup(exclude);
		return AST_TEST_FAIL;
	}

	ao2_cleanup(include);
	ao2_cleanup(exclude);
	return AST_TEST_PASS;
}

AST_TEST_DEFINE(filter_legacy_exclude)
{
	struct ao2_container *include = NULL;
	struct ao2_container *exclude = NULL;
	int res;

	switch (cmd) {
	case TEST_INIT:
		info->name = "filter_legacy_exclude";
		info->category = TEST_CATEGORY;
		info->summary = "Legacy exclude filter parsing (! prefix)";
		info->description =
			"Verifies add_filter() with '!' prefix creates an "
			"exclude filter.";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	create_filter_containers(&include, &exclude);

	res = add_filter("eventfilter", "!Channel: Local/",
		include, exclude);
	if (res != 0) {
		ast_test_status_update(test, "add_filter failed\n");
		ao2_cleanup(include);
		ao2_cleanup(exclude);
		return AST_TEST_FAIL;
	}

	if (ao2_container_count(exclude) != 1) {
		ast_test_status_update(test,
			"Expected 1 exclude filter, got %d\n",
			ao2_container_count(exclude));
		ao2_cleanup(include);
		ao2_cleanup(exclude);
		return AST_TEST_FAIL;
	}

	if (ao2_container_count(include) != 0) {
		ast_test_status_update(test,
			"Expected 0 include filters, got %d\n",
			ao2_container_count(include));
		ao2_cleanup(include);
		ao2_cleanup(exclude);
		return AST_TEST_FAIL;
	}

	ao2_cleanup(include);
	ao2_cleanup(exclude);
	return AST_TEST_PASS;
}

AST_TEST_DEFINE(filter_advanced_include_name)
{
	struct ao2_container *include = NULL;
	struct ao2_container *exclude = NULL;
	int res;

	switch (cmd) {
	case TEST_INIT:
		info->name = "filter_advanced_include_name";
		info->category = TEST_CATEGORY;
		info->summary = "Advanced filter with action(include) and name()";
		info->description =
			"Verifies add_filter() with advanced syntax creates "
			"an include filter with event_name set.";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	create_filter_containers(&include, &exclude);

	res = add_filter("eventfilter(action(include),name(Newchannel))", "",
		include, exclude);
	if (res != 0) {
		ast_test_status_update(test, "add_filter failed\n");
		ao2_cleanup(include);
		ao2_cleanup(exclude);
		return AST_TEST_FAIL;
	}

	if (ao2_container_count(include) != 1) {
		ast_test_status_update(test,
			"Expected 1 include filter, got %d\n",
			ao2_container_count(include));
		ao2_cleanup(include);
		ao2_cleanup(exclude);
		return AST_TEST_FAIL;
	}

	ao2_cleanup(include);
	ao2_cleanup(exclude);
	return AST_TEST_PASS;
}

AST_TEST_DEFINE(filter_advanced_exclude_header)
{
	struct ao2_container *include = NULL;
	struct ao2_container *exclude = NULL;
	int res;

	switch (cmd) {
	case TEST_INIT:
		info->name = "filter_advanced_exclude_header";
		info->category = TEST_CATEGORY;
		info->summary = "Advanced filter with exclude, header, and method";
		info->description =
			"Verifies add_filter() with advanced syntax creates "
			"an exclude filter targeting a specific header with "
			"starts_with method.";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	create_filter_containers(&include, &exclude);

	res = add_filter(
		"eventfilter(action(exclude),header(Channel),method(starts_with))",
		"Local/", include, exclude);
	if (res != 0) {
		ast_test_status_update(test, "add_filter failed\n");
		ao2_cleanup(include);
		ao2_cleanup(exclude);
		return AST_TEST_FAIL;
	}

	if (ao2_container_count(exclude) != 1) {
		ast_test_status_update(test,
			"Expected 1 exclude filter, got %d\n",
			ao2_container_count(exclude));
		ao2_cleanup(include);
		ao2_cleanup(exclude);
		return AST_TEST_FAIL;
	}

	ao2_cleanup(include);
	ao2_cleanup(exclude);
	return AST_TEST_PASS;
}

AST_TEST_DEFINE(filter_invalid_empty)
{
	struct ao2_container *include = NULL;
	struct ao2_container *exclude = NULL;
	int res;

	switch (cmd) {
	case TEST_INIT:
		info->name = "filter_invalid_empty";
		info->category = TEST_CATEGORY;
		info->summary = "Empty legacy filter returns error";
		info->description =
			"Verifies add_filter() returns -1 for an empty legacy "
			"filter pattern.";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	create_filter_containers(&include, &exclude);

	res = add_filter("eventfilter", "", include, exclude);
	if (res != -1) {
		ast_test_status_update(test,
			"Expected -1 for empty legacy filter, got %d\n", res);
		ao2_cleanup(include);
		ao2_cleanup(exclude);
		return AST_TEST_FAIL;
	}

	ao2_cleanup(include);
	ao2_cleanup(exclude);
	return AST_TEST_PASS;
}

AST_TEST_DEFINE(filter_invalid_null_pattern)
{
	struct ao2_container *include = NULL;
	struct ao2_container *exclude = NULL;
	int res;

	switch (cmd) {
	case TEST_INIT:
		info->name = "filter_invalid_null_pattern";
		info->category = TEST_CATEGORY;
		info->summary = "NULL filter pattern returns error";
		info->description =
			"Verifies add_filter() returns -1 when the filter "
			"pattern is NULL.";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	create_filter_containers(&include, &exclude);

	res = add_filter("eventfilter", NULL, include, exclude);
	if (res != -1) {
		ast_test_status_update(test,
			"Expected -1 for NULL pattern, got %d\n", res);
		ao2_cleanup(include);
		ao2_cleanup(exclude);
		return AST_TEST_FAIL;
	}

	ao2_cleanup(include);
	ao2_cleanup(exclude);
	return AST_TEST_PASS;
}

/* ---- should_send_event tests ---- */

AST_TEST_DEFINE(send_no_filters)
{
	struct ao2_container *include = NULL;
	struct ao2_container *exclude = NULL;
	int res;

	switch (cmd) {
	case TEST_INIT:
		info->name = "send_no_filters";
		info->category = TEST_CATEGORY;
		info->summary = "No filters = send all events";
		info->description =
			"Verifies should_send_event() returns 1 when no "
			"filters are configured.";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	create_filter_containers(&include, &exclude);

	res = should_send_event(include, exclude, "Newchannel", SAMPLE_BODY);
	if (res != 1) {
		ast_test_status_update(test,
			"Expected 1 (send), got %d\n", res);
		ao2_cleanup(include);
		ao2_cleanup(exclude);
		return AST_TEST_FAIL;
	}

	ao2_cleanup(include);
	ao2_cleanup(exclude);
	return AST_TEST_PASS;
}

AST_TEST_DEFINE(send_include_match)
{
	struct ao2_container *include = NULL;
	struct ao2_container *exclude = NULL;
	int res;

	switch (cmd) {
	case TEST_INIT:
		info->name = "send_include_match";
		info->category = TEST_CATEGORY;
		info->summary = "Include filter matches = send";
		info->description =
			"Verifies should_send_event() returns 1 when an "
			"include filter matches the event body.";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	create_filter_containers(&include, &exclude);

	/* Include: regex matching "Channel: PJSIP/" in body */
	add_filter("eventfilter", "Channel: PJSIP/", include, exclude);

	res = should_send_event(include, exclude, "Newchannel", SAMPLE_BODY);
	if (res != 1) {
		ast_test_status_update(test,
			"Expected 1 (send), got %d\n", res);
		ao2_cleanup(include);
		ao2_cleanup(exclude);
		return AST_TEST_FAIL;
	}

	ao2_cleanup(include);
	ao2_cleanup(exclude);
	return AST_TEST_PASS;
}

AST_TEST_DEFINE(send_include_no_match)
{
	struct ao2_container *include = NULL;
	struct ao2_container *exclude = NULL;
	int res;

	switch (cmd) {
	case TEST_INIT:
		info->name = "send_include_no_match";
		info->category = TEST_CATEGORY;
		info->summary = "Include filter does not match = reject";
		info->description =
			"Verifies should_send_event() returns 0 when an "
			"include filter does not match.";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	create_filter_containers(&include, &exclude);

	/* Include: regex that won't match the sample body */
	add_filter("eventfilter", "Channel: SIP/", include, exclude);

	res = should_send_event(include, exclude, "Newchannel", SAMPLE_BODY);
	if (res != 0) {
		ast_test_status_update(test,
			"Expected 0 (reject), got %d\n", res);
		ao2_cleanup(include);
		ao2_cleanup(exclude);
		return AST_TEST_FAIL;
	}

	ao2_cleanup(include);
	ao2_cleanup(exclude);
	return AST_TEST_PASS;
}

AST_TEST_DEFINE(send_exclude_match)
{
	struct ao2_container *include = NULL;
	struct ao2_container *exclude = NULL;
	int res;

	switch (cmd) {
	case TEST_INIT:
		info->name = "send_exclude_match";
		info->category = TEST_CATEGORY;
		info->summary = "Exclude filter matches = reject";
		info->description =
			"Verifies should_send_event() returns 0 when an "
			"exclude filter matches the event.";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	create_filter_containers(&include, &exclude);

	/* Exclude: regex matching Channel: PJSIP/ */
	add_filter("eventfilter", "!Channel: PJSIP/", include, exclude);

	res = should_send_event(include, exclude, "Newchannel", SAMPLE_BODY);
	if (res != 0) {
		ast_test_status_update(test,
			"Expected 0 (reject), got %d\n", res);
		ao2_cleanup(include);
		ao2_cleanup(exclude);
		return AST_TEST_FAIL;
	}

	ao2_cleanup(include);
	ao2_cleanup(exclude);
	return AST_TEST_PASS;
}

AST_TEST_DEFINE(send_exclude_no_match)
{
	struct ao2_container *include = NULL;
	struct ao2_container *exclude = NULL;
	int res;

	switch (cmd) {
	case TEST_INIT:
		info->name = "send_exclude_no_match";
		info->category = TEST_CATEGORY;
		info->summary = "Exclude filter does not match = send";
		info->description =
			"Verifies should_send_event() returns 1 when an "
			"exclude filter does not match (exclude-only mode).";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	create_filter_containers(&include, &exclude);

	/* Exclude: regex that won't match */
	add_filter("eventfilter", "!Channel: Local/", include, exclude);

	res = should_send_event(include, exclude, "Newchannel", SAMPLE_BODY);
	if (res != 1) {
		ast_test_status_update(test,
			"Expected 1 (send), got %d\n", res);
		ao2_cleanup(include);
		ao2_cleanup(exclude);
		return AST_TEST_FAIL;
	}

	ao2_cleanup(include);
	ao2_cleanup(exclude);
	return AST_TEST_PASS;
}

AST_TEST_DEFINE(send_include_exclude_combined)
{
	struct ao2_container *include = NULL;
	struct ao2_container *exclude = NULL;
	int res;

	switch (cmd) {
	case TEST_INIT:
		info->name = "send_include_exclude_combined";
		info->category = TEST_CATEGORY;
		info->summary = "Combined include+exclude filter logic";
		info->description =
			"Verifies the combined filter logic: include first, "
			"then exclude. An event matching include but also "
			"matching exclude is rejected.";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	create_filter_containers(&include, &exclude);

	/* Include: match events with Channel: PJSIP/ */
	add_filter("eventfilter", "Channel: PJSIP/", include, exclude);
	/* Exclude: also match 100 in body */
	add_filter("eventfilter", "!CallerIDNum: 100", include, exclude);

	/* SAMPLE_BODY matches both include and exclude → reject */
	res = should_send_event(include, exclude, "Newchannel", SAMPLE_BODY);
	if (res != 0) {
		ast_test_status_update(test,
			"Expected 0 (reject by exclude), got %d\n", res);
		ao2_cleanup(include);
		ao2_cleanup(exclude);
		return AST_TEST_FAIL;
	}

	ao2_cleanup(include);
	ao2_cleanup(exclude);
	return AST_TEST_PASS;
}

/* ---- Advanced filter with name() ---- */

AST_TEST_DEFINE(send_name_filter_match)
{
	struct ao2_container *include = NULL;
	struct ao2_container *exclude = NULL;
	int res;

	switch (cmd) {
	case TEST_INIT:
		info->name = "send_name_filter_match";
		info->category = TEST_CATEGORY;
		info->summary = "Name filter matches event name";
		info->description =
			"Verifies should_send_event() matches when an "
			"include filter with name(Newchannel) is used "
			"and the event is 'Newchannel'.";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	create_filter_containers(&include, &exclude);

	add_filter("eventfilter(action(include),name(Newchannel))", "",
		include, exclude);

	res = should_send_event(include, exclude, "Newchannel", SAMPLE_BODY);
	if (res != 1) {
		ast_test_status_update(test,
			"Expected 1 (send), got %d\n", res);
		ao2_cleanup(include);
		ao2_cleanup(exclude);
		return AST_TEST_FAIL;
	}

	/* Different event name should not match */
	res = should_send_event(include, exclude, "Hangup", SAMPLE_BODY);
	if (res != 0) {
		ast_test_status_update(test,
			"Expected 0 (reject for Hangup), got %d\n", res);
		ao2_cleanup(include);
		ao2_cleanup(exclude);
		return AST_TEST_FAIL;
	}

	ao2_cleanup(include);
	ao2_cleanup(exclude);
	return AST_TEST_PASS;
}

/* ---- Advanced filter with header() + method() ---- */

AST_TEST_DEFINE(send_header_starts_with)
{
	struct ao2_container *include = NULL;
	struct ao2_container *exclude = NULL;
	int res;

	switch (cmd) {
	case TEST_INIT:
		info->name = "send_header_starts_with";
		info->category = TEST_CATEGORY;
		info->summary = "Header filter with starts_with method";
		info->description =
			"Verifies include filter with header(Channel) and "
			"method(starts_with) correctly matches header values.";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	create_filter_containers(&include, &exclude);

	add_filter(
		"eventfilter(action(include),header(Channel),method(starts_with))",
		"PJSIP/", include, exclude);

	res = should_send_event(include, exclude, "Newchannel", SAMPLE_BODY);
	if (res != 1) {
		ast_test_status_update(test,
			"Expected 1 (send), got %d\n", res);
		ao2_cleanup(include);
		ao2_cleanup(exclude);
		return AST_TEST_FAIL;
	}

	/* Body with a different channel should not match */
	{
		char other_body[] = "Channel: SIP/200-00000003\r\n";
		res = should_send_event(include, exclude, "Newchannel", other_body);
		if (res != 0) {
			ast_test_status_update(test,
				"Expected 0 (reject for SIP channel), got %d\n", res);
			ao2_cleanup(include);
			ao2_cleanup(exclude);
			return AST_TEST_FAIL;
		}
	}

	ao2_cleanup(include);
	ao2_cleanup(exclude);
	return AST_TEST_PASS;
}

AST_TEST_DEFINE(send_header_exact)
{
	struct ao2_container *include = NULL;
	struct ao2_container *exclude = NULL;
	int res;

	switch (cmd) {
	case TEST_INIT:
		info->name = "send_header_exact";
		info->category = TEST_CATEGORY;
		info->summary = "Header filter with exact method";
		info->description =
			"Verifies include filter with header(Context) and "
			"method(exact) matches only the exact value.";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	create_filter_containers(&include, &exclude);

	add_filter(
		"eventfilter(action(include),header(Context),method(exact))",
		"from-internal", include, exclude);

	res = should_send_event(include, exclude, "Newchannel", SAMPLE_BODY);
	if (res != 1) {
		ast_test_status_update(test,
			"Expected 1 (send), got %d\n", res);
		ao2_cleanup(include);
		ao2_cleanup(exclude);
		return AST_TEST_FAIL;
	}

	/* Partial match should fail with exact */
	{
		char other_body[] = "Context: from-internal-extra\r\n";
		res = should_send_event(include, exclude, "Test", other_body);
		if (res != 0) {
			ast_test_status_update(test,
				"Expected 0 (reject for partial match), got %d\n", res);
			ao2_cleanup(include);
			ao2_cleanup(exclude);
			return AST_TEST_FAIL;
		}
	}

	ao2_cleanup(include);
	ao2_cleanup(exclude);
	return AST_TEST_PASS;
}

AST_TEST_DEFINE(send_header_contains)
{
	struct ao2_container *include = NULL;
	struct ao2_container *exclude = NULL;
	int res;

	switch (cmd) {
	case TEST_INIT:
		info->name = "send_header_contains";
		info->category = TEST_CATEGORY;
		info->summary = "Header filter with contains method";
		info->description =
			"Verifies include filter with header(Channel) and "
			"method(contains) matches substring in header value.";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	create_filter_containers(&include, &exclude);

	add_filter(
		"eventfilter(action(include),header(Channel),method(contains))",
		"100", include, exclude);

	res = should_send_event(include, exclude, "Newchannel", SAMPLE_BODY);
	if (res != 1) {
		ast_test_status_update(test,
			"Expected 1 (send), got %d\n", res);
		ao2_cleanup(include);
		ao2_cleanup(exclude);
		return AST_TEST_FAIL;
	}

	ao2_cleanup(include);
	ao2_cleanup(exclude);
	return AST_TEST_PASS;
}

AST_TEST_DEFINE(send_header_ends_with)
{
	struct ao2_container *include = NULL;
	struct ao2_container *exclude = NULL;
	int res;

	switch (cmd) {
	case TEST_INIT:
		info->name = "send_header_ends_with";
		info->category = TEST_CATEGORY;
		info->summary = "Header filter with ends_with method";
		info->description =
			"Verifies include filter with header(Channel) and "
			"method(ends_with) matches suffix of header value.";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	create_filter_containers(&include, &exclude);

	add_filter(
		"eventfilter(action(include),header(Channel),method(ends_with))",
		"00000001", include, exclude);

	res = should_send_event(include, exclude, "Newchannel", SAMPLE_BODY);
	if (res != 1) {
		ast_test_status_update(test,
			"Expected 1 (send), got %d\n", res);
		ao2_cleanup(include);
		ao2_cleanup(exclude);
		return AST_TEST_FAIL;
	}

	ao2_cleanup(include);
	ao2_cleanup(exclude);
	return AST_TEST_PASS;
}

/* ---- Module lifecycle ---- */

static int load_module(void)
{
	AST_TEST_REGISTER(json_basic_parsing);
	AST_TEST_REGISTER(json_entity_id);
	AST_TEST_REGISTER(json_empty_body);
	AST_TEST_REGISTER(json_malformed_lines);
	AST_TEST_REGISTER(filter_legacy_include);
	AST_TEST_REGISTER(filter_legacy_exclude);
	AST_TEST_REGISTER(filter_advanced_include_name);
	AST_TEST_REGISTER(filter_advanced_exclude_header);
	AST_TEST_REGISTER(filter_invalid_empty);
	AST_TEST_REGISTER(filter_invalid_null_pattern);
	AST_TEST_REGISTER(send_no_filters);
	AST_TEST_REGISTER(send_include_match);
	AST_TEST_REGISTER(send_include_no_match);
	AST_TEST_REGISTER(send_exclude_match);
	AST_TEST_REGISTER(send_exclude_no_match);
	AST_TEST_REGISTER(send_include_exclude_combined);
	AST_TEST_REGISTER(send_name_filter_match);
	AST_TEST_REGISTER(send_header_starts_with);
	AST_TEST_REGISTER(send_header_exact);
	AST_TEST_REGISTER(send_header_contains);
	AST_TEST_REGISTER(send_header_ends_with);

	return AST_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	AST_TEST_UNREGISTER(json_basic_parsing);
	AST_TEST_UNREGISTER(json_entity_id);
	AST_TEST_UNREGISTER(json_empty_body);
	AST_TEST_UNREGISTER(json_malformed_lines);
	AST_TEST_UNREGISTER(filter_legacy_include);
	AST_TEST_UNREGISTER(filter_legacy_exclude);
	AST_TEST_UNREGISTER(filter_advanced_include_name);
	AST_TEST_UNREGISTER(filter_advanced_exclude_header);
	AST_TEST_UNREGISTER(filter_invalid_empty);
	AST_TEST_UNREGISTER(filter_invalid_null_pattern);
	AST_TEST_UNREGISTER(send_no_filters);
	AST_TEST_UNREGISTER(send_include_match);
	AST_TEST_UNREGISTER(send_include_no_match);
	AST_TEST_UNREGISTER(send_exclude_match);
	AST_TEST_UNREGISTER(send_exclude_no_match);
	AST_TEST_UNREGISTER(send_include_exclude_combined);
	AST_TEST_UNREGISTER(send_name_filter_match);
	AST_TEST_UNREGISTER(send_header_starts_with);
	AST_TEST_UNREGISTER(send_header_exact);
	AST_TEST_UNREGISTER(send_header_contains);
	AST_TEST_UNREGISTER(send_header_ends_with);

	return 0;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, "AMI Kafka Publisher Tests",
	.support_level = AST_MODULE_SUPPORT_CORE,
	.load = load_module,
	.unload = unload_module,
	.requires = "app_ami_kafka",
);

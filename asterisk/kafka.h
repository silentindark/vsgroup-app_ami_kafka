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

#ifndef _ASTERISK_KAFKA_H
#define _ASTERISK_KAFKA_H

#include <stdint.h>
#include <stddef.h>

/*! \file
 * \brief Kafka producer and consumer client
 *
 * This file contains the Asterisk API for Kafka. Connections are configured
 * in \c kafka.conf. You can get a producer by name using \ref
 * ast_kafka_get_producer(), or a consumer using \ref ast_kafka_get_consumer().
 *
 * Producer support uses \ref ast_kafka_produce().
 *
 * Consumer support uses a callback-based model: subscribe to topics with
 * \ref ast_kafka_consumer_subscribe() and messages are delivered via callback
 * from the internal poll thread.
 *
 * The underlying \c librdkafka library is thread safe, so producers and
 * consumers can be shared across threads.
 */

/*!
 * Opaque handle for the Kafka producer.
 */
struct ast_kafka_producer;

/*!
 * Opaque handle for the Kafka consumer.
 */
struct ast_kafka_consumer;

/*!
 * \brief Callback type for received Kafka messages.
 *
 * This callback is invoked from the internal poll thread for each message
 * consumed from subscribed topics.
 *
 * \param topic The topic the message was received from.
 * \param partition The partition number.
 * \param offset The message offset.
 * \param payload Pointer to the message payload.
 * \param len Length of the payload in bytes.
 * \param key Pointer to the message key (may be NULL).
 * \param key_len Length of the key in bytes.
 * \param userdata User-supplied pointer from subscribe call.
 */
typedef void (*ast_kafka_message_cb)(
	const char *topic, int32_t partition, int64_t offset,
	const void *payload, size_t len,
	const void *key, size_t key_len,
	void *userdata);

/*!
 * \brief Gets the given Kafka producer.
 *
 * The returned producer is an AO2 managed object, which must be freed with
 * \ref ao2_cleanup().
 *
 * \param name The name of the connection.
 * \return The producer object.
 * \return \c NULL if connection not found, or some other error.
 */
struct ast_kafka_producer *ast_kafka_get_producer(const char *name);

/*!
 * \brief Produces a message to a Kafka topic.
 *
 * \param producer The producer to use.
 * \param topic The topic to produce to.
 * \param key The message key (may be NULL).
 * \param payload The message payload.
 * \param len The length of the payload.
 * \return 0 on success.
 * \return -1 on failure.
 */
int ast_kafka_produce(struct ast_kafka_producer *producer,
	const char *topic,
	const char *key,
	const void *payload,
	size_t len);

/*!
 * \brief Key-value pair for Kafka message headers.
 *
 * Used with \ref ast_kafka_produce_hdrs() to attach metadata headers
 * to produced messages. Both name and value must be non-NULL.
 */
struct ast_kafka_header {
	const char *name;
	const char *value;
};

/*!
 * \brief Produces a message to a Kafka topic with optional headers.
 *
 * Behaves identically to \ref ast_kafka_produce() but also attaches
 * key-value headers to the message. If \a headers is NULL or
 * \a header_count is 0, no headers are attached.
 *
 * \param producer The producer to use.
 * \param topic The topic to produce to.
 * \param key The message key (may be NULL).
 * \param payload The message payload.
 * \param len The length of the payload.
 * \param headers Array of key-value header pairs (may be NULL).
 * \param header_count Number of headers in the array.
 * \return 0 on success.
 * \return -1 on failure.
 */
int ast_kafka_produce_hdrs(struct ast_kafka_producer *producer,
	const char *topic,
	const char *key,
	const void *payload,
	size_t len,
	const struct ast_kafka_header *headers,
	size_t header_count);

/*!
 * \brief Gets the given Kafka consumer.
 *
 * The returned consumer is an AO2 managed object, which must be freed with
 * \ref ao2_cleanup(). The connection must have \c group_id configured.
 *
 * \param name The name of the connection.
 * \return The consumer object.
 * \return \c NULL if connection not found, group_id not set, or some other error.
 */
struct ast_kafka_consumer *ast_kafka_get_consumer(const char *name);

/*!
 * \brief Subscribe a consumer to one or more topics.
 *
 * The callback will be invoked from the internal poll thread for each
 * message received. Topics are specified as a comma-separated string.
 *
 * \param consumer The consumer to subscribe.
 * \param topics Comma-separated list of topic names.
 * \param callback Function to call for each received message.
 * \param userdata Opaque pointer passed to the callback.
 * \return 0 on success.
 * \return -1 on failure.
 */
int ast_kafka_consumer_subscribe(struct ast_kafka_consumer *consumer,
	const char *topics,
	ast_kafka_message_cb callback,
	void *userdata);

/*!
 * \brief Unsubscribe a consumer from its topics.
 *
 * \param consumer The consumer to unsubscribe.
 * \return 0 on success.
 * \return -1 on failure.
 */
int ast_kafka_consumer_unsubscribe(struct ast_kafka_consumer *consumer);

/*!
 * \brief Ensure a Kafka topic exists, creating it if necessary.
 *
 * Uses the librdkafka Admin API (CreateTopics) to create the topic.
 * If the topic already exists, this function succeeds silently.
 *
 * \param producer The producer whose connection is used for admin operations.
 * \param topic The topic name to ensure.
 * \param num_partitions Number of partitions (used only on creation).
 * \param replication_factor Replication factor (used only on creation).
 * \return 0 on success or if the topic already exists.
 * \return -1 on failure.
 */
int ast_kafka_ensure_topic(struct ast_kafka_producer *producer,
	const char *topic, int num_partitions, int replication_factor);

/*!
 * \brief Asynchronously ensure a Kafka topic exists, creating it if necessary.
 *
 * Non-blocking version of \ref ast_kafka_ensure_topic(). The topic creation
 * request is queued on an internal taskprocessor and executed in the
 * background. This is suitable for use in \c load_module() where blocking
 * would delay Asterisk startup when brokers are unreachable.
 *
 * \param producer The producer whose connection is used for admin operations.
 * \param topic The topic name to ensure.
 * \param num_partitions Number of partitions (used only on creation).
 * \param replication_factor Replication factor (used only on creation).
 * \return 0 if the task was queued successfully.
 * \return -1 on failure (invalid arguments or taskprocessor error).
 */
int ast_kafka_ensure_topic_async(struct ast_kafka_producer *producer,
	const char *topic, int num_partitions, int replication_factor);

#endif /* _ASTERISK_KAFKA_H */

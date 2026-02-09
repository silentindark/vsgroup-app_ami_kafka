/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright 2015-2024 The Wazo Authors  (see the AUTHORS file)
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

/*! \file
 * \brief Kafka producer client
 *
 * This file contains the Asterisk API for Kafka. Connections are configured
 * in \c kafka.conf. You can get a producer by name, using \ref
 * ast_kafka_get_producer().
 *
 * Only produce support is implemented, using \ref ast_kafka_produce().
 *
 * The underlying \c librdkafka library is thread safe, so producers can be
 * shared across threads.
 */

/*!
 * Opaque handle for the Kafka producer.
 */
struct ast_kafka_producer;

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

#endif /* _ASTERISK_KAFKA_H */

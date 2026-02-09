#
# Makefile for Asterisk AMI Kafka module
# Copyright (C) 2024, Wazo Communication Inc.
#
# This program is free software, distributed under the terms of
# the GNU General Public License Version 2. See the LICENSE file
# at the top of the source tree.
#

ASTLIBDIR:=$(shell awk '/moddir/{print $$3}' /etc/asterisk/asterisk.conf 2> /dev/null)
ifeq ($(strip $(ASTLIBDIR)),)
	MODULES_DIR:=$(INSTALL_PREFIX)/usr/lib64/asterisk/modules
else
	MODULES_DIR:=$(INSTALL_PREFIX)$(ASTLIBDIR)
endif
ifeq ($(strip $(DOCDIR)),)
	DOCUMENTATION_DIR:=$(INSTALL_PREFIX)/var/lib/asterisk/documentation/thirdparty
else
	DOCUMENTATION_DIR:=$(INSTALL_PREFIX)$(DOCDIR)
endif
INSTALL = install
ASTETCDIR = $(INSTALL_PREFIX)/etc/asterisk
SAMPLENAME = ami_kafka.conf.sample
CONFNAME = $(basename $(SAMPLENAME))

TARGET = app_ami_kafka.so
OBJECTS = app_ami_kafka.o
TEST_TARGET = test_app_ami_kafka.so
TEST_OBJECTS = test_app_ami_kafka.o
CFLAGS += -I../vsgroup-res_kafka
CFLAGS += -DHAVE_STDINT_H=1
CFLAGS += -Wall -Wextra -Wno-unused-parameter -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations -Winit-self -Wmissing-format-attribute \
          -Wformat=2 -g -fPIC -D_GNU_SOURCE -D'AST_MODULE="app_ami_kafka"' -D'AST_MODULE_SELF_SYM=__internal_app_ami_kafka_self'
LDFLAGS = -Wall -shared

.PHONY: install install-test test clean

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) $(OBJECTS) -o $@ $(LIBS)

test_app_ami_kafka.o: test_app_ami_kafka.c
	$(CC) -c $(CFLAGS) -D'AST_MODULE="test_app_ami_kafka"' \
	    -D'AST_MODULE_SELF_SYM=__internal_test_app_ami_kafka_self' -o $@ $<

$(TEST_TARGET): $(TEST_OBJECTS)
	$(CC) $(LDFLAGS) $(TEST_OBJECTS) -o $@ $(LIBS)

test: $(TEST_TARGET)

%.o: %.c $(HEADERS)
	$(CC) -c $(CFLAGS) -o $@ $<

install: $(TARGET)
	mkdir -p $(DESTDIR)$(MODULES_DIR)
	mkdir -p $(DESTDIR)$(DOCUMENTATION_DIR)
	install -m 644 $(TARGET) $(DESTDIR)$(MODULES_DIR)
	install -m 644 documentation/* $(DESTDIR)$(DOCUMENTATION_DIR)
	@echo " +--- app_ami_kafka installed ---+"
	@echo " +                                       +"
	@echo " + app_ami_kafka has been installed       +"
	@echo " + If you would like to install the       +"
	@echo " + sample configuration file run:         +"
	@echo " +                                       +"
	@echo " +           make samples                +"
	@echo " +---------------------------------------+"

install-test: $(TEST_TARGET)
	mkdir -p $(DESTDIR)$(MODULES_DIR)
	install -m 644 $(TEST_TARGET) $(DESTDIR)$(MODULES_DIR)

clean:
	rm -f $(OBJECTS) $(TEST_OBJECTS)
	rm -f $(TARGET) $(TEST_TARGET)

samples:
	$(INSTALL) -m 644 $(SAMPLENAME) $(DESTDIR)$(ASTETCDIR)/$(CONFNAME)
	@echo " ------- ami_kafka config installed ---------"

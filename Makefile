MKFILE_DIR:=$(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))
CC=gcc

OUTPUT_DIR=$(MKFILE_DIR)/out
CFLAGS=-I.

.PHONY: help
## Self help
help:
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) | sort | awk 'BEGIN {FS = ":.*?## "}; {printf "\033[36m%-30s\033[0m %s\n", $$1, $$2}'

%.o: %.c output-dir
	@$(CC) -Wall -c -o $(OUTPUT_DIR)/$(notdir $(@)) $< $(CFLAGS)


list-unit-test : munit/munit.o ## Runs unit tests for list module
# all unit tests are ifdef-ed into the same code file they test.
# the below __UNIT_TESTING__ (+ building as executable) is needed
# to be able to run the tests later on
	@echo "++ Running lists unit tests"
	@$(CC) -Wall -D __UNIT_TESTING__ -o $(OUTPUT_DIR)/ut_lists list/list.c $(OUTPUT_DIR)/munit.o $(CFLAGS)
	@$(OUTPUT_DIR)/ut_lists


fmem-unit-test: munit/munit.o list/list.o ## Runs unit tests for list module
# all unit tests are ifdef-ed into the same code file they test.
# the below __UNIT_TESTING__ (+ building as executable) is needed
# to be able to run the tests later on
	@echo "++ Running fixed mem allocatory unit tests"
	@$(CC) -Wall -D __UNIT_TESTING__ -D __BAD_MEM__ -o $(OUTPUT_DIR)/ut_fmem fmem/fmem.c $(OUTPUT_DIR)/munit.o   $(OUTPUT_DIR)/list.o $(CFLAGS)
	@$(OUTPUT_DIR)/ut_fmem

unit-tests: list-unit-test fmem-unit-test


output-dir: ## Makes output directory
	@mkdir -p $(OUTPUT_DIR)

clean: ## Cleans all built objects
	@echo "Deleting $(OUTPUT_DIR)"
	@rm -r $(OUTPUT_DIR)

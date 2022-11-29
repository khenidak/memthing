MKFILE_DIR:=$(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))
CC=gcc

OUTPUT_DIR=$(MKFILE_DIR)/out
CFLAGS=-g -I.

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


example-things-mem: list/list.o fmem/fmem.o ## runs memory alloc example (non persisted)
	@echo "++ Running mem alloc example (non presisted)"
# bad mem poisons the memory boundries
	@$(CC) -Wall -D __BAD_MEM__ -o $(OUTPUT_DIR)/example_things_mem things_mem.c $(OUTPUT_DIR)/fmem.o  $(OUTPUT_DIR)/list.o $(CFLAGS)
	@echo "+ Running init to create memory.."
	@$(OUTPUT_DIR)/example_things_mem -i
	@echo "exit ..."
	@echo "+ Running READ to read memory.."
	@$(OUTPUT_DIR)/example_things_mem -r
	@echo "exit ..."
	@echo "+ Running CLEANUP to remove memory.."
	@$(OUTPUT_DIR)/example_things_mem -c

example-things-mem-persisted: list/list.o fmem/fmem.o ## runs memory alloc example (persisted)
	@echo "++ Running mem alloc example (presisted)"
# bad mem poisons the memory boundries
	@$(CC) -Wall -D __BAD_MEM__ -o $(OUTPUT_DIR)/example_things_mem_persisted things_mem_persisted.c $(OUTPUT_DIR)/fmem.o  $(OUTPUT_DIR)/list.o $(CFLAGS)
	@echo "+ Running init to create memory.."
	@$(OUTPUT_DIR)/example_things_mem_persisted -i
	@echo "exit ..."
	@echo "+ Running READ to read memory.."
	@$(OUTPUT_DIR)/example_things_mem_persisted -r
	@echo "exit ..."
	@echo "+ Running CLEANUP to remove memory.."
	@$(OUTPUT_DIR)/example_things_mem_persisted -c



output-dir: ## Makes output directory
	@mkdir -p $(OUTPUT_DIR)

clean: ## Cleans all built objects
	@echo "Deleting $(OUTPUT_DIR)"
	@rm -r $(OUTPUT_DIR)

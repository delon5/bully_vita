# Host-side tests for the texture cache.
#
# The cache is plain C driven entirely through GL and a handful of Vita OS
# calls, so it can be exercised on a build machine with stand-ins for both. It
# compiles against the real VitaSDK headers, so the enum values and function
# signatures are the ones it will meet on the device.
#
# make VITASDK=/path/to/vitasdk

VITASDK ?= $(VITASDK)
OUT     ?= out

CFLAGS := -idirafter $(VITASDK)/arm-vita-eabi/include -I../loader -I. \
          -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -g -O1
LDFLAGS := -pthread

TESTS := test_budget test_revisit test_regress test_no_backing_store test_alloc_trace
FAKES := fake_vitagl.c fake_vita_os.c

.PHONY: all check clean
all: check

# The allocation tracer stands alone: it needs neither the GL fake nor the OS
# one, and linking them would pull in a second definition of the loader stubs.
$(OUT)/test_alloc_trace: test_alloc_trace.c fake_alloc_env.h ../loader/alloc_trace.c \
                         ../loader/config.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -DLOADER_ALLOC_TRACE $< $(LDFLAGS) -o $@

$(OUT)/%: %.c $(FAKES) harness.h fake_vitagl.h ../loader/texture_cache.c ../loader/config.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $< $(FAKES) $(LDFLAGS) -o $@

check: $(addprefix $(OUT)/,$(TESTS))
	@mkdir -p $(OUT)/cache
	@fail=0; for t in $(TESTS); do \
	  rm -rf $(OUT)/cache && mkdir -p $(OUT)/cache; \
	  printf '\n=== %s ===\n' $$t; \
	  TEXCACHE_DIR=$(OUT)/cache ./$(OUT)/$$t || fail=1; \
	done; \
	if [ $$fail -ne 0 ]; then echo; echo "FAILED"; exit 1; fi; \
	echo; echo "all suites passed"

clean:
	rm -rf $(OUT)

# After building, check that no required glibc symbol version is newer than GLIBC_2.17; otherwise the library fails
# to load when preloaded into processes of older images. The library is deleted if the check fails or cannot run
libnvcodec_container_fix.so: nvcodec_container_fix.c
	gcc -shared -fPIC -O2 -Wall -Wextra -o $@ $< -ldl
	@symbols=$$(objdump -T $@) || { echo "objdump -T $@ failed"; rm -f $@; exit 1; }; \
	newest=$$(echo "$$symbols" | grep -oE 'GLIBC_[0-9]+(\.[0-9]+)*' | sort -uV | tail -1); \
	if [ -z "$$newest" ]; then echo "no glibc symbol versions found in $@"; rm -f $@; exit 1; fi; \
	if [ "$$(printf '%s\nGLIBC_2.17\n' "$$newest" | sort -V | tail -1)" != GLIBC_2.17 ]; then \
		echo "$@ needs $$newest:"; echo "$$symbols" | grep "$$newest"; rm -f $@; exit 1; fi; \
	echo "$@: newest glibc symbol version is $$newest"

test: tests/test_unit.c nvcodec_container_fix.c
	mkdir -p tmp
	gcc -O0 -g -Wall -Wextra -Wno-format-truncation -o tmp/test_unit tests/test_unit.c -ldl
	./tmp/test_unit

clean:
	rm -f libnvcodec_container_fix.so tmp/test_unit

.PHONY: test clean

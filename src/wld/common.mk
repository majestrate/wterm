# wld: common.mk

.deps/$(dir): | .deps
	@mkdir "$@"

$(dir)/%.o: $(dir)/%.c | .deps/$(dir)
	$(compile) $(WLD_PACKAGE_CFLAGS)

$(dir)/%.lo: $(dir)/%.c | .deps/$(dir)
	$(compile) $(WLD_PACKAGE_CFLAGS) -fPIC


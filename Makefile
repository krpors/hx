SUBDIRS = src # List of subdirs with Makefiles to execute

all: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -C $@
clean:
	cd $(SUBDIRS) && make clean # Note: Subdir makefiles MUST have clean rules

.PHONY: $(SUBDIRS)

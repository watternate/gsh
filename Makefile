# Top level makefile.
LINK=--no-print-directory
all:
	cd src && $(MAKE) $@ $(LINK)
	cd cli && $(MAKE) $@ $(LINK)
	cd formula && $(MAKE) $@ $(LINK)

clean:
	cd src && $(MAKE) $@
	cd cli && $(MAKE) $@
	cd formula && $(MAKE) $@
	rm -rf lib/*

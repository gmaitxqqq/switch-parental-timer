.PHONY: all sysmodule companion clean

all: sysmodule companion

sysmodule:
	$(MAKE) -C sysmodule

companion:
	$(MAKE) -C companion

clean:
	$(MAKE) -C sysmodule clean
	$(MAKE) -C companion clean

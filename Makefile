DIST_NAME = blackout-screen-install.run
STAGE := $(shell mktemp -d)

.ONESHELL:
dist:
	trap 'rm -rf $(STAGE)' EXIT
	$(MAKE) -C src/blackout-overlay-c clean
	mkdir -p $(STAGE)/blackout-screen
	cp -r src systemd bin $(STAGE)/blackout-screen/
	cp packaging/install.sh packaging/uninstall.sh $(STAGE)/blackout-screen/
	chmod +x $(STAGE)/blackout-screen/install.sh $(STAGE)/blackout-screen/uninstall.sh
	tar czf $(STAGE)/payload.tar.gz -C $(STAGE) blackout-screen
	cat packaging/stub.sh $(STAGE)/payload.tar.gz > $(DIST_NAME)
	chmod +x $(DIST_NAME)

clean-dist:
	rm -f $(DIST_NAME)

.PHONY: dist clean-dist

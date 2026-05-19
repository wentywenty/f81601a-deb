KDIR=/lib/modules/`uname -r`/build
obj-m := f81601a.o

MODULE_NAME  := f81601a
MODULE_VER   := 1.03.20250515
DEB_PKG      := $(MODULE_NAME)-dkms_$(MODULE_VER)_all.deb
DEB_BUILD    := /tmp/deb-build-$(MODULE_NAME)
DKMS_DEST    := usr/src/$(MODULE_NAME)-$(MODULE_VER)

all: clean module deploy

module:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean

deploy:
	echo done

# --- Debian packaging ---

deb: clean-deb
	@mkdir -p $(DEB_BUILD)/DEBIAN $(DEB_BUILD)/$(DKMS_DEST)
	@echo "==> Copying source files..."
	cp Makefile dkms.conf f81601a.c $(DEB_BUILD)/$(DKMS_DEST)/
	@echo "==> Writing control file..."
	@echo 'Package: $(MODULE_NAME)-dkms'             >  $(DEB_BUILD)/DEBIAN/control
	@echo 'Version: $(MODULE_VER)'                    >> $(DEB_BUILD)/DEBIAN/control
	@echo 'Architecture: all'                         >> $(DEB_BUILD)/DEBIAN/control
	@echo 'Section: kernel'                           >> $(DEB_BUILD)/DEBIAN/control
	@echo 'Priority: optional'                        >> $(DEB_BUILD)/DEBIAN/control
	@echo 'Maintainer: Flora <2321901849@qq.com>'     >> $(DEB_BUILD)/DEBIAN/control
	@echo 'Depends: dkms (>= 2.2.0.3)'                >> $(DEB_BUILD)/DEBIAN/control
	@echo 'Description: Fintek F81601A PCIe CAN kernel module (DKMS)' >> $(DEB_BUILD)/DEBIAN/control
	@echo ' This package contains the source for the Fintek F81601A'  >> $(DEB_BUILD)/DEBIAN/control
	@echo ' PCIe CAN driver, packaged with DKMS to auto-rebuild on'   >> $(DEB_BUILD)/DEBIAN/control
	@echo ' kernel upgrades. Supports up to 2 CAN channels.'          >> $(DEB_BUILD)/DEBIAN/control
	@echo "==> Writing maintainer scripts..."
	@printf '#!/bin/sh\nset -e\ncase "$$1" in\n  configure)\n    if ! dkms status -m $(MODULE_NAME) -v $(MODULE_VER) 2>/dev/null | grep -q added; then\n      dkms add -m $(MODULE_NAME) -v $(MODULE_VER)\n    fi\n    if [ -d "/lib/modules/$$(uname -r)/build" ]; then\n      dkms build -m $(MODULE_NAME) -v $(MODULE_VER)\n      dkms install -m $(MODULE_NAME) -v $(MODULE_VER)\n    else\n      echo "Skipping build: kernel headers not found."\n      echo "Run: apt install linux-headers-$$(uname -r) && dkms build -m $(MODULE_NAME) -v $(MODULE_VER)"\n    fi\n  ;;\nesac\n' > $(DEB_BUILD)/DEBIAN/postinst
	@printf '#!/bin/sh\nset -e\ncase "$$1" in\n  remove|upgrade|deconfigure)\n    dkms remove -m $(MODULE_NAME) -v $(MODULE_VER) --all 2>/dev/null || true\n  ;;\nesac\n' > $(DEB_BUILD)/DEBIAN/prerm
	@chmod 755 $(DEB_BUILD)/DEBIAN/postinst $(DEB_BUILD)/DEBIAN/prerm
	@echo "==> Building $(DEB_PKG)..."
	dpkg-deb --root-owner-group --build $(DEB_BUILD) $(DEB_PKG)
	@rm -rf $(DEB_BUILD)
	@ls -lh $(DEB_PKG) && echo "==> Done."

clean-deb:
	rm -rf $(DEB_BUILD) *.deb

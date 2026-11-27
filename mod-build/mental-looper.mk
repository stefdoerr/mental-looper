######################################
#
# mental-looper
#
######################################

# Buildroot recipe for mod-plugin-builder and the MOD Cloud Builder
# (builder.mod.audio/buildroot). Upload this file as is: the builder takes the
# bundle name from the file name. `make release` bumps the tag below.

MENTAL_LOOPER_VERSION = v0.2.0
MENTAL_LOOPER_SITE = https://github.com/stefdoerr/mental-looper.git
MENTAL_LOOPER_SITE_METHOD = git
MENTAL_LOOPER_GIT_SUBMODULES = y
MENTAL_LOOPER_BUNDLES = mental-looper.lv2

# needed for submodules support (dpf)
MENTAL_LOOPER_PRE_DOWNLOAD_HOOKS += MOD_PLUGIN_BUILDER_DOWNLOAD_WITH_SUBMODULES

MENTAL_LOOPER_TARGET_MAKE = $(TARGET_MAKE_ENV) $(TARGET_CONFIGURE_OPTS) $(MAKE) -C $(@D) PLUGIN_FORMATS=lv2

define MENTAL_LOOPER_BUILD_CMDS
	$(MENTAL_LOOPER_TARGET_MAKE)
endef

define MENTAL_LOOPER_INSTALL_TARGET_CMDS
	install -d $(TARGET_DIR)/usr/lib/lv2
	cp -rL $(@D)/bin/mental-looper.lv2 $(TARGET_DIR)/usr/lib/lv2/
endef

$(eval $(generic-package))

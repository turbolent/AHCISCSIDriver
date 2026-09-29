NAME = AHCISCSIDriver
PROJECTVERSION = 1.1
LANGUAGE = English
LOCAL_RESOURCES = Localizable.strings
GLOBAL_RESOURCES = Default.table
TOOLS = AHCISCSIDriver_reloc.tproj
OTHERSRCS = Makefile Makefile.preamble Makefile.postamble README.md
MAKEFILEDIR = /NextDeveloper/Makefiles/app
MAKEFILE = bundle.make
SOURCEMODE = 444
-include Makefile.preamble
include $(MAKEFILEDIR)/$(MAKEFILE)
-include Makefile.postamble
-include Makefile.dependencies

# lib/image's validation as lib/boot's validator (boot_validator_t), with the image id.
fcn_add_module(bootimg SOURCES bootimg.c DEPENDS boot image sha256)

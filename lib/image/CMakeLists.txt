# The flash device interface (lib/flash/flash.h) is used as a header only: the validation calls
# the operations of whatever device it is given and links no flash implementation.
fcn_add_module(image SOURCES image.c DEPENDS crc sha256)

# Classical CAN, independent of any controller hardware: the frame, its length on the wire,
# fault confinement, and the controller model that puts them on a bus timeline.
fcn_add_module(can
    SOURCES can_frame.c can_bits.c can_fc.c can_ctrl.c can_txq.c
    DEPENDS crc)

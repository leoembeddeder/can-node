# The UDS server (ISO 14229-1): sessions and their timing, the general response behaviour, the
# services of phase 5 and the download state machine. lib/boot gives the status values the
# download's backend answers with (mapped to negative response codes here); lib/dtc the read
# functions of ReadDTCInformation.
fcn_add_module(uds SOURCES uds.c uds_program.c DEPENDS boot dtc)

// Copyright information found in source file:
// Copyright 2022 ETH Zurich and University of Bologna.

// Licensing information found in source file:
// Licensed under Solderpad Hardware License, Version 0.51, see LICENSE for details.
// SPDX-License-Identifier: SHL-0.51

#include <termios.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>

#include <sys/time.h>
#include <stdint.h>
#include <vp/vp.hpp>
#include <queue>
#include <vp/itf/io.hpp>
#include <thread>
#include <vp/controller.hpp>

//
// Register map, based on CTU CAN FD controller
// 

// TODO: fill


class CanController : public vp::Component
{

};

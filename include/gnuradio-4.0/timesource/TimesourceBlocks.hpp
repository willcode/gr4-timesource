/* -*- c++ -*- */
/*
 * Copyright 2026 Jeff Long
 * SPDX-License-Identifier: MIT
 */
/*| role: registers the timing sources under their type names for the block library the build
        generates from this header.
*/
#pragma once

#include <gnuradio-4.0/BlockRegistry.hpp>

#include <timesource/GpsSource.hpp>

GR_REGISTER_BLOCK(timesource::GpsSource)

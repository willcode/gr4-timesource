/* -*- c++ -*- */
/*
 * Copyright 2026 Jeff Long
 * SPDX-License-Identifier: MIT
 */
/*| role: registers PpsCorrelator under its type name, for float, complex<float> and
        uint8 samples, in the block library the build generates. The build reads this header
        where it builds the block.
*/
#pragma once

#include <gnuradio-4.0/BlockRegistry.hpp>

#include <timesource/PpsCorrelator.hpp>

#include <complex>
#include <cstdint>

GR_REGISTER_BLOCK(timesource::PpsCorrelator, [T], [ float, std::complex<float>, std::uint8_t ])

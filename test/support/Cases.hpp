/* -*- c++ -*- */
/*
 * Copyright 2026 Jeff Long
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <boost/ut.hpp>

#include <cstdio>
#include <string_view>
#include <utility>

namespace timesource::test {

/*| role: the cases one test binary holds, run whole or one at a time.
    contract: each case is registered with ctest under its own name and selected by that name
        on the command line. A run without an argument runs every case of the file.
    trap: Boost.UT's own filter reports success for a run in which nothing ran, so an entry
        whose name drifted from its case would pass forever. report() refuses a run that
        admitted no case.
*/
class Cases {
public:
    Cases(int argc, char** argv) : _only(argc > 1 ? std::string_view{argv[1]} : std::string_view{}) {}

    template <typename Body>
    void operator()(std::string_view name, Body&& body) {
        if (!_only.empty() && _only != name) {
            return;
        }
        ++_ran;
        boost::ut::detail::test{"test", name} = std::forward<Body>(body);
    }

    /*| contract: the exit status of the run, with the summary printed: 1 where an expectation
            failed, 2 where the name matched no case, 0 otherwise. The status is taken here and
            not from the test runner's destructor, so a binary may end with std::_Exit.
    */
    [[nodiscard]] int finish() const {
        const bool failed = boost::ut::cfg<boost::ut::override>.run({.report_errors = true});
        if (_ran == 0) {
            std::fprintf(stderr, "no case named \"%.*s\" in this binary\n", static_cast<int>(_only.size()), _only.data());
            return 2;
        }
        return failed ? 1 : 0;
    }

private:
    std::string_view _only;
    int              _ran = 0;
};

} // namespace timesource::test

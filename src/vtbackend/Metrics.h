/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <vtbackend/Sequencer.h> // Sequence

#include <algorithm>
#include <map>
#include <string>

namespace terminal
{

/// Used for collecting VT sequence usage metrics.
struct Metrics
{
    // XXX Too bad the key is a string.
    std::map<std::string, uint64_t> sequences;

    void operator()(sequence const& seq) { sequences[seq.text()]++; }

    /// @returns an ordered list of collected metrics, with highest frequencey first.
    [[nodiscard]] std::vector<std::pair<std::string, uint64_t>> ordered() const
    {
        std::vector<std::pair<std::string, uint64_t>> vec;
        for (auto const& [name, freq]: sequences)
            vec.emplace_back(name, freq);

        std::sort(vec.begin(), vec.end(), [](auto const& a, auto const& b) {
            if (a.second > b.second)
                return true;
            if (a.second == b.second)
                return a.first > b.first;
            return false;
        });
        return vec;
    }
};

} // namespace terminal

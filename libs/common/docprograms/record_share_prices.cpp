/**
 * @file record_share_prices.cpp
 */

/********************************************************************************
 *
 * This file is part of the Geneva library collection. The following license
 * applies to this file:
 *
 * ------------------------------------------------------------------------------
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * ------------------------------------------------------------------------------
 *
 * Note that other files in the Geneva library collection may use a different
 * license. Please see the licensing information in each file.
 *
 ********************************************************************************
 *
 * See the NOTICE file in the top-level directory of the Geneva library
 * collection for a list of contributors and copyright information.
 *
 ********************************************************************************/

/**
 * Records share prices with a time tag: a typed stream of common's recorder with a wall-clock
 * time column, a few rows, and the close. The rows go to records/quotes.csv in the working
 * directory; the program prints that file and fails unless it holds the header and every row.
 */

#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "common/record/GRecorder.hpp"

using namespace Gem::Common::Record;

int main() {
    //! [arch-common:record-declare]
    auto quotes = recorder().stream<std::string, double, std::uint64_t>(
        "quotes",
        {"symbol", "price", "volume"},
        withTime::wall
    );
    //! [arch-common:record-declare]

    //! [arch-common:record-append]
    quotes.append("ACME", 17.25, 1200);
    quotes.append("INITECH", 4.10, 800);
    quotes.append("ACME", 17.40, 300);
    //! [arch-common:record-append]
    quotes.close();

    std::ifstream in(recorder().directory() / "quotes.csv");
    std::vector<std::string> lines;
    for(std::string line; std::getline(in, line);) {
        std::cout << line << '\n';
        lines.push_back(line);
    }
    return lines.size() == 4 && lines[0] == "time,symbol,price,volume" ? 0 : 1;
}

/**
 * @file GHelperFunctionsTest.cpp
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

// Standard headers go here
#include <iostream>
#include <string>
#include <vector>

// Boost headers go here

// Geneva headers go here
#include "common/GCommonHelperFunctions.hpp"

using namespace Gem::Common;

int main(int argc, char **argv) {
    { // Test of the parsing of an integer string
        std::string const in_single = "1, 2, 3, 4";
        std::vector<unsigned int> result_single;
        result_single = stringToUIntVec(in_single);

        std::vector<unsigned int>::iterator it;
        for(it = result_single.begin(); it != result_single.end(); ++it) {
            std::cout << *it << " ";
        }
        std::cout << '\n';
    }

    { // Test of tuple parsing
        std::string const in_tuple = "(1,2), (3,4 ) , (3    ,  5)";
        std::vector<std::tuple<unsigned int, unsigned int>> result_tuple;
        result_tuple = stringToUIntTupleVec(in_tuple);

        std::vector<std::tuple<unsigned int, unsigned int>>::iterator it;
        for(it = result_tuple.begin(); it != result_tuple.end(); ++it) {
            std::cout << *it << " ";
        }
        std::cout << '\n';
    }
}

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
#include <vtbackend/Sequence.h>

#include <catch2/catch.hpp>

#include <string_view>

using terminal::sequence_parameter_builder;
using terminal::sequence_parameters;
using namespace std::string_view_literals;

TEST_CASE("sequence_parameter_builder.empty")
{
    auto parameters = sequence_parameters {};
    auto builder = sequence_parameter_builder { parameters };
    builder.fixiate();
    CHECK(parameters.str().empty());
}

TEST_CASE("sequence_parameter_builder.0")
{
    auto parameters = sequence_parameters {};
    auto builder = sequence_parameter_builder { parameters };
    builder.multiplyBy10AndAdd(0);
    builder.fixiate();
    CHECK(parameters.str().empty());
}

TEST_CASE("sequence_parameter_builder.1")
{
    auto parameters = sequence_parameters {};
    auto builder = sequence_parameter_builder { parameters };
    builder.multiplyBy10AndAdd(1);
    builder.fixiate();
    CHECK(parameters.str() == "1");
}

TEST_CASE("sequence_parameter_builder.one")
{
    auto parameters = sequence_parameters {};
    auto builder = sequence_parameter_builder { parameters };
    builder.multiplyBy10AndAdd(1);
    builder.multiplyBy10AndAdd(2);
    builder.multiplyBy10AndAdd(3);
    builder.fixiate();
    CHECK(parameters.str() == "123");
}

TEST_CASE("sequence_parameter_builder.two")
{
    auto parameters = sequence_parameters {};
    auto builder = sequence_parameter_builder { parameters };
    builder.multiplyBy10AndAdd(1);
    builder.multiplyBy10AndAdd(2);
    builder.nextParameter();
    builder.multiplyBy10AndAdd(3);
    builder.fixiate();
    CHECK(parameters.str() == "12;3");
}

TEST_CASE("sequence_parameter_builder.complex.1")
{
    auto parameters = sequence_parameters {};
    auto builder = sequence_parameter_builder { parameters };

    // "1;38:2::171:178:191;4"
    builder.apply(1);           // 1
    builder.nextParameter();    // ;
    builder.apply(38);          // 38
    builder.nextSubParameter(); // :
    builder.apply(2);           // 2
    builder.nextSubParameter(); // :
    builder.nextSubParameter(); // :
    builder.apply(171);         // 171
    builder.nextSubParameter(); // :
    builder.apply(178);         // 178
    builder.nextSubParameter(); // :
    builder.apply(191);         // 191
    builder.nextParameter();    // ;
    builder.apply(4);           // 4

    builder.fixiate();

    INFO(parameters.subParameterBitString());
    CHECK(parameters.str() == "1;38:2::171:178:191;4");
    CHECK(parameters.at(0) == 1);
    CHECK(parameters.subParameterCount(0) == 0);
    CHECK(parameters.at(1) == 38);
    CHECK(parameters.subParameterCount(1) == 5);
    CHECK(parameters.at(1) == 38);
    CHECK(parameters.at(2) == 2);
    CHECK(parameters.at(3) == 0);
    CHECK(parameters.at(4) == 171);
    CHECK(parameters.at(5) == 178);
    CHECK(parameters.at(6) == 191);
    CHECK(parameters.at(7) == 4);
    CHECK(parameters.subParameterCount(7) == 0);
}

TEST_CASE("sequence_parameter_builder.complex.2")
{
    auto parameters = sequence_parameters {};
    auto builder = sequence_parameter_builder { parameters };

    // ";12::34:56;7;89"
    // "0;12::34:56;7;89"
    builder.nextParameter();       // ;
    builder.multiplyBy10AndAdd(1); // 1
    builder.multiplyBy10AndAdd(2); // 2
    builder.nextSubParameter();    // :
    builder.nextSubParameter();    // :
    builder.multiplyBy10AndAdd(3); // 3
    builder.multiplyBy10AndAdd(4); // 4
    builder.nextSubParameter();    // :
    builder.multiplyBy10AndAdd(5); // 5
    builder.multiplyBy10AndAdd(6); // 6
    builder.nextParameter();       // ;
    builder.multiplyBy10AndAdd(7); // 7
    builder.nextParameter();       // ;
    builder.multiplyBy10AndAdd(8); // 8
    builder.multiplyBy10AndAdd(9); // 9

    builder.fixiate();
    INFO(parameters.subParameterBitString());
    CHECK(parameters.str() == "0;12::34:56;7;89");
}

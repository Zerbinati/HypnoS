/*
  HypnoS, a UCI chess playing engine derived from Stockfish
  Copyright (C) 2004-2025 The Stockfish developers (see AUTHORS file)

  HypnoS is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  HypnoS is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <iostream>
#include <ctime>

#include "bitboard.h"
#include "misc.h"
#include "position.h"
#include "types.h"
#include "uci.h"
#include "tune.h"

void showLogo();

void showLogo() {
    constexpr const char* CYAN  = "\033[31m";
    constexpr const char* RESET = "\033[0m";

    std::cout << CYAN << R"(
|_|   _  _  _  __
| |\/|_)| |(_)_\
   / |   
)" << RESET << std::endl;
}

using namespace Hypnos;

int main(int argc, char* argv[]) {

    showLogo();

    std::cout << engine_info() << std::endl;
    std::cout << compiler_info();

    std::cout << "\nBuild date/time       : "
              << __DATE__ << " " << __TIME__ << std::endl;

    Bitboards::init();    Position::init();

    UCIEngine uci(argc, argv);

    Tune::init(uci.engine_options());

    uci.loop();

    return 0;
}
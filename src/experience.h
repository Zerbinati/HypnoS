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

#ifndef EXPERIENCE_H_INCLUDED
#define EXPERIENCE_H_INCLUDED

#include "types.h"

//using namespace std;
using u8    = std::uint8_t;
using u16   = std::uint16_t;
using u32   = std::uint32_t;
using usize = std::size_t;

namespace Experience {

using ExpKey   = Hypnos::Key;
using ExpMove  = Hypnos::Move;
using ExpValue = Hypnos::Value;
using ExpDepth = Hypnos::Depth;

inline constexpr ExpDepth MinDepth = 4;

namespace V1 {

struct ExpEntry {
    ExpKey   key;         // 8 bytes
    ExpMove  move;        // 4 bytes
    ExpValue value;       // 4 bytes
    ExpDepth depth;       // 4 bytes
    u8       padding[4];  // 4 bytes

              ExpEntry()                     = delete;
              ExpEntry(const ExpEntry& exp)  = delete;
    ExpEntry& operator=(const ExpEntry& exp) = delete;

    explicit ExpEntry(const ExpKey k, const ExpMove m, const ExpValue v, const ExpDepth d) {
        key        = k;
        move       = m;
        value      = v;
        depth      = d;
        padding[0] = padding[2] = 0x00;
        padding[1] = padding[3] = 0xFF;
    }

    void merge(const ExpEntry* exp) {
        assert(key == exp->key);
        assert(move == exp->move);

        if (depth == exp->depth)
            value = (value + exp->value) / 2;
        else if (depth < exp->depth)
        {
            value = exp->value;
            depth = exp->depth;
        }
    }

    [[nodiscard]] int compare(const ExpEntry* exp) const {
        static constexpr int DepthScale = 5;

        const int thisScaledValue  = value * std::max(depth / DepthScale, 1);
        const int otherScaledValue = exp->value * std::max(exp->depth / DepthScale, 1);

        return thisScaledValue != otherScaledValue ? thisScaledValue - otherScaledValue
                                                   : depth - exp->depth;
    }
};

static_assert(sizeof(ExpEntry) == 24);

}

namespace V2 {

struct ExpEntry {
    ExpKey   key;         // 8 bytes
    ExpMove  move;        // 4 bytes
    ExpValue value;       // 4 bytes
    ExpDepth depth;       // 4 bytes
    u16      count;       // 2 bytes (A scaled version of count)
    u8       padding[2];  // 2 bytes

              ExpEntry()                     = delete;
              ExpEntry(const ExpEntry& exp)  = delete;
    ExpEntry& operator=(const ExpEntry& exp) = delete;

    explicit ExpEntry(const ExpKey k, const ExpMove m, const ExpValue v, const ExpDepth d) :
        ExpEntry(k, m, v, d, 1) {}

    explicit
    ExpEntry(const ExpKey k, const ExpMove m, const ExpValue v, const ExpDepth d, const u16 c) {
        key        = k;
        move       = m;
        value      = v;
        depth      = d;
        count      = c;
        padding[0] = padding[1] = 0x00;
    }

    void merge(const ExpEntry* exp) {
        assert(key == exp->key);
        assert(move == exp->move);

        // Merge the count
        count =
          static_cast<u16>(std::min<u32>(count + exp->count, std::numeric_limits<u16>::max()));

        // Merge value and depth only if 'exp' is better or equal
        if (depth == exp->depth)
            value = (value + exp->value) / 2;
        else if (depth < exp->depth)
        {
            value = exp->value;
            depth = exp->depth;
        }
    }

    int compare(const ExpEntry* exp) const {
        static constexpr int DepthScale = 10;
        static constexpr int CountScale = 3;

        auto scaledValue = [](const int v, const int d, const int c) -> int {
            return v * std::max(d / DepthScale, 1) * std::max(c / CountScale, 1);
        };

        int v = scaledValue(value, depth, count) - scaledValue(exp->value, exp->depth, exp->count);

        if (v)
            return v;

        if ((v = count - exp->count) != 0)
            return v;

        return depth - exp->depth;
    }
};

static_assert(sizeof(ExpEntry) == 24);

}

namespace Current = V2;

// Experience structure
struct ExpEntryEx: Current::ExpEntry {
    ExpEntryEx* next = nullptr;

                ExpEntryEx()                      = delete;
                ExpEntryEx(const ExpEntryEx& exp) = delete;
    ExpEntryEx& operator=(const ExpEntryEx& exp)  = delete;

    explicit
    ExpEntryEx(const ExpKey k, const ExpMove m, const ExpValue v, const ExpDepth d, const u8 c) :
        Current::ExpEntry(k, m, v, d, c) {}

    [[nodiscard]] ExpEntryEx* find(const ExpMove m) const {
        auto* exp = const_cast<ExpEntryEx*>(this);

        do
        {
            if (exp->move == m)
                return exp;

            exp = exp->next;
        } while (exp);

        return nullptr;
    }

    [[nodiscard]] ExpEntryEx* find(const ExpMove mv, const ExpDepth minDepth) const {
        auto* temp = const_cast<ExpEntryEx*>(this);

        do
        {
            if (temp->move == mv)
            {
                if (temp->depth < minDepth)
                    temp = nullptr;

                break;
            }

            temp = temp->next;
        } while (temp);

        return temp;
    }

    std::pair<int, bool> quality(Hypnos::Position& pos, int evalImportance) const;
};

}

namespace Experience {

void init();
bool enabled();

void unload();
void save();

void wait_for_loading_finished();

const ExpEntryEx* probe(ExpKey k);
const ExpEntryEx* find_best_entry(ExpKey k);

void defrag(int argc, char* argv[]);
void merge(int argc, char* argv[]);
void show_exp(Hypnos::Position& pos, bool extended);
void convert_compact_pgn(int argc, char* argv[]);

void pause_learning();
void resume_learning();
bool is_learning_paused();

void add_pv_experience(ExpKey k, ExpMove m, ExpValue v, ExpDepth d);
void add_multipv_experience(ExpKey k, ExpMove m, ExpValue v, ExpDepth d);

}

#endif  // #ifndef EXPERIENCE_H_INCLUDED

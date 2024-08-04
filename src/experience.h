/*
  HypnoS, a UCI chess playing engine derived from Stockfish
  Copyright (C) 2004-2024 The Stockfish developers (see AUTHORS file)

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

using namespace std;
using namespace Stockfish;

constexpr inline Depth EXP_MIN_DEPTH = 4;

namespace Experience {
namespace V1 {
struct ExpEntry {
    Key     key;           //8 bytes
    Move    move;          //4 bytes
    Value   value;         //4 bytes
    Depth   depth;         //4 bytes
    uint8_t padding[4]{};  //4 bytes

              ExpEntry()                     = delete;
              ExpEntry(const ExpEntry& exp)  = delete;
    ExpEntry& operator=(const ExpEntry& exp) = delete;

    explicit ExpEntry(const Key k, const Move m, const Value v, const Depth d) {
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

        if (depth > exp->depth)
            return;

        if (depth == exp->depth)
        {
            value = (value + exp->value) / 2;
        }
        else
        {
            value = exp->value;
            depth = exp->depth;
        }
    }

    int compare(const ExpEntry* exp) const {
        int v = value * std::max(depth / 5, 1) - exp->value * std::max(exp->depth / 5, 1);
        if (!v)
            v = depth - exp->depth;

        return v;
    }
};

static_assert(sizeof(ExpEntry) == 24);
}

namespace V2 {
struct ExpEntry {
    Key      key;           //8 bytes
    Move     move;          //4 bytes
    Value    value;         //4 bytes
    Depth    depth;         //4 bytes
    uint16_t count;         //2 bytes (A scaled version of count)
    uint8_t  padding[2]{};  //2 bytes

              ExpEntry()                     = delete;
              ExpEntry(const ExpEntry& exp)  = delete;
    ExpEntry& operator=(const ExpEntry& exp) = delete;

    explicit ExpEntry(const Key k, const Move m, const Value v, const Depth d) :
        ExpEntry(k, m, v, d, 1) {}

    explicit ExpEntry(const Key k, const Move m, const Value v, const Depth d, const uint16_t c) {
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

        //Merge the count
        count = static_cast<uint16_t>(
          std::min(static_cast<uint32_t>(count) + static_cast<uint32_t>(exp->count),
                   static_cast<uint32_t>(std::numeric_limits<uint16_t>::max())));

        //Merge value and depth if 'exp' is better or equal
        if (depth > exp->depth)
            return;

        if (depth == exp->depth)
        {
            value = (value + exp->value) / 2;
        }
        else
        {
            value = exp->value;
            depth = exp->depth;
        }
    }

    int compare(const ExpEntry* exp) const {
        int v = value * std::max(depth / 10, 1) * std::max(count / 3, 1)
              - exp->value * std::max(exp->depth / 10, 1) * std::max(exp->count / 3, 1);
        if (v)
            return v;

        v = count - exp->count;
        if (v)
            return v;

        v = depth - exp->depth;
        return v;
    }
};

static_assert(sizeof(ExpEntry) == 24);
}

namespace Current = V2;

//Experience structure
struct ExpEntryEx: Current::ExpEntry {
    ExpEntryEx* next = nullptr;

                ExpEntryEx()                      = delete;
                ExpEntryEx(const ExpEntryEx& exp) = delete;
    ExpEntryEx& operator=(const ExpEntryEx& exp)  = delete;

    explicit ExpEntryEx(const Key k, const Move m, const Value v, const Depth d, const uint8_t c) :
        ExpEntry(k, m, v, d, c) {}

    [[nodiscard]] ExpEntryEx* find(const Move m) const {
        auto* exp = const_cast<ExpEntryEx*>(this);
        do
        {
            if (exp->move == m)
                return exp;

            exp = exp->next;
        } while (exp);

        return nullptr;
    }

    [[nodiscard]] ExpEntryEx* find(const Move mv, const Depth minDepth) const {
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

    std::pair<int, bool> quality(Position& pos, int evalImportance) const;
};
}

namespace Experience {
void init();
bool enabled();

void unload();
void save();

void wait_for_loading_finished();

const ExpEntryEx* probe(Key k);

void defrag(int argc, char* argv[]);
void merge(int argc, char* argv[]);
void show_exp(Position& pos, bool extended);
void convert_compact_pgn(int argc, char* argv[]);

void pause_learning();
void resume_learning();
bool is_learning_paused();

void add_pv_experience(Key k, Move m, Value v, Depth d);
void add_multipv_experience(Key k, Move m, Value v, Depth d);
}

#endif  //EXPERIENCE_H_INCLUDED

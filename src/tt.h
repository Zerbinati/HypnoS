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

#ifndef TT_H_INCLUDED
#define TT_H_INCLUDED

#include "misc.h"
#include "types.h"

namespace Hypnos {

/// TTEntry struct is the 16 bytes transposition table entry, defined as below:
///
/// key        64 bit
/// depth       8 bit
/// generation  5 bit
/// pv node     1 bit
/// bound type  2 bit
/// move       16 bit
/// value      16 bit
/// eval value 16 bit

struct TTEntry {

  Move  move()  const { return (Move )move16; }
  Value value() const { return (Value)value16; }
  Value eval()  const { return (Value)eval16; }
  Depth depth() const { return (Depth)depth8 + DEPTH_OFFSET; }
  bool  is_pv() const { return (bool)(genBound8 & 0x4); }
  Bound bound() const { return (Bound)(genBound8 & 0x3); }

  void save(Key k, Value v, bool pv, Bound b, Depth d, Move m, Value ev);

private:
  friend class TranspositionTable;

  uint64_t key;
  uint8_t  depth8;
  uint8_t  genBound8;
  uint16_t move16;
  int16_t  value16;
  int16_t  eval16;
};


/// A TranspositionTable is an array of Cluster, of size clusterCount. Each
/// cluster consists of ClusterSize number of TTEntry. Each non-empty TTEntry
/// contains information on exactly one position. The size of a Cluster should
/// divide the size of a cache line for best performance, as the cacheline is
/// prefetched when possible.

class TranspositionTable {

  static constexpr int ClusterSize = 2;

  struct Cluster {
    TTEntry entry[ClusterSize];
  };

  static_assert(sizeof(Cluster) == 32, "Unexpected Cluster size");

  // Constants used to refresh the hash table periodically
  static constexpr unsigned GENERATION_BITS  = 3;                                // nb of bits reserved for other things
  static constexpr int      GENERATION_DELTA = (1 << GENERATION_BITS);           // increment for generation field
  static constexpr int      GENERATION_CYCLE = 255 + (1 << GENERATION_BITS);     // cycle length
  static constexpr int      GENERATION_MASK  = (0xFF << GENERATION_BITS) & 0xFF; // mask to pull out generation number

public:
 ~TranspositionTable() { aligned_large_pages_free(table); }
  void new_search() { generation8 += GENERATION_DELTA; } // Lower bits are used for other things
  void infinite_search() { generation8 += GENERATION_DELTA; }
  uint8_t generation() const { return generation8; }
  TTEntry* probe(const Key key, bool& found) const;
  int hashfull() const;
  void resize(size_t mbSize);
  void clear();
  void set_hash_file_name(const std::string& fname);
  void save();
  void load();
  void load_epd_to_hash();
  std::string hashfilename = "Hypnos.hsh";

  // The key is used to get the index of the cluster
  TTEntry* first_entry(const Key key) const {
    return &table[(key * (__uint128_t)clusterCount) >> 64].entry[0];
  }

private:
  friend struct TTEntry;

  size_t clusterCount;
  Cluster* table;
  uint8_t generation8; // Size must be not bigger than TTEntry::genBound8
};

extern TranspositionTable TT;
std::vector<std::string> split(const std::string &s, char delim);
Move san_to_move(Position& pos, std::string& str);
Value uci_to_score(std::string &str);
} // namespace Hypnos

#endif // #ifndef TT_H_INCLUDED

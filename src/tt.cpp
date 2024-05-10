/*
  SugaR, a UCI chess playing engine derived from Stockfish
  Copyright (C) 2004-2023 The Stockfish developers (see AUTHORS file)

  SugaR is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  SugaR is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <cstring>   // For std::memset
#include <iostream>
#include <thread>

#include <fstream>
#include "uci.h"
using std::string;
#include <sstream>
#include <string>
#include <sstream>
#include <vector>
#include <iterator>
#include "position.h"
#include "thread.h"

#include "bitboard.h"
#include "misc.h"
#include "thread.h"
#include "tt.h"
#include "uci.h"

namespace Stockfish {

//https://stackoverflow.com/questions/236129/most-elegant-way-to-split-a-string
template<typename Out>
void split(const std::string &s, char delim, Out result) {
	std::stringstream ss;
	ss.str(s);
	std::string item;
	while (std::getline(ss, item, delim)) {
		*(result++) = item;
	}
}

std::vector<std::string> split(const std::string &s, char delim) {
	std::vector<std::string> elems;
	split(s, delim, std::back_inserter(elems));
	return elems;
}

TranspositionTable TT; // Our global transposition table

/// TTEntry::save() populates the TTEntry with a new node's data, possibly
/// overwriting an old position. Update is not atomic and can be racy.

void TTEntry::save(Key k, Value v, bool pv, Bound b, Depth d, Move m, Value ev) {

  // Preserve any existing move for the same position
  if (m || k != key)
      move16 = (uint16_t)m;

  // Overwrite less valuable entries (cheapest checks first)
  if (   b == BOUND_EXACT
      || k != key
      || d - DEPTH_OFFSET + 3 * pv >= depth8)
  {
      assert(d > DEPTH_OFFSET);
      assert(d < 256 + DEPTH_OFFSET);

      key       =  k;
      depth8    = (uint8_t)(d - DEPTH_OFFSET);
      genBound8 = (uint8_t)(TT.generation8 | uint8_t(pv) << 2 | b);
      value16   = (int16_t)v;
      eval16    = (int16_t)ev;
  }
}


/// TranspositionTable::resize() sets the size of the transposition table,
/// measured in megabytes. Transposition table consists of a power of 2 number
/// of clusters and each cluster consists of ClusterSize number of TTEntry.

void TranspositionTable::resize(size_t mbSize) {

  Threads.main()->wait_for_search_finished();

  aligned_large_pages_free(table);

  clusterCount = mbSize * 1024 * 1024 / sizeof(Cluster);

  table = static_cast<Cluster*>(aligned_large_pages_alloc(clusterCount * sizeof(Cluster)));
  if (!table)
  {
      std::cerr << "Failed to allocate " << mbSize
                << "MB for transposition table." << std::endl;
      exit(EXIT_FAILURE);
  }

  clear();
}


/// TranspositionTable::clear() initializes the entire transposition table to zero,
//  in a multi-threaded way.

void TranspositionTable::clear() {

  std::vector<std::thread> threads;

  for (size_t idx = 0; idx < size_t(Options["Threads"]); ++idx)
  {
      threads.emplace_back([this, idx]() {

          // Thread binding gives faster search on systems with a first-touch policy
          if (Options["Threads"] > 8)
              WinProcGroup::bind_this_thread(idx);

          // Each thread will zero its part of the hash table
          const size_t stride = size_t(clusterCount / Options["Threads"]),
                       start  = size_t(stride * idx),
                       len    = idx != size_t(Options["Threads"]) - 1 ?
                                stride : clusterCount - start;

          std::memset(&table[start], 0, len * sizeof(Cluster));
      });
  }

  for (std::thread& th : threads)
      th.join();
}

void TranspositionTable::set_hash_file_name(const std::string& fname) { hashfilename = fname; }

void TranspositionTable::save() {
	std::ofstream out(hashfilename, std::ios::out | std::ios::binary);
	if (!out.is_open())
	{
		sync_cout << "info string Could not create file: " << hashfilename << sync_endl;
		return;
	}

	size_t savedEntries = 0;
	for (size_t i = 0; i < clusterCount; ++i)
	{
		for (int j = 0; j < ClusterSize; ++j)
		{
			if (!table[i].entry[j].depth8 || (table[i].entry[j].genBound8 & GENERATION_MASK) != generation8)
				continue;

			++savedEntries;
			if (!out.write((const char*)(&table[i].entry[j]), sizeof(TTEntry)))
			{
				sync_cout << "info string Error while saving to: " << hashfilename << sync_endl;
				return;
			}
		}
	}

	if(savedEntries == 0)
		sync_cout << "info string Hash table is empty" << sync_endl;
	else
		sync_cout << "info string Saved " << savedEntries << " hash entries to: " << hashfilename << sync_endl;
}

void TranspositionTable::load() {
	std::ifstream in;
	in.open(hashfilename, std::ios::in | std::ios::binary);
	if (!in.is_open())
	{
		sync_cout << "info string Could not open file: " << hashfilename << sync_endl;
		return;
	}

	in.ignore(std::numeric_limits<std::streamsize>::max());
	std::streamsize fileSize = in.gcount();

	if ((fileSize % sizeof(TTEntry)) != 0)
	{
		sync_cout << "info string Persisted hash file [" << hashfilename << "] is corrupted" << sync_endl;
		return;
	}

	size_t hashEntries = fileSize / sizeof(TTEntry);
	if (hashEntries == 0)
	{
		sync_cout << "info string Persisted hash file [" << hashfilename << "] is empty" << sync_endl;
		return;
	}

	//Seek back to begining of file
	in.seekg(0, std::ios::beg);

	TTEntry entry;
	for (size_t i = 0; i < hashEntries; ++i)
	{
		if (!in.read((char*)&entry, sizeof(TTEntry)))
		{
			sync_cout << "info string Error while reading from: " << hashfilename << sync_endl;
			return;
		}

		bool found;
		TTEntry* temp = probe((Key)entry.key, found);

		assert(!found || temp->key == entry.key);

		temp->save((Key)entry.key, entry.value(), entry.is_pv(), entry.bound(), entry.depth(), entry.move(), entry.eval());
	}

	sync_cout << "info string Loaded " << hashEntries << " hash entries from: " << hashfilename << sync_endl;
}

enum { SAN_MOVE_NORMAL, SAN_PAWN_CAPTURE };

//taken from stockfish-TCEC6-PA_GTB
template <int MoveType> inline Move test_move(Position &pos, Square fromsquare, Square tosquare, PieceType promotion)
{
	Move move;

	if (MoveType == SAN_MOVE_NORMAL) {
		if (promotion != NO_PIECE_TYPE) {
			move = make<PROMOTION>(fromsquare, tosquare, promotion);
		}
		else {
			move = make<NORMAL>(fromsquare, tosquare);
		}
	}
	else if (MoveType == SAN_PAWN_CAPTURE) {
		if (pos.ep_square() == tosquare) {
			move = make<EN_PASSANT>(fromsquare, tosquare);
		}
		else {
			if (promotion != NO_PIECE_TYPE) {
				move = make<PROMOTION>(fromsquare, tosquare, promotion);
			}
			else {
				move = make<NORMAL>(fromsquare, tosquare);
			}
		}
	}
	//if (pos.pseudo_legal(move) && pos.legal(move, pos.pinned_pieces(pos.side_to_move()))) {
	if (pos.pseudo_legal(move) && pos.legal(move)) {
#ifdef SAN_DEBUG
		sync_cout << "found a move: " << move_to_uci(move, false) << sync_endl;
#endif
		return move;
	}
	else {
#ifdef SAN_DEBUG
		sync_cout << "invalid move: " << move_to_uci(move, false) << sync_endl;
#endif
		return MOVE_NONE; // invalid;
	}
	return MOVE_NONE;
}

//taken from stockfish-TCEC6-PA_GTB
Move san_to_move(Position& pos, std::string& str)
{
	std::string uci = str;
	PieceType promotion = NO_PIECE_TYPE;
	bool castles = false;
	bool capture = false;
	Move move = MOVE_NONE;

	size_t idx = uci.find_first_of("+#");
	if (idx != std::string::npos) {
		uci.erase(idx); // erase to end of the string
	}
	idx = uci.find_first_of("=");
	if (idx != std::string::npos) {
		char promo = uci.at(idx);
		switch (promo) {
		case 'Q': promotion = QUEEN; break;
		case 'R': promotion = ROOK; break;
		case 'B': promotion = BISHOP; break;
		case 'N': promotion = KNIGHT; break;
		default: return MOVE_NONE; // invalid
		}
		uci.erase(idx);
	}
	else { // check the last char, is it QRBN?
		char promo2 = uci.at(uci.size() - 1);
		switch (promo2) {
		case 'Q': promotion = QUEEN; break;
		case 'R': promotion = ROOK; break;
		case 'B': promotion = BISHOP; break;
		case 'N': promotion = KNIGHT; break;
		default:; // nixda
		}
		if (promotion != NO_PIECE_TYPE)
			uci.erase(uci.size() - 1);
	}
	idx = uci.find_first_of("x");
	if (idx != std::string::npos) {
		capture = true;
		uci.erase(idx, 1);
	}

	char piece = str.at(0);
	PieceType piecetype;
	std::string thepiece;

	switch (piece) {
	case 'N': piecetype = KNIGHT; break;
	case 'B': piecetype = BISHOP; break;
	case 'R': piecetype = ROOK; break;
	case 'Q': piecetype = QUEEN; break;
	case 'K': piecetype = KING; break;
	case '0':
	case 'O':
		castles = true; piecetype = NO_PIECE_TYPE; break;
	default: piecetype = PAWN;
	}

	if (castles) { // chess 960?
		if (uci == "0-0" || uci == "O-O") {
			if (pos.side_to_move() == WHITE) {
				move = make<CASTLING>(SQ_E1, SQ_H1);
			}
			else {
				move = make<CASTLING>(SQ_E8, SQ_H8);
			}
		}
		else if (uci == "0-0-0" || uci == "O-O-O") {
			if (pos.side_to_move() == WHITE) {
				move = make<CASTLING>(SQ_E1, SQ_A1);
			}
			else {
				move = make<CASTLING>(SQ_E8, SQ_A8);
			}
		}
		if (pos.pseudo_legal(move) && pos.legal(move)) {
			return move;
		}
		return MOVE_NONE; // invalid
	}

	// normal move or promotion
	int torank = uci.at(uci.size() - 1) - '1';
	int tofile = uci.at(uci.size() - 2) - 'a';
	int disambig_r = -1;
	int disambig_f = -1;
	if (piecetype != PAWN && piecetype != KING && uci.size() > 3) {
		char ambig = uci.at(uci.size() - 3);
		if (ambig >= 'a' && ambig <= 'h') {
			disambig_f = ambig - 'a';
		}
		else if (ambig >= '1' && ambig <= '8') {
			disambig_r = ambig - '1';
		}
		else {
			return MOVE_NONE; // invalid;
		}
	}

	Square tosquare = Square((torank * 8) + tofile);
	Bitboard bb;	

	switch (piecetype)
	{
	case PAWN:
	case KNIGHT:
	case BISHOP:
	case ROOK:
	case QUEEN:
	case KING:
		bb = pos.pieces(pos.side_to_move(), piecetype);
		break;
	default:
		return MOVE_NONE; // invalid
	}

	int piececount = popcount(bb);
	while (bb)
	{
		Square s = pop_lsb(bb);

		if (piececount > 1 && (disambig_r >= 0 || disambig_f >= 0))
		{
			if (disambig_r >= 0 && rank_of(s) != Rank(disambig_r))
				continue;
			else if (disambig_f >= 0 && file_of(s) == File(disambig_f))
				continue;
		}

		if (piecetype != PAWN || !capture)
			move = test_move<SAN_MOVE_NORMAL>(pos, s, tosquare, promotion);
		else
			move = test_move<SAN_PAWN_CAPTURE>(pos, s, tosquare, promotion);

		if (move != MOVE_NONE)
			return move;
	}

	return MOVE_NONE;
}

//taken from stockfish-TCEC6-PA_GTB
Value uci_to_score(std::string &str)
{
	Value uci = (Value)atoi(str.c_str());
	Value v = VALUE_NONE;

	if (uci > 32000) {
		v = VALUE_MATE - (32767 - uci);
	}
	else if (uci < -32000) {
		v = -VALUE_MATE + (32767 + uci);
	}
	else {
		v = uci * int(PawnValue) / 100;
	}
	return v;
}

void TranspositionTable::load_epd_to_hash() {
	std::string line;
	std::ifstream myfile(hashfilename);
	Position pos;
	Move bm;
	int ce;
	int depth;
	generation8 = 4; //for storing the positions

	if (myfile.is_open())
	{
		while (getline(myfile, line))
		{
			std::vector<std::string> x = split(line, ';');

			//extract and set position
			std::size_t i = x[0].find("acd"); //depth searched. Is after the fen string
			StateListPtr states(new std::deque<StateInfo>(1));
			sync_cout << x[0].substr(0, i) << sync_endl;
			pos.set(x[0].substr(0, i), Options["UCI_Chess960"], &states->back(), Threads.main());
			
			//depth
			depth = std::stoi(x[0].substr(i + 4));
			sync_cout << depth << sync_endl;

			bm = MOVE_NONE;
			ce = -1000000;

			for (std::vector<int>::size_type j = 1; j <= x.size(); j++) {
				if (bm == MOVE_NONE) {
					i = x[j].find("bm ");
					if (i == 1) {
						sync_cout << x[j].substr(i + 3) << sync_endl;
						std::string stri = x[j].substr(i + 3);
						bm = san_to_move(pos, stri);
						if (bm != MOVE_NONE)
							sync_cout << "move ok" << sync_endl;
						continue;
					}
				}
				if (ce == -1000000) {
					i = x[j].find("ce ");
					if (i == 1) {
						std::string stri = x[j].substr(i + 3);
						ce = uci_to_score(stri);
						sync_cout << "ce " << ce << sync_endl;
						continue;
					}
				}
			}

			TTEntry* tte;
			bool ttHit;
			tte = TT.probe(pos.key(), ttHit);

			tte->save(pos.key(), (Value)ce, true, BOUND_EXACT, (Depth)depth, 
				bm, VALUE_NONE);
		}
		myfile.close();
	}
}

/// TranspositionTable::probe() looks up the current position in the transposition
/// table. It returns true and a pointer to the TTEntry if the position is found.
/// Otherwise, it returns false and a pointer to an empty or least valuable TTEntry
/// to be replaced later. The replace value of an entry is calculated as its depth
/// minus 8 times its relative age. TTEntry t1 is considered more valuable than
/// TTEntry t2 if its replace value is greater than that of t2.

TTEntry* TranspositionTable::probe(const Key key, bool& found) const {

  TTEntry* const tte = first_entry(key);
  for (int i = 0; i < ClusterSize; ++i)
      if (tte[i].key == key || !tte[i].depth8)
      {
          tte[i].genBound8 = uint8_t(generation8 | (tte[i].genBound8 & (GENERATION_DELTA - 1))); // Refresh

          return found = (bool)tte[i].depth8, &tte[i];
      }

  // Find an entry to be replaced according to the replacement strategy
  TTEntry* replace = tte;
  for (int i = 1; i < ClusterSize; ++i)
      // Due to our packed storage format for generation and its cyclic
      // nature we add GENERATION_CYCLE (256 is the modulus, plus what
      // is needed to keep the unrelated lowest n bits from affecting
      // the result) to calculate the entry age correctly even after
      // generation8 overflows into the next cycle.
      if (  replace->depth8 - ((GENERATION_CYCLE + generation8 - replace->genBound8) & GENERATION_MASK)
          >   tte[i].depth8 - ((GENERATION_CYCLE + generation8 -   tte[i].genBound8) & GENERATION_MASK))
          replace = &tte[i];

  return found = false, replace;
}


/// TranspositionTable::hashfull() returns an approximation of the hashtable
/// occupation during a search. The hash is x permill full, as per UCI protocol.

int TranspositionTable::hashfull() const {

  int cnt = 0;
  for (int i = 0; i < 1000; ++i)
      for (int j = 0; j < ClusterSize; ++j)
          cnt += table[i].entry[j].depth8 && (table[i].entry[j].genBound8 & GENERATION_MASK) == generation8;

  return cnt / ClusterSize;
}

} // namespace Stockfish

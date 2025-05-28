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

#include "evaluate.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <tuple>

#include "nnue/network.h"
#include "nnue/nnue_misc.h"
#include "position.h"
#include "types.h"
#include "uci.h"
#include "nnue/nnue_accumulator.h"

namespace Hypnos {

int Eval::MaterialisticEvaluationStrategy = 0;
int Eval::PositionalEvaluationStrategy = 0;
bool Eval::useDynamicStrategy = false;

// Aggressive style: bonus for knights near the enemy king
int Eval::calculate_aggressiveness_bonus(const Position& pos) {
    int bonus = 0;
    for (Square s = SQ_A1; s <= SQ_H8; ++s) {
        if (pos.piece_on(s) == make_piece(pos.side_to_move(), KNIGHT) &&
            pos.is_near_enemy_king(s)) {
            bonus += 20;  // Bonus
        }
    }
    return bonus;
}

// Defensive style: penalty for isolated pawns and bonus for castling
int Eval::calculate_defensiveness_bonus(const Position& pos) {
    int penalty = 0;
    Bitboard pawns = pos.pieces(pos.side_to_move(), PAWN);
    for (Square s = SQ_A1; s <= SQ_H8; ++s) {
        if (pos.piece_on(s) == make_piece(pos.side_to_move(), PAWN) &&
            pos.is_isolated(s, pawns)) {
            penalty -= 15;  // Penalità pedoni isolati
        }
    }
    if (pos.can_castle(CastlingRights(CastlingRights::KING_SIDE | CastlingRights::QUEEN_SIDE))) {
        penalty += 40;  // Bonus arrocco
    }
    return penalty;
}

// Positional style: bonus for bishop pairs and rooks on the seventh rank
int Eval::calculate_positional_bonus(const Position& pos) {
    int bonus = 0;
    for (Square s = SQ_A1; s <= SQ_H8; ++s) {
        if (pos.piece_on(s) == make_piece(pos.side_to_move(), BISHOP)) {
            bonus += 10;  // Bonus for the bishop pair
        }
        if (pos.piece_on(s) == make_piece(pos.side_to_move(), ROOK) &&
            pos.is_on_seventh_rank(s, pos.side_to_move())) {
            bonus += 15;  // Bonus for rooks on the seventh rank
        }
    }
    return bonus;
}

// Returns a static, purely materialistic evaluation of the position from
// the point of view of the side to move. It can be divided by PawnValue to get
// an approximation of the material advantage on the board in terms of pawns.
int Eval::simple_eval(const Position& pos) {
    Color c = pos.side_to_move();
    return PawnValue * (pos.count<PAWN>(c) - pos.count<PAWN>(~c))
         + (pos.non_pawn_material(c) - pos.non_pawn_material(~c));
}

bool Eval::use_smallnet(const Position& pos) { return std::abs(simple_eval(pos)) > 962; }

// Evaluate is the evaluator for the outer world. It returns a static evaluation
// of the position from the point of view of the side to move.
Value Eval::evaluate(const Eval::NNUE::Networks&    networks,
                     const Position&                pos,
                     Eval::NNUE::AccumulatorStack&  accumulators,
                     Eval::NNUE::AccumulatorCaches& caches,
                     int                            optimism) {

    assert(!pos.checkers());

    bool smallNet           = use_smallnet(pos);
    auto [psqt, positional] = smallNet ? networks.small.evaluate(pos, accumulators, &caches.small)
                                       : networks.big.evaluate(pos, accumulators, &caches.big);

    Value nnue = (125 * psqt + 131 * positional) / 128;

// Evaluation adjustment based on style
if (style == Aggressive) {
    nnue += Eval::calculate_aggressiveness_bonus(pos);

    // Adds specific bonuses for the aggressive style
    for (Square s = SQ_A1; s <= SQ_H8; ++s) {
        if (pos.piece_on(s) == make_piece(pos.side_to_move(), KNIGHT) && pos.is_near_enemy_king(s)) {
            nnue += 20; // Bonus for knights near the enemy king
        }
        if (pos.piece_on(s) == make_piece(pos.side_to_move(), PAWN) &&
            relative_rank(pos.side_to_move(), s) >= RANK_5) {
            nnue += 10; // Bonus for advanced pawns
        }
    }
} else if (style == Defensive) {
    nnue -= Eval::calculate_aggressiveness_bonus(pos);
    nnue += Eval::calculate_defensiveness_bonus(pos);

    // Penalizes isolated pawns
    Bitboard pawnSet = pos.pieces(pos.side_to_move(), PAWN);
    for (Square s = SQ_A1; s <= SQ_H8; ++s) {
        if (pos.piece_on(s) == make_piece(pos.side_to_move(), PAWN) && pos.is_isolated(s, pawnSet)) {
            nnue -= 15; // Penalty for isolated pawns
        }
    }

    // Bonus for castling
    if (pos.can_castle(CastlingRights(CastlingRights::KING_SIDE | CastlingRights::QUEEN_SIDE))) {
        nnue += 40;
    }
} else if (style == Positional) {
    nnue += Eval::calculate_positional_bonus(pos);

    // Adds specific bonuses for the positional style
    for (Square s = SQ_A1; s <= SQ_H8; ++s) {
        if (pos.piece_on(s) == make_piece(pos.side_to_move(), BISHOP)) {
            nnue += 10; // Bonus for the bishop pair
        }
        if (pos.piece_on(s) == make_piece(pos.side_to_move(), ROOK) &&
            pos.is_on_seventh_rank(s, pos.side_to_move())) {
            nnue += 15; // Bonus for rooks on the seventh rank
        }
    }
}
    // Re-evaluate the position when higher eval accuracy is worth the time spent
    if (smallNet && (std::abs(nnue) < 236))
    {
        std::tie(psqt, positional) = networks.big.evaluate(pos, accumulators, &caches.big);
        nnue                       = (125 * psqt + 131 * positional) / 128;
        smallNet                   = false;
    }

    // Blend optimism and eval with nnue complexity
    int nnueComplexity = std::abs(psqt - positional);
    optimism += optimism * nnueComplexity / 468;
    nnue -= nnue * nnueComplexity / 18000;

    int material = 535 * pos.count<PAWN>() + pos.non_pawn_material();
    int v        = (nnue * (77777 + material) + optimism * (7777 + material)) / 77777;

    // Damp down the evaluation linearly when shuffling
    v -= v * pos.rule50_count() / 212;

    // Guarantee evaluation does not hit the tablebase range
    v = std::clamp(v, VALUE_TB_LOSS_IN_MAX_PLY + 1, VALUE_TB_WIN_IN_MAX_PLY - 1);

    return v;
}

// Like evaluate(), but instead of returning a value, it returns
// a string (suitable for outputting to stdout) that contains the detailed
// descriptions and values of each evaluation term. Useful for debugging.
// Trace scores are from white's point of view
std::string Eval::trace(Position& pos, const Eval::NNUE::Networks& networks) {

    if (pos.checkers())
        return "Final evaluation: none (in check)";

    Eval::NNUE::AccumulatorStack accumulators;
    auto                         caches = std::make_unique<Eval::NNUE::AccumulatorCaches>(networks);

    std::stringstream ss;
    ss << std::showpoint << std::noshowpos << std::fixed << std::setprecision(2);
    ss << '\n' << NNUE::trace(pos, networks, *caches) << '\n';

    ss << std::showpoint << std::showpos << std::fixed << std::setprecision(2) << std::setw(15);

    auto [psqt, positional] = networks.big.evaluate(pos, accumulators, &caches->big);
    Value v                 = psqt + positional;
    v                       = pos.side_to_move() == WHITE ? v : -v;
    ss << "NNUE evaluation        " << 0.01 * UCIEngine::to_cp(v, pos) << " (white side)\n";

    v = evaluate(networks, pos, accumulators, *caches, VALUE_ZERO);
    v = pos.side_to_move() == WHITE ? v : -v;
    ss << "Final evaluation       " << 0.01 * UCIEngine::to_cp(v, pos) << " (white side)";
    ss << " [with scaled NNUE, ...]";
    ss << "\n";

    return ss.str();
}

}  // namespace Hypnos

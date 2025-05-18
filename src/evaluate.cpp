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
#include <cmath> // Per std::abs
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "incbin/incbin.h"
#include "misc.h"
#include "nnue/evaluate_nnue.h"
#include "nnue/nnue_architecture.h"
#include "position.h"
#include "thread.h"
#include "types.h"
#include "uci.h"

// Macro to embed the default efficiently updatable neural network (NNUE) file
// data in the engine binary (using incbin.h, by Dale Weiler).
// This macro invocation will declare the following three variables
//     const unsigned char        gEmbeddedNNUEData[];  // a pointer to the embedded data
//     const unsigned char *const gEmbeddedNNUEEnd;     // a marker to the end
//     const unsigned int         gEmbeddedNNUESize;    // the size of the embedded file
// Note that this does not work in Microsoft Visual Studio.
#if !defined(_MSC_VER) && !defined(NNUE_EMBEDDING_OFF)
INCBIN(EmbeddedNNUEBig, EvalFileDefaultNameBig);
INCBIN(EmbeddedNNUESmall, EvalFileDefaultNameSmall);
#else
const unsigned char        gEmbeddedNNUEBigData[1]   = {0x0};
const unsigned char* const gEmbeddedNNUEBigEnd       = &gEmbeddedNNUEBigData[1];
const unsigned int         gEmbeddedNNUEBigSize      = 1;
const unsigned char        gEmbeddedNNUESmallData[1] = {0x0};
const unsigned char* const gEmbeddedNNUESmallEnd     = &gEmbeddedNNUESmallData[1];
const unsigned int         gEmbeddedNNUESmallSize    = 1;
#endif


namespace Hypnos {

namespace Eval {


// Global Variables
ShashinStyle CurrentStyle = {0, 0, 0, 0, 0, 0}; // Neutral default style (no influence)

int hysteresisTal = 200;        // Hysteresis for switching to Tal style.
int hysteresisPetrosian = 300;  // Hysteresis for switching to Petrosian style.
int hysteresisCapablanca = 100; // Hysteresis for switching to Capablanca style.

int talCount = 0;               // Counter for Tal style usage.
int petrosianCount = 0;         // Counter for Petrosian style usage.
int capablancaCount = 0;        // Counter for Capablanca style usage.

static int moveCounter = 0;     // Tracks the number of moves made.
int nodeTrigger = 0;            // Node trigger for style updates.
Value lastEvalScore = VALUE_ZERO; // Last evaluation score (updated dynamically).
int lastNodeTrigger = 0;        // Node count at the last trigger.
Value lastScore = VALUE_ZERO;   // Last stored score.
Value currentScore = VALUE_ZERO; // Current evaluation score (default initialized).

// Global Function Declarations
void apply_penalty_progression(); // Handles progression of penalties.

// Apply Dynamic Adjustments to Shashin Weights
void apply_dynamic_shashin_weights(int& talWeight, int& petrosianWeight, int& capablancaWeight, const Position& pos) {
    // Check if Shashin Dynamic Style is enabled
    bool isDynamicStyleEnabled = Options["Shashin Dynamic Style"];

    if (!isDynamicStyleEnabled) {
        // Use static weights if dynamic style is disabled
        sync_cout << "info string Shashin Dynamic Style OFF: Using static weights" << sync_endl;
        sync_cout << "info string Static Weights: Tal(" << talWeight
                  << "), Petrosian(" << petrosianWeight
                  << "), Capablanca(" << capablancaWeight << ")" << sync_endl;
        return; // No adjustments needed
    }

    // Log dynamic style activation
    sync_cout << "info string Shashin Dynamic Style ON: Applying dynamic adjustments" << sync_endl;

    // Determine the dynamic phase of the game
    int phase = NNUE::determine_dynamic_phase(pos);

    // Positional indicators
    PositionalIndicators indicators = compute_positional_indicators(pos);

    // Adjust weights dynamically based on indicators
    talWeight += indicators.kingSafety / 2;          // Increase Tal based on opponent's king safety
    petrosianWeight += indicators.flankControl / 2;  // Increase Petrosian based on flank control
    capablancaWeight += indicators.centerControl / 2; // Increase Capablanca based on center control

    // Apply further phase-based adjustments
    if (phase == 0) { // Opening
        capablancaWeight += 5; // Favor balance in opening
    } else if (phase == 1) { // Middlegame
        talWeight += 5;       // Favor aggression in middlegame
    } else if (phase == 2) { // Endgame
        petrosianWeight += 5; // Favor defense in endgame
    }

    // Normalize weights to ensure the total is at most 100
    int totalWeight = talWeight + petrosianWeight + capablancaWeight;
    if (totalWeight > 0) {
        talWeight = (talWeight * 100) / totalWeight;
        petrosianWeight = (petrosianWeight * 100) / totalWeight;
        capablancaWeight = (capablancaWeight * 100) / totalWeight;
    }

    // Log the adjusted dynamic weights
    sync_cout << "info string Dynamic Weights Applied: Tal(" << talWeight
              << "), Petrosian(" << petrosianWeight
              << "), Capablanca(" << capablancaWeight << ")" << sync_endl;
}

// Function to Set Shashin Style
void set_shashin_style(Style style) {
    switch (style) {
        case Style::Tal:
            CurrentStyle = {25, 5, 0, 25, 3, 0};
            break;
        case Style::Capablanca:
            CurrentStyle = {10, 15, 10, 10, 15, 10};
            break;
        case Style::Petrosian:
            CurrentStyle = {0, 5, 25, 0, 3, 25};
            break;
    }
}

// Return true if Shashin Style usage is enabled
bool style_is_enabled() {
    return (bool)Options["Use Shashin Style"];
}

// String-Compatible Shashin Style Setter
void set_shashin_style(const std::string& style) {

    if (!style_is_enabled()) {
        CurrentStyle = {0, 0, 0, 0, 0, 0}; // Force neutral style if disabled
        sync_cout << "info string Shashin Style change ignored (disabled)" << sync_endl;
        return;
    }

    if (style == "Tal") {
        set_shashin_style(Style::Tal);          // Set aggressive, tactical style.
    } 
    else if (style == "Capablanca") {
        set_shashin_style(Style::Capablanca);   // Set balanced, positional style.
    } 
    else if (style == "Petrosian") {
        set_shashin_style(Style::Petrosian);    // Set defensive, strategic style.
    } 
    else {
        set_shashin_style(Style::Capablanca);   // Fallback for invalid input.
        sync_cout << "info string Shashin Style fallback to Capablanca (invalid input)" << sync_endl;
        return;
    }

    // Confirm applied style
    sync_cout << "info string Shashin Style is now: " << style << sync_endl;
}

// Apply Custom Blend of Shashin Styles
void set_shashin_custom_blend(int talWeight, int petrosianWeight, int capablancaWeight) {
    int totalWeight = talWeight + petrosianWeight + capablancaWeight;

    if (totalWeight == 0) {
        set_shashin_style(Style::Capablanca); // Default to Capablanca if all weights are zero.
        return;
    }

    // Calculate style ratios based on weights.
    float talRatio = static_cast<float>(talWeight) / totalWeight;
    float petrosianRatio = static_cast<float>(petrosianWeight) / totalWeight;
    float capablancaRatio = static_cast<float>(capablancaWeight) / totalWeight;

    // Blend attack, defense, and balance values.
    CurrentStyle.attack = std::clamp(static_cast<int>(25 * talRatio + 10 * capablancaRatio + 0 * petrosianRatio), 0, 30);
    CurrentStyle.defense = std::clamp(static_cast<int>(5 * talRatio + 15 * capablancaRatio + 25 * petrosianRatio), 0, 30);
    CurrentStyle.balance = std::clamp(static_cast<int>(10 * talRatio + 10 * capablancaRatio + 5 * petrosianRatio), 0, 30);
}

// Apply Progressive Penalty to Style Hysteresis
void apply_penalty_progression() {
    static int consecutiveTal = 0;          // Tracks consecutive Tal style dominance.
    static int consecutivePetrosian = 0;   // Tracks consecutive Petrosian style dominance.
    static int consecutiveCapablanca = 0;  // Tracks consecutive Capablanca style dominance.

    // Update consecutive counters based on dominant style.
    if (CurrentStyle.attack > 10) { // Tal dominant
        consecutiveTal++;
        consecutivePetrosian = 0;
        consecutiveCapablanca = 0;
    } else if (CurrentStyle.defense > 10) { // Petrosian dominant
        consecutivePetrosian++;
        consecutiveTal = 0;
        consecutiveCapablanca = 0;
    } else { // Capablanca dominant
        consecutiveCapablanca++;
        consecutiveTal = 0;
        consecutivePetrosian = 0;
    }

    // Apply hysteresis adjustments for prolonged dominance.
    if (consecutiveTal > 5) { // Adjust hysteresis for prolonged Tal dominance.
        hysteresisTal += 10;
        hysteresisPetrosian -= 5;
        hysteresisCapablanca -= 5;
        consecutiveTal = 0; // Reset counter.
    }

    if (consecutivePetrosian > 5) { // Adjust hysteresis for prolonged Petrosian dominance.
        hysteresisPetrosian += 10;
        hysteresisTal -= 5;
        hysteresisCapablanca -= 5;
        consecutivePetrosian = 0; // Reset counter.
    }

    if (consecutiveCapablanca > 5) { // Adjust hysteresis for prolonged Capablanca dominance.
        hysteresisCapablanca -= 10;
        hysteresisTal += 5;
        hysteresisPetrosian += 5;
        consecutiveCapablanca = 0; // Reset counter.
    }
}

// Additional Functions
int compute_material_imbalance(const Position& pos) {
    // Compute the material imbalance for non-pawn pieces.
    return pos.non_pawn_material(WHITE) - pos.non_pawn_material(BLACK);
}

int compute_center_control(const Position& pos) {
    // Evaluate control over central squares.
    Square centralSquares[] = { SQ_D4, SQ_E4, SQ_D5, SQ_E5 };
    int control = 0;

    for (Square sq : centralSquares) {
        // Increment control if a piece occupies the square.
        if (pos.piece_on(sq) != NO_PIECE) {
            control++;
        }
    }
    return control;
}

void recalibrate_parameters(Value score) {
    // Recalibrate hysteresis values based on the score and style usage.
    int totalStyles = talCount + petrosianCount + capablancaCount;

    if (totalStyles == 0) return; // Exit if no styles are tracked.

    // Calculate style ratios.
    float talRatio = static_cast<float>(talCount) / totalStyles;
    float petrosianRatio = static_cast<float>(petrosianCount) / totalStyles;
    float capablancaRatio = static_cast<float>(capablancaCount) / totalStyles;

    // Adjust hysteresis based on score changes.
    Value deltaScore = std::abs(Threads.main()->bestPreviousScore - score);

    if (deltaScore > hysteresisTal / 2) {
        hysteresisTal += 10;
    }
    if (deltaScore < hysteresisCapablanca / 3) {
        hysteresisCapablanca -= 5;
    }

    // Adjust hysteresis based on style dominance.
    if (talRatio > 0.5) { // Tal dominant.
        hysteresisTal += 100;
        hysteresisCapablanca -= 40;
        hysteresisPetrosian -= 20;
    }

    if (petrosianRatio > 0.5) { // Petrosian dominant.
        hysteresisPetrosian += 10;
        hysteresisTal -= 5;
        hysteresisCapablanca -= 5;
    }

    if (capablancaRatio < 0.2) { // Capablanca underutilized.
        hysteresisCapablanca -= 50;
        hysteresisTal += 30;
    }

    // Clamp hysteresis values to predefined ranges.
    hysteresisTal = std::clamp(hysteresisTal, 150, 500);
    hysteresisPetrosian = std::clamp(hysteresisPetrosian, 100, 400);
    hysteresisCapablanca = std::clamp(hysteresisCapablanca, 30, 200);

    // Enforce Capablanca style after prolonged underuse.
    ++moveCounter;
    if (moveCounter > 50 && capablancaCount < totalStyles / 3) {
        set_shashin_style("Capablanca");
        moveCounter = 0;
    }
}

// Calculate Dynamic Blend of Shashin Styles
void calculate_dynamic_blend(int& talWeight, int& petrosianWeight, int& capablancaWeight, const Position& pos) {
    // Use material and score delta to adjust style weights.
    int totalMaterial = pos.non_pawn_material(WHITE) + pos.non_pawn_material(BLACK);
    Value deltaScore = std::abs(Threads.main()->bestPreviousScore - Threads.main()->currentScore);

    // Increase Tal weight for aggressive scenarios.
    if (totalMaterial > 2000 && deltaScore > 50) {
        talWeight = std::clamp(talWeight + 10, 0, 100);
        capablancaWeight = std::clamp(capablancaWeight - 5, 0, 100);
        petrosianWeight = std::clamp(petrosianWeight - 5, 0, 100);
    }

    // Increase Petrosian weight for defensive scenarios.
    if (deltaScore < 20 && totalMaterial < 1500) {
        talWeight = std::clamp(talWeight - 5, 0, 100);
        capablancaWeight = std::clamp(capablancaWeight - 5, 0, 100);
        petrosianWeight = std::clamp(petrosianWeight + 10, 0, 100);
    }

    // Default to Capablanca for balanced positions.
    if (std::abs(deltaScore) < 30 && totalMaterial >= 1500 && totalMaterial <= 2500) {
        talWeight = std::clamp(talWeight - 5, 0, 100);
        capablancaWeight = std::clamp(capablancaWeight + 10, 0, 100);
        petrosianWeight = std::clamp(petrosianWeight - 5, 0, 100);
    }
}

// Dynamic Shashin Style Function
void dynamic_shashin_style(const Position& pos, Value& score, int totalMaterial) {

    // Abort if Shashin Style is disabled
    if (!style_is_enabled()) {
        return;
    }

    static Style lastStyle = Style::Capablanca;   // Tracks the last applied style.
    static int lastChangeNodes = 0;              // Tracks nodes since the last style change.

    // Ignore small score changes.
    if (std::abs(score - lastScore) < toleranceBuffer) {
        return; // Skip updates if score change is insignificant.
    }
    lastScore = score; // Update the last known score.

    // Avoid frequent function executions.
    const int nodesSearched = Threads.nodes_searched();
    if (nodesSearched - lastNodeTrigger < 1500) { // Check every 1,500 nodes.
        return;
    }
    lastNodeTrigger = nodesSearched; // Update node trigger.

    // Check if dynamic Shashin style is enabled via UCI options.
    if (!Options["Shashin Dynamic Style"]) {
        return; // Exit if the option is disabled.
    }

    // Parameters and constants.
    constexpr int minNodeInterval = 50;         // Minimum interval for updates.
    constexpr int hysteresisThreshold = 10;     // Threshold for hysteresis.

    // Update NNUE and Shashin weights based on the game phase.
    int phase = determine_dynamic_phase(pos);   // Determine the current game phase.
    int talWeight = 20, petrosianWeight = 20, capablancaWeight = 20; // Default weights.
    Eval::NNUE::update_weights(phase, pos, talWeight, petrosianWeight, capablancaWeight); // Adjust weights dynamically.

    // Calculate Positional Indicators and Update Style
    // Compute positional indicators for the current position.
    const PositionalIndicators indicators = compute_positional_indicators(pos);

    // Precalculate values for the CurrentStyle parameters.
    const int attackBase = 20 + indicators.centerControl - indicators.kingSafety / 4;  // Base value for attack.
    const int defenseBase = 10 - indicators.centerControl + indicators.kingSafety / 3; // Base value for defense.
    const int balanceBase = 25 + indicators.centerControl / 3 - indicators.materialImbalance / 6; // Base value for balance.

    // Clamp calculated values to predefined ranges.
    CurrentStyle.attack = std::clamp(attackBase, 15, 28);
    CurrentStyle.defense = std::clamp(defenseBase, 5, 15);
    CurrentStyle.balance = std::clamp(balanceBase, 20, 30);

    // Ensure the total of style components does not exceed 70.
    if (CurrentStyle.attack + CurrentStyle.defense + CurrentStyle.balance > 70) {
        CurrentStyle.attack = std::clamp(CurrentStyle.attack, 15, 25); // Adjust attack.
        CurrentStyle.defense = std::clamp(CurrentStyle.defense, 5, 20); // Adjust defense.
        CurrentStyle.balance = 70 - CurrentStyle.attack - CurrentStyle.defense; // Balance remaining points.
    }

    // Check if sufficient nodes have been searched for a style change.
    if (nodesSearched - lastChangeNodes < minNodeInterval) {
        return; // Skip style update if not enough nodes have passed.
    }

    // Manage Hysteresis and Dynamically Adjust Style
    // Calculate deltaScore to avoid unnecessary style changes.
    constexpr int hysteresisIncrement = 10;    // Increase hysteresis for stable changes.
    constexpr int hysteresisDecrement = 5;     // Decrease hysteresis when less relevant.
    const Value deltaScore = std::abs(score - Threads.main()->bestPreviousScore);
    if (deltaScore < hysteresisThreshold) {
        return; // Skip updates if score variation is below threshold.
    }

    // Update hysteresis values based on deltaScore.
    hysteresisTal += (deltaScore > 50 ? hysteresisIncrement : -hysteresisDecrement);
    hysteresisPetrosian += (deltaScore < 20 ? hysteresisIncrement : -hysteresisDecrement);
    hysteresisCapablanca += (std::abs(deltaScore) < 30 ? hysteresisIncrement : -hysteresisDecrement);

    // Clamp hysteresis values within valid ranges.
    hysteresisTal = std::clamp(hysteresisTal, 150, 500);
    hysteresisPetrosian = std::clamp(hysteresisPetrosian, 100, 400);
    hysteresisCapablanca = std::clamp(hysteresisCapablanca, 30, 200);

    // Determine new style based on dynamic thresholds.
    Style newStyle = lastStyle;
    const float attackThreshold = hysteresisTal * 1.2f + CurrentStyle.attack;
    const float defenseThreshold = hysteresisPetrosian * 1.2f + CurrentStyle.defense;
    const float balanceThreshold = hysteresisCapablanca * 1.2f + CurrentStyle.balance;

    if (totalMaterial > 2000 && score > attackThreshold) { // Favor Tal for high material and score.
        newStyle = Style::Tal;
        ++talCount;
    } else if (score < -defenseThreshold) { // Favor Petrosian for defensive situations.
        newStyle = Style::Petrosian;
        ++petrosianCount;
    } else if (std::abs(score) < balanceThreshold) { // Favor Capablanca for balanced positions.
        newStyle = Style::Capablanca;
        ++capablancaCount;
    }

    // Change style only if necessary and update state.
    if (newStyle != lastStyle) {
        set_shashin_style(newStyle);      // Apply the new style.
        lastStyle = newStyle;             // Update last applied style.
        lastChangeNodes = nodesSearched; // Track node count for the change.
    }

    // Apply penalty progression to stabilize style transitions.
    apply_penalty_progression();

    // Recalibrate hysteresis parameters based on the current score.
    recalibrate_parameters(score);

    // Increment move counter and reset if Capablanca style dominates.
    if (++moveCounter > 50 && newStyle == Style::Capablanca) {
        moveCounter = 0; // Reset to allow minor oscillations.
    }
}

// Calculate Distance from a Square to the Board Center
int distance_to_center(Square sq) {
    const int centerFiles[] = {FILE_D, FILE_E}; // Files closest to the center.
    const int centerRanks[] = {RANK_4, RANK_5}; // Ranks closest to the center.

    int file = file_of(sq);                     // File of the given square.
    int rank = rank_of(sq);                     // Rank of the given square.

    // Minimum distance from the square to center files and ranks.
    int minFileDistance = std::min(std::abs(file - centerFiles[0]), std::abs(file - centerFiles[1]));
    int minRankDistance = std::min(std::abs(rank - centerRanks[0]), std::abs(rank - centerRanks[1]));

    return minFileDistance + minRankDistance;   // Total Manhattan distance to the center.
}

// Return the Value of a Chess Piece
int piece_value(Piece piece) {
    // Assign standard values to each piece type.
    switch (type_of(piece)) {
    case PAWN:   return 100;    // Pawn value.
    case KNIGHT: return 320;    // Knight value.
    case BISHOP: return 330;    // Bishop value.
    case ROOK:   return 500;    // Rook value.
    case QUEEN:  return 900;    // Queen value.
    case KING:   return 20000;  // King value (infinite for practical purposes).
    default:     return 0;      // No piece or invalid piece.
    }
}

// Calculate Material Factor of the Position
int compute_material_factor(const Position& pos) {
    int materialFactor = 0; // Accumulates the total material value.

    // Iterate through all squares on the board.
    for (Square sq = SQ_A1; sq <= SQ_H8; ++sq) {
        if (pos.piece_on(sq) != NO_PIECE && type_of(pos.piece_on(sq)) != KING) {
            materialFactor += piece_value(pos.piece_on(sq));
        }
    }
    return materialFactor; // Return the total material value.
}

// Function to dynamically determine the game phase
int determine_dynamic_phase(const Position& pos) {
    // Count the heavy pieces (rooks and queens)
    int heavyPieces = popcount(pos.pieces(ROOK)) + popcount(pos.pieces(QUEEN));

    // Count the light pieces (knights and bishops)
    int lightPieces = popcount(pos.pieces(KNIGHT)) + popcount(pos.pieces(BISHOP));

    // Count the advanced pawns
    int advancedPawnsWhite = popcount(pos.pieces(PAWN, WHITE) & (Rank6BB | Rank7BB | Rank8BB));
    int advancedPawnsBlack = popcount(pos.pieces(PAWN, BLACK) & (Rank3BB | Rank2BB | Rank1BB));
    int advancedPawns = advancedPawnsWhite + advancedPawnsBlack;

    // Calculate the total material
    int remainingMaterial = compute_material_factor(pos);

    // Determine the phase based on combined logic
    int phase;

    // Condition for opening
    if (remainingMaterial > 3000 && heavyPieces >= 4 && lightPieces >= 3) {
        phase = 0; // Opening
    }
    // Condition for middlegame
    else if (remainingMaterial >= 2000 && remainingMaterial <= 3000 && heavyPieces <= 3 && lightPieces >= 1) {
        phase = 1; // Middlegame
    }
    // Condition for middlegame
    else if (remainingMaterial >= 2000 && remainingMaterial <= 3000 && heavyPieces <= 3 && lightPieces >= 1) {
        phase = 1; // Middlegame
    }
    // Condition for endgame
    else if (remainingMaterial < 2000 && heavyPieces <= 2 && lightPieces <= 2 && advancedPawns >= 1) {
        phase = 2; // Endgame
    }
    // Fallback
    else {
        phase = 1; // Default to Middlegame
    }

    return phase;
}

// Calculate Adjacent Squares to a Given Square
Bitboard adjacent_squares(Square sq) {
    Bitboard adjacent = 0;

    // Add squares to the left and right.
    if (file_of(sq) > FILE_A)
        adjacent |= square_bb(Square(sq - 1));
    if (file_of(sq) < FILE_H)
        adjacent |= square_bb(Square(sq + 1));

    // Add squares above and below.
    if (rank_of(sq) > RANK_1)
        adjacent |= square_bb(Square(sq - 8));
    if (rank_of(sq) < RANK_8)
        adjacent |= square_bb(Square(sq + 8));

    return adjacent;
}

// Compute King Safety
int compute_king_safety(const Position& pos) {
    Square ownKing = pos.square<KING>(pos.side_to_move());               // King's square.
    Bitboard attackers = pos.attackers_to(ownKing, ~pos.side_to_move()); // Attacking pieces.

    int penalty = popcount(attackers) * 10; // Penalty for attackers near the king.

    // Bonus for pawn shield around the king.
    Bitboard kingShield = pos.pieces(PAWN, pos.side_to_move()) & adjacent_squares(ownKing);
    int shieldBonus = popcount(kingShield) * 5;

    // Higher values indicate weaker king safety.
    return penalty - shieldBonus;
}

// Compute Open File Control
int compute_open_file_control(const Position& pos) {
    int openFileControl = 0;
    Bitboard pawns = pos.pieces(PAWN);  // All pawns.
    Bitboard rooks = pos.pieces(ROOK); // All rooks.

    for (File f = FILE_A; f <= FILE_H; ++f) {
        Bitboard fileMask = file_bb(f); // Bitboard mask for the file.

        // If the file is open, count rook control.
        if (!(pawns & fileMask)) {
            openFileControl += popcount(rooks & fileMask) * 5;
        }
    }
    return openFileControl;
}

// Compute Center Dominance
int compute_center_dominance(const Position& pos) {
    // Mask for central squares.
    Bitboard centerSquares = (square_bb(SQ_D4) | square_bb(SQ_D5) |
                              square_bb(SQ_E4) | square_bb(SQ_E5));
    
    // Pieces occupying central squares.
    Bitboard piecesInCenter = pos.pieces() & centerSquares;

    // Attacks on central squares.
    Bitboard attacksOnCenter = 0;
    for (Square sq = SQ_A1; sq <= SQ_H8; ++sq) {
        if (centerSquares & square_bb(sq)) {
            attacksOnCenter |= pos.attackers_to(sq);
        }
    }

    // Evaluate based on occupation and attacks.
    return (popcount(piecesInCenter) * 3) + popcount(attacksOnCenter);
}

// Compute Aggressivity
int compute_aggressivity(const Position& pos) {
    Square enemyKing = pos.square<KING>(~pos.side_to_move()); // Opponent's king.
    return popcount(pos.attackers_to(enemyKing));             // Count attackers on the king.
}

// Compute Positional Advantage
int compute_position(const Position& pos) {
    // Count own pieces in the central squares.
    Bitboard centerSquares = SQ_D4 | SQ_D5 | SQ_E4 | SQ_E5;
    return popcount(pos.pieces(pos.side_to_move()) & centerSquares);
}

// Compute Defensive Capability
int compute_defense(const Position& pos) {
    Square ownKing = pos.square<KING>(pos.side_to_move()); // Own king's square.
    return popcount(pos.attackers_to(ownKing));            // Count defenders around the king.
}

// Compute Flank Control
int compute_flank_control(const Position& pos) {
    // Critical squares on the flanks.
    Square flankSquares[] = { SQ_A4, SQ_A5, SQ_H4, SQ_H5 };
    int control = 0;

    for (Square sq : flankSquares) {
        if (pos.attackers_to(sq) & pos.pieces(WHITE)) // Add control for white pieces.
            control++;
        if (pos.attackers_to(sq) & pos.pieces(BLACK)) // Subtract control for black pieces.
            control--;
    }
    return control;
}

// Calculate Advanced Open File Control
int compute_advanced_open_file_control(const Position& pos) {
    int control = 0;

    // Iterate through all files.
    for (File f = FILE_A; f <= FILE_H; ++f) {
        // Check if the file is open.
        if (pos.is_open_file(Square(f))) {
            // Evaluate all ranks on the open file.
            for (Rank r = RANK_1; r <= RANK_8; ++r) {
                Square sq = make_square(f, r);

                // Add control for white rooks and queens.
                if (pos.attackers_to(sq) & pos.pieces(ROOK, WHITE)) {
                    control += 2; // Double weight for rooks.
                }
                if (pos.attackers_to(sq) & pos.pieces(QUEEN, WHITE)) {
                    control += 1; // Single weight for queens.
                }

                // Subtract control for black rooks and queens.
                if (pos.attackers_to(sq) & pos.pieces(ROOK, BLACK)) {
                    control -= 2;
                }
                if (pos.attackers_to(sq) & pos.pieces(QUEEN, BLACK)) {
                    control -= 1;
                }
            }
        }
    }
    return control;
}

// Calculate Piece Activity Based on Advanced Squares
int compute_piece_activity(const Position& pos) {
    // Advanced squares that influence activity evaluation.
    Square advancedSquares[] = { SQ_D4, SQ_E4, SQ_D5, SQ_E5, SQ_F4, SQ_F5 };
    int activity = 0;

    // Evaluate activity for each advanced square.
    for (Square sq : advancedSquares) {
        if (pos.attackers_to(sq) & pos.pieces(WHITE))
            activity++; // Increment for white control.
        if (pos.attackers_to(sq) & pos.pieces(BLACK))
            activity--; // Decrement for black control.
    }
    return activity;
}

// Compute Positional Indicators
PositionalIndicators compute_positional_indicators(const Position& pos) {
    // Initialize all indicators to zero.
    PositionalIndicators indicators = {0, 0, 0, 0, 0, 0, 0, 0};

    // Calculate individual indicators.
    indicators.kingSafety = compute_king_safety(pos);              // King safety evaluation.
    indicators.openFileControl = compute_advanced_open_file_control(pos); // Advanced open file control.
    indicators.centerDominance = compute_center_dominance(pos);    // Center dominance evaluation.
    indicators.materialImbalance = compute_material_imbalance(pos); // Material imbalance evaluation.
    indicators.centerControl = compute_center_control(pos);        // Central control evaluation.
    indicators.flankControl = compute_flank_control(pos);          // Flank control (new indicator).
    indicators.pieceActivity = compute_piece_activity(pos);        // Piece activity (new indicator).
    indicators.defensivePosition = compute_defense(pos);           // Defensive position evaluation.

    return indicators; // Return the structure containing all indicators.
}

std::unordered_map<NNUE::NetSize, EvalFile> EvalFiles = {
  {NNUE::Big, {"EvalFile", EvalFileDefaultNameBig, "None"}},
  {NNUE::Small, {"EvalFileSmall", EvalFileDefaultNameSmall, "None"}}};

// Tries to load a NNUE network at startup time, or when the engine
// receives a UCI command "setoption name EvalFile value nn-[a-z0-9]{12}.nnue"
// The name of the NNUE network is always retrieved from the EvalFile option.
// We search the given network in three locations: internally (the default
// network may be embedded in the binary), in the active working directory and
// in the engine directory. Distro packagers may define the DEFAULT_NNUE_DIRECTORY
// variable to have the engine search in a special directory in their distro.
void NNUE::init() {

    for (auto& [netSize, evalFile] : EvalFiles)
    {
        // Replace with
        // Options[evalFile.option_name]
        // once fishtest supports the uci option EvalFileSmall
        std::string user_eval_file =
          netSize == Small ? evalFile.default_name : Options[evalFile.option_name];

        if (user_eval_file.empty())
            user_eval_file = evalFile.default_name;

#if defined(DEFAULT_NNUE_DIRECTORY)
        std::vector<std::string> dirs = {"<internal>", "", CommandLine::binaryDirectory,
                                         stringify(DEFAULT_NNUE_DIRECTORY)};
#else
        std::vector<std::string> dirs = {"<internal>", "", CommandLine::binaryDirectory};
#endif

        for (const std::string& directory : dirs)
        {
            if (evalFile.selected_name != user_eval_file)
            {
                if (directory != "<internal>")
                {
                    std::ifstream stream(directory + user_eval_file, std::ios::binary);
                    if (NNUE::load_eval(user_eval_file, stream, netSize))
                        evalFile.selected_name = user_eval_file;
                }

                if (directory == "<internal>" && user_eval_file == evalFile.default_name)
                {
                    // C++ way to prepare a buffer for a memory stream
                    class MemoryBuffer: public std::basic_streambuf<char> {
                       public:
                        MemoryBuffer(char* p, size_t n) {
                            setg(p, p, p + n);
                            setp(p, p + n);
                        }
                    };

                    MemoryBuffer buffer(
                      const_cast<char*>(reinterpret_cast<const char*>(
                        netSize == Small ? gEmbeddedNNUESmallData : gEmbeddedNNUEBigData)),
                      size_t(netSize == Small ? gEmbeddedNNUESmallSize : gEmbeddedNNUEBigSize));
                    (void) gEmbeddedNNUEBigEnd;  // Silence warning on unused variable
                    (void) gEmbeddedNNUESmallEnd;

                    std::istream stream(&buffer);
                    if (NNUE::load_eval(user_eval_file, stream, netSize))
                        evalFile.selected_name = user_eval_file;
                }
            }
        }
    }
}

// Verifies that the last net used was loaded successfully
void NNUE::verify() {

    for (const auto& [netSize, evalFile] : EvalFiles)
    {
        // Replace with
        // Options[evalFile.option_name]
        // once fishtest supports the uci option EvalFileSmall
        std::string user_eval_file =
          netSize == Small ? evalFile.default_name : Options[evalFile.option_name];
        if (user_eval_file.empty())
            user_eval_file = evalFile.default_name;

        if (evalFile.selected_name != user_eval_file)
        {
            std::string msg1 =
              "Network evaluation parameters compatible with the engine must be available.";
            std::string msg2 =
              "The network file " + user_eval_file + " was not loaded successfully.";
            std::string msg3 = "The UCI option EvalFile might need to specify the full path, "
                               "including the directory name, to the network file.";
            std::string msg4 = "The default net can be downloaded from: "
                               "https://tests.stockfishchess.org/api/nn/"
                             + evalFile.default_name;
            std::string msg5 = "The engine will be terminated now.";

            sync_cout << "info string ERROR: " << msg1 << sync_endl;
            sync_cout << "info string ERROR: " << msg2 << sync_endl;
            sync_cout << "info string ERROR: " << msg3 << sync_endl;
            sync_cout << "info string ERROR: " << msg4 << sync_endl;
            sync_cout << "info string ERROR: " << msg5 << sync_endl;

            exit(EXIT_FAILURE);
        }

        sync_cout << "info string NNUE evaluation using " << user_eval_file << sync_endl;
    }
}

// Determine Game Phase Based on Total Material
int determine_phase(const Position& pos, int totalMaterial) {
    // Scores for mobility and pawn structure (implement if not present).
    int mobilityScore = pos.mobility_score();  // Evaluates piece mobility.
    int pawnStructureScore = pos.pawn_structure_score();  // Evaluates pawn structure quality.

    // Opening phase: High material and good mobility.
    if (totalMaterial > 12000 && mobilityScore > 30)
        return 0; // Opening.

    // Middlegame phase: Moderate material or dynamic factors.
    else if (totalMaterial > 3000 || mobilityScore > 15 || pawnStructureScore < 50)
        return 1; // Middlegame.

    // Endgame phase: Low material and less dynamism.
    return 2; // Endgame.
}

// Blend NNUE Evaluation with a Simpler Evaluation
int blend_nnue_with_simple(int nnue, int simpleEval, int nnueComplexity, int materialImbalance) {
    // Calculate complexity factor (limits influence of NNUE in high-complexity positions).
    int complexityFactor = std::min(50, nnueComplexity / 2);

    // Adjust weight based on material imbalance.
    int imbalanceFactor = std::abs(materialImbalance) > 200 ? 10 : 0;

    // Determine blend weight (scaled between 50 and 100).
    int weight = std::clamp(100 - complexityFactor - imbalanceFactor, 50, 100);

    // Combine NNUE and simple evaluation using the calculated weight.
    return (nnue * weight + simpleEval * (100 - weight)) / 100;
}

// Apply Dampened Shuffling to Avoid Excessive Changes
int dampened_shuffling(int shuffling) {
    // Use logarithmic dampening for high shuffling values.
    return shuffling < 20 ? shuffling : int(15 * std::log2(shuffling + 1));
}

// Returns a static, purely materialistic evaluation of the position from
// the point of view of the given color. It can be divided by PawnValue to get
// an approximation of the material advantage on the board in terms of pawns.
int simple_eval(const Position& pos, Color c) {
    return PawnValue * (pos.count<PAWN>(c) - pos.count<PAWN>(~c))
         + (pos.non_pawn_material(c) - pos.non_pawn_material(~c));
}

// Evaluate is the evaluator for the outer world. It returns a static evaluation
// of the position from the point of view of the side to move.
Value evaluate(const Position& pos) {

    assert(!pos.checkers());

    Value score = Eval::NNUE::evaluate<Eval::NNUE::NetSize::Big>(pos, false, nullptr, false);

// Calculate Total Material on the Board
int totalMaterial = 0;

// Iterate over both colors (WHITE and BLACK).
for (Color c : {WHITE, BLACK}) {
    // Add the material value of each piece type for the current color.
    totalMaterial += pos.count<PAWN>(c) * 100;    // Pawns: 100 per piece.
    totalMaterial += pos.count<KNIGHT>(c) * 320;  // Knights: 320 per piece.
    totalMaterial += pos.count<BISHOP>(c) * 330;  // Bishops: 330 per piece.
    totalMaterial += pos.count<ROOK>(c) * 500;    // Rooks: 500 per piece.
    totalMaterial += pos.count<QUEEN>(c) * 900;   // Queens: 900 per piece.
}

// Determine Game Phase and Positional Indicators
int phase = determine_phase(pos, totalMaterial); // Determine the current game phase based on material.

int talWeight = 0, petrosianWeight = 0, capablancaWeight = 0; // Initialize dynamic weights for styles.

// Update dynamic weights based on position and game phase.
Hypnos::Eval::NNUE::update_weights(phase, pos, talWeight, petrosianWeight, capablancaWeight);

// Dynamically adapt style if enabled in the options.
if (Options["Shashin Dynamic Style"]) {
    dynamic_shashin_style(pos, score, totalMaterial);
}

// Apply bonuses based on Shashin dynamic style.
Value aggressivityBonus = talWeight * compute_aggressivity(pos); // Aggression bonus for Tal style.
Value positionalBonus   = capablancaWeight * compute_position(pos); // Positional bonus for Capablanca style.
Value defensiveBonus    = petrosianWeight * compute_defense(pos); // Defensive bonus for Petrosian style.

// Sum Shashin style bonuses into the score.
score += aggressivityBonus + positionalBonus + defensiveBonus;

return score; // Return the final evaluation score.
}

// Functions to Calculate Dynamic Weights
int calculate_tal_weight(const Position& pos, PositionalIndicators indicators) {
    (void)pos; // Silence unused parameter warning.
    return 3 * indicators.centerDominance + 2 * indicators.kingSafety + indicators.openFileControl;
}

int calculate_capablanca_weight(const Position& pos, PositionalIndicators indicators) {
    (void)pos;
    return 2 * indicators.materialImbalance + indicators.centerControl + indicators.openFileControl;
}

int calculate_petrosian_weight(const Position& pos, PositionalIndicators indicators) {
    (void)pos;
    return 2 * indicators.flankControl + indicators.defensivePosition + indicators.pieceActivity;
}

// Update Weights Dynamically Based on Game Phase
void update_weights(int phase, const Position& pos, int& talWeight, int& petrosianWeight, int& capablancaWeight) {
    static int lastPhase = -1;
    static int lastTalWeight = -1, lastPetrosianWeight = -1, lastCapablancaWeight = -1;

    // Avoid redundant updates if weights and phase are unchanged.
    if (phase == lastPhase && talWeight == lastTalWeight &&
        petrosianWeight == lastPetrosianWeight && capablancaWeight == lastCapablancaWeight) {
        return;
    }

    // Update the last recorded state.
    lastPhase = phase;
    lastTalWeight = talWeight;
    lastPetrosianWeight = petrosianWeight;
    lastCapablancaWeight = capablancaWeight;

    // Calculate positional indicators for the position.
    PositionalIndicators indicators = compute_positional_indicators(pos);

    // Dynamically blend weights based on the game phase.
    float phaseFactor = phase / 100.0f; // Normalize phase between 0 (endgame) and 1 (opening).
    talWeight = static_cast<int>((1 - phaseFactor) * indicators.centerDominance + phaseFactor * indicators.kingSafety);
    capablancaWeight = static_cast<int>((1 - phaseFactor) * indicators.materialImbalance + phaseFactor * indicators.centerControl);
     petrosianWeight = static_cast<int>((1 - phaseFactor) * indicators.flankControl + phaseFactor * indicators.pieceActivity);

    // Add additional calculations to finalize weights.
    talWeight += calculate_tal_weight(pos, indicators);
    capablancaWeight += calculate_capablanca_weight(pos, indicators);
    petrosianWeight += calculate_petrosian_weight(pos, indicators);

    // Additional evaluation with simple parameters.
    int simpleEval = simple_eval(pos, pos.side_to_move()); // Simple evaluation based on side-to-move.
    bool smallNet = std::abs(simpleEval) > SmallNetThreshold;
    bool psqtOnly = std::abs(simpleEval) > PsqtOnlyThreshold;
    int nnueComplexity;
    int v;

    Value nnue = smallNet ? NNUE::evaluate<NNUE::Small>(pos, true, &nnueComplexity, psqtOnly)
                          : NNUE::evaluate<NNUE::Big>(pos, true, &nnueComplexity, false);

// Adjust NNUE Score Based on Sacrifices and Symmetry
// Temporary increment for speculative sacrifices.
if (pos.is_sacrifice()) {
    nnue += 30 * NNUE::StrategyMaterialWeight / 100; // Reward promising sacrifices based on material weight.
}

// Penalty for symmetry in pawn structure.
if (pos.is_symmetric()) {
    nnue -= 20 * NNUE::StrategyPositionalWeight / 100; // Penalize symmetric structures using positional weight.
}

    int optimism = pos.this_thread()->optimism[pos.side_to_move()];
    int shufflingPenalty = dampened_shuffling(pos.rule50_count());

    // Define the adjustEval lambda to blend evaluations and apply penalties
    const auto adjustEval = [&](int optDiv, int nnueDiv, int pawnCountConstant, int pawnCountMul,
                                int npmConstant, int evalDiv, int shufflingConstant,
                                int shufflingDiv) {
        // Blend optimism and eval with nnue complexity and material imbalance
        optimism += optimism * (nnueComplexity + std::abs(simpleEval - nnue)) / optDiv;
        nnue -= nnue * (nnueComplexity + std::abs(simpleEval - nnue)) / nnueDiv;

        int npm = pos.non_pawn_material() / 64;

        v = (nnue * (npm + pawnCountConstant + pawnCountMul * pos.count<PAWN>()) +
             optimism * (npmConstant + npm)) / evalDiv;

        // Penalità shuffling
													   
        v = v * (shufflingConstant - shufflingPenalty) / shufflingDiv;
    };

// Apply Conservative Parameters Based on Conditions
// If the smallNet flag is not set, use standard conservative parameters.
if (!smallNet) {
    adjustEval(513, 32395, 919, 11, 145, 1036, 178, 204);
    // Parameters:
    // - Material threshold: 800
    // - Positional scale: 45000
    // - King safety adjustment: 800
    // - Aggressivity factor: 10
    // - Piece activity factor: 120
    // - Endgame threshold: 1500
    // - NNUE evaluation scaling factors: 256, 256
} 
// If only PSQT (Piece-Square Table) evaluation is used, apply simplified parameters.
else if (psqtOnly) {
    adjustEval(517, 32857, 908, 7, 155, 1019, 224, 238);
    // Parameters:
    // - Material threshold: 750
    // - Positional scale: 42000
    // - King safety adjustment: 700
    // - Aggressivity factor: 8
    // - Piece activity factor: 110
    // - Endgame threshold: 1400
    // - NNUE evaluation scaling factors: 240, 240
} 
// Use default conservative parameters for other cases.
else {
    adjustEval(499, 32793, 903, 9, 147, 1067, 208, 211);
    // Parameters:
    // - Material threshold: 700
    // - Positional scale: 40000
    // - King safety adjustment: 750
    // - Aggressivity factor: 9
    // - Piece activity factor: 115
    // - Endgame threshold: 1300
    // - NNUE evaluation scaling factors: 230, 230
}

// Guarantee evaluation does not hit the tablebase range
v = std::clamp(v, VALUE_TB_LOSS_IN_MAX_PLY + 1, VALUE_TB_WIN_IN_MAX_PLY - 1);

// Penalize King Safety Based on Position
int kingSafetyPenalty = pos.king_safety_score(pos.side_to_move());
v -= kingSafetyPenalty * NNUE::StrategyPositionalWeight / 100;

    return; // No value to return because the function is declared as void.
	}

// Like evaluate(), but instead of returning a value, it returns
// a string (suitable for outputting to stdout) that contains the detailed
// descriptions and values of each evaluation term. Useful for debugging.
// Trace scores are from white's point of view
std::string trace(Position& pos) {

    if (pos.checkers())
        return "Final evaluation: none (in check)";

    // Reset any global variable used in eval
    pos.this_thread()->bestValue       = VALUE_ZERO;
    pos.this_thread()->rootSimpleEval  = VALUE_ZERO;
    pos.this_thread()->optimism[WHITE] = VALUE_ZERO;
    pos.this_thread()->optimism[BLACK] = VALUE_ZERO;

    std::stringstream ss;
    ss << std::showpoint << std::noshowpos << std::fixed << std::setprecision(2);
    ss << '\n' << NNUE::trace(pos) << '\n';

    ss << std::showpoint << std::showpos << std::fixed << std::setprecision(2) << std::setw(15);

    Value v;
    v = NNUE::evaluate<NNUE::Big>(pos, false);
    v = pos.side_to_move() == WHITE ? v : -v;
    ss << "NNUE evaluation        " << 0.01 * UCI::to_cp(v) << " (white side)\n";
    ss << "Material weight: " << NNUE::StrategyMaterialWeight << "\n";
    ss << "Positional weight: " << NNUE::StrategyPositionalWeight << "\n";
    ss << "King safety penalty applied: " << pos.king_safety_score(pos.side_to_move()) << "\n";

    v = evaluate(pos);
    v = pos.side_to_move() == WHITE ? v : -v;
    ss << "Final evaluation       " << 0.01 * UCI::to_cp(v) << " (white side)";
    ss << " [with scaled NNUE, material imbalance, and optimism blending]";
    ss << "\n";

    return ss.str();
}

}  // namespace Eval
}  // namespace Hypnos

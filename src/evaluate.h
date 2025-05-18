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

#ifndef EVALUATE_H_INCLUDED
#define EVALUATE_H_INCLUDED

#include <string>
#include <unordered_map>

#include "types.h"
#include "position.h"

namespace Hypnos {

class Position;  // Forward declaration.

namespace Eval {

// Main data structure
// Enumeration to represent Shashin styles
enum class Style {
Tal, // Aggressive style
Petrosian, // Defensive style
Capablanca // Balanced style
};

// Structure for Shashin Style
struct ShashinStyle {
int aggressivityWeight; // Weight for aggressive style
int positionalWeight; // Weight for positional control
int defensiveWeight; // Weight for defensive style
int attack; // Value for aggressive style
int defense; // Value for defensive style
int balance; // Value for balanced style
};

// Positional indicators used in evaluation
struct PositionalIndicators {
    int kingSafety;          // Safety of the king.
    int openFileControl;     // Control of open files.
    int centerDominance;     // Dominance of central squares.
    int materialImbalance;   // Advantage in material asymmetry.
    int centerControl;       // Control of specific central squares.
    int flankControl;        // Control of the flanks (side files).
    int pieceActivity;       // Activity and mobility of pieces.
    int defensivePosition;   // Strength of defensive setups.
};


// Variable Declarations
extern ShashinStyle CurrentStyle; // Current dynamic style in use.
bool style_is_enabled(); // Returns true if a Shashin style is enabled

extern int styleBuffer[3]; // Buffer for styles
extern Value lastScore; // Last evaluated score
extern Value lastEvalScore; // Add this declaration
extern int lastNodeTrigger; // Last node used for style change

// Main Shashin Functions
void apply_penalty_progression();                      // Apply penalties based on progression.
void set_shashin_style(const std::string& style);      // Set Shashin style from a string.
void debug_shashin_style();                            // Debug Shashin style details.
void dynamic_shashin_style(const Position& pos, Value& score, int totalMaterial); // Apply dynamic style.
void recalibrate_parameters(Value score);              // Recalibrate evaluation parameters.
void set_shashin_style(Style style);                   // Set Shashin style using enum.
void set_shashin_style(const std::string& style);      // String version for style setting.
void set_shashin_custom_blend(int talWeight, int petrosianWeight, int capablancaWeight); // Custom blend style.
void calculate_dynamic_blend(int& talWeight, int& petrosianWeight, int& capablancaWeight, const Position& pos); // Calculate weights dynamically.
void apply_dynamic_shashin_weights(int& talWeight, int& petrosianWeight, int& capablancaWeight, const Position& pos);
PositionalIndicators compute_positional_indicators(const Position& pos); // Evaluate positional indicators.

// Shashin Positional Metrics
int compute_king_safety(const Position& pos);          // King safety evaluation.
int compute_open_file_control(const Position& pos);    // Open file control.
int compute_center_dominance(const Position& pos);     // Center square dominance.
int compute_material_imbalance(const Position& pos);   // Material imbalance.
int compute_center_control(const Position& pos);       // Central square control.
int compute_aggressivity(const Position& pos);           // Aggression and pressure.
int compute_position(const Position& pos);             // General position evaluation.
int compute_defense(const Position& pos);              // Defensive strength.

// New indicators
int compute_flank_control(const Position& pos);        // Control of the flanks.
int compute_advanced_open_file_control(const Position& pos); // Advanced open file analysis.
int compute_piece_activity(const Position& pos);       // Activity of pieces.
int distance_to_center(Square sq);                     // Distance of a square to the center.
int piece_value(Piece piece);                          // Value of a piece.

// Helper functions
Bitboard adjacent_squares(Square sq);                  // Bitboard of squares adjacent to a given square.
int compute_material_factor(const Position& pos);      // Evaluate material factor.
int determine_dynamic_phase(const Position& pos);      // Determine game phase dynamically.

// General Evaluation Functions
Value evaluate(const Position& pos);                   // Overall position evaluation.
int simple_eval(const Position& pos, Color c);         // Simple evaluation for a specific color.
int determine_phase(const Position& pos, int totalMaterial); // Determine game phase based on material.
int blend_nnue_with_simple(int nnue, int simpleEval, int nnueComplexity, int materialImbalance); // Blend NNUE and simple evals.
int dampened_shuffling(int shuffling);                 // Dampen shuffling effects in evaluations.
int calculate_tal_weight(const Position& pos, PositionalIndicators indicators); // Tal style weight.
int calculate_capablanca_weight(const Position& pos, PositionalIndicators indicators); // Capablanca style weight.
int calculate_petrosian_weight(const Position& pos, PositionalIndicators indicators); // Petrosian style weight.
void update_weights(int phase, const Position& pos, int& talWeight, int& petrosianWeight, int& capablancaWeight); // Update style weights.

// State Variables and Functions
Value bestPreviousScore();  // Retrieves the best previous score (if supported).
int depth();                // Current search depth of the engine.

// General Constants and Parameters
constexpr Value toleranceBuffer = 15;        // Buffer for insignificant score variations.
constexpr int styleChangeThreshold = 3;      // Threshold for switching styles dynamically.

constexpr inline int SmallNetThreshold = 1165, PsqtOnlyThreshold = 2500;

std::string trace(Position& pos);

//#define EvalFileDefaultNameBig "nn-1c0000000000.nnue"
//#define EvalFileDefaultNameSmall "nn-37f18f62d772.nnue"

namespace NNUE {
    extern int StrategyMaterialWeight;       // Weight for material-based strategy adjustments.
    extern int StrategyPositionalWeight;    // Weight for positional-based strategy adjustments.

    enum NetSize : int;                      // Enumeration for neural network sizes.

    void init();                             // Initialize the NNUE evaluation.
    void verify();                           // Verify NNUE integrity.

    // Update dynamic weights based on game phase and position.
    void update_weights(int phase, const Position& pos, int& talWeight, int& petrosianWeight, int& capablancaWeight);
}

struct EvalFile {
    std::string option_name;                 // Option name in the UCI interface.
    std::string default_name;                // Default file name for the evaluation file.
    std::string selected_name;               // Currently selected evaluation file name.
};

extern std::unordered_map<NNUE::NetSize, EvalFile> EvalFiles; // Map of NNUE sizes to evaluation files.


}  // namespace Eval
}  // namespace Hypnos

#endif  // #ifndef EVALUATE_H_INCLUDED

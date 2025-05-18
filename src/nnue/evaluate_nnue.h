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

// header used in NNUE evaluation function

#ifndef NNUE_EVALUATE_NNUE_H_INCLUDED
#define NNUE_EVALUATE_NNUE_H_INCLUDED

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>

#include "../misc.h"
#include "nnue_architecture.h"
#include "nnue_feature_transformer.h"
#include "../types.h"
#include "../position.h"
#include "../evaluate.h"

namespace Hypnos {
class Position;
}

namespace Hypnos::Eval::NNUE {


// Calculate the total material on the board.
int calculate_material(const Position& pos);

// Determine the dynamic phase of the game based on the position.
int determine_dynamic_phase(const Position& pos);

// Global Variables for Dynamic Weights
extern int StrategyMaterialWeight;  // Weight for material evaluation in the dynamic strategy.
extern int StrategyPositionalWeight; // Weight for positional evaluation in the dynamic strategy.

// Apply dynamic blending of Tal, Petrosian, and Capablanca weights.
void apply_dynamic_blend(int talWeight, int petrosianWeight, int capablancaWeight);

// Update weights dynamically based on the game phase and position.
void update_weights(int phase, const Position& pos, int& talWeight, int& petrosianWeight, int& capablancaWeight);

// Overload: Update weights dynamically using only the position and a blended approach.
void update_weights_with_blend(const Position& pos, int& talWeight, int& petrosianWeight, int& capablancaWeight);

// Adjust NNUE evaluation weights based on the current Shashin style.
void adjust_nnue_for_style(Style currentStyle);

// Set a custom blend for NNUE evaluation weights based on Shashin styles.
void set_shashin_custom_blend_nnue(int talWeight, int petrosianWeight, int capablancaWeight);

// Update weights dynamically based on the current Shashin style.
void update_weights_with_style(const Position& pos, Style currentStyle);

// Hash Value of Evaluation Function Structure
constexpr std::uint32_t HashValue[2] = {
    // Placeholder values for the hash, replace with actual computation or predefined constants.
  FeatureTransformer<TransformedFeatureDimensionsBig, nullptr>::get_hash_value()
    ^ Network<TransformedFeatureDimensionsBig, L2Big, L3Big>::get_hash_value(),
  FeatureTransformer<TransformedFeatureDimensionsSmall, nullptr>::get_hash_value()
    ^ Network<TransformedFeatureDimensionsSmall, L2Small, L3Small>::get_hash_value()};

// Deleter for automating release of memory area
template<typename T>
struct AlignedDeleter {
    void operator()(T* ptr) const {
        ptr->~T();
        std_aligned_free(ptr);
    }
};

template<typename T>
struct LargePageDeleter {
    void operator()(T* ptr) const {
        ptr->~T();
        aligned_large_pages_free(ptr);
    }
};

template<typename T>
using AlignedPtr = std::unique_ptr<T, AlignedDeleter<T>>;

template<typename T>
using LargePagePtr = std::unique_ptr<T, LargePageDeleter<T>>;

Value evaluate(const Value psqt, const Value positional, const int delta, const bool adjusted);

// Template specialization declarations
template<NetSize Net_Size>
Value evaluate(const Position& pos,
               bool            adjusted   = false,
               int*            complexity = nullptr,
               bool            psqtOnly   = false);

std::string trace(Position& pos);
void  hint_common_parent_position(const Position& pos);
bool load_eval(const std::string name, std::istream& stream, NetSize netSize);
bool save_eval(std::ostream& stream, NetSize netSize);
bool save_eval(const std::optional<std::string>& filename, NetSize netSize);

}  // namespace Hypnos::Eval::NNUE

#endif  // #ifndef NNUE_EVALUATE_NNUE_H_INCLUDED

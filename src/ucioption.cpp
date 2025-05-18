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

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstddef>
#include <iosfwd>
#include <istream>
#include <map>
#include <ostream>
#include <sstream>
#include <string>

#include "book/book.h"
#include "evaluate.h"
#include "experience.h"
#include "misc.h"
#include "search.h"
#include "syzygy/tbprobe.h"
#include "thread.h"
#include "tt.h"
#include "types.h"
#include "uci.h"
#include "nnue/evaluate_nnue.h"

using std::string;

namespace Hypnos {

UCI::OptionsMap Options;  // Global object

namespace UCI {

// Trim Function
std::string trim(const std::string& str);
// Remove leading and trailing spaces from a string
std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(' ');
    size_t last = str.find_last_not_of(' ');
    return (first == std::string::npos || last == std::string::npos) ? "" : str.substr(first, last - first + 1);
}
// 'On change' actions, triggered by an option's value change
static void on_clear_hash(const Option&) { Search::clear(); }
static void on_hash_size(const Option& o) { TT.resize(size_t(o)); }
static void on_logger(const Option& o) { start_logger(o); }
static void on_threads(const Option& o) { Threads.set(size_t(o)); }
static void on_book(const Option& o) { Book::on_book((string) o); }
static void on_tb_path(const Option& o) { Tablebases::init(o); }

// Callback to Initialize Experience When Enabled
static void on_exp_enabled(const Option& /*o*/) {
    Experience::init(); // Reinitialize experience-related data.
}

// Callback to Load Experience File
static void on_exp_file(const Option& /*o*/) {
    Experience::init(); // Reload experience file and associated data.
}

// Callback to Load Evaluation File
static void on_eval_file(const Option&) {
    Eval::NNUE::init(); // Reinitialize NNUE evaluation network.
}

// Callback to Update Strategy Material Weight
static void on_strategy_material_weight(const Option& o) {
    Eval::NNUE::StrategyMaterialWeight = 10 * (int)o; // Scale and update material weight.
}

// Callback to Update Strategy Positional Weight
static void on_strategy_positional_weight(const Option& o) {
    Eval::NNUE::StrategyPositionalWeight = 10 * (int)o; // Scale and update positional weight.
}

// Our case insensitive less() function as required by UCI protocol
bool CaseInsensitiveLess::operator()(const string& s1, const string& s2) const {

    return std::lexicographical_compare(s1.begin(), s1.end(), s2.begin(), s2.end(),
                                        [](char c1, char c2) { return tolower(c1) < tolower(c2); });
}


// Initializes the UCI options to their hard-coded default values
void init(OptionsMap& o) {

    constexpr int MaxHashMB = Is64Bit ? 33554432 : 2048;

    o["Debug Log File"] << Option("", on_logger);
    o["Threads"] << Option(1, 1, 1024, on_threads);
    o["Clean Search"] << Option(false);
    o["Hash"] << Option(16, 1, MaxHashMB, on_hash_size);
    o["Clear Hash"] << Option(on_clear_hash);
    o["Ponder"] << Option(false);
    o["MultiPV"] << Option(1, 1, 500);
    o["Skill Level"] << Option(20, 0, 20);
    o["MoveOverhead"] << Option(10, 0, 5000);
    o["Minimum Thinking Time"] << Option(100, 0, 5000);
    o["Time Contempt"] << Option(20, -100, 100);
    o["nodestime"] << Option(0, 0, 10000);
    o["UCI_Chess960"] << Option(false);
    o["UCI_LimitStrength"] << Option(false);
    o["UCI_Elo"] << Option(1320, 1320, 3190);
    o["UCI_ShowWDL"] << Option(false);
    o["Book File"] << Option("<empty>", on_book);
    o["Book Width"] << Option(1, 1, 20);
    o["Book Depth"] << Option(255, 1, 255);
    o["SyzygyPath"] << Option("<empty>", on_tb_path);
    o["SyzygyProbeDepth"] << Option(1, 1, 100);
    o["Syzygy50MoveRule"] << Option(true);
    o["SyzygyProbeLimit"] << Option(7, 0, 7);
    o["Experience Enabled"] << Option(false, on_exp_enabled);
    o["Experience File"] << Option("Hypnos.exp", on_exp_file);
    o["Experience Readonly"] << Option(false);
    o["Experience Book"] << Option(false);
    o["Experience Book Width"] << Option(1, 1, 20);
    o["Experience Book Eval Importance"] << Option(5, 0, 10);
    o["Experience Book Min Depth"] << Option(27, Experience::MinDepth, 64);
    o["Experience Book Max Moves"] << Option(16, 1, 100);
    o["EvalFile"] << Option(EvalFileDefaultNameBig, on_eval_file);
    o["EvalFileSmall"] << Option(EvalFileDefaultNameSmall, on_eval_file);
    o["Variety"] << Option(0, 0, 40);
    o["Variety Max Score"] << Option(0, 0, 50);
    o["Variety Max Moves"] << Option(0, 0, 40);    // Manual Weight Adjustment Option
    o["NNUE ManualWeights"] << Option(false, [](const Option& opt) {
    // Check if Manual Weights option is enabled or disabled.
     if (opt) {
        sync_cout << "info string NNUE ManualWeights enabled. Using user-defined weights." << sync_endl;
    } else {
        sync_cout << "info string NNUE ManualWeights disabled. Using dynamic weights." << sync_endl;
    }
});

    o["NNUE StrategyMaterialWeight"] 
        << Option(0, -12, 12, on_strategy_material_weight); // Material weight adjustment.
    o["NNUE StrategyPositionalWeight"] 
        << Option(0, -12, 12, on_strategy_positional_weight); // Positional weight adjustment.

    o["Use Exploration Factor"] << Option(false, [](const Option& opt) {
        sync_cout << "info string Use Exploration Factor is now: "
                  << (opt ? "enabled" : "disabled") << sync_endl;
    });

    // Exploration Factor (now uses 0-30 and is divided by 10 in code)
    o["Exploration Factor"] << Option(2, 0, 30, [](const Option& v) { 
        Search::exploration_factor = float(int(v)) / 10.0;
});

	// Exploration Decay Factor (now uses 1-50 and is divided by 10 in code)
    o["Use Exploration Decay"] << Option(false);
    o["Exploration Decay Factor"] << Option(10, 1, 50, [](const Option& v) { 
        Search::exploration_decay_factor = float(int(v)) / 10.0;
});

    o["Dynamic Exploration"] << Option(false, [](const Option& opt) { // Toggle for dynamic exploration.
        sync_cout << "info string Dynamic Exploration is now: " 
                  << (opt ? "enabled" : "disabled") << sync_endl;
});

    o["Shashin Dynamic Style"] << Option(false, [](const Option& opt) {
        sync_cout << "info string Shashin Dynamic Style is now: " 
                  << (opt ? "enabled" : "disabled") << sync_endl;
});

    o["Use Shashin Style"] << Option(false, [](const Option& opt) {
        if (!(bool)opt) {
            Eval::CurrentStyle = {0, 0, 0, 0, 0, 0}; // Neutral style
            sync_cout << "info string Shashin Style disabled: using HypnoS-like evaluation" << sync_endl;
        } else {
            // Re-apply selected style from current combo
            std::string style = Options["Shashin Style"];
            Eval::set_shashin_style(style);
            sync_cout << "info string Shashin Style enabled: " << style << sync_endl;
        }
    });

    o["Shashin Style"] << Option(
        "Capablanca var Tal var Capablanca var Petrosian", 
        "Capablanca", // Default value.
        [](const Option& opt) {
           std::string selectedStyle = static_cast<std::string>(opt);
        // Management of predefined styles
           Eval::set_shashin_style(selectedStyle);
        sync_cout << "info string Shashin Style is now: " << selectedStyle << sync_endl;
    }
);

    o["Enable Custom Blend"] << Option(false, [](const Option& opt) {
        bool isEnabled = static_cast<bool>(opt);
        int talWeight = Options["Blend Weight Tal"];
        int capablancaWeight = Options["Blend Weight Capablanca"];
        int petrosianWeight = Options["Blend Weight Petrosian"];

    // Calculate the total weight
    int total = talWeight + capablancaWeight + petrosianWeight;
    bool adjusted = false;

    if (total > 100) {
        // Excess weight to redistribute
        int excess = total - 100;
        adjusted = true;

        // Count the active weights (non-zero)
        int activeWeights = (talWeight > 0) + (capablancaWeight > 0) + (petrosianWeight > 0);

        // Proportional distribution of the excess only on active weights
        if (activeWeights > 0) {
            if (talWeight > 0) {
                talWeight -= std::ceil((float)excess / activeWeights);
                talWeight = std::max(0, talWeight); // Avoid negative values
            }
            if (capablancaWeight > 0) {
                capablancaWeight -= std::ceil((float)excess / activeWeights);
                capablancaWeight = std::max(0, capablancaWeight);
            }
            if (petrosianWeight > 0) {
                petrosianWeight -= std::ceil((float)excess / activeWeights);
                petrosianWeight = std::max(0, petrosianWeight);
            }
        }
    }

    // Detailed debug to check updated weights
    sync_cout << "Debug: Final Weights After Adjustment - Tal(" << talWeight
              << "), Capablanca(" << capablancaWeight
              << "), Petrosian(" << petrosianWeight << ")" << sync_endl;

    // Apply static weights for the Custom Blend
    if (isEnabled) {
        Eval::set_shashin_custom_blend(talWeight, petrosianWeight, capablancaWeight);
        sync_cout << "info string Custom Blend Active: Updated Static Weights Applied" << sync_endl;
    } else if (Options["Shashin Dynamic Style"]) {
        Eval::apply_dynamic_shashin_weights(talWeight, petrosianWeight, capablancaWeight, *globalPos);
        sync_cout << "info string Dynamic Weights Applied" << sync_endl;
    } else {
        Eval::set_shashin_custom_blend(talWeight, petrosianWeight, capablancaWeight);
        sync_cout << "info string Static Weights Applied" << sync_endl;
    }

    if (adjusted) {
        sync_cout << "info string Warning: Weights exceeded 100. Values have been adjusted automatically." << sync_endl;
    }
});

// Logic for Tal
    o["Blend Weight Tal"] << Option(70, 0, 100, [](const Option& opt) {
        int talWeight = static_cast<int>(opt);
        int capablancaWeight = Options["Blend Weight Capablanca"];
        int petrosianWeight = Options["Blend Weight Petrosian"];

        bool adjusted = false;
        int total = talWeight + capablancaWeight + petrosianWeight;

        if (total > 100) {
        int excess = total - 100;
        adjusted = true;

        int activeWeights = (talWeight > 0) + (capablancaWeight > 0) + (petrosianWeight > 0);

        if (activeWeights > 0) {
            if (capablancaWeight > 0) {
                capablancaWeight -= std::ceil((float)excess / activeWeights);
                capablancaWeight = std::max(0, capablancaWeight);
            }
            if (petrosianWeight > 0) {
                petrosianWeight -= std::ceil((float)excess / activeWeights);
                petrosianWeight = std::max(0, petrosianWeight);
            }
        }
    }

    if (Options["Enable Custom Blend"]) {
        Eval::set_shashin_custom_blend(talWeight, petrosianWeight, capablancaWeight);
        sync_cout << "info string Custom Blend Active: Updated Static Weights Applied" << sync_endl;
    }

    sync_cout << "info string Updated Blend Weights: Tal(" << talWeight
              << "), Capablanca(" << capablancaWeight
              << "), Petrosian(" << petrosianWeight << ")" << sync_endl;

    if (adjusted) {
        sync_cout << "info string Warning: Weights exceeded 100. Values have been adjusted automatically." << sync_endl;
    }
});

    // Logic for Capablanca
    o["Blend Weight Capablanca"] << Option(0, 0, 100, [](const Option& opt) {
        int capablancaWeight = static_cast<int>(opt);
        int talWeight = Options["Blend Weight Tal"];
        int petrosianWeight = Options["Blend Weight Petrosian"];

        bool adjusted = false;
        int total = talWeight + capablancaWeight + petrosianWeight;

        if (total > 100) {
        int excess = total - 100;
        adjusted = true;

        int activeWeights = (talWeight > 0) + (capablancaWeight > 0) + (petrosianWeight > 0);

        if (activeWeights > 0) {
            if (talWeight > 0) {
                talWeight -= std::ceil((float)excess / activeWeights);
                talWeight = std::max(0, talWeight);
            }
            if (petrosianWeight > 0) {
                petrosianWeight -= std::ceil((float)excess / activeWeights);
                petrosianWeight = std::max(0, petrosianWeight);
            }
        }
    }

    if (Options["Enable Custom Blend"]) {
        Eval::set_shashin_custom_blend(talWeight, petrosianWeight, capablancaWeight);
        sync_cout << "info string Custom Blend Active: Updated Static Weights Applied" << sync_endl;
    }

    sync_cout << "info string Updated Blend Weights: Tal(" << talWeight
              << "), Capablanca(" << capablancaWeight
              << "), Petrosian(" << petrosianWeight << ")" << sync_endl;

    if (adjusted) {
        sync_cout << "info string Warning: Weights exceeded 100. Values have been adjusted automatically." << sync_endl;
    }
});

    // Logic for Petrosian
    o["Blend Weight Petrosian"] << Option(30, 0, 100, [](const Option& opt) {
        int petrosianWeight = static_cast<int>(opt);
        int talWeight = Options["Blend Weight Tal"];
        int capablancaWeight = Options["Blend Weight Capablanca"];

        bool adjusted = false;
        int total = talWeight + capablancaWeight + petrosianWeight;

        if (total > 100) {
        int excess = total - 100;
        adjusted = true;

        int activeWeights = (talWeight > 0) + (capablancaWeight > 0) + (petrosianWeight > 0);

        if (activeWeights > 0) {
            if (talWeight > 0) {
                talWeight -= std::ceil((float)excess / activeWeights);
                talWeight = std::max(0, talWeight);
            }
            if (capablancaWeight > 0) {
                capablancaWeight -= std::ceil((float)excess / activeWeights);
                capablancaWeight = std::max(0, capablancaWeight);
            }
        }
    }

    if (Options["Enable Custom Blend"]) {
        Eval::set_shashin_custom_blend(talWeight, petrosianWeight, capablancaWeight);
        sync_cout << "info string Custom Blend Active: Updated Static Weights Applied" << sync_endl;
    }

    sync_cout << "info string Updated Blend Weights: Tal(" << talWeight
              << "), Capablanca(" << capablancaWeight
              << "), Petrosian(" << petrosianWeight << ")" << sync_endl;

    if (adjusted) {
        sync_cout << "info string Warning: Weights exceeded 100. Values have been adjusted automatically." << sync_endl;
    }
});
}

// Used to print all the options default values in chronological
// insertion order (the idx field) and in the format defined by the UCI protocol.
std::ostream& operator<<(std::ostream& os, const OptionsMap& om) {

    for (size_t idx = 0; idx < om.size(); ++idx)
        for (const auto& it : om)
            if (it.second.idx == idx)
            {
                const Option& o = it.second;
                os << "\noption name " << it.first << " type " << o.type;

                if (o.type == "string" || o.type == "check" || o.type == "combo")
                    os << " default " << o.defaultValue;

                if (o.type == "spin")
                    os << " default " << int(stof(o.defaultValue)) << " min " << o.min << " max "
                       << o.max;

                break;
            }

    return os;
}


// Option class constructors and conversion operators

Option::Option(const char* v, OnChange f) :
    type("string"),
    min(0),
    max(0),
    on_change(f) {
    defaultValue = currentValue = v;
}

Option::Option(bool v, OnChange f) :
    type("check"),
    min(0),
    max(0),
    on_change(f) {
    defaultValue = currentValue = (v ? "true" : "false");
}

Option::Option(OnChange f) :
    type("button"),
    min(0),
    max(0),
    on_change(f) {}

Option::Option(double v, int minv, int maxv, OnChange f) :
    type("spin"),
    min(minv),
    max(maxv),
    on_change(f) {
    defaultValue = currentValue = std::to_string(v);
}

Option::Option(const char* v, const char* cur, OnChange f) :
    type("combo"),
    min(0),
    max(0),
    on_change(f) {
    defaultValue = v;
    currentValue = cur;
}

Option::operator int() const {
    assert(type == "check" || type == "spin");
    return (type == "spin" ? std::stoi(currentValue) : currentValue == "true");
}

Option::operator std::string() const {
    assert(type == "string");
    return currentValue;
}

bool Option::operator==(const char* s) const {
    assert(type == "combo");
    return !CaseInsensitiveLess()(currentValue, s) && !CaseInsensitiveLess()(s, currentValue);
}


// Inits options and assigns idx in the correct printing order

void Option::operator<<(const Option& o) {

    static size_t insert_order = 0;

    *this = o;
    idx   = insert_order++;
}


// Updates currentValue and triggers on_change() action. It's up to
// the GUI to check for option's limits, but we could receive the new value
// from the user by console window, so let's check the bounds anyway.
Option& Option::operator=(const string& v) {

    assert(!type.empty());

    if ((type != "button" && type != "string" && v.empty())
        || (type == "check" && v != "true" && v != "false")
        || (type == "spin" && (stof(v) < min || stof(v) > max)))
        return *this;

    if (type == "combo")
    {
        OptionsMap         comboMap;  // To have case insensitive compare
        string             token;
        std::istringstream ss(defaultValue);
        while (ss >> token)
            comboMap[token] << Option();
        if (!comboMap.count(v) || v == "var")
            return *this;
    }

    if (type != "button")
        currentValue = v;

    if (on_change)
        on_change(*this);

    return *this;
}

}  // namespace UCI

}  // namespace Hypnos

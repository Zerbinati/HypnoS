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

#include <cassert>
#include <iostream>
#include <iomanip>
#include <string>
#include <sstream>
#include <fstream>
#include <vector>
#include <cstdio>  //For: remove()
#include <condition_variable>
#include <mutex>
#include <thread>
#include "misc.h"
#include "uci.h"
#include "position.h"
#include "thread.h"
#include "experience.h"

//using namespace std;
using namespace Hypnos;

#define USE_GOOGLE_SPARSEHASH_DENSEMAP
#define USE_CUSTOM_HASHER

#ifdef USE_GOOGLE_SPARSEHASH_DENSEMAP
    #include "sparsehash/dense_hash_map"

    #ifdef USE_CUSTOM_HASHER
// Custom Hash functor for "Key" type
struct KeyHasher {
    // Hash operator
    constexpr usize operator()(const Key& key) const { return key & 0x00000000FFFFFFFFULL; }

    // Compare operator
    constexpr bool operator()(const Key& key1, const Key& key2) const { return key1 == key2; }
};

template<typename tKey, typename tVal>
class SugaRMap: public google::dense_hash_map<tKey, tVal, KeyHasher, KeyHasher> {
   public:
    SugaRMap(tKey emptyKey, tKey deletedKey) {
        google::dense_hash_map<tKey, tVal, KeyHasher, KeyHasher>::set_empty_key(emptyKey);
        google::dense_hash_map<tKey, tVal, KeyHasher, KeyHasher>::set_deleted_key(deletedKey);
    }
};
    #else
template<typename tKey, typename tVal>
class SugaRMap: public google::dense_hash_map<tKey, tVal> {
   public:
    SugaRMap(tKey emptyKey, tKey deletedKey) {
        google::dense_hash_map<tKey, tVal>::set_empty_key(emptyKey);
        google::dense_hash_map<tKey, tVal>::set_deleted_key(deletedKey);
    }
};
    #endif

template<typename tVal>
class SugaRKeyMap: public SugaRMap<Key, tVal> {
   public:
    SugaRKeyMap() :
        SugaRMap<Key, tVal>(Key{0}, (Key) -1) {}
};
#else
    #include <unordered_map>
template<typename tKey, typename tVal>
using SugaRMap = unordered_map<tKey, tVal>;
template<typename tVal>
using SugaRKeyMap = SugaRMap<Key, tVal>;
#endif

using i64 = std::int64_t;

namespace Experience {

class ExperienceReader {
   protected:
    bool  match;
    usize entriesCount;

   public:
    ExperienceReader() :
        match(false),
        entriesCount(0) {}
    virtual ~ExperienceReader() = default;

   protected:
    bool check_signature_set_count(std::ifstream&     input,
                                   const usize        inputLength,
                                   const std::string& signature,
                                   const usize        entrySize) {
        assert(input && input.is_open() && inputLength);

        // Check if data length contains full experience entries
        auto check_exp_count = [&]() -> bool {
            const usize entriesDataLength = inputLength - signature.length();
            entriesCount                  = entriesDataLength / entrySize;

            if (entriesCount * entrySize != entriesDataLength)
            {
                entriesCount = 0;
                return false;
            }

            return true;
        };

        // Check if file signature is matching
        auto check_signature = [&]() -> bool {
            if (signature.empty())
                return true;

            // If inpout length is less than the signature length then it can't be a match!
            if (inputLength < signature.length())
                return false;

            // Start from the beginning of the file
            input.seekg(std::ios::beg);

            // Allocate memory for signature
            char* sigBuffer = (char*) malloc(signature.length());

            if (!sigBuffer)
            {
                sync_cout << "info string Failed to allocate " << signature.length()
                          << " bytes for experience signature verification" << sync_endl;

                return false;
            }

            if (!input.read(sigBuffer, signature.length()))
            {
                free(sigBuffer);

                sync_cout << "info string Failed to read " << signature.length()
                          << " bytes for experience signature verification" << sync_endl;

                return false;
            }

            const bool signatureMatching =
              memcmp(sigBuffer, signature.c_str(), signature.length()) == 0;

            // Free memory
            free(sigBuffer);

            return signatureMatching;
        };

        // Start fresh
        match = check_exp_count() && check_signature();

        // Restore file pointer if it is not a match
        if (!match)
            input.seekg(std::ios::beg);

        return match;
    }

   public:
    [[nodiscard]] usize entries_count() const { return entriesCount; }

    virtual int  get_version()                                            = 0;
    virtual bool check_signature(std::ifstream& input, usize inputLength) = 0;
    virtual bool read(std::ifstream& input, Current::ExpEntry* exp)       = 0;
};

////////////////////////////////////////////////////////////////
// V1
////////////////////////////////////////////////////////////////
namespace V1 {

constexpr auto ExperienceSignature = "SugaR";
constexpr int  ExperienceVersion   = 1;

class ExperienceReader final: public Experience::ExperienceReader {
   public:
    explicit ExperienceReader() :
        entry(ExpKey{0}, ExpMove::none(), (ExpValue) 0, (ExpDepth) 0) {}

    int get_version() override { return ExperienceVersion; }

    bool check_signature(std::ifstream& input, const usize inputLength) override {
        return check_signature_set_count(input, inputLength, ExperienceSignature, sizeof(ExpEntry));
    }

    bool read(std::ifstream& input, Current::ExpEntry* exp) override {
        assert(match && input.is_open());

        if (!input.read((char*) &entry, sizeof(ExpEntry)))
            return false;

        exp->key   = entry.key;
        exp->move  = (ExpMove) entry.move;
        exp->value = (ExpValue) entry.value;
        exp->depth = (ExpDepth) entry.depth;
        exp->count = 1;

        return true;
    }

   private:
    ExpEntry entry;
};

}

////////////////////////////////////////////////////////////////
// V2
////////////////////////////////////////////////////////////////
namespace V2 {

constexpr auto ExperienceSignature = "SugaR Experience version 2";
constexpr int  ExperienceVersion   = 2;

class ExperienceReader final: public Experience::ExperienceReader {
   public:
    explicit ExperienceReader() = default;

    int get_version() override { return ExperienceVersion; }

    bool check_signature(std::ifstream& input, const usize inputLength) override {
        return check_signature_set_count(input, inputLength, ExperienceSignature, sizeof(ExpEntry));
    }

    bool read(std::ifstream& input, Current::ExpEntry* exp) override {
        assert(match && input.is_open());

        if (!input.read((char*) exp, sizeof(ExpEntry)))
            return false;

        return true;
    }
};

}

////////////////////////////////////////////////////////////////
// Type aliases
////////////////////////////////////////////////////////////////
using ExpMap           = SugaRKeyMap<ExpEntryEx*>;
using ExpIterator      = ExpMap::iterator;
using ExpConstIterator = ExpMap::const_iterator;

////////////////////////////////////////////////////////////////
// ExpEntryEx::quality
////////////////////////////////////////////////////////////////
std::pair<int, bool> ExpEntryEx::quality(Position& pos, const int evalImportance) const {
    static constexpr int QualityEvalImportanceMax = 10;

    assert(evalImportance >= 0 && evalImportance <= QualityEvalImportanceMax);

    // Draw detection
    bool maybeDraw = false;

    // Quality based on move count
    int q = count * (QualityEvalImportanceMax - evalImportance);

    // Quality based on difference in evaluation
    if (evalImportance)
    {
        static constexpr int QualityExperienceMovesAhead = 10;

        const auto us   = pos.side_to_move();
        const auto them = ~us;

        // Calculate quality based on evaluation improvement of next moves
        std::vector<ExpMove> moves;  // Used for doing/undoing of experience moves
        std::array<StateInfo, QualityExperienceMovesAhead> states{};

        std::array<i64, COLOR_NB> sum{};
        std::array<i64, COLOR_NB> weight{};

        // Start our sum/weight with something positive!
        sum[us]    = count;
        weight[us] = 1;

        // Look ahead
        auto              me                = us;
        const ExpEntryEx* lastExp[COLOR_NB] = {nullptr, nullptr};
        const ExpEntryEx* temp1             = this;

        while (true)
        {
            // To be used later
            lastExp[me] = temp1;

            // Do the move
            moves.emplace_back(temp1->move);
            pos.do_move(moves.back(), states[moves.size() - 1]);
            me = ~me;

            if (!maybeDraw)
                maybeDraw = pos.is_draw(pos.game_ply());

            if (moves.size() >= QualityExperienceMovesAhead)
                break;

            // Probe the new position
            temp1 = probe(pos.key());

            if (!temp1)
                break;

            // Find best next experience move (shallow search)
            const ExpEntryEx* temp2 = temp1->next;

            while (temp2)
            {
                if (temp2->compare(temp1) > 0)
                    temp1 = temp2;

                temp2 = temp2->next;
            }

            if (lastExp[me])
            {
                sum[me] += static_cast<i64>(temp1->value - lastExp[me]->value);
                ++weight[me];
            }
        }

        // Undo moves
        for (auto it = moves.rbegin(); it != moves.rend(); ++it)
            pos.undo_move(*it);

        // Calculate quality
        i64 s = sum[us];
        i64 w = weight[us];

        if (weight[them])
        {
            s -= sum[them];
            w += weight[them];
        }

        q += static_cast<int>(s * evalImportance / w);
    }
    else
    {
        // Shallow draw detection when 'evalImportance' is zero!
        StateInfo st{};
        pos.do_move(move, st);
        maybeDraw = pos.is_draw(pos.game_ply());
        pos.undo_move(move);
    }

    return {q / QualityEvalImportanceMax, maybeDraw};
}

// Experience data
namespace {

#ifndef NDEBUG
constexpr usize WriteBufferSize = 1024;
#else
constexpr usize WriteBufferSize = 1024 * 1024 * 16;
#endif

class ExperienceData {
   private:
    std::string _filename;

    std::vector<ExpEntryEx*> _expData;
    std::vector<ExpEntryEx*> _newPvExp;
    std::vector<ExpEntryEx*> _newMultiPvExp;
    std::vector<ExpEntryEx*> _oldExpData;

    ExpMap _mainExp;

    bool                    _loading;
    std::atomic<bool>       _abortLoading;
    std::atomic<bool>       _loadingResult;
    std::thread*            _loaderThread;
    std::condition_variable _loadingCond;
    std::mutex              _loaderMutex;

    void clear() {
        // Make sure we are not loading an experience file
        _abortLoading.store(true, std::memory_order_relaxed);
        wait_for_load_finished();
        assert(_loaderThread == nullptr);

        // Clear new exp (this will also flush all new experience data to '_oldExpData'
        // which we will delete later in this function
        clear_new_exp();

        // Free main exp data
        for (ExpEntryEx*& p : _expData)
            free(p);

        // Delete previous game experience data
        for (ExpEntryEx*& p : _oldExpData)
            delete p;

        // Clear
        _mainExp.clear();
        _oldExpData.clear();
        _expData.clear();
    }

    void clear_new_exp() {
        // Copy exp data to another buffer to be deleted when the whole object is destroyed or new exp file is loaded
        for (auto& newExp : {_newPvExp, _newMultiPvExp})
            std::copy(newExp.begin(), newExp.end(), back_inserter(_oldExpData));

        // Clear vectors
        _newPvExp.clear();
        _newMultiPvExp.clear();
    }

    bool link_entry(ExpEntryEx* exp) {
        ExpIterator itr = _mainExp.find(exp->key);

        // If new entry: insert into map and continue
        if (itr == _mainExp.end())
        {
            _mainExp[exp->key] = exp;
            return true;
        }

        // If existing entry and same move exists then merge
        ExpEntryEx* exp2 = itr->second->find(exp->move);
        if (exp2)
        {
            exp2->merge(exp);
            return false;
        }

        // If existing entry and different move then insert sorted based on pseudo-quality
        exp2 = itr->second;

        do
        {
            if (exp->compare(exp2) > 0)
            {
                if (exp2 == itr->second)
                {
                    itr->second = exp;
                    exp->next   = exp2;
                }
                else
                {
                    exp->next  = exp2->next;
                    exp2->next = exp;
                }

                return true;
            }

            if (!exp2->next)
            {
                exp2->next = exp;
                return true;
            }

            exp2 = exp2->next;
        } while (true);

        // Should never reach here!
        __builtin_unreachable();
    }

    bool _load(const std::string& fn) {
        std::ifstream in(Utility::map_path(fn), std::ios::in | std::ios::binary | std::ios::ate);

        if (!in.is_open())
        {
            sync_cout << "info string Could not open experience file: " << fn << sync_endl;
            return false;
        }

        const usize inSize = in.tellg();

        if (inSize == 0)
        {
            sync_cout << "info string The experience file [" << fn << "] is empty" << sync_endl;
            return false;
        }

        // Define readers
        // Order should be from most recent to oldest
        class ExpReaders {
           public:
            std::vector<std::pair<const char*, ExperienceReader*>> readers;

            ExpReaders() {
                readers.emplace_back("Experience (V2) reader", new V2::ExperienceReader());
                readers.emplace_back("Experience (V1) reader", new V1::ExperienceReader());

#ifndef NDEBUG
                int latest = 0;

                for (auto& rp : readers)
                    latest += rp.second->get_version() == Current::ExperienceVersion ? 1 : 0;

                assert(latest == 1);
#endif
            }

            ~ExpReaders() {
                for (auto rp : readers)
                    delete rp.second;
            }
        } expReaders;

        ExperienceReader* reader = nullptr;
        for (auto& rp : expReaders.readers)
        {
            if (!rp.second)
            {
                sync_cout << "info string Could not allocate memory for " << rp.first << sync_endl;
                continue;
            }

            if (rp.second->check_signature(in, inSize))
            {
                reader = rp.second;
                break;
            }
        }

        if (!reader)
        {
            sync_cout << "info string The file [" << fn << "] is not a valid experience file"
                      << sync_endl;
            return false;
        }

        if (reader->get_version() != Current::ExperienceVersion)
            sync_cout << "info string Importing experience version (" << reader->get_version()
                      << ") from file [" << fn << "]" << sync_endl;

        // Allocate buffer for ExpEntryEx data
        const usize expCount = reader->entries_count();
        auto*       expData  = (ExpEntryEx*) malloc(expCount * sizeof(ExpEntryEx));

        if (!expData)
        {
            std::cerr << "info string Failed to allocate " << expCount * sizeof(ExpEntryEx)
                      << " bytes for experience data from file [" << fn << "]" << std::endl;
            return false;
        }

        // Few variables to be used for statistical information
        const usize prevPosCount = _mainExp.size();

        // Load experience entries
        usize       duplicateMoves = 0;
        ExpEntryEx* exp            = expData;

        for (usize i = 0; i < expCount; ++i, ++exp)
        {
            if (_abortLoading.load(std::memory_order_relaxed))
                break;

            // Prepare to read
            exp->next = nullptr;

            // Read
            if (!reader->read(in, exp))
            {
                sync_cout << "info string Failed to read experience entry #" << i + 1 << " of "
                          << expCount << sync_endl;

                delete expData;
                return false;
            }

            // Merge
            if (!link_entry(exp))
                duplicateMoves++;
        }

        // Close input file
        in.close();

        // Add buffer to vector so that it will be released later
        _expData.push_back(expData);

        // Stop if aborted
        if (_abortLoading.load(std::memory_order_relaxed))
            return false;

        if (reader->get_version() != Current::ExperienceVersion)
        {
            sync_cout << "info string Upgrading experience file (" << fn << ") from version ("
                      << reader->get_version() << ") to version (" << Current::ExperienceVersion
                      << ")" << sync_endl;
            save(fn, true, true);
        }

        // Stop if aborted
        if (_abortLoading.load(std::memory_order_relaxed))
            return false;

        // Show some statistics
        if (prevPosCount)
        {
            sync_cout << "info string " << fn << " -> Total new moves: " << expCount
                      << ". Total new positions: " << (_mainExp.size() - prevPosCount)
                      << ". Duplicate moves: " << duplicateMoves << sync_endl;
        }
        else
        {
            sync_cout << "info string " << fn << " -> Total moves: " << expCount
                      << ". Total positions: " << _mainExp.size()
                      << ". Duplicate moves: " << duplicateMoves
                      << ". Fragmentation: " << std::setprecision(2) << std::fixed
                      << 100.0 * (double) duplicateMoves / (double) expCount << "%" << sync_endl;
        }

        return true;
    }

    bool _save(const std::string& fn, const bool saveAll) {
        std::fstream out;
        out.open(Utility::map_path(fn), std::ios::out | std::ios::binary | std::ios::app);

        if (!out.is_open())
        {
            sync_cout << "info string Failed to open experience file [" << fn << "] for writing"
                      << sync_endl;
            return false;
        }

        // If this is a new file then we need to write the signature first
        out.seekg(0, std::fstream::end);
        const usize length = out.tellg();
        out.seekg(0, std::fstream::beg);

        if (length == 0)
        {
            out.seekp(0, std::fstream::beg);

            out << Current::ExperienceSignature;
            if (!out)
            {
                sync_cout << "info string Failed to write signature to experience file [" << fn
                          << "]" << sync_endl;
                return false;
            }
        }

        // Reposition writing pointer to end of file
        out.seekp(std::ios::end);

        std::vector<char> writeBuffer;
        writeBuffer.reserve(WriteBufferSize);

        auto write_entry = [&](const Current::ExpEntry* exp, const bool force) -> bool {
            if (exp)
            {
                const char* data = reinterpret_cast<const char*>(exp);
                writeBuffer.insert(writeBuffer.end(), data, data + sizeof(Current::ExpEntry));
            }

            bool success = true;
            if (force || writeBuffer.size() >= WriteBufferSize)
            {
                out.write(writeBuffer.data(), writeBuffer.size());
                if (!out)
                    success = false;

                writeBuffer.clear();
            }

            return success;
        };

        if (saveAll)
        {
            usize allMoves     = 0;
            usize allPositions = 0;

            for (ExpEntryEx* expEx : _newPvExp)
                link_entry(expEx);

            for (ExpEntryEx* expEx : _newMultiPvExp)
                link_entry(expEx);

            for (auto& x : _mainExp)
            {
                allPositions++;
                ExpEntryEx* exp = x.second;

                // Scale counts
                u16         maxCount = std::numeric_limits<u8>::min();
                ExpEntryEx* exp1     = exp;

                while (exp1)
                {
                    maxCount = std::max(maxCount, exp1->count);
                    exp1     = exp1->next;
                }

                // Scale down
                const u16 scale = 1 + maxCount / 128;
                exp1            = exp;

                while (exp1)
                {
                    exp1->count = std::max(exp1->count / scale, 1);
                    exp1        = exp1->next;
                }

                // Save
                while (exp)
                {
                    if (exp->depth >= MinDepth)
                    {
                        allMoves++;

                        if (!write_entry(exp, false))
                        {
                            sync_cout
                              << "info string Failed to save experience entry to experience file ["
                              << fn << "]" << sync_endl;
                            return false;
                        }
                    }

                    exp = exp->next;
                }
            }

            sync_cout << "info string Saved " << allPositions << " position(s) and " << allMoves
                      << " moves to experience file: " << fn << sync_endl;
        }
        else
        {
            for (auto& newExp : {_newPvExp, _newMultiPvExp})
            {
                for (const ExpEntryEx* exp : newExp)
                {
                    if (exp->depth < MinDepth)
                        continue;

                    if (!write_entry(exp, false))
                    {
                        sync_cout
                          << "info string Failed to save experience entry to experience file ["
                          << fn << "]" << sync_endl;
                        return false;
                    }
                }
            }

            sync_cout << "info string Saved " << _newPvExp.size() << " PV and "
                      << _newMultiPvExp.size() << " MultiPV entries to experience file: " << fn
                      << sync_endl;
        }

        //Flush buffer
        write_entry(nullptr, true);

        //Clear new moves
        clear_new_exp();

        return true;
    }

   public:
    ExperienceData() {
        _loading = false;
        _abortLoading.store(false, std::memory_order_relaxed);
        _loadingResult.store(false, std::memory_order_relaxed);
        _loaderThread = nullptr;
    }

    ~ExperienceData() { clear(); }

    [[nodiscard]] std::string filename() const { return _filename; }

    [[nodiscard]] bool has_new_exp() const { return !_newPvExp.empty() || !_newMultiPvExp.empty(); }

    bool load(const std::string& filename, bool synchronous) {
        // Make sure we are not already in the process of loading same/other experience file
        wait_for_load_finished();

        // Load requested experience file
        _filename = filename;
        _loadingResult.store(false, std::memory_order_relaxed);

        // Block
        {
            _loading = true;
            std::lock_guard lg1(_loaderMutex);

            _loaderThread = new std::thread(std::thread([this, filename]() {
                // Load
                const bool loadingResult = _load(filename);
                _loadingResult.store(loadingResult, std::memory_order_relaxed);

                // Copy pointer of loader thread so that we can
                // clear the variable now and delete it later
                std::thread* t = _loaderThread;
                _loaderThread  = nullptr;

                // Notify
                {
                    std::lock_guard lg2(_loaderMutex);
                    _loading = false;
                    _loadingCond.notify_one();
                }

                // Detach and delete loader thread
                t->detach();
                delete t;
            }));
        }

        return !synchronous || wait_for_load_finished();
    }

    bool wait_for_load_finished() {
        std::unique_lock ul(_loaderMutex);
        _loadingCond.wait(ul, [&] { return !_loading; });
        return loading_result();
    }

    [[nodiscard]] bool loading_result() const {
        return _loadingResult.load(std::memory_order_relaxed);
    }

    void save(const std::string& fn, const bool saveAll, const bool ignoreLoadingCheck) {
        // Make sure we are not already in the process of loading same/other experience file
        if (!ignoreLoadingCheck)
            wait_for_load_finished();

        if (!has_new_exp() && (!saveAll || _mainExp.empty()))
            return;

        //Step 1: Create backup only if 'saveAll' is 'true'
        const std::string expFilename = Utility::map_path(fn);
        std::string       backupExpFilename;

        if (saveAll && Utility::file_exists(expFilename))
        {
            backupExpFilename = expFilename + ".bak";

            // If backup file already exists then delete it
            if (Utility::file_exists(backupExpFilename))
            {
                if (remove(backupExpFilename.c_str()) != 0)
                {
                    sync_cout << "info string Could not deleted existing backup file: "
                              << backupExpFilename << sync_endl;

                    // Clear backup filename
                    backupExpFilename.clear();
                }
            }

            // Rename current experience file
            if (!backupExpFilename.empty())
            {
                if (rename(expFilename.c_str(), backupExpFilename.c_str()) != 0)
                {
                    sync_cout << "info string Could not create backup of current experience file"
                              << sync_endl;

                    // Clear backup filename
                    backupExpFilename.clear();
                }
            }
        }

        // Step 2: Save
        if (!_save(fn, saveAll))
        {
            // Step 2a: Restore backup in case of failure while saving
            if (!backupExpFilename.empty())
            {
                if (rename(backupExpFilename.c_str(), expFilename.c_str()) != 0)
                {
                    sync_cout << "info string Could not restore backup experience file: "
                              << backupExpFilename << sync_endl;
                }
            }
        }
    }

    [[nodiscard]] const ExpEntryEx* probe(const Key k) const {
        ExpConstIterator itr = _mainExp.find(k);
        if (itr == _mainExp.end())
            return nullptr;

        assert(itr->second->key == k);

        return itr->second;
    }

    void add_pv_experience(const Key k, const Move m, const Value v, const Depth d) {
        auto* exp = new ExpEntryEx(k, m, v, d, 1);

        if (exp)
        {
            _newPvExp.emplace_back(exp);
            link_entry(exp);
        }
    }

    void add_multipv_experience(const Key k, const Move m, const Value v, const Depth d) {
        auto* exp = new ExpEntryEx(k, m, v, d, 1);

        if (exp)
        {
            _newMultiPvExp.emplace_back(exp);
            link_entry(exp);
        }
    }
};

ExperienceData* currentExperience = nullptr;
bool            experienceEnabled = true;
bool            learningPaused    = false;

}

////////////////////////////////////////////////////////////////
// Global experience functions
////////////////////////////////////////////////////////////////
void init() {
    experienceEnabled = Options["Experience Enabled"];

    if (!experienceEnabled)
    {
        unload();
        return;
    }

    const std::string filename = Options["Experience File"];

    if (currentExperience)
    {
        if (currentExperience->filename() == filename && currentExperience->loading_result())
            return;


        unload();
    }

    currentExperience = new ExperienceData();
    currentExperience->load(filename, false);
}

bool enabled() { return experienceEnabled; }

void unload() {
    save();

    delete currentExperience;
    currentExperience = nullptr;
}

void save() {
    if (!currentExperience || !currentExperience->has_new_exp()
        || static_cast<bool>(Options["Experience Readonly"]))
        return;

    currentExperience->save(currentExperience->filename(), false, false);
}

const ExpEntryEx* probe(const Key k) {
    assert(experienceEnabled);
    if (!currentExperience)
        return nullptr;

    return currentExperience->probe(k);
}

const ExpEntryEx* find_best_entry(const Key k) {
    const ExpEntryEx* bestEntry    = nullptr;
    const ExpEntryEx* currentEntry = probe(k);

    while (currentEntry)
    {
        if (!bestEntry || currentEntry->compare(bestEntry) > 0)
            bestEntry = currentEntry;

        currentEntry = currentEntry->next;
    }

    return bestEntry;
}

void wait_for_loading_finished() {
    if (!currentExperience)
        return;

    currentExperience->wait_for_load_finished();
}

// Defrag command:
// Format:  defrag [filename]
// Example: defrag C:\Path to\Experience\file.exp
// Note:    'filename' is optional. If omitted, then the default experience filename (BlackHole.exp) will be used
//          'filename' can contain spaces and can be a full path. If filename contains spaces, it is best to enclose it in quotations
void defrag(const int argc, char* argv[]) {
    // Make sure experience has finished loading
    // Not exactly needed here, but the messages shown when exp loading finish will
    // disturb the progress messages shown by this function
    wait_for_loading_finished();

    if (argc != 1)
    {
        sync_cout << "info string Error : Incorrect defrag command" << sync_endl;
        sync_cout << "info string Syntax: defrag [filename]" << sync_endl;
        return;
    }

    std::string filename = Utility::map_path(Utility::unquote(argv[0]));

    // Print message
    sync_cout << "\nDefragmenting experience file: " << filename << sync_endl;

    // Map filename
    filename = Utility::map_path(filename);

    // Load
    ExperienceData exp;

    if (!exp.load(filename, true))
        return;

    // Save
    exp.save(filename, true, false);
}

// Merge command:
// Format:  merge filename filename1 filename2 ... filenameX
// Example: defrag "C:\Path to\Experience\file.exp"
// Note:    'filename' is the target filename, which will also merged with the rest of the files if it exists
//          'filename1' ... 'filenameX' are the names of the experience files to be merged (along with filename)
//          'filename' can contain spaces but in that case it needs to eb quoted. It can also be a full path
void merge(const int argc, char* argv[]) {
    // Make sure experience has finished loading
    // Not exactly needed here, but the messages shown when exp loading finish will
    // disturb the progress messages shown by this function
    wait_for_loading_finished();

    // Step 1: Check
    if (argc < 2)
    {
        sync_cout << "info string Error : Incorrect merge command" << sync_endl;
        sync_cout << "info string Syntax: merge <filename> <filename1> [filename2] ... [filenameX]"
                  << sync_endl;
        sync_cout
          << "info string The first <filename> is also the target experience file which will contain all the merged data"
          << sync_endl;
        sync_cout
          << "info string The files <filename1> ... <filenameX> are the other experience files to be merged"
          << sync_endl;
        return;
    }

    // Step 2: Collect filenames
    std::vector<std::string> filenames;

    for (int i = 0; i < argc; ++i)
        filenames.push_back(Utility::map_path(Utility::unquote(argv[i])));

    // Step 3: The first filename is also the target filename
    const std::string targetFilename = filenames.front();

    // Print message
    sync_cout << "\nMerging experience files: ";
    for (const auto& fn : filenames)
        std::cout << "\n\t" << fn;

    std::cout << "\nTarget file: " << targetFilename << "\n" << sync_endl;

    //Step 4: Load and merge
    ExperienceData exp;

    for (const auto& fn : filenames)
        exp.load(fn, true);

    exp.save(targetFilename, true, false);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Convert compact PGN data to experience entries
//
// Compact PGN consists of the following format:
// {fen-string,w|b|d,move[:score:depth],move[:score:depth],move:score:depth[:score:depth],...}
//
// *) fen-string: Represents the start position of the game, which is not necesserly the normal start position
// *) w|b|d: Indicates the game result from PGN (to be validated), w= white win, b = black win, d = draw
// *) move[:score:depth]
//      - move : The move in long algebraic form, example e2e4
//      - score: The engine evaluation of the position from side to move point of view. This is an optional field
//      - depth: The depth of the move as read from engine evaluation. This is an optional field
///////////////////////////////////////////////////////////////////////////////////////////////////////////////
void convert_compact_pgn(const int argc, char* argv[]) {
    // Make sure experience has finished loading
    // Not exactly needed here, but the messages shown when exp loading finish will
    // disturb the progress messages shown by this function
    wait_for_loading_finished();

    if (argc < 2)
    {
        sync_cout << "Expecting at least 2 arguments, received: " << argc << sync_endl;
        return;
    }

    //////////////////////////////////////////////////////////////////////////
    // Collect input
    std::string inputPath  = Utility::unquote(argv[0]);
    std::string outputPath = Utility::unquote(argv[1]);
    const int   maxPly     = argc >= 3 ? atoi(argv[2]) : 1000;
    Value       maxValue   = argc >= 4 ? (Value) atoi(argv[3]) : (Value) VALUE_MATE;
    Depth       minDepth   = argc >= 5 ? std::max((Depth) atoi(argv[4]), MinDepth) : MinDepth;
						   
    Depth       maxDepth = argc >= 6 ? std::max((Depth) atoi(argv[5]), MinDepth) : (Depth) MAX_PLY;

    sync_cout << std::endl
              << "Building experience from PGN: " << std::endl
              << "\tCompact PGN file: " << inputPath << std::endl
              << "\tExperience file : " << outputPath << std::endl
              << "\tMax ply         : " << maxPly << std::endl
              << "\tMax value       : " << maxValue << std::endl
              << "\tDepth range     : " << minDepth << " - " << maxDepth << std::endl
              << sync_endl;

    //////////////////////////////////////////////////////////////////
    // Conversion information
    struct GLOBAL_COMPACT_PGN_CONVERSION_DATA {
        // Game statistics
        usize numGames           = 0;
        usize numGamesWithErrors = 0;
        usize numGamesIgnored    = 0;

        // Move statistics
        usize numMovesWithScores        = 0;
        usize numMovesWithScoresIgnored = 0;
        usize numMovesWithoutScores     = 0;

        // WBD statistics
        usize wbd[COLOR_NB + 1] = {0, 0, 0};

        // Input stream
        std::fstream inputStream;
        usize        inputStreamSize = 0;

        // Output stream
        std::fstream outputStream;
        usize        outputStreamBase;

        // Buffer
        std::vector<char> buffer;
    } globalConversionData;

    //////////////////////////////////////////////////////////////////
    // Game conversion information
    struct COMPACT_PGN_CONVERSION_DATA {
        Color detectedWinnerColor;
        bool  drawDetected;
        int   resultWeight[COLOR_NB + 1];

        Position pos;

        COMPACT_PGN_CONVERSION_DATA() { clear(); }

        void clear() {
            detectedWinnerColor = COLOR_NB;
            drawDetected        = false;
            memset((void*) &resultWeight, 0, sizeof(resultWeight));
        }
    } gameData;

    //////////////////////////////////////////////////////////////////////////
    // Input stream
    globalConversionData.inputStream.open(inputPath, std::ios::in | std::ios::ate);
    if (!globalConversionData.inputStream.is_open())
    {
        sync_cout << "Could not open <" << inputPath << "> for reading" << sync_endl;
        return;
    }

    globalConversionData.inputStreamSize = globalConversionData.inputStream.tellg();
    globalConversionData.inputStream.seekg(0, std::ios::beg);

    //////////////////////////////////////////////////////////////////////////
    // Output stream
    globalConversionData.outputStream.open(outputPath, std::ios::out | std::ios::binary
                                                         | std::ios::app | std::ios::ate);
    if (!globalConversionData.outputStream.is_open())
    {
        sync_cout << "Could not open <" << outputPath << "> for writing" << sync_endl;
        return;
    }

    globalConversionData.outputStreamBase = globalConversionData.outputStream.tellp();

    // If the output file is a new file, then we need to write the signature
    if (globalConversionData.outputStreamBase == 0)
    {
        globalConversionData.outputStream << Current::ExperienceSignature;
        globalConversionData.outputStreamBase = globalConversionData.outputStream.tellp();
    }

    //////////////////////////////////////////////////////////////////////////
    // Buffer
    globalConversionData.buffer.reserve(WriteBufferSize);

    //////////////////////////////////////////////////////////////////
    // Experience Data writing routine
    auto write_data = [&](const bool force) {
        if (force || globalConversionData.buffer.size() >= WriteBufferSize)
        {
            globalConversionData.outputStream.write(globalConversionData.buffer.data(),
                                                    globalConversionData.buffer.size());
            globalConversionData.buffer.clear();

            const usize numMoves = globalConversionData.numMovesWithScores
                                 + globalConversionData.numMovesWithScoresIgnored
                                 + globalConversionData.numMovesWithoutScores;
            usize inputStreamPos = globalConversionData.inputStream.tellg();

            // Fix for end-of-input stream value of -1!
            if (inputStreamPos == (usize) -1)
                inputStreamPos = globalConversionData.inputStreamSize;

            sync_cout << std::fixed << std::setprecision(2) << std::setw(6) << std::setfill(' ')
                      << ((double) inputStreamPos * 100.0
                          / (double) globalConversionData.inputStreamSize)
                      << "% ->"
                      << " Games: " << globalConversionData.numGames
                      << " (errors: " << globalConversionData.numGamesWithErrors << "),"
                      << " WBD: " << globalConversionData.wbd[WHITE] << "/"
                      << globalConversionData.wbd[BLACK] << "/"
                      << globalConversionData.wbd[COLOR_NB] << ","
                      << " Moves: " << numMoves << " (" << globalConversionData.numMovesWithScores
                      << " with scores, " << globalConversionData.numMovesWithoutScores
                      << " without scores, " << globalConversionData.numMovesWithScoresIgnored
                      << " ignored)."
                      << " Exp size: "
                      << format_bytes((usize) globalConversionData.outputStream.tellp()
                                        - globalConversionData.outputStreamBase,
                                      2)
                      << sync_endl;
        }
    };

    //////////////////////////////////////////////////////////////////
    // Helper function for splitting strings
    auto tokenize = [](const std::string& str, const char delimiter) -> std::vector<std::string> {
        std::istringstream       iss(str);
        std::vector<std::string> fields;

        std::string field;

        while (getline(iss, field, delimiter))
            fields.push_back(field);

        return fields;
    };

    //////////////////////////////////////////////////////////////////
    // Conversion routine
    auto convert_compact_pgn_to_exp = [&](const std::string& compactPgn) -> bool {
        constexpr Value    GOOD_SCORE          = PawnValue * 3;
        constexpr Value    OK_SCORE            = GOOD_SCORE / 2;
        constexpr auto     MAX_DRAW_SCORE      = (Value) 50;
        constexpr int      MIN_WEIGHT_FOR_DRAW = 8;
        constexpr int      MIN_WEIGHT_FOR_WIN  = 16;
        constexpr int      MIN_PLY_PER_GAME    = 16;
        constexpr Bitboard DarkSquares         = 0xAA55AA55AA55AA55ULL;

        // Clear current game data
        gameData.clear();

        // Increment games counter
        ++globalConversionData.numGames;

        // Split compact PGN into its main three parts
        std::vector<std::string> tokens = tokenize(compactPgn, ',');

        if (tokens.size() < 3)
        {
            ++globalConversionData.numGamesWithErrors;
            return false;
        }

        //////////////////////////////////////////////////////////////////
        //Read FEN string
        const std::string fen = tokens[0];

        //Setup Position
        StateListPtr states(new std::deque<StateInfo>(1));
        gameData.pos.set(fen, false, &states->back(), Threads.main());

        //////////////////////////////////////////////////////////////////
        //Read result
        std::string resultStr = tokens[1];

        //Find winner color from result-string
        Color winnerColor;
        if (resultStr == "w")
            winnerColor = WHITE;
        else if (resultStr == "b")
            winnerColor = BLACK;
        else if (resultStr == "d")
            winnerColor = COLOR_NB;
        else
            return false;

        //////////////////////////////////////////////////////////////////
        // Read moves
        int               gamePly = 0;
        Current::ExpEntry tempExp((Key) 0, Move::none(), VALUE_NONE, DEPTH_NONE);
        std::vector<char> tempBuffer;

        for (usize i = 2; i < tokens.size(); ++i)
        {
            ++gamePly;

            // Get move and score
            std::string _move;
            std::string _score;
            std::string _depth;

            std::vector<std::string> tok = tokenize(tokens[i], ':');

            if (!tok.empty())
                _move = tok[0];
            if (tok.size() >= 2)
                _score = tok[1];
            if (tok.size() >= 3)
                _depth = tok[2];

            if (tok.size() >= 4)
            {
                ++globalConversionData.numGamesWithErrors;
                return false;
            }

            // Cleanup move
            while (_move.back() == '+' || _move.back() == '#' || _move.back() == '\r'
                   || _move.back() == '\n')
                _move.pop_back();

            // Check if move is empty
            if (_move.empty())
            {
                ++globalConversionData.numGamesWithErrors;
                return false;
            }

            // Parse the move
            Move move = UCI::to_move(gameData.pos, _move);
            if (move == Move::none())
            {
                ++globalConversionData.numGamesWithErrors;
                return false;
            }

            const Depth depth = _depth.empty() ? DEPTH_NONE : (Depth) stoi(_depth);
            const Value score = _score.empty() ? VALUE_NONE : (Value) stoi(_score);

            if (depth != DEPTH_NONE && score != VALUE_NONE)
            {
                if (depth >= minDepth && depth <= maxDepth && abs(score) <= maxValue)
                {
                    ++globalConversionData.numMovesWithScores;

                    // Assign to temporary experience
                    tempExp.key   = gameData.pos.key();
                    tempExp.move  = move;
                    tempExp.value = score;
                    tempExp.depth = depth;

                    // Add to global buffer
                    const char* data = reinterpret_cast<const char*>(&tempExp);
                    tempBuffer.insert(tempBuffer.end(), data, data + sizeof(tempExp));
                }
                else
                {
                    ++globalConversionData.numMovesWithScoresIgnored;
                }

                //////////////////////////////////////////////////////////////////
                // Guess game result and apply sanity checks (we can't trust PGN scores blindly)
                if (std::abs(score) >= VALUE_TB_WIN_IN_MAX_PLY)
                {
                    const Color winnerColorBasedOnThisMove =
                      score > 0 ? gameData.pos.side_to_move() : ~gameData.pos.side_to_move();

                    if (gameData.detectedWinnerColor == COLOR_NB)
                    {
                        gameData.detectedWinnerColor = winnerColorBasedOnThisMove;
                        if (gameData.detectedWinnerColor != winnerColor)
                        {
                            ++globalConversionData.numGamesIgnored;
                            return false;
                        }
                    }
                    else if (gameData.detectedWinnerColor != winnerColorBasedOnThisMove)
                    {
                        ++globalConversionData.numGamesIgnored;
                        return false;
                    }
                }
                else if (gameData.pos.is_draw(gameData.pos.is_draw(gameData.pos.game_ply())))
                {
                    gameData.drawDetected = true;
                }

                // Detect score pattern
                if (abs(score) >= GOOD_SCORE)
                {
                    gameData.resultWeight[COLOR_NB] = 0;
                    gameData.resultWeight[score > 0 ? gameData.pos.side_to_move()
                                                    : ~gameData.pos.side_to_move()] +=
                      score < 0 ? 4 : 2;
                    gameData.resultWeight[score > 0 ? ~gameData.pos.side_to_move()
                                                    : gameData.pos.side_to_move()] = 0;
                }
                else if (abs(score) >= OK_SCORE)
                {
                    gameData.resultWeight[COLOR_NB] /= 2;
                    gameData.resultWeight[score > 0 ? gameData.pos.side_to_move()
                                                    : ~gameData.pos.side_to_move()] +=
                      score < 0 ? 2 : 1;
                    gameData.resultWeight[score > 0 ? ~gameData.pos.side_to_move()
                                                    : gameData.pos.side_to_move()] /= 2;
                }
                else if (abs(score) <= MAX_DRAW_SCORE)
                {
                    gameData.resultWeight[COLOR_NB] += 2;
                    gameData.resultWeight[WHITE] = 0;
                    gameData.resultWeight[BLACK] = 0;
                }
                else
                {
                    gameData.resultWeight[COLOR_NB] += 1;
                    gameData.resultWeight[WHITE] /= 2;
                    gameData.resultWeight[BLACK] /= 2;
                }
            }
            else
            {
                ++globalConversionData.numMovesWithoutScores;
            }

            // Do the move
            states->emplace_back();
            gameData.pos.do_move(move, states->back());

            //////////////////////////////////////////////////////////////////
            // Detect draw by insufficient material
            if (!gameData.drawDetected)
            {
                const int num_pieces = gameData.pos.count<ALL_PIECES>();

                if (num_pieces == 2)  // KvK
                {
                    gameData.drawDetected = true;
                }
                else if (num_pieces == 3
                         && (gameData.pos.count<BISHOP>() + gameData.pos.count<KNIGHT>())
                              == 1)  // KvK + 1 minor piece
                {
                    gameData.drawDetected = true;
                }
                else if (num_pieces == 4 && gameData.pos.count<BISHOP>(WHITE) == 1
                         && gameData.pos.count<BISHOP>(BLACK)
                              == 1)  // KBvKB, bishops of the same color
                {
                    if (((gameData.pos.pieces(WHITE, BISHOP) & DarkSquares)
                         && (gameData.pos.pieces(BLACK, BISHOP) & DarkSquares))
                        || ((gameData.pos.pieces(WHITE, BISHOP) & ~DarkSquares)
                            && (gameData.pos.pieces(BLACK, BISHOP) & ~DarkSquares)))
                        gameData.drawDetected = true;
                }
            }

            // If draw is detected but game result isn't draw then reject the game
            if (gameData.drawDetected && gameData.detectedWinnerColor != COLOR_NB)
            {
                ++globalConversionData.numGamesIgnored;
                return false;
            }
        }

        // Does the game have enough moves?
        if (gamePly < MIN_PLY_PER_GAME)
        {
            ++globalConversionData.numGamesIgnored;
            return false;
        }

        // If winner isn't yet identified, check result weights and try to identify it
        if (gameData.detectedWinnerColor == COLOR_NB)
        {
            if (gameData.resultWeight[WHITE] >= MIN_WEIGHT_FOR_WIN)
                gameData.detectedWinnerColor = WHITE;
            else if (gameData.resultWeight[BLACK] >= MIN_WEIGHT_FOR_WIN)
                gameData.detectedWinnerColor = BLACK;
        }

        //////////////////////////////////////////////////////////////////
        // More sanity checks
        if ((gameData.detectedWinnerColor != winnerColor)
            || (winnerColor != COLOR_NB && gameData.resultWeight[winnerColor] < MIN_WEIGHT_FOR_WIN)
            || (winnerColor == COLOR_NB && !gameData.drawDetected
                && gameData.resultWeight[COLOR_NB] < MIN_WEIGHT_FOR_DRAW))
        {
            ++globalConversionData.numGamesIgnored;
            return false;
        }

        // Update WBD stats
        ++globalConversionData.wbd[winnerColor];

        // Copy to global buffer
        globalConversionData.buffer.insert(globalConversionData.buffer.end(), tempBuffer.begin(),
                                           tempBuffer.end());

        return true;
    };

    //////////////////////////////////////////////////////////////////
    // Loop
    std::string line;

    while (std::getline(globalConversionData.inputStream, line))
    {
        //Skip empty lines
        if (line.empty())
            continue;

        if (line.front() != '{' || line.back() != '}')
            continue;

        line = line.substr(1, line.size() - 2);

        if (convert_compact_pgn_to_exp(line))
            write_data(false);
    }

    //Final commit
    write_data(true);

    //////////////////////////////////////////////////////////////////
    //Defragment outouf file
    if (globalConversionData.numMovesWithScores)
    {
        //If we don't close the output stream here then defragmentation will not be able to create a backup of the file!
        globalConversionData.outputStream.close();

        sync_cout << "Conversion complete" << std::endl
                  << std::endl
                  << "Defragmenting: " << outputPath << sync_endl;

        ExperienceData exp;
        if (!exp.load(outputPath, true))
            return;

        //Save
        exp.save(outputPath, true, false);
    }
}

void show_exp(Position& pos, const bool extended) {
    // Make sure experience has finished loading
    wait_for_loading_finished();

    sync_cout << pos << std::endl;

    std::cout << "Experience: ";
    const ExpEntryEx* expEx = Experience::probe(pos.key());

    if (!expEx)
    {
        std::cout << "No experience data found for this position" << sync_endl;
        return;
    }

    const int evalImportance = (int) Options["Experience Book Eval Importance"];
    std::vector<std::pair<const ExpEntryEx*, int>> quality;
    const ExpEntryEx*                              temp = expEx;

    while (temp)
    {
        quality.emplace_back(temp, temp->quality(pos, evalImportance).first);
        temp = temp->next;
    }

    //Sort experience moves based on quality
    std::stable_sort(
      quality.begin(), quality.end(),
      [](const std::pair<const ExpEntryEx*, int>& a, const std::pair<const ExpEntryEx*, int>& b) {
          return a.second > b.second;
      });

    std::cout << std::endl;
    int expCount = 0;

    for (const std::pair<const ExpEntryEx*, int>& pr : quality)
    {
        std::cout << std::setw(2) << std::setfill(' ') << std::left << ++expCount << ": "
                  << std::setw(5) << std::setfill(' ') << std::left
                  << UCI::move(pr.first->move, pos.is_chess960()) << ", depth: " << std::setw(2)
                  << std::setfill(' ') << std::left << pr.first->depth << ", eval: " << std::setw(6)
                  << std::setfill(' ') << std::left << UCI::value(pr.first->value);

        if (extended)
        {
            std::cout << ", count: " << std::setw(6) << std::setfill(' ') << std::left
                      << pr.first->count;

            if (pr.second != VALUE_NONE)
                std::cout << ", quality: " << std::setw(6) << std::setfill(' ') << std::left
                          << pr.second;
            else
                std::cout << ", quality: " << std::setw(6) << std::setfill(' ') << std::left
                          << "N/A";
        }

        std::cout << std::endl;

        expEx = expEx->next;
    }

    std::cout << sync_endl;
}

void pause_learning() { learningPaused = true; }

void resume_learning() { learningPaused = false; }

bool is_learning_paused() { return learningPaused; }

void add_pv_experience(const Key k, const Move m, const Value v, const Depth d) {
    if (!currentExperience)
        return;

    assert((bool) Options["Experience Readonly"] == false);

    currentExperience->add_pv_experience(k, m, v, d);
}

void add_multipv_experience(const Key k, const Move m, const Value v, const Depth d) {
    if (!currentExperience)
        return;

    assert((bool) Options["Experience Readonly"] == false);

    currentExperience->add_multipv_experience(k, m, v, d);
}
}

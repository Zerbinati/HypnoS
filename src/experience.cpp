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

using namespace std;
using namespace Stockfish;

#define USE_GOOGLE_SPARSEHASH_DENSEMAP
#define USE_CUSTOM_HASHER

#ifdef USE_GOOGLE_SPARSEHASH_DENSEMAP
    #include "sparsehash/dense_hash_map"

    #ifdef USE_CUSTOM_HASHER
//Custom Hash functor for "Key" type
struct KeyHasher {
    //Hash operator
    constexpr inline size_t operator()(const Key& key) const { return key & 0x00000000FFFFFFFFULL; }

    //Compare operator
    constexpr inline bool operator()(const Key& key1, const Key& key2) const {
        return key1 == key2;
    }
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
class SugaRKeyMap: public SugaRMap<Stockfish::Key, tVal> {
   public:
    SugaRKeyMap() :
        SugaRMap<Key, tVal>(static_cast<Key>(0), static_cast<Key>(-1)) {}
};
#else
    #include <unordered_map>
template<typename tKey, typename tVal>
using SugaRMap = unordered_map<tKey, tVal>;
template<typename tVal>
using SugaRKeyMap = SugaRMap<Key, tVal>;
#endif

namespace Experience {
class ExperienceReader {
   protected:
    bool   match;
    size_t entriesCount;

   public:
    ExperienceReader() :
        match(false),
        entriesCount(0) {}
    virtual ~ExperienceReader() = default;

   protected:
    bool check_signature_set_count(ifstream&     input,
                                   const size_t  inputLength,
                                   const string& signature,
                                   const size_t  entrySize) {
        assert(input && input.is_open() && inputLength);

        //Check if data length contains full experience entries
        auto check_exp_count = [&]() -> bool {
            const size_t entriesDataLength = inputLength - signature.length();
            entriesCount                   = entriesDataLength / entrySize;

            if (entriesCount * entrySize != entriesDataLength)
            {
                entriesCount = 0;
                return false;
            }

            return true;
        };

        //Check if file signature is matching
        auto check_signature = [&]() -> bool {
            if (signature.empty())
                return true;

            //If inpout length is less than the signature length then it can't be a match!
            if (inputLength < signature.length())
                return false;

            //Start from the beginning of the file
            input.seekg(ios::beg);

            //Allocate memory for signature
            const auto sigBuffer = static_cast<char*>(malloc(signature.length()));
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

            //Free memory
            free(sigBuffer);

            return signatureMatching;
        };

        //Start fresh
        match = check_exp_count() && check_signature();

        //Restore file pointer if it is not a match
        if (!match)
            input.seekg(ios::beg);

        return match;
    }

   public:
    [[nodiscard]] size_t entries_count() const { return entriesCount; }

    virtual int  get_version()                                        = 0;
    virtual bool check_signature(ifstream& input, size_t inputLength) = 0;
    virtual bool read(ifstream& input, Current::ExpEntry* exp)        = 0;
};

////////////////////////////////////////////////////////////////
// V1
////////////////////////////////////////////////////////////////
namespace V1 {
const string  ExperienceSignature = "SugaR";
constexpr int ExperienceVersion   = 1;

class ExperienceReader final: public Experience::ExperienceReader {
    ExpEntry entry;

   public:
    explicit ExperienceReader() :
        entry(static_cast<Key>(0), MOVE_NONE, 0, 0) {}

    int get_version() override { return ExperienceVersion; }

    bool check_signature(ifstream& input, const size_t inputLength) override {
        return check_signature_set_count(input, inputLength, ExperienceSignature, sizeof(ExpEntry));
    }

    bool read(ifstream& input, Current::ExpEntry* exp) override {
        assert(match && input.is_open());

        if (!input.read(reinterpret_cast<char*>(&entry), sizeof(ExpEntry)))
            return false;

        exp->key   = entry.key;
        exp->move  = entry.move;
        exp->value = entry.value;
        exp->depth = entry.depth;
        exp->count = 1;

        return true;
    }
};
}

////////////////////////////////////////////////////////////////
// V2
////////////////////////////////////////////////////////////////
namespace V2 {
const string  ExperienceSignature = "SugaR Experience version 2";
constexpr int ExperienceVersion   = 2;

class ExperienceReader final: public Experience::ExperienceReader {
   public:
    explicit ExperienceReader() = default;

    int get_version() override { return ExperienceVersion; }

    bool check_signature(ifstream& input, size_t inputLength) override {
        return check_signature_set_count(input, inputLength, ExperienceSignature, sizeof(ExpEntry));
    }

    bool read(ifstream& input, ExpEntry* exp) override {
        assert(match && input.is_open());

        if (!input.read(reinterpret_cast<char*>(exp), sizeof(ExpEntry)))
            return false;

        return true;
    }
};
}

////////////////////////////////////////////////////////////////
// Typedefs
////////////////////////////////////////////////////////////////
typedef SugaRKeyMap<ExpEntryEx*>                 ExpMap;
typedef SugaRKeyMap<ExpEntryEx*>::iterator       ExpIterator;
typedef SugaRKeyMap<ExpEntryEx*>::const_iterator ExpConstIterator;

////////////////////////////////////////////////////////////////
// ExpEntryEx::quality
////////////////////////////////////////////////////////////////
pair<int, bool> ExpEntryEx::quality(Position& pos, const int evalImportance) const {
    constexpr int QualityEvalImportanceMax = 10;

    assert(evalImportance >= 0 && evalImportance <= QualityEvalImportanceMax);

    //Draw detection
    bool maybeDraw = false;

    //Quality based on move count
    int q = count * (QualityEvalImportanceMax - evalImportance);

    //Quality based on difference in evaluation
    if (evalImportance)
    {
        constexpr int QualityExperienceMovesAhead = 10;
        const Color   us                          = pos.side_to_move();
        const Color   them                        = ~us;

        //Calculate quality based on evaluation improvement of next moves
        vector<Move> moves;  //Used for doing/undoing of experience moves
        array<StateInfo, QualityExperienceMovesAhead> states{};

        array<int64_t, COLOR_NB> sum    = {0, 0};
        array<int64_t, COLOR_NB> weight = {0, 0};

        //Start our sum/weight with something positive!
        sum[us]    = count;
        weight[us] = 1;

        //Look ahead
        Color             me                = us;
        const ExpEntryEx* lastExp[COLOR_NB] = {nullptr, nullptr};
        const ExpEntryEx* temp1             = this;
        while (true)
        {
            //To be used later
            lastExp[me] = temp1;

            //Do the move
            moves.emplace_back(temp1->move);
            pos.do_move(moves.back(), states[moves.size() - 1]);
            me = ~me;

            if (!maybeDraw)
                maybeDraw = pos.is_draw(pos.game_ply());

            if (moves.size() >= QualityExperienceMovesAhead)
                break;

            //Probe the new position
            temp1 = probe(pos.key());
            if (!temp1)
                break;

            //Find best next experience move (shallow search)
            const ExpEntryEx* temp2 = temp1 ? temp1->next : nullptr;
            while (temp2)
            {
                if (temp2->compare(temp1) > 0)
                    temp1 = temp2;

                temp2 = temp2->next;
            }

            if (lastExp[me])
            {
                sum[me] += static_cast<int64_t>(temp1->value - lastExp[me]->value);
                ++weight[me];
            }
        }

        //Undo moves
        for (auto it = moves.rbegin(); it != moves.rend(); ++it)
            pos.undo_move(*it);

        //Calculate quality
        int64_t s = 0;
        int64_t w = 0;

        if (weight[us])  //Will always be true since weight[us] is initialized to 1 earlier
        {
            s += sum[us];
            w += weight[us];
        }

        if (weight[them])
        {
            s -= sum[them];
            w += weight[them];
        }

        q += static_cast<int>(s * evalImportance / w);
    }
    else
    {
        //Shallow draw detection when 'evalImportance' is zero!
        StateInfo st{};
        pos.do_move(move, st);
        maybeDraw = pos.is_draw(pos.game_ply());
        pos.undo_move(move);
    }

    return {q / QualityEvalImportanceMax, maybeDraw};
}

//Experience data
namespace {
#ifndef NDEBUG
constexpr size_t WriteBufferSize = 1024;
#else
constexpr size_t WriteBufferSize = 1024 * 1024 * 16;
#endif

class ExperienceData {
   private:
    string _filename;

    vector<ExpEntryEx*> _expData;
    vector<ExpEntryEx*> _newPvExp;
    vector<ExpEntryEx*> _newMultiPvExp;
    vector<ExpEntryEx*> _oldExpData;

    ExpMap _mainExp;

    bool               _loading;
    atomic<bool>       _abortLoading{};
    atomic<bool>       _loadingResult{};
    thread*            _loaderThread;
    condition_variable _loadingCond;
    mutex              _loaderMutex;

   private:
    void clear() {
        //Make sure we are not loading an experience file
        _abortLoading.store(true, memory_order_relaxed);
        wait_for_load_finished();
        assert(_loaderThread == nullptr);

        //Clear new exp (this will also flush all new experience data to '_oldExpData' which we will delete later in this function
        clear_new_exp();

        //Free main exp data
        for (ExpEntryEx*& p : _expData)
            free(p);

        //Delete previous game experience data
        for (ExpEntryEx*& p : _oldExpData)
            delete p;

        //Clear
        _mainExp.clear();
        _oldExpData.clear();
        _expData.clear();
    }

    void clear_new_exp() {
        //Copy exp data to another buffer to be deleted when the whole object is destroyed or new exp file is loaded
        for (auto& newExp : {_newPvExp, _newMultiPvExp})
            std::copy(newExp.begin(), newExp.end(), back_inserter(_oldExpData));

        //Clear vectors
        _newPvExp.clear();
        _newMultiPvExp.clear();
    }

    bool link_entry(ExpEntryEx* exp) {
        const ExpIterator itr = _mainExp.find(exp->key);

        //If new entry: insert into map and continue
        if (itr == _mainExp.end())
        {
            _mainExp[exp->key] = exp;
            return true;
        }

        //If existing entry and same move exists then merge
        ExpEntryEx* exp2 = itr->second->find(exp->move);
        if (exp2)
        {
            exp2->merge(exp);
            return false;
        }

        //If existing entry and different move then insert sorted based on pseudo-quality
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

        //Should never reach here!
        assert(false);
        return true;
    }

    bool _load(const string& fn) {
        ifstream in(Utility::map_path(fn), ios::in | ios::binary | ios::ate);
        if (!in.is_open())
        {
            sync_cout << "info string Could not open experience file: " << fn << sync_endl;
            return false;
        }

        const size_t inSize = in.tellg();
        if (inSize == 0)
        {
            sync_cout << "info string The experience file [" << fn << "] is empty" << sync_endl;
            return false;
        }

        //Define readers
        //Order should be from most recent to oldest
        class ExpReaders {
           public:
            vector<pair<const char*, ExperienceReader*>> readers;

           public:
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

        //Allocate buffer for ExpEntryEx data
        const size_t expCount = reader->entries_count();
        auto*        expData  = static_cast<ExpEntryEx*>(malloc(expCount * sizeof(ExpEntryEx)));
        if (!expData)
        {
            sync_cout << "info string Failed to allocate " << expCount * sizeof(ExpEntryEx)
                      << " bytes for experience data from file [" << fn << "]" << sync_endl;
            return false;
        }

        //Few variables to be used for statistical information
        const size_t prevPosCount = _mainExp.size();

        //Load experience entries
        size_t      duplicateMoves = 0;
        ExpEntryEx* exp            = expData;
        for (size_t i = 0; i < expCount; ++i, ++exp)
        {
            if (_abortLoading.load(memory_order_relaxed))
                break;

            //Prepare to read
            exp->next = nullptr;

            //Read
            if (!reader->read(in, exp))
            {
                sync_cout << "info string Failed to read experience entry #" << i + 1 << " of "
                          << expCount << sync_endl;

                delete expData;
                return false;
            }

            //Merge
            if (!link_entry(exp))
                duplicateMoves++;
        }

        //Close input file
        in.close();

        //Add buffer to vector so that it will be released later
        _expData.push_back(expData);

        //Stop if aborted
        if (_abortLoading.load(memory_order_relaxed))
            return false;

        if (reader->get_version() != Current::ExperienceVersion)
        {
            sync_cout << "info string Upgrading experience file (" << fn << ") from version ("
                      << reader->get_version() << ") to version (" << Current::ExperienceVersion
                      << ")" << sync_endl;
            save(fn, true, true);
        }

        //Stop if aborted
        if (_abortLoading.load(memory_order_relaxed))
            return false;

        //Show some statistics
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
                      << ". Fragmentation: " << setprecision(2) << fixed
                      << 100.0 * static_cast<double>(duplicateMoves) / static_cast<double>(expCount)
                      << "%" << sync_endl;
        }

        return true;
    }

    bool _save(const string& fn, bool saveAll) {
        fstream out;
        out.open(Utility::map_path(fn), ios::out | ios::binary | ios::app);
        if (!out.is_open())
        {
            sync_cout << "info string Failed to open experience file [" << fn << "] for writing"
                      << sync_endl;
            return false;
        }

        //If this is a new file then we need to write the signature first
        out.seekg(0, std::fstream::end);
        size_t length = out.tellg();
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

        //Reposition writing pointer to end of file
        out.seekp(ios::end);

        vector<char> writeBuffer;
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
            size_t allMoves     = 0;
            size_t allPositions = 0;

            for (ExpEntryEx* expEx : _newPvExp)
                link_entry(expEx);

            for (ExpEntryEx* expEx : _newMultiPvExp)
                link_entry(expEx);

            ExpEntryEx* exp = nullptr;
            for (auto& x : _mainExp)
            {
                allPositions++;
                exp = x.second;

                //Scale counts
                uint16_t    maxCount = numeric_limits<uint8_t>::min();
                ExpEntryEx* exp1     = exp;
                while (exp1)
                {
                    maxCount = max(maxCount, exp1->count);
                    exp1     = exp1->next;
                }

                //Scale down
                uint16_t scale = 1 + maxCount / 128;
                exp1           = exp;
                while (exp1)
                {
                    exp1->count = max(exp1->count / scale, 1);
                    exp1        = exp1->next;
                }

                //Save
                while (exp)
                {
                    if (exp->depth >= EXP_MIN_DEPTH)
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
                    if (exp->depth < EXP_MIN_DEPTH)
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
        _abortLoading.store(false, memory_order_relaxed);
        _loadingResult.store(false, memory_order_relaxed);
        _loaderThread = nullptr;
    }

    ~ExperienceData() { clear(); }

    [[nodiscard]] string filename() const { return _filename; }

    [[nodiscard]] bool has_new_exp() const { return !_newPvExp.empty() || !_newMultiPvExp.empty(); }

    bool load(const string& filename, const bool synchronous) {
        //Make sure we are not already in the process of loading same/other experience file
        wait_for_load_finished();

        //Load requested experience file
        _filename = filename;
        _loadingResult.store(false, memory_order_relaxed);

        //Block
        {
            _loading = true;
            lock_guard<mutex> lg1(_loaderMutex);
            _loaderThread = new thread(thread([this, filename]() {
                //Load
                const bool loadingResult = _load(filename);
                _loadingResult.store(loadingResult, memory_order_relaxed);

                //Copy pointer of loader thread so that we can
                //clear the variable now and and deleted later
                thread* t     = _loaderThread;
                _loaderThread = nullptr;

                //Notify
                {
                    lock_guard<mutex> lg2(_loaderMutex);
                    _loading = false;
                    _loadingCond.notify_one();
                }

                //Detach and delete loader thread
                t->detach();
                delete t;
            }));
        }

        return synchronous ? wait_for_load_finished() : true;
    }

    bool wait_for_load_finished() {
        unique_lock<mutex> ul(_loaderMutex);
        _loadingCond.wait(ul, [&] { return !_loading; });
        return loading_result();
    }

    [[nodiscard]] bool loading_result() const { return _loadingResult.load(memory_order_relaxed); }

    void save(const string& fn, const bool saveAll, const bool ignoreLoadingCheck) {
        //Make sure we are not already in the process of loading same/other experience file
        if (!ignoreLoadingCheck)
            wait_for_load_finished();

        if (!has_new_exp() && (!saveAll || _mainExp.empty()))
            return;

        //Step 1: Create backup only if 'saveAll' is 'true'
        const string expFilename = Utility::map_path(fn);
        string       backupExpFilename;
        if (saveAll && Utility::file_exists(expFilename))
        {
            backupExpFilename = expFilename + ".bak";

            //If backup file already exists then delete it
            if (Utility::file_exists(backupExpFilename))
            {
                if (remove(backupExpFilename.c_str()) != 0)
                {
                    sync_cout << "info string Could not deleted existing backup file: "
                              << backupExpFilename << sync_endl;

                    //Clear backup filename
                    backupExpFilename.clear();
                }
            }

            //Rename current experience file
            if (!backupExpFilename.empty())
            {
                if (rename(expFilename.c_str(), backupExpFilename.c_str()) != 0)
                {
                    sync_cout << "info string Could not create backup of current experience file"
                              << sync_endl;

                    //Clear backup filename
                    backupExpFilename.clear();
                }
            }
        }

        //Step 2: Save
        if (!_save(fn, saveAll))
        {
            //Step 2a: Restore backup in case of failure while saving
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
        const ExpConstIterator itr = _mainExp.find(k);
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

    const string filename = Options["Experience File"];
    if (currentExperience)
    {
        if (currentExperience->filename() == filename && currentExperience->loading_result())
            return;

        if (currentExperience)
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

void wait_for_loading_finished() {
    if (!currentExperience)
        return;

    currentExperience->wait_for_load_finished();
}

//Defrag command:
//Format:  defrag [filename]
//Example: defrag C:\Path to\Experience\file.exp
//Note:    'filename' is optional. If omitted, then the default experience filename (Hypnos.exp) will be used
//         'filename' can contain spaces and can be a full path. If filename contains spaces, it is best to enclose it in quotations
void defrag(int argc, char* argv[]) {
    //Make sure experience has finished loading
    //Not exactly needed here, but the messages shown when exp loading finish will
    //disturb the progress messages shown by this function
    wait_for_loading_finished();

    if (argc != 1)
    {
        sync_cout << "info string Error : Incorrect defrag command" << sync_endl;
        sync_cout << "info string Syntax: defrag [filename]" << sync_endl;
        return;
    }

    string filename = Utility::map_path(Utility::unquote(argv[0]));

    //Print message
    sync_cout << "\nDefragmenting experience file: " << filename << sync_endl;

    //Map filename
    filename = Utility::map_path(filename);

    //Load
    ExperienceData exp;
    if (!exp.load(filename, true))
        return;

    //Save
    exp.save(filename, true, false);
}

//Merge command:
//Format:  merge filename filename1 filename2 ... filenameX
//Example: defrag "C:\Path to\Experience\file.exp"
//Note:    'filename' is the target filename, which will also merged with the rest of the files if it exists
//         'filename1' ... 'filenameX' are the names of the experience files to be merged (along with filename)
//         'filename' can contain spaces but in that case it needs to eb quoted. It can also be a full path
void merge(int argc, char* argv[]) {
    //Make sure experience has finished loading
    //Not exactly needed here, but the messages shown when exp loading finish will
    //disturb the progress messages shown by this function
    wait_for_loading_finished();

    //Step 1: Check
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

    //Step 2: Collect filenames
    vector<string> filenames;
    for (int i = 0; i < argc; ++i)
        filenames.push_back(Utility::map_path(Utility::unquote(argv[i])));

    //Step 3: The first filename is also the target filename
    const string targetFilename = filenames.front();

    //Print message
    sync_cout << "\nMerging experience files: ";
    for (const string& fn : filenames)
        cout << "\n\t" << fn;

    cout << "\nTarget file: " << targetFilename << "\n" << sync_endl;

    //Step 4: Load and merge
    ExperienceData exp;
    for (const string& fn : filenames)
        exp.load(fn, true);

    exp.save(targetFilename, true, false);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////
//Convert compact PGN data to experience entries
//
//Compact PGN consists of the following format:
// {fen-string,w|b|d,move[:score:depth],move[:score:depth],move:score:depth[:score:depth],...}
//
// *) fen-string: Represents the start position of the game, which is not necesserly the normal start position
// *) w|b|d: Indicates the game result from PGN (to be validated), w= white win, b = black win, d = draw
// *) move[:score:depth]
//      - move : The move in long algebraic form, example e2e4
//      - score: The engine evaluation of the position from side to move point of view. This is an optional field
//      - depth: The depth of the move as read from engine evaluation. This is an optional field
///////////////////////////////////////////////////////////////////////////////////////////////////////////////
void convert_compact_pgn(int argc, char* argv[]) {
    //Make sure experience has finished loading
    //Not exactly needed here, but the messages shown when exp loading finish will
    //disturb the progress messages shown by this function
    wait_for_loading_finished();

    if (argc < 2)
    {
        sync_cout << "Expecting at least 2 arguments, received: " << argc << sync_endl;
        return;
    }

    //////////////////////////////////////////////////////////////////////////
    // Collect input
    const string inputPath  = Utility::unquote(argv[0]);
    const string outputPath = Utility::unquote(argv[1]);
    const int    maxPly     = argc >= 3 ? atoi(argv[2]) : 1000;
    const Value  maxValue   = argc >= 4 ? atoi(argv[3]) : static_cast<Value>(VALUE_MATE);
    const Depth  minDepth   = argc >= 5 ? max(atoi(argv[4]), EXP_MIN_DEPTH) : EXP_MIN_DEPTH;
    const Depth  maxDepth =
      argc >= 6 ? max(atoi(argv[5]), EXP_MIN_DEPTH) : static_cast<Depth>(MAX_PLY);

    sync_cout << endl
              << "Building experience from PGN: " << endl
              << "\tCompact PGN file: " << inputPath << endl
              << "\tExperience file : " << outputPath << endl
              << "\tMax ply         : " << maxPly << endl
              << "\tMax value       : " << maxValue << endl
              << "\tDepth range     : " << minDepth << " - " << maxDepth << endl
              << sync_endl;

    //////////////////////////////////////////////////////////////////
    //Conversion information
    struct GLOBAL_COMPACT_PGN_CONVERSION_DATA {
        //Game statistics
        size_t numGames           = 0;
        size_t numGamesWithErrors = 0;
        size_t numGamesIgnored    = 0;

        //Move statistics
        size_t numMovesWithScores        = 0;
        size_t numMovesWithScoresIgnored = 0;
        size_t numMovesWithoutScores     = 0;

        //WBD statistics
        size_t wbd[COLOR_NB + 1] = {0, 0, 0};

        //Input stream
        fstream inputStream;
        size_t  inputStreamSize = 0;

        //Output stream
        fstream outputStream;
        size_t  outputStreamBase{};

        //Buffer
        vector<char> buffer;
    } globalConversionData;

    //////////////////////////////////////////////////////////////////
    //Game conversion information
    struct COMPACT_PGN_CONVERSION_DATA {
        Color detectedWinnerColor;
        bool  drawDetected{};
        int   resultWeight[COLOR_NB + 1]{};

        Position pos{};

        COMPACT_PGN_CONVERSION_DATA() { clear(); }

        void clear() {
            detectedWinnerColor = COLOR_NB;
            drawDetected        = false;
            memset((void*) &resultWeight, 0, sizeof(resultWeight));
        }
    } gameData;

    //////////////////////////////////////////////////////////////////////////
    //Input stream
    globalConversionData.inputStream.open(inputPath, ios::in | ios::ate);
    if (!globalConversionData.inputStream.is_open())
    {
        sync_cout << "Could not open <" << inputPath << "> for reading" << sync_endl;
        return;
    }

    globalConversionData.inputStreamSize = globalConversionData.inputStream.tellg();
    globalConversionData.inputStream.seekg(0, ios::beg);

    //////////////////////////////////////////////////////////////////////////
    //Output stream
    globalConversionData.outputStream.open(outputPath,
                                           ios::out | ios::binary | ios::app | ios::ate);
    if (!globalConversionData.outputStream.is_open())
    {
        sync_cout << "Could not open <" << outputPath << "> for writing" << sync_endl;
        return;
    }

    globalConversionData.outputStreamBase = globalConversionData.outputStream.tellp();

    //If the output file is a new file, then we need to write the signature
    if (globalConversionData.outputStreamBase == 0)
    {
        globalConversionData.outputStream << Current::ExperienceSignature.c_str();
        globalConversionData.outputStreamBase = globalConversionData.outputStream.tellp();
    }

    //////////////////////////////////////////////////////////////////////////
    //Buffer
    globalConversionData.buffer.reserve(WriteBufferSize);

    //////////////////////////////////////////////////////////////////
    //Experience Data writing routine
    auto write_data = [&](bool force) {
        if (force || globalConversionData.buffer.size() >= WriteBufferSize)
        {
            globalConversionData.outputStream.write(globalConversionData.buffer.data(),
                                                    globalConversionData.buffer.size());
            globalConversionData.buffer.clear();

            const size_t numMoves = globalConversionData.numMovesWithScores
                                  + globalConversionData.numMovesWithScoresIgnored
                                  + globalConversionData.numMovesWithoutScores;
            size_t inputStreamPos = globalConversionData.inputStream.tellg();

            //Fix for end-of-input stream value of -1!
            if (inputStreamPos == static_cast<size_t>(-1))
                inputStreamPos = globalConversionData.inputStreamSize;

            sync_cout << fixed << setprecision(2) << setw(6) << setfill(' ')
                      << (static_cast<double>(inputStreamPos) * 100.0
                          / static_cast<double>(globalConversionData.inputStreamSize))
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
                      << format_bytes(static_cast<size_t>(globalConversionData.outputStream.tellp())
                                        - globalConversionData.outputStreamBase,
                                      2)
                      << sync_endl;
        }
    };

    //////////////////////////////////////////////////////////////////
    //Helper function for splitting strings
    auto tokenize = [](const string& str, const char delimiter) -> vector<string> {
        istringstream  iss(str);
        vector<string> fields;

        string field;
        while (getline(iss, field, delimiter))
            fields.push_back(field);

        return fields;
    };

    //////////////////////////////////////////////////////////////////
    //Conversion routine
    auto convert_compact_pgn_to_exp = [&](const string& compactPgn) -> bool {
        constexpr Value    GOOD_SCORE          = PawnValue * 3;
        constexpr Value    OK_SCORE            = GOOD_SCORE / 2;
        constexpr Value    MAX_DRAW_SCORE      = 50;
        constexpr int      MIN_WEIGHT_FOR_DRAW = 8;
        constexpr int      MIN_WEIGHT_FOR_WIN  = 16;
        constexpr int      MIN_PLY_PER_GAME    = 16;
        constexpr Bitboard DarkSquares         = 0xAA55AA55AA55AA55ULL;

        //Clear current game data
        gameData.clear();

        //Increment games counter
        ++globalConversionData.numGames;

        //Split compact PGN into its main three parts
        const vector<string> tokens = tokenize(compactPgn, ',');

        if (tokens.size() < 3)
        {
            ++globalConversionData.numGamesWithErrors;
            return false;
        }

        //////////////////////////////////////////////////////////////////
        //Read FEN string
        const string& fen = tokens[0];

        //Setup Position
        const StateListPtr states(new deque<StateInfo>(1));
        gameData.pos.set(fen, false, &states->back(), Threads.main());

        //////////////////////////////////////////////////////////////////
        //Read result
        const string& resultStr = tokens[1];

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
        Current::ExpEntry tempExp(0, MOVE_NONE, VALUE_NONE, DEPTH_NONE);
        vector<char>      tempBuffer;
        for (size_t i = 2; i < tokens.size(); ++i)
        {
            ++gamePly;

            //Get move and score
            string _move;
            string _score;
            string _depth;

            vector<string> tok = tokenize(tokens[i], ':');
            if (tok.size() >= 1)
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

            //Cleanup move
            while (_move.back() == '+' || _move.back() == '#' || _move.back() == '\r'
                   || _move.back() == '\n')
                _move.pop_back();

            //Check if move is empty
            if (_move.empty())
            {
                ++globalConversionData.numGamesWithErrors;
                return false;
            }

            //Parse the move
            const Move move = UCI::to_move(gameData.pos, _move);
            if (move == MOVE_NONE)
            {
                ++globalConversionData.numGamesWithErrors;
                return false;
            }

            const Depth depth = _depth.empty() ? DEPTH_NONE : stoi(_depth);
            const Value score = _score.empty() ? VALUE_NONE : stoi(_score);

            if (depth != DEPTH_NONE && score != VALUE_NONE)
            {
                if (depth >= minDepth && depth <= maxDepth && abs(score) <= maxValue)
                {
                    ++globalConversionData.numMovesWithScores;

                    //Assign to temporary experience
                    tempExp.key   = gameData.pos.key();
                    tempExp.move  = move;
                    tempExp.value = score;
                    tempExp.depth = depth;

                    //Add to global buffer
                    const char* data = reinterpret_cast<const char*>(&tempExp);
                    tempBuffer.insert(tempBuffer.end(), data, data + sizeof(tempExp));
                }
                else
                {
                    ++globalConversionData.numMovesWithScoresIgnored;
                }

                //////////////////////////////////////////////////////////////////
                //Guess game result and apply sanity checks (we can't trust PGN scores blindly)
                if (abs(score) >= VALUE_TB_WIN_IN_MAX_PLY)
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

                //Detect score pattern
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

            //Do the move
            states->emplace_back();
            gameData.pos.do_move(move, states->back());

            //////////////////////////////////////////////////////////////////
            //Detect draw by insufficient material
            if (!gameData.drawDetected)
            {
                int num_pieces = gameData.pos.count<ALL_PIECES>();

                if (num_pieces == 2)  //KvK
                {
                    gameData.drawDetected = true;
                }
                else if (num_pieces == 3
                         && (gameData.pos.count<BISHOP>() + gameData.pos.count<KNIGHT>())
                              == 1)  //KvK + 1 minor piece
                {
                    gameData.drawDetected = true;
                }
                else if (num_pieces == 4 && gameData.pos.count<BISHOP>(WHITE) == 1
                         && gameData.pos.count<BISHOP>(BLACK)
                              == 1)  //KBvKB, bishops of the same color
                {
                    if (((gameData.pos.pieces(WHITE, BISHOP) & DarkSquares)
                         && (gameData.pos.pieces(BLACK, BISHOP) & DarkSquares))
                        || ((gameData.pos.pieces(WHITE, BISHOP) & ~DarkSquares)
                            && (gameData.pos.pieces(BLACK, BISHOP) & ~DarkSquares)))
                        gameData.drawDetected = true;
                }
            }

            //If draw is detected but game result isn't draw then reject the game
            if (gameData.drawDetected && gameData.detectedWinnerColor != COLOR_NB)
            {
                ++globalConversionData.numGamesIgnored;
                return false;
            }
        }

        //Does the game have enough moves?
        if (gamePly < MIN_PLY_PER_GAME)
        {
            ++globalConversionData.numGamesIgnored;
            return false;
        }

        //If winner isn't yet identified, check result weights and try to identify it
        if (gameData.detectedWinnerColor == COLOR_NB)
        {
            if (gameData.resultWeight[WHITE] >= MIN_WEIGHT_FOR_WIN)
                gameData.detectedWinnerColor = WHITE;
            else if (gameData.resultWeight[BLACK] >= MIN_WEIGHT_FOR_WIN)
                gameData.detectedWinnerColor = BLACK;
        }

        //////////////////////////////////////////////////////////////////
        //More sanity checks
        if ((gameData.detectedWinnerColor != winnerColor)
            || (winnerColor != COLOR_NB && gameData.resultWeight[winnerColor] < MIN_WEIGHT_FOR_WIN)
            || (winnerColor == COLOR_NB && !gameData.drawDetected
                && gameData.resultWeight[COLOR_NB] < MIN_WEIGHT_FOR_DRAW))
        {
            ++globalConversionData.numGamesIgnored;
            return false;
        }

        //Update WBD stats
        ++globalConversionData.wbd[winnerColor];

        //Copy to global buffer
        globalConversionData.buffer.insert(globalConversionData.buffer.end(), tempBuffer.begin(),
                                           tempBuffer.end());

        return true;
    };

    //////////////////////////////////////////////////////////////////
    //Loop
    string line;
    while (getline(globalConversionData.inputStream, line))
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

        sync_cout << "Conversion complete" << endl
                  << endl
                  << "Defragmenting: " << outputPath << sync_endl;

        ExperienceData exp;
        if (!exp.load(outputPath, true))
            return;

        //Save
        exp.save(outputPath, true, false);
    }
}

void show_exp(Position& pos, const bool extended) {
    //Make sure experience has finished loading
    wait_for_loading_finished();

    sync_cout << pos << endl;

    cout << "Experience: ";
    const ExpEntryEx* expEx = probe(pos.key());
    if (!expEx)
    {
        cout << "No experience data found for this position" << sync_endl;
        return;
    }

    const int evalImportance = Options["Experience Book Eval Importance"];
    vector<pair<const ExpEntryEx*, int>> quality;
    const ExpEntryEx*                    temp = expEx;
    while (temp)
    {
        quality.emplace_back(temp, temp->quality(pos, evalImportance).first);
        temp = temp->next;
    }

    //Sort experience moves based on quality
    stable_sort(quality.begin(), quality.end(),
                [](const pair<const ExpEntryEx*, int>& a, const pair<const ExpEntryEx*, int>& b) {
                    return a.second > b.second;
                });

    cout << endl;
    int expCount = 0;
    for (const pair<const ExpEntryEx*, int>& pr : quality)
    {
        cout << setw(2) << setfill(' ') << left << ++expCount << ": " << setw(5) << setfill(' ')
             << left << UCI::move(pr.first->move, pos.is_chess960()) << ", depth: " << setw(2)
             << setfill(' ') << left << pr.first->depth << ", eval: " << setw(6) << setfill(' ')
             << left << UCI::value(pr.first->value);

        if (extended)
        {
            cout << ", count: " << setw(6) << setfill(' ') << left << pr.first->count;

            if (pr.second != VALUE_NONE)
                cout << ", quality: " << setw(6) << setfill(' ') << left << pr.second;
            else
                cout << ", quality: " << setw(6) << setfill(' ') << left << "N/A";
        }

        cout << endl;

        expEx = expEx->next;
    }

    cout << sync_endl;
}

void pause_learning() { learningPaused = true; }

void resume_learning() { learningPaused = false; }

bool is_learning_paused() { return learningPaused; }

void add_pv_experience(Key k, Move m, Value v, Depth d) {
    if (!currentExperience)
        return;

    assert(static_cast<bool>(Options["Experience Readonly"]) == false);

    currentExperience->add_pv_experience(k, m, v, d);
}

void add_multipv_experience(const Key k, const Move m, const Value v, const Depth d) {
    if (!currentExperience)
        return;

    assert(static_cast<bool>(Options["Experience Readonly"]) == false);

    currentExperience->add_multipv_experience(k, m, v, d);
}
}

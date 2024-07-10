#ifndef LEARN_H_INCLUDED
#define LEARN_H_INCLUDED

#include <unordered_map>
#include "../types.h"
#include "../ucioption.h"

enum class LearningMode {
    Off      = 1,
    Standard = 2,
    Self     = 3,
};

struct LearningMove {
    Hypnos::Depth depth       = 0;
    Hypnos::Value score       = Hypnos::VALUE_NONE;
    Hypnos::Move  move        = Hypnos::Move::none();
    int               performance = 100;
};

struct PersistedLearningMove {
    Hypnos::Key key;
    LearningMove    learningMove;
};

class LearningData {
   private:
    bool         isPaused;
    bool         isReadOnly;
    bool         needPersisting;
    LearningMode learningMode;

    std::unordered_multimap<Hypnos::Key, LearningMove*> HT;
    std::vector<void*>                                      mainDataBuffers;
    std::vector<void*>                                      newMovesDataBuffers;

   private:
    bool load(const std::string& filename);
    void insert_or_update(PersistedLearningMove* plm, bool qLearning);

   public:
    LearningData();
    ~LearningData();

    void        pause();
    void        resume();
    inline bool is_paused() const { return isPaused; };

    void         set_learning_mode(Hypnos::OptionsMap& options, const std::string& lm);
    LearningMode learning_mode() const;
    inline bool  is_enabled() const { return learningMode != LearningMode::Off; }

    void        set_readonly(bool ro) { isReadOnly = ro; }
    inline bool is_readonly() const { return isReadOnly; }

    void clear();
    void init(Hypnos::OptionsMap& o);
    void persist(const Hypnos::OptionsMap& o);

    void add_new_learning(Hypnos::Key key, const LearningMove& lm);

    int probeByMaxDepthAndScore(Hypnos::Key key, const LearningMove*& learningMove);
    const LearningMove* probe_move(Hypnos::Key key, Hypnos::Move move);
};

extern LearningData LD;

#endif  // #ifndef LEARN_H_INCLUDED

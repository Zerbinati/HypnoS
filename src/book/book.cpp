#include "../misc.h"
#include "../uci.h"
#include "polyglot/polyglot.h"
#include "ctg/ctg.h"
#include "book.h"

using namespace std;

namespace Hypnos::Book {
namespace {
Book* create_book(const string& filename) {
    size_t extIndex = filename.find_last_of('.');
    if (extIndex == string::npos)
        return nullptr;

    string ext = filename.substr(extIndex + 1);

    if (ext == "ctg" || ext == "cto" || ext == "ctb")
        return new CTG::CtgBook();
    else if (ext == "bin")
        return new Polyglot::PolyglotBook();
    else
        return nullptr;
}
}

Book* book;

void init() {
    book = nullptr;

    on_book((string) Options["Book File"]);
}

void on_book(const string& filename) {
    // Close previous book if any
    delete book;
    book = nullptr;

    // Load new book
    if (Utility::is_empty_filename(filename))
        return;

    // Create book object for the given book type
    string fn      = Utility::map_path(filename);
    Book*  newBook = create_book(fn);
    if (newBook == nullptr)
    {
        sync_cout << "info string Unknown book type: " << filename << sync_endl;
        return;
    }

    // Open/Initialize the book
    if (!newBook->open(fn))
    {
        delete newBook;
        return;
    }

    book = newBook;
}

Move probe(const Position& pos) {
    int  moveNumber = 1 + pos.game_ply() / 2;
    Move bookMove   = Move::none();

    if (book != nullptr && (int) Options["Book Depth"] >= moveNumber)
        bookMove = book->probe(pos, (size_t) (int) Options["Book Width"], true);

    return bookMove;
}

void show_moves(const Position& pos) {
    cout << pos << endl << endl;

    if (book == nullptr)
        cout << "No book loaded" << endl;
    else
    {
        cout << "Book (" << book->type() << "): " << (std::string) Options["Book File"] << endl;
        book->show_moves(pos);
    }
}
}

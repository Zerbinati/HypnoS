<p align="center">
  <img src="http://outskirts.altervista.org/forum/ext/dmzx/imageupload/img-files/2/ca292f8/8585091/34788e79c6bbe7cf7bb578c6fb4d11f8.jpg">
</p>

<h1 align="center">HypnoS</h1>

## HypnoS Overview


HypnoS is a free and strong UCI chess engine derived from Stockfish 
that analyzes chess positions and computes the optimal moves.

HypnoS does not include a graphical user interface (GUI) that is required 
to display a chessboard and to make it easy to input moves. These GUIs are 
developed independently from HypnoS and are available online.

HypnoS development is currently supported on the Openbench framework. OpenBench (created by Andrew Grant) is an open-source Sequential Probability Ratio Testing (SPRT) framework designed for self-play testing of engines. OpenBench makes use of distributed computing, allowing anyone to contribute CPU time to further the development of some of the world's most powerful engines.

A big thank you goes to the guys at http://chess.grantnet.us/ especially to Andrew Grant for running and improving this testing framework


OpenBench's [Discord server](https://discord.com/invite/9MVg7fBTpM)

## UCI options

The following is a list of options supported by HypnoS (on top of all UCI options supported by Stockfish)
  * #### CTG/BIN Book 1 File
    The file name of the first book file which could be a polyglot (BIN) or Chessbase (CTG) book. To disable this book, use: ```<empty>```
    If the book (CTG or BIN) is in a different directory than the engine executable, then configure the full path of the book file, example:
    ```C:\Path\To\My\Book.ctg``` or ```/home/username/path/to/book/bin```

  * #### Book 1 Width
    The number of moves to consider from the book for the same position. To play best book move, set this option to ```1```. If a value ```n``` (greater than ```1```) is configured, the engine will pick **randomly** one of the top ```n``` moves available in the book for the given position

  * #### Book 1 Depth
    The maximum number of moves to play from the book
	
  * #### (CTG) Book 1 Only Green
    This option is only used if the loaded book is a CTG book. When set to ```true```, the engine will only play Green moves from the book (if any). If no green moves found, then no book move is made
	This option has no effect or use if the loaded book is a Polyglot (BIN) book
    
  * #### CTG/BIN Book 2 File
    Same explanation as **CTG/BIN Book 1 File**, but for the second book

  * #### Book 2 Width
    Same explanation as **Book 1 Width**, but for the second book

  * #### Book 2 Depth
    Same explanation as **Book 1 Depth**, but for the second book

  * #### (CTG) Book 2 Only Green
    Same explanation as **(CTG) Book 1 Only Green**, but for the second book

  * #### Self-Learning
	Experience file structure:

1. e4 (from start position)
1. c4 (from start position)
1. Nf3 (from start position)
1 .. c5 (after 1. e4)
1 .. d6 (after 1. e4)

2 positions and a total of 5 moves in those positions

Now imagine HypnoS plays 1. e4 again, it will store this move in the experience file, but it will be duplicate because 1. e4 is already stored. The experience file will now contain the following:
1. e4 (from start position)
1. c4 (from start position)
1. Nf3 (from start position)
1 .. c5 (after 1. e4)
1 .. d6 (after 1. e4)
1. e4 (from start position)

Now we have 2 positions, 6 moves, and 1 duplicate move (so effectively the total unique moves is 5)

Duplicate moves are a problem and should be removed by merging with existing moves. The merge operation will take the move with the highst depth and ignore the other ones. However, when the engine loads the experience file it will only merge duplicate moves in memory without saving the experience file (to make startup and loading experience file faster)

At this point, the experience file is considered fragmented because it contains duplicate moves. The fragmentation percentage is simply: (total duplicate moves) / (total unique moves) * 100
In this example we have a fragmentation level of: 1/6 * 100 = 16.67%

  * #### Experience Readonly
  Default: False If activated, the experience file is only read.
  
  * #### Experience Book
  HypnoS play using the moves stored in the experience file as if it were a book

  * #### Experience Book Width
    The number of moves to consider from the book for the same position. To play best book move, set this option to ```1```.	
	
  * #### Experience Book Min Depth
  Parameter with default value of 27 to limit the minimum depth of experience move that can be used by the experience book.
  When responding to "isready" command, make sure experience data has fully loaded before responding with "readyok" " (for better UCI compatibility) 

  * #### Experience Book Max Moves
	This is a setup to limit the number of moves that can be played by the experience book.
	If you configure 16, the engine will only play 16 moves (if available).
	
  * #### Experience Book Eval Importance 
Experience book move quality logic:

The quality of experience book moves has been revised heavily based on feedback from users. The new logic relies on a new parameter called (Experience Book Eval Importance) which defines how much weight to assign to experience move evaluation vs. count.

The maximum value for this new parameter is: 10, which means the experience move quality will be 100% based on evaluation, and 0% based on count

The minimum value for this new parameter is: 0, which means the experience move quality will be 0% based on evaluation, and 100% based on count

The default value for this new parameter is: 5, which means the experience move quality will be 50% based on evaluation, and 50% based on count																			  


  * #### Persistent Hash
The Persistent hash function will use the following logic: 
will only save valid entries from the hash file, so the final persisted file size is not necessary the same as the Hash size.
Also, when loading, the current hash will not be resized, it will simply load the file entries into the current hash without resizing.

  * #### NeverClearHash
Default: false
You can set the NeverClearHash option to avoid that the hash could be cleared by a Clear Hash or ucinewgame command.

  * #### HashFile
Default: Hypnos.hsh
but you can set any name as long as you keep the integrity of the file extension

  * #### SaveHashtoFile
Stop the search/analysis, return to the UCI parameters then, then click the "SaveHashtoFile" button. 

  * #### LoadHashfromFile
To later reload the saved information, first make sure the "Hash File Name" is for the file you want, then click "LoadHashfromFile".
Once it loads, you may continue analysis.
You must make sure the current hash filename is the same as what was used when you saved the file.

  * #### SmartMultiPVMode

This option performs additional checks only if rootNode is true, while the base code only applies motion checking.
Effect of differences
Greater control: New changes allow you to skip additional movements based on smartMultiPvMode logic, which can optimize the search process.

The extended code provides finer control over the order of moves in rootMoves based on smartMultiPvMode. 
This can lead to performance optimization in some specific cases, avoiding unnecessary sorting when certain conditions are not met.

To be used only in analysis mode

  * #### Variety
Integer, Default: 0, Min: 0, Max: 40 To play different opening lines from default (0), if not from book (see below).
Higher variety -> more probable loss of ELO

  * #### Variety Max Moves
Adjust how many moves we want HypnoS to use the move variety feature.  

  * #### Options to control engine evaluation strategy
1) Materialistic Evaluation Strategy: Minimum = -12, Maximum = +12, Default = 0. Lower values will cause the engine assign less value to material differences between the sides. More values will cause the engine to assign more value to the material difference.
2) Positional Evaluation Strategy: Minimum = -12, Maximum = +12, Default = 0. Lower values will cause the engine assign less value to positional differences between the sides. More values will cause the engine to assign more value to the positional difference.

The default value for both options (0 = zero) is equivalent to the default evaluation strategy of Stockfish.
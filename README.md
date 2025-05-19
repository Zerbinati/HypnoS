<p align="center">
  <img src="http://outskirts.altervista.org/forum/ext/dmzx/imageupload/img-files/2/ca292f8/8585091/34788e79c6bbe7cf7bb578c6fb4d11f8.jpg">
</p>

<h1 align="center">HypnoS</h1>

  ### License

HypnoS is based on the Stockfish engine and is distributed under the GNU General Public License v3.0.
See the [LICENSE](./LICENSE) file for details.


  ### HypnoS Overview


HypnoS is a free and strong UCI chess engine derived from Stockfish 
that analyzes chess positions and computes the optimal moves.

HypnoS does not include a graphical user interface (GUI) that is required 
to display a chessboard and to make it easy to input moves. These GUIs are 
developed independently from HypnoS and are available online.

  ### Acknowledgements

This project is built upon the  [Stockfish](https://github.com/official-stockfish/Stockfish)  and would not have been possible without the exceptional work of the Stockfish developers.  
While HypnoS has diverged from the latest upstream version due to structural differences and the integration of a custom learning system, the core foundation, ideas, and architecture remain deeply rooted in Stockfish.  
I am sincerely grateful to the entire Stockfish team for making such an outstanding engine openly available to the community.

* Andrew Grant for the [OpenBench](https://github.com/AndyGrant/OpenBench) platform.

HypnoS development is currently supported through the OpenBench framework.  
OpenBench, created by Andrew Grant, is an open-source Sequential Probability Ratio Testing (SPRT) framework designed for self-play testing of chess engines.  
It leverages distributed computing, allowing anyone to contribute CPU time to support the development of some of the world’s strongest chess engines.


  ### UCI options


  ### CTG/BIN Book File

The file name of the first book file which could be a polyglot (BIN) or Chessbase (CTG) book. To disable this book, use: ```<empty>```
If the book (CTG or BIN) is in a different directory than the engine executable, then configure the full path of the book file, example:
```C:\Path\To\My\Book.ctg``` or ```/home/username/path/to/book/bin```

  ### Book Width

The number of moves to consider from the book for the same position. To play best book move, set this option to 1. If a value ```n``` (greater than 1 is configured, the engine will pick **randomly** one of the top ```n``` moves available in the book for the given position

  ### Book Depth

The maximum number of moves to play from the book
	
  ### Self-Learning

*	### Experience file structure:

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

  ### Experience Readonly

  Default: False If activated, the experience file is only read.
  
  ### Experience Book

  HypnoS play using the moves stored in the experience file as if it were a book

  ### Experience Book Width

The number of moves to consider from the book for the same position.

  ### Experience Book Eval Importance

The quality of experience book moves has been revised heavily based on feedback from users. The new logic relies on a new parameter called (Experience Book Eval Importance) which defines how much weight to assign to experience move evaluation vs. count.

The maximum value for this new parameter is: 10, which means the experience move quality will be 100% based on evaluation, and 0% based on count

The minimum value for this new parameter is: 0, which means the experience move quality will be 0% based on evaluation, and 100% based on count

The default value for this new parameter is: 5, which means the experience move quality will be 50% based on evaluation, and 50% based on count	

  ### Experience Book Min Depth

Type: Integer
Default Value: 27
Range: Max to 64
This option sets the minimum depth threshold for moves to be included in the engine's Experience Book. Only moves that have been searched at least to the specified depth will be considered for inclusion.

  ### Experience Book Max Moves

Type: Integer
Default Value: 16
Range: 1 to 100
	This is a setup to limit the number of moves that can be played by the experience book.
	If you configure 16, the engine will only play 16 moves (if available).

  ### Variety

Enables randomization of move selection in balanced positions not covered by the opening book.  
A higher value increases the probability of deviating from the mainline, potentially at the cost of Elo.

This option is mainly intended for testing, analysis, or generating varied self-play games.

Set to `0` for fully deterministic behavior.  
Typical useful range: `10–20` for light variety in early game.

 Note: Variety works in combination with `Variety Max Score` and `Variety Max Moves`, which control the conditions under which randomness can apply.
  ### Variety Max Score

Maximum score threshold (in centipawns) below which randomization of the best move is allowed.  
If the absolute evaluation is below this value, the engine may apply a small, controlled random bonus  
to the best move score in order to increase variability in balanced positions.

- A value of `0` disables the feature (fully deterministic behavior).
- Typical values range from `10` to `30`.
- The hard maximum is `50`, beyond which the randomness could affect clearly winning or losing positions and is not recommended.

This feature is primarily intended for testing purposes, to introduce diversity in games that would otherwise be repetitive.

  ### Variety Max Moves

Maximum game ply (half-moves) under which the variety bonus can be applied.  
Once the game progresses beyond this ply count, the randomization feature is disabled.

- `0` disables the feature entirely.
- Values between `10` and `30` are typical for introducing diversity early in the game.
- The hard cap is `40`, since variety in late-game scenarios is generally undesirable.

This setting prevents randomness from affecting important endgame decisions. 

  ### Options to control engine evaluation strategy

1) Materialistic Evaluation Strategy: Minimum = -12, Maximum = +12, Default = 0. Lower values will cause the engine assign less value to material differences between the sides. More values will cause the engine to assign more value to the material difference.

2) Positional Evaluation Strategy: Minimum = -12, Maximum = +12, Default = 0. Lower values will cause the engine assign less value to positional differences between the sides. More values will cause the engine to assign more value to the positional difference.

The default value for both options (0 = zero) is equivalent to the default evaluation strategy of Stockfish.


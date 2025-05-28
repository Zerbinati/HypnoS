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
	
  ### Custom Engine Features

This engine build introduces several advanced options that allow users to tailor the evaluation and search behavior, enabling deeper experimentation and stylistic flexibility.

  ### Select Style

Defines the overall strategic style of the engine. It affects how the evaluation guides move selection and prioritizes different aspects of the position. Available options:

  * #### Default – The standard Stockfish behavior.

  * #### Aggressive – Prioritizes tactical play, piece activity, and initiative.

  * #### Defensive – Focuses on solidity, safety, and simplification.

  * #### Positional – Emphasizes long-term factors like weak squares and pawn structures.

Useful for simulating different playstyles or preparing for varied opponents.

  ### Exploration Mode

When enabled, this mode introduces a controlled degree of randomness into the move ordering during search. This can help:

  * #### Explore non-mainline variations.

  * #### Increase diversity in analysis.

  * #### Simulate human-like unpredictability.

  * #### Disabled by default to preserve competitive strength.

  ### Dynamic Strategy

Enables a phase-aware evaluation model, where the engine dynamically adjusts the balance between material and positional values depending on the game phase:

  * #### In the opening and middlegame, positional aspects are weighted more.

  * #### In the endgame, material importance increases.

This improves evaluation fidelity and strategic adaptability.

  ### Materialistic Evaluation Strategy

Allows the user to manually tune the importance of material balance in the evaluation. Accepted values: -12 to +12 (scaled internally):

  * #### Positive values increase the material emphasis.

  * #### Negative values reduce it, allowing positional or stylistic factors to dominate.

  ### Positional Evaluation Strategy

Similar to the material strategy, this option modifies the weight given to positional evaluation features:

  * #### Positive values amplify features like king safety, mobility, and structure.

  * #### Negative values reduce their impact in the final score.

  ### Example Configurations

For a “creative human-like” engine:

Select Style = Aggressive

Exploration Mode = On

Dynamic Strategy = On

For technical training or deep analysis:

Select Style = Positional

Exploration Mode = Off

Dynamic Strategy = Off

  ### Manual Weights Adjustment

1) Materialistic Evaluation Strategy: Minimum = -12, Maximum = +12, Default = 0. Lower values will cause the engine assign less value to material differences between the sides. More values will cause the engine to assign more value to the material difference.

2) Positional Evaluation Strategy: Minimum = -12, Maximum = +12, Default = 0. Lower values will cause the engine assign less value to positional differences between the sides. More values will cause the engine to assign more value to the positional difference.

The default value for both options (0 = zero) is equivalent to the default evaluation strategy of Stockfish.


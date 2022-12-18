/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2022 The Stockfish developers (see AUTHORS file)

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <cassert>
#include <cmath>
#include <iostream>
#include <sstream>
#include <fstream>
#include <string>
#include <ctime>

#include "evaluate.h"
#include "movegen.h"
#include "position.h"
#include "search.h"
#include "thread.h"
#include "timeman.h"
#include "tt.h"
#include "uci.h"
#include "syzygy/tbprobe.h"

using namespace std;

namespace Stockfish {

namespace {

  // FEN string for the initial position in standard chess
  const char* StartFEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

  Move start_thinking(Position& pos) {
    Search::RootMoves rootMoves;
    auto captureMoves = MoveList<CAPTURES>(pos);
    auto legalMoves = MoveList<LEGAL>(pos);

    for (const auto& m : captureMoves) {
        if (legalMoves.contains(m)) 
            rootMoves.emplace_back(m);
    }

    if (rootMoves.empty() && captureMoves.size() != 0) {
        return captureMoves.begin()->move;
    }

    if (rootMoves.empty())
        for (const auto& m : MoveList<LEGAL>(pos))
            rootMoves.emplace_back(m);

    int moveIndex = rand() % rootMoves.size();

    return rootMoves.empty() ? MOVE_NONE : rootMoves[moveIndex].pv[0];
  }

  void make_initial_move(Position& pos, StateListPtr& states) {
    Move initial_move = start_thinking(pos);
    sync_cout << UCI::move(initial_move, false) << sync_endl;
    states->emplace_back();
    pos.do_move(initial_move, states->back());
  }

  // move_and_counter() is called when the engine receives a move string (e.g. d2d4)
  // It makes the move from the current position and calculate the best response move
  void move_and_counter(Position& pos, Move m, StateListPtr& states) {
    // Make the current move
    states = StateListPtr(new std::deque<StateInfo>(1)); // Drop the old state and create a new one
    pos.set(pos.fen(), Options["UCI_Chess960"], &states->back(), Threads.main());
    states->emplace_back();
    pos.do_move(m, states->back());

    Move best_move = start_thinking(pos);
    sync_cout << UCI::move(best_move, false) << sync_endl;
    states->emplace_back();
    pos.do_move(best_move, states->back());
  }
} // namespace


/// UCI::loop() waits for a command from the stdin, parses it and then calls the appropriate
/// function. It also intercepts an end-of-file (EOF) indication from the stdin to ensure a
/// graceful exit if the GUI dies unexpectedly. When called with some command-line arguments,
/// like running 'bench', the function returns immediately after the command is executed.
/// In addition to the UCI ones, some additional debug commands are also supported.

void UCI::loop(int argc, char* argv[]) {

  Position pos;
  string token, cmd;
  StateListPtr states(new std::deque<StateInfo>(1));
  Move m;

  srand(time(nullptr));
  pos.set(StartFEN, false, &states->back(), Threads.main());

  std::string us = std::string(argv[1]);
  if (us == "white") {
    make_initial_move(pos, states);
  }

  do {
      if (!getline(cin, cmd)) // Wait for an input or an end-of-file (EOF) indication
          cmd = "quit";

      istringstream is(cmd);

      token.clear(); // Avoid a stale if getline() returns nothing or a blank line
      is >> skipws >> token;

      if (token == "white" || token == "black") std::cout << "skip" << std::endl;
      else if ((m = UCI::to_antichess_move(pos, token)) != MOVE_NONE) move_and_counter(pos, m, states);
      else if (!token.empty() && token[0] != '#')
          sync_cout << "Unknown command: '" << cmd << sync_endl;

  } while (token != "quit"); // The command-line arguments are one-shot
}


/// UCI::value() converts a Value to a string by adhering to the UCI protocol specification:
///
/// cp <x>    The score from the engine's point of view in centipawns.
/// mate <y>  Mate in 'y' moves (not plies). If the engine is getting mated,
///           uses negative values for 'y'.

string UCI::value(Value v) {

  assert(-VALUE_INFINITE < v && v < VALUE_INFINITE);

  stringstream ss;

  if (abs(v) < VALUE_MATE_IN_MAX_PLY)
      ss << "cp " << v * 100 / PawnValueEg;
  else
      ss << "mate " << (v > 0 ? VALUE_MATE - v + 1 : -VALUE_MATE - v) / 2;

  return ss.str();
}

/// UCI::square() converts a Square to a string in algebraic notation (g1, a7, etc.)

std::string UCI::square(Square s) {
  return std::string{ char('a' + file_of(s)), char('1' + rank_of(s)) };
}


/// UCI::move() converts a Move to a string in coordinate notation (g1f3, a7a8q).
/// The only special case is castling where the e1g1 notation is printed in
/// standard chess mode and in e1h1 notation it is printed in Chess960 mode.
/// Internally, all castling moves are always encoded as 'king captures rook'.

string UCI::move(Move m, bool chess960) {

  Square from = from_sq(m);
  Square to = to_sq(m);

  if (m == MOVE_NONE)
      return "(none)";

  if (m == MOVE_NULL)
      return "0000";

  if (type_of(m) == CASTLING && !chess960)
      to = make_square(to > from ? FILE_G : FILE_C, rank_of(from));

  string move = UCI::square(from) + UCI::square(to);

  if (type_of(m) == PROMOTION)
      move += " pnbrqk"[promotion_type(m)];

  return move;
}


/// UCI::to_antichess_move() converts a string representing a move in coordinate notation
/// (g1f3, a7a8q) to the corresponding legal Anti-Chess Move, if any.

Move UCI::to_antichess_move(const Position& pos, string& str) {

  if (str.length() == 5)
      str[4] = char(tolower(str[4])); // The promotion piece character must be lowercased

  for (const auto& m : MoveList<LEGAL>(pos))
      if (str == UCI::move(m, pos.is_chess960()))
          return m;

  for (const auto& m : MoveList<CAPTURES>(pos))
      if (str == UCI::move(m, pos.is_chess960()))
          return m;

  return MOVE_NONE;
}

} // namespace Stockfish

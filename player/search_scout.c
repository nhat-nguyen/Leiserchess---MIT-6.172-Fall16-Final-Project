// Copyright (c) 2015 MIT License by 6.172 Staff

// This file contains the scout search routine. Although this routine contains
//   some duplicated logic from the searchPV routine in search.c, it is
//   convenient to maintain it separately. This allows one to easily
//   parallelize scout search separately from searchPV.

#include "./tbassert.h"
// #include "./simple_mutex.h"


typedef int simple_mutex_t;

static inline void init_simple_mutex(simple_mutex_t* mutex) {
  *mutex = 0;
}

static inline void simple_acquire(simple_mutex_t* mutex) {
  while (!__sync_bool_compare_and_swap(mutex, 0, 1)) {
    continue;  // Did not acquire lock yet.
  }
}

static inline void simple_release(simple_mutex_t* mutex) {
  __sync_bool_compare_and_swap(mutex, 1, 0);
  // if (!__sync_bool_compare_and_swap(mutex, 1, 0)) {
  //   printf("ERROR!\n");
  // }
}

#include <cilk/cilk.h>

// Checks whether a node's parent has aborted.
//   If this occurs, we should just stop and return 0 immediately.
static inline bool parallel_parent_aborted(searchNode* node) {
  searchNode* pred = node->parent;
  while (pred != NULL) {
    if (pred->abort) {
      return true;
    }
    pred = pred->parent;
  }
  return false;
}

// Checks whether this node has aborted due to a cut-off.
//   If this occurs, we should actually return the score.
static inline bool parallel_node_aborted(searchNode* node) {
  if (node->abort) {
    return true;
  }
  return false;
}

// Initialize a scout search node for a "Null Window" search.
//   https://chessprogramming.wikispaces.com/Scout
//   https://chessprogramming.wikispaces.com/Null+Window
static inline void initialize_scout_node(searchNode *node, int depth) {
  node->type = SEARCH_SCOUT;
  node->beta = -(node->parent->alpha);
  node->alpha = node->beta - 1;
  node->depth = depth;
  node->ply = node->parent->ply + 1;
  node->subpv = 0;
  node->legal_move_count = 0;
  node->fake_color_to_move = color_to_move_of(&(node->position));
  // point of view = 1 for white, -1 for black
  node->pov = 1 - node->fake_color_to_move * 2;
  node->best_move_index = 0;  // index of best move found
  node->abort = false;
}

static inline bool search_process_score_local(searchNode *node, move_t mv, int mv_index,
                                moveEvaluationResult *result, searchType_t type) {
  if (result->score > node->best_score) {
    node->best_score = result->score;
    node->best_move_index = mv_index;
    node->subpv = mv;

    // write best move into right position in PV buffer.
    // memcpy(node->subpv + 1, result->next_node.subpv,
    //        sizeof(move_t) * (MAX_PLY_IN_SEARCH - 1));
    // node->subpv[MAX_PLY_IN_SEARCH - 1] = 0;

    // if (type != SEARCH_SCOUT && result->score > node->alpha) {
    //   node->alpha = result->score;
    // }

    if (result->score >= node->beta) {
      if (mv != killer[KMT(node->ply, 0)] && ENABLE_TABLES) {
        killer[KMT(node->ply, 1)] = killer[KMT(node->ply, 0)];
        killer[KMT(node->ply, 0)] = mv;
      }
      return true;
    }
  }
  return false;
}


static inline void perform_scout_search_expand_serial(int *break_flag, 
             searchNode *node,
             sortable_move_t *move_list,
             // sortable_move_t *sorted_move_list,
             // uint32_t *range_tree,
             uint64_t *node_count_serial,
             move_t killer_a,
             move_t killer_b,
             int *number_of_moves_evaluated) {
  // if (*break_flag) return;
  
  
  int local_index = (*number_of_moves_evaluated)++;
  move_t mv = get_move(move_list[local_index]);

  // if (TRACE_MOVES) {
  //   print_move_info(mv, node->ply);
  // }

  // increase node count
  // __sync_fetch_and_add(node_count_serial, 1);
  // (*node_count_serial)++;
  
    
  moveEvaluationResult result = evaluateMove(node, mv, killer_a, killer_b,
                                             SEARCH_SCOUT,
                                             node_count_serial);
  if (!(result.type == MOVE_ILLEGAL || result.type == MOVE_IGNORE
      || abortf || parallel_parent_aborted(node)))
  {

    // A legal move is a move that's not KO, but when we are in quiescence
    // we only want to count moves that has a capture.
    if (result.type == MOVE_EVALUATED) {
      node->legal_move_count++;
    }

    // process the score. Note that this mutates fields in node.
    bool cutoff = search_process_score_local(node, mv, local_index, &result, SEARCH_SCOUT);

    if (cutoff) {
      node->abort = true;
      *break_flag = 1;
    }
  }
}


void perform_scout_search_expand(int *break_flag, 
					   simple_mutex_t *mutex, 
					   searchNode *node,
					   sortable_move_t *move_list,
					   uint64_t *node_count_serial,
					   move_t killer_a,
					   move_t killer_b,
					   int *number_of_moves_evaluated) {
  if (*break_flag) return;
  
  // simple_acquire(mutex);
  
  int local_index = __sync_fetch_and_add(number_of_moves_evaluated,1);  
  move_t mv = get_move(move_list[local_index]);
  
  // if (TRACE_MOVES) {
  //   print_move_info(mv, node->ply);
  // }

  // increase node count
  // __sync_fetch_and_add(node_count_serial, 1);
  // (*node_count_serial)++;  
  
  // simple_release(mutex);
    
  moveEvaluationResult result = evaluateMove(node, mv, killer_a, killer_b,
                                             SEARCH_SCOUT,
                                             node_count_serial);
  if (!(result.type == MOVE_ILLEGAL || result.type == MOVE_IGNORE
      || abortf || parallel_parent_aborted(node)))
  {

    // A legal move is a move that's not KO, but when we are in quiescence
    // we only want to count moves that has a capture.
    if (result.type == MOVE_EVALUATED) {
      node->legal_move_count++;
    }

    // process the score. Note that this mutates fields in node.
    simple_acquire(mutex);
    bool cutoff = search_process_score_local(node, mv, local_index, &result, SEARCH_SCOUT);
    simple_release(mutex);

    if (cutoff) {
      node->abort = true;
      *break_flag = 1;
    }
  }
}

// static const uint64_t sq_to_board_bit[100] = {
// 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL,
// 0ULL, 1ULL<<0, 1ULL<<1, 1ULL<<2, 1ULL<<3, 1ULL<<4, 1ULL<<5, 1ULL<<6, 1ULL<<7, 0ULL,
// 0ULL, 1ULL<<8, 1ULL<<9, 1ULL<<10, 1ULL<<11, 1ULL<<12, 1ULL<<13, 1ULL<<14, 1ULL<<15, 0ULL,
// 0ULL, 1ULL<<16, 1ULL<<17, 1ULL<<18, 1ULL<<19, 1ULL<<20, 1ULL<<21, 1ULL<<22, 1ULL<<23, 0ULL,
// 0ULL, 1ULL<<24, 1ULL<<25, 1ULL<<26, 1ULL<<27, 1ULL<<28, 1ULL<<29, 1ULL<<30, 1ULL<<31, 0ULL,
// 0ULL, 1ULL<<32, 1ULL<<33, 1ULL<<34, 1ULL<<35, 1ULL<<36, 1ULL<<37, 1ULL<<38, 1ULL<<39, 0ULL,
// 0ULL, 1ULL<<40, 1ULL<<41, 1ULL<<42, 1ULL<<43, 1ULL<<44, 1ULL<<45, 1ULL<<46, 1ULL<<47, 0ULL,
// 0ULL, 1ULL<<48, 1ULL<<49, 1ULL<<50, 1ULL<<51, 1ULL<<52, 1ULL<<53, 1ULL<<54, 1ULL<<55, 0ULL,
// 0ULL, 1ULL<<56, 1ULL<<57, 1ULL<<58, 1ULL<<59, 1ULL<<60, 1ULL<<61, 1ULL<<62, 1ULL<<63, 0ULL,
// 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL};

static inline bool valid_move(searchNode *node, move_t mv) {
  if (!mv)
    return false;
  ptype_t  pce = ptype_mv_of(mv);
  rot_t    ro  = rot_of(mv);   // rotation
  square_t fs  = from_square(mv);
  square_t ts  = to_square(mv);
  if (ptype_of(node -> position.board[fs]) != pce)
    return false;
  if (color_of(node -> position.board[fs]) != (node -> position.ply & 1))
    return false;
  if (fs == ts && !ro && pce != KING)
    return false;
  // uint64_t laser_map = mark_laser_path_bit(&node -> position, opp_color(node -> position.ply & 1));
  if (node -> position.laser[opp_color(node -> position.ply & 1)] & sq_to_board_bit[fs])
    return false;
  return true;
}

static inline void my_sort_incremental(sortable_move_t *move_list, int num_of_moves) {
  int t = num_of_moves / 4 - 1;
  if (t < 2)
    t = 2;
  for (int j = 0; j < num_of_moves; j++) {
    sortable_move_t insert = move_list[j];
    int hole = j;
    while (hole > 0 && insert > move_list[hole-1]) {
      move_list[hole] = move_list[hole-1];
      hole--;
    }
    // memmove(move_list + hole + 1, move_list + hole, (j - hole) * sizeof(sortable_move_t));
    move_list[hole] = insert;
  }
  // int t = num_of_moves / 2 - 1;
  for (int j = t + 1; j < num_of_moves; j++) {
    sortable_move_t insert = move_list[j];
    if (insert < move_list[t]) {
      move_list[j] = move_list[num_of_moves - 1];
      move_list[num_of_moves - 1] = insert;
      num_of_moves --;
      j--;
      continue;
    }
    int hole = j;
    while (hole > 0 && insert > move_list[hole-1]) {
      move_list[hole] = move_list[hole-1];
      hole--;
    }
    // memmove(move_list + hole + 1, move_list + hole, (j - hole) * sizeof(sortable_move_t));
    move_list[hole] = insert;
  }
}

static inline void my_sort_incremental2(sortable_move_t *move_list, int num_of_moves) {
  num_of_moves -= 1;
  
  for (int j = 0; j <= num_of_moves; j++)
    if (move_list[j] < (1LL << 32)) {
      sortable_move_t temp = move_list[num_of_moves];
      move_list[num_of_moves] = move_list[j];
      move_list[j] = temp;
      j -= 1;
      num_of_moves -= 1;
    }

  for (int j = 0; j <= num_of_moves; j++) {
    sortable_move_t insert = move_list[j];
    int hole = j;
    while (hole > 0 && insert > move_list[hole-1]) {
      move_list[hole] = move_list[hole-1];
      hole--;
    }
    // memmove(move_list + hole + 1, move_list + hole, (j - hole) * sizeof(sortable_move_t));
    move_list[hole] = insert;
  }
}

static score_t scout_search(searchNode *node, int depth,
                            uint64_t *node_count_serial) {
  // Initialize the search node.
  initialize_scout_node(node, depth);

  // check whether we should abort
  if (should_abort_check() || parallel_parent_aborted(node)) {
    return 0;
  }

  // Pre-evaluate this position.
  leafEvalResult pre_evaluation_result = evaluate_as_leaf(node, SEARCH_SCOUT);

  // If we decide to stop searching, return the pre-evaluation score.
  if (pre_evaluation_result.type == MOVE_EVALUATED) {
    return pre_evaluation_result.score;
  }

  // Populate some of the fields of this search node, using some
  //  of the information provided by the pre-evaluation.
  int hash_table_move = pre_evaluation_result.hash_table_move;
  node->best_score = pre_evaluation_result.score;
  node->quiescence = pre_evaluation_result.should_enter_quiescence;

  // Grab the killer-moves for later use.
  move_t killer_a = killer[KMT(node->ply, 0)];
  move_t killer_b = killer[KMT(node->ply, 1)];
  // Store the sorted move list on the stack.
  //   MAX_NUM_MOVES is all that we need.
  
  
  int number_of_moves_evaluated = 0;
  int break_flag = 0;
  

    
  // A simple mutex. See simple_mutex.h for implementation details.
  

  // Sort the move list.

  // sort_incremental(move_list, num_of_moves);
  // if (valid_move(node, hash_table_move))
  //   move_list[0] = hash_table_move;
  // use this after testing
  sortable_move_t move_list2[3];

  if (valid_move(node, hash_table_move)) {
    move_list2[number_of_moves_evaluated] = hash_table_move;
    perform_scout_search_expand_serial(&break_flag, node, move_list2, node_count_serial, killer_a, killer_b, &number_of_moves_evaluated);
  }
  
  
  if (!break_flag && killer_a != hash_table_move && valid_move(node, killer_a)) {
    move_list2[number_of_moves_evaluated] = killer_a;
    perform_scout_search_expand_serial(&break_flag, node, move_list2, node_count_serial, killer_a, killer_b, &number_of_moves_evaluated);
  }
  
  if (!break_flag && killer_b != hash_table_move && killer_b != killer_a && valid_move(node, killer_b)) {
    move_list2[number_of_moves_evaluated] = killer_b;
    perform_scout_search_expand_serial(&break_flag, node, move_list2, node_count_serial, killer_a, killer_b, &number_of_moves_evaluated);
  }
  // static long long count = 0, cnt = 0, count1 = 0, cnt1 = 0;
  // count += 1;
  if (!break_flag) {
    sortable_move_t move_list[MAX_NUM_MOVES];
    int num_of_moves = get_sortable_move_list2(node, move_list, hash_table_move, killer_a, killer_b);
    // cnt += 1;
    // count1 += num_of_moves;
    // for (int i = 0; i < num_of_moves; i++)
    //   if (move_list[i] < (1LL << 32))
    //     cnt1 += 1;
    // printf("%.5lf %.5lf %.5lf\n", 1.0 * cnt / count, 1.0 * cnt1 / count1, 1.0 * count1 / cnt);
    my_sort_incremental2(move_list, num_of_moves);
    simple_mutex_t mutex;
    init_simple_mutex(&mutex);

    int lim = num_of_moves; 
    if (lim>5) lim = 5;

    for (int mv_index = number_of_moves_evaluated; mv_index < lim && !break_flag; mv_index++) {
      // Get the next move from the move list.
      perform_scout_search_expand_serial(&break_flag, node, move_list, node_count_serial, killer_a, killer_b, &number_of_moves_evaluated);
    }
    
    if (node -> depth > 1 && !break_flag) {
      cilk_for (int mv_index = lim; mv_index < num_of_moves; mv_index++) {
        perform_scout_search_expand(&break_flag, &mutex, node, move_list, node_count_serial, killer_a, killer_b, &number_of_moves_evaluated);
        if (break_flag)
          break;
      }
    }else {
      for (int mv_index = lim; mv_index < num_of_moves && !break_flag; mv_index++) {
        perform_scout_search_expand_serial(&break_flag, node, move_list, node_count_serial, killer_a, killer_b, &number_of_moves_evaluated);
      }
    }
    if (parallel_parent_aborted(node)) {
      return 0;
    }

    if (node->quiescence == false) {
      update_best_move_history(&(node->position), node->best_move_index,
                               move_list, number_of_moves_evaluated);
    }
  }else {
    if (parallel_parent_aborted(node)) {
      return 0;
    }

    if (node->quiescence == false) {
      update_best_move_history(&(node->position), node->best_move_index,
                               move_list2, number_of_moves_evaluated);
    }
  }

  tbassert(abs(node->best_score) != -INF, "best_score = %d\n",
           node->best_score);

  // Reads node->position.key, node->depth, node->best_score, and node->ply
  update_transposition_table(node);

  return node->best_score;
}



#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <memory>
#include <string>

#include "betting_abstraction.h"
#include "betting_abstraction_params.h"
#include "betting_tree.h"
#include "board_tree.h"
#include "buckets.h"
#include "canonical_cards.h"
#include "card_abstraction.h"
#include "card_abstraction_params.h"
#include "cards.h"
#include "cfr_config.h"
#include "cfr_params.h"
#include "cfr_values.h"
#include "constants.h"
#include "files.h"
#include "game.h"
#include "game_params.h"
#include "hand_tree.h"
#include "io.h"
#include "params.h"

using namespace std;

void Show(Node *node, const string &action_sequence, unsigned int target_p,
	  const Buckets &buckets, const CFRValues &values, bool ***seen) {
  if (node->Street() == 1) return;
  if (node->Terminal()) return;
  unsigned int pa = node->PlayerActing();
  unsigned int num_succs = node->NumSuccs();
  // Have both player's strategies and want to show them.
  // if (num_succs > 1 && (target_p == kMaxUInt || pa == target_p)) {
  if (num_succs > 1) {
    unsigned int nt = node->NonterminalID();
    unsigned int st = node->Street();
    if (seen[st][pa][nt]) return;
    seen[st][pa][nt] = true;
    unsigned int dsi = node->DefaultSuccIndex();
    int *i_values = nullptr;
    double *d_values = nullptr;
    if (values.Ints(pa, st)) {
      values.Values(pa, st, nt, &i_values);
    } else {
      values.Values(pa, st, nt, &d_values);
    }
    unsigned int max_card = Game::MaxCard();
    unsigned int hcp = 0;
    for (unsigned int hi = 1; hi <= max_card; ++hi) {
      for (unsigned int lo = 0; lo < hi; ++lo) {
	if (action_sequence == "") {
	  printf("Root ");
	} else {
	  printf("%s ", action_sequence.c_str());
	}
	OutputTwoCards(hi, lo);
	unsigned int offset, b = kMaxUInt;
	if (buckets.None(st)) {
	  offset = hcp * num_succs;
	} else {
	  b = buckets.Bucket(st, hcp);
	  offset = b * num_succs;
	}
	double sum = 0;
	if (i_values) {
	  for (unsigned int s = 0; s < num_succs; ++s) {
	    int iv = i_values[offset + s];
	    if (iv > 0) sum += iv;
	  }
	} else {
	  for (unsigned int s = 0; s < num_succs; ++s) {
	    double dv = d_values[offset + s];
	    if (dv > 0) sum += dv;
	  }
	}
	if (sum == 0) {
	  for (unsigned int s = 0; s < num_succs; ++s) {
	    printf(" %f", s == dsi ? 1.0 : 0);
	  }
	} else {
	  for (unsigned int s = 0; s < num_succs; ++s) {
	    if (i_values) {
	      int iv = i_values[offset + s];
	      printf(" %f (%i)", iv > 0 ? iv / sum : 0,
		     i_values[offset + s]);
	    } else {
	      double dv = d_values[offset + s];
	      printf(" %f (%f)", dv > 0 ? dv / sum : 0,
		     d_values[offset + s]);
	    }
	  }
	}
	if (! buckets.None(st)) {
	  printf(" (b %u)", b);
	}
	printf(" (pa %u nt %u)\n", node->PlayerActing(),
	       node->NonterminalID());
	fflush(stdout);
	++hcp;
      }
    }
  }
  for (unsigned int s = 0; s < num_succs; ++s) {
    string action = node->ActionName(s);
    Show(node->IthSucc(s), action_sequence + action, target_p, buckets,
	 values, seen);
  }
}

static void Usage(const char *prog_name) {
  fprintf(stderr, "USAGE: %s <game params> <card params> <betting params> "
	  "<CFR params> <it> [current|cum] ([p0|p1])\n", prog_name);
  exit(-1);
}

int main(int argc, char *argv[]) {
  if (argc != 7 && argc != 8) Usage(argv[0]);
  Files::Init();
  unique_ptr<Params> game_params = CreateGameParams();
  game_params->ReadFromFile(argv[1]);
  Game::Initialize(*game_params);
  unique_ptr<Params> card_params = CreateCardAbstractionParams();
  card_params->ReadFromFile(argv[2]);
  unique_ptr<CardAbstraction>
    card_abstraction(new CardAbstraction(*card_params));
  unique_ptr<Params> betting_params = CreateBettingAbstractionParams();
  betting_params->ReadFromFile(argv[3]);
  unique_ptr<BettingAbstraction>
    betting_abstraction(new BettingAbstraction(*betting_params));
  unique_ptr<Params> cfr_params = CreateCFRParams();
  cfr_params->ReadFromFile(argv[4]);
  unique_ptr<CFRConfig>
    cfr_config(new CFRConfig(*cfr_params));
  unsigned int it;
  if (sscanf(argv[5], "%u", &it) != 1) Usage(argv[0]);
  bool current;
  string c = argv[6];
  if (c == "current")  current = true;
  else if (c == "cum") current = false;
  else                 Usage(argv[0]);
  unsigned int target_p = kMaxUInt;
  if (betting_abstraction->Asymmetric()) {
    if (argc == 7) {
      fprintf(stderr, "Expect p0/p1 argument for asymmetric systems\n");
      exit(-1);
    }
    string p_arg = argv[7];
    if (p_arg == "p0")      target_p = 0;
    else if (p_arg == "p1") target_p = 1;
    else                    Usage(argv[0]);
  } else {
    if (argc == 8) {
      fprintf(stderr, "Don't expect p0/p1 argument for symmetric systems\n");
      exit(-1);
    }
  }

  // Excessive to load all buckets.  Only need buckets for the preflop.
  Buckets buckets(*card_abstraction, false);
  unique_ptr<BettingTree> betting_tree;
  if (betting_abstraction->Asymmetric()) {
    betting_tree.reset(BettingTree::BuildAsymmetricTree(*betting_abstraction,
							target_p));
  } else {
    betting_tree.reset(BettingTree::BuildTree(*betting_abstraction));
  }
  unsigned int num_players = Game::NumPlayers();
  unique_ptr<bool []> players(new bool[num_players]);
  if (betting_abstraction->Asymmetric()) {
    for (unsigned int p = 0; p < num_players; ++p) {
      // Have both player's strategies and want to show them.
      // players[p] = (p == target_p);
      players[p] = true;
    }
  } else {
    for (unsigned int p = 0; p < num_players; ++p) {
      players[p] = true;
    }
  }
  unsigned int max_street = Game::MaxStreet();
  unique_ptr<bool []> streets(new bool[max_street + 1]);
  for (unsigned int st = 0; st <= max_street; ++st) {
    streets[st] = (st == 0);
  }
  CFRValues values(players.get(), ! current, streets.get(),
		   betting_tree.get(), 0, 0, *card_abstraction,
		   buckets.NumBuckets(), nullptr);
  char dir[500];
  sprintf(dir, "%s/%s.%u.%s.%u.%u.%u.%s.%s", Files::OldCFRBase(),
	  Game::GameName().c_str(), Game::NumPlayers(),
	  card_abstraction->CardAbstractionName().c_str(),
	  Game::NumRanks(), Game::NumSuits(), Game::MaxStreet(),
	  betting_abstraction->BettingAbstractionName().c_str(),
	  cfr_config->CFRConfigName().c_str());
  if (betting_abstraction->Asymmetric()) {
    char buf[20];
    sprintf(buf, ".p%u", target_p);
    strcat(dir, buf);
  }
  values.Read(dir, it, betting_tree->Root(), "x", kMaxUInt);
  bool ***seen = new bool **[max_street + 1];
  for (unsigned int st = 0; st <= max_street; ++st) {
    seen[st] = new bool *[num_players];
    for (unsigned int p = 0; p < num_players; ++p) {
      unsigned int num_nt = betting_tree->NumNonterminals(p, st);
      seen[st][p] = new bool[num_nt];
      for (unsigned int i = 0; i < num_nt; ++i) {
	seen[st][p][i] = false;
      }
    }
  }
  Show(betting_tree->Root(), "", target_p, buckets, values, seen);
  for (unsigned int st = 0; st <= max_street; ++st) {
    for (unsigned int p = 0; p < num_players; ++p) {
      delete [] seen[st][p];
    }
    delete [] seen[st];
  }
  delete [] seen;
}

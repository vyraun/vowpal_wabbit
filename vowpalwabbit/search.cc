/*
Copyright (c) by respective owners including Yahoo!, Microsoft, and
individual contributors. All rights reserved.  Released under a BSD (revised)
license as described in the file LICENSE.
 */

// TODO: does add_example_conditioning need offset for LDF mode?

#include "search.h"
#include "v_hashmap.h"
#include "hash.h"
#include "rand48.h"
#include "cost_sensitive.h"
#include "multiclass.h"
#include "memory.h"
#include "constant.h"
#include "cb.h"
#include "search_sequencetask.h"
#include "gd.h" // for GD::foreach_feature

using namespace LEARNER;
using namespace std;
namespace CS = COST_SENSITIVE;
namespace MC = MULTICLASS;

namespace Search {
  search_task* all_tasks[] = { &SequenceTask2::task,
                               &SequenceTask_DemoLDF2::task,
                               NULL };   // must NULL terminate!


  const bool PRINT_UPDATE_EVERY_EXAMPLE =0;
  const bool PRINT_UPDATE_EVERY_PASS =0;
  const bool PRINT_CLOCK_TIME =0;

  string   neighbor_feature_space("neighbor");
  string   condition_feature_space("search_condition");

  uint32_t AUTO_CONDITION_FEATURES = 1, AUTO_HAMMING_LOSS = 2, EXAMPLES_DONT_CHANGE = 4, IS_LDF = 8;
  enum SearchState { INITIALIZE, INIT_TEST, INIT_TRAIN, LEARN, GET_TRUTH_STRING };
  enum RollMethod { POLICY, ORACLE, MIX_PER_STATE, MIX_PER_ROLL, NO_ROLLOUT };

  // a data structure to hold conditioning information
  struct prediction {
    ptag    me;     // the id of the current prediction (the one being memoized)
    size_t  cnt;    // how many variables are we conditioning on?
    ptag*   tags;   // which variables are they?
    action* acts;   // and which actions were taken at each?
    uint32_t hash;  // a hash of the above
  };

  // parameters for auto-conditioning
  struct auto_condition_settings {
    size_t max_bias_ngram_length;   // add a "bias" feature for each ngram up to and including this length. eg., if it's 1, then you get a single feature for each conditional
    size_t max_quad_ngram_length;   // add bias *times* input features for each ngram up to and including this length
    float  feature_value;           // how much weight should the conditional features get?
  };
  
  struct search_private {
    vw* all;

    bool auto_condition_features;  // do you want us to automatically add conditioning features?
    bool auto_hamming_loss;        // if you're just optimizing hamming loss, we can do it for you!
    bool examples_dont_change;     // set to true if you don't do any internal example munging
    bool is_ldf;                   // set to true if you'll generate LDF data
    v_array<int32_t> neighbor_features; // ugly encoding of neighbor feature requirements
    auto_condition_settings acset; // settings for auto-conditioning

    size_t A;                      // total number of actions, [1..A]; 0 means ldf
    size_t num_learners;           // total number of learners;
    bool cb_learner;               // do contextual bandit learning on action (was "! rollout_all_actions" which was confusing)
    SearchState state;             // current state of learning
    size_t learn_learner_id;       // we allow user to use different learners for different states
    int mix_per_roll_policy;       // for MIX_PER_ROLL, we need to choose a policy to use; this is where it's stored (-2 means "not selected yet")
    
    size_t t;                      // current search step
    size_t T;                      // length of root trajectory
    v_array<example> learn_ec_copy;// copy of example(s) at learn_t
    example* learn_ec_ref;         // reference to example at learn_t, when there's no example munging
    size_t learn_ec_ref_cnt;       // how many are there (for LDF mode only; otherwise 1)
    v_array<ptag> learn_condition_on;      // a copy of the tags used for conditioning at the training position
    v_array<action> learn_condition_on_act;// the actions taken
    char* learn_condition_on_names;// the names of the actions
    v_array<action> learn_allowed_actions; // which actions were allowed at training time?
    v_array<action> ptag_to_action;// tag to action mapping for conditioning
    
    void* allowed_actions_cache;   // either a CS::label* or CB::label* depending on cb_learner
    
    bool loss_declared;            // did run declare any loss (implicitly or explicitly)?
    v_array<action> train_trajectory; // the training trajectory
    size_t learn_t;                // what time step are we learning on?
    size_t learn_a_idx;            // what action index are we trying?
    bool done_with_all_actions;    // set to true when there are no more learn_a_idx to go

    float test_loss;               // loss incurred when run INIT_TEST
    float learn_loss;              // loss incurred when run LEARN
    float train_loss;              // loss incurred when run INIT_TRAIN
    
    bool last_example_was_newline; // used so we know when a block of examples has passed
    bool hit_new_pass;             // have we hit a new pass?

    // if we're printing to stderr we need to remember if we've printed the header yet
    // (i.e., we do this if we're driving)
    bool printed_output_header;

    // various strings for different search states
    bool should_produce_string;
    stringstream *pred_string;
    stringstream *truth_string;
    stringstream *bad_string_stream;

    // parameters controlling interpolation
    float  beta;                   // interpolation rate
    float  alpha;                  // parameter used to adapt beta for dagger (see above comment), should be in (0,1)
    float  gamma;

    RollMethod rollout_method;     // 0=policy, 1=oracle, 2=mix_per_state, 3=mix_per_roll
    RollMethod rollin_method;
    float subsample_timesteps;     // train at every time step or just a (random) subset?

    bool   allow_current_policy;   // should the current policy be used for training? true for dagger
    bool   adaptive_beta;          // used to implement dagger-like algorithms. if true, beta = 1-(1-alpha)^n after n updates, and policy is mixed with oracle as \pi' = (1-beta)\pi^* + beta \pi
    size_t passes_per_policy;      // if we're not in dagger-mode, then we need to know how many passes to train a policy

    uint32_t current_policy;       // what policy are we training right now?

    // various statistics for reporting
    size_t num_features;
    uint32_t total_number_of_policies;
    size_t read_example_last_id;
    size_t passes_since_new_policy;
    size_t read_example_last_pass;
    size_t total_examples_generated;
    size_t total_predictions_made;
    size_t total_cache_hits;

    vector<example*> ec_seq;  // the collected examples
    v_hashmap<unsigned char*, action> cache_hash_map;
    
    // for foreach_feature temporary storage for conditioning
    uint32_t ec_conditioning_idx;
    example* ec_conditioning_ec;
    stringstream ec_conditioning_audit_ss;
    
    LEARNER::learner* base_learner;
    clock_t start_clock_time;

    example*empty_example;

    search_task* task;    // your task!
  };

  string   audit_feature_space("conditional");
  uint32_t conditional_constant = 8290743;
  uint32_t example_number = 0;

  int random_policy(search_private& priv, bool allow_current, bool allow_optimal) {
    if (false) { // was: if rollout_all_actions
      uint32_t seed = (uint32_t) priv.read_example_last_id * 2147483 + (uint32_t)(priv.t * 2147483647);
      msrand48(seed * 2147483647);
    }
    
    if (priv.beta >= 1) {
      if (allow_current) return (int)priv.current_policy;
      if (priv.current_policy > 0) return (((int)priv.current_policy)-1);
      if (allow_optimal) return -1;
      std::cerr << "internal error (bug): no valid policies to choose from!  defaulting to current" << std::endl;
      return (int)priv.current_policy;
    }

    int num_valid_policies = (int)priv.current_policy + allow_optimal + allow_current;
    int pid = -1;

    if (num_valid_policies == 0) {
      std::cerr << "internal error (bug): no valid policies to choose from!  defaulting to current" << std::endl;
      return (int)priv.current_policy;
    } else if (num_valid_policies == 1) {
      pid = 0;
    } else {
      float r = frand48();
      pid = 0;

      if (r > priv.beta) {
        r -= priv.beta;
        while ((r > 0) && (pid < num_valid_policies-1)) {
          pid ++;
          r -= priv.beta * powf(1.f - priv.beta, (float)pid);
        }
      }
    }
    // figure out which policy pid refers to
    if (allow_optimal && (pid == num_valid_policies-1))
      return -1; // this is the optimal policy

    pid = (int)priv.current_policy - pid;
    if (!allow_current)
      pid--;

    return pid;
  }

  int select_learner(search_private& priv, int policy, size_t learner_id) {
    if (policy<0) return policy;  // optimal policy
    else          return policy*priv.num_learners+learner_id;
  }


  bool should_print_update(vw& all, bool hit_new_pass=false) {
    //uncomment to print out final loss after all examples processed
    //commented for now so that outputs matches make test
    //if( parser_done(all.p)) return true;
    
    if (PRINT_UPDATE_EVERY_EXAMPLE) return true;
    if (PRINT_UPDATE_EVERY_PASS && hit_new_pass) return true;
    return (all.sd->weighted_examples >= all.sd->dump_interval) && !all.quiet && !all.bfgs;
  }

  
  bool might_print_update(vw& all) {
    // basically do should_print_update but check me and the next
    // example because of off-by-ones

    if (PRINT_UPDATE_EVERY_EXAMPLE) return true;
    if (PRINT_UPDATE_EVERY_PASS) return true;  // TODO: make this better
    return (all.sd->weighted_examples + 1. >= all.sd->dump_interval) && !all.quiet && !all.bfgs;
  }

  bool must_run_test(vw&all, vector<example*>ec) {
    return
        (all.final_prediction_sink.size() > 0) ||   // if we have to produce output, we need to run this
        might_print_update(all) ||                  // if we have to print and update to stderr
        (all.raw_prediction > 0) ||                 // we need raw predictions
        // or:
        //   it's not quiet AND
        //     current_pass == 0
        //     OR holdout is off
        //     OR it's a test example
        ( //   (! all.quiet) &&  // had to disable this because of library mode!
          ( all.holdout_set_off ||                    // no holdout
            ec[0]->test_only ||
            (all.current_pass == 0)                   // we need error rates for progressive cost
            ) )
        ;
  }

  void clear_seq(vw&all, search_private& priv) {
    if (priv.ec_seq.size() > 0)
      for (size_t i=0; i < priv.ec_seq.size(); i++)
        VW::finish_example(all, priv.ec_seq[i]);
    priv.ec_seq.clear();
  }

  float safediv(float a,float b) { if (b == 0.f) return 0.f; else return (a/b); }

  void to_short_string(string in, size_t max_len, char*out) {
    for (size_t i=0; i<max_len; i++)
      out[i] = ((i >= in.length()) || (in[i] == '\n') || (in[i] == '\t')) ? ' ' : in[i];

    if (in.length() > max_len) {
      out[max_len-2] = '.';
      out[max_len-1] = '.';
    }
    out[max_len] = 0;
  }

  void print_update(search_private& priv) {
    vw& all = *priv.all;
    if (!priv.printed_output_header && !all.quiet) {
      const char * header_fmt = "%-10s %-10s %8s %15s %24s %22s %5s %5s %15s %15s %15s\n";
      fprintf(stderr, header_fmt, "average", "since", "instance", "example",   "current true",  "current predicted", "cur",  "cur", "predic.", "cache", "examples");
      fprintf(stderr, header_fmt, "loss",    "last",  "counter",  "weight",    "output prefix",  "output prefix",    "pass", "pol", "made",    "hits",  "gener.");
      cerr.precision(5);
      priv.printed_output_header = true;
    }

    if (!should_print_update(all, priv.hit_new_pass))
      return;

    char true_label[21];
    char pred_label[21];
    to_short_string(priv.truth_string->str(), 20, true_label);
    to_short_string(priv.pred_string->str() , 20, pred_label);

    float avg_loss = 0.;
    float avg_loss_since = 0.;
    if (!all.holdout_set_off && all.current_pass >= 1) {
      avg_loss       = safediv((float)all.sd->holdout_sum_loss, (float)all.sd->weighted_holdout_examples);
      avg_loss_since = safediv((float)all.sd->holdout_sum_loss_since_last_dump, (float)all.sd->weighted_holdout_examples_since_last_dump);

      all.sd->weighted_holdout_examples_since_last_dump = 0;
      all.sd->holdout_sum_loss_since_last_dump = 0.0;
    } else {
      avg_loss       = safediv((float)all.sd->sum_loss, (float)all.sd->weighted_examples);
      avg_loss_since = safediv((float)all.sd->sum_loss_since_last_dump, (float) (all.sd->weighted_examples - all.sd->old_weighted_examples));
    }

    fprintf(stderr, "%-10.6f %-10.6f %8ld %15f   [%s] [%s] %5d %5d %15lu %15lu %15lu",
            avg_loss,
            avg_loss_since,
            (long int)all.sd->example_number,
            all.sd->weighted_examples,
            true_label,
            pred_label,
            //(long unsigned int)priv.num_features,
            (int)priv.read_example_last_pass,
            (int)priv.current_policy,
            (long unsigned int)priv.total_predictions_made,
            (long unsigned int)priv.total_cache_hits,
            (long unsigned int)priv.total_examples_generated);

    if (PRINT_CLOCK_TIME) {
      size_t num_sec = (size_t)(((float)(clock() - priv.start_clock_time)) / CLOCKS_PER_SEC);
      fprintf(stderr, " %15lusec", num_sec);
    }

    if (!all.holdout_set_off && all.current_pass >= 1)
      fprintf(stderr, " h");

    fprintf(stderr, "\n");

    all.sd->sum_loss_since_last_dump = 0.0;
    all.sd->old_weighted_examples = all.sd->weighted_examples;
    fflush(stderr);
    VW::update_dump_interval(all);
  }

  void add_neighbor_features(search& sch) {
    // TODO: rewrite this using foreach_feature
    size_t neighbor_constant = 8349204823;
    vw*all = sch.priv->all;
    if (sch.priv->neighbor_features.size() == 0) return;
    uint32_t wpp = sch.priv->all->wpp << sch.priv->all->reg.stride_shift;

    for (int32_t n=0; n<(int32_t)sch.priv->ec_seq.size(); n++) {
      example*me = sch.priv->ec_seq[n];
      for (int32_t*enc=sch.priv->neighbor_features.begin; enc!=sch.priv->neighbor_features.end; ++enc) {
        int32_t offset = (*enc) >> 24;
        size_t  old_ns = (*enc) & 0xFF;
        size_t  enc_offset = wpp * ((2 * (size_t)(*enc)) + ((*enc < 0) ? 1 : 0));

        if ((n + offset >= 0) && (n + offset < (int32_t)sch.priv->ec_seq.size())) { // we're okay on position
          example*you = sch.priv->ec_seq[n+offset];
          size_t  you_size = you->atomics[old_ns].size();

          if (you_size > 0) {
            if (me->atomics[neighbor_namespace].size() == 0)
              me->indices.push_back(neighbor_namespace);

            me->atomics[neighbor_namespace].resize(me->atomics[neighbor_namespace].size() + you_size + 1);
            for (feature*f = you->atomics[old_ns].begin; f != you->atomics[old_ns].end; ++f) {
              feature f2 = { (*f).x, (uint32_t)( ((*f).weight_index * neighbor_constant + enc_offset) & sch.priv->all->reg.weight_mask ) };
              me->atomics[neighbor_namespace].push_back(f2);
            }

            if (all->audit && (all->current_pass==0)) {
              assert(you->atomics[old_ns].size() == you->audit_features[old_ns].size());
              for (audit_data*f = you->audit_features[old_ns].begin; f != you->audit_features[old_ns].end; ++f) {
                uint32_t wi = (uint32_t)((*f).weight_index * neighbor_constant + enc_offset) & sch.priv->all->reg.weight_mask;
                audit_data f2 = { NULL, NULL, wi, f->x, true };

                f2.space = (char*) calloc_or_die(neighbor_feature_space.length()+1, sizeof(char));
                strcpy(f2.space, neighbor_feature_space.c_str());

                f2.feature = (char*) calloc_or_die( strlen(f->feature) + 6, sizeof(char) );
                f2.feature[0] = '@';
                f2.feature[1] = (offset > 0) ? '+' : '-';
                f2.feature[2] = (char)(abs(offset) + '0');
                f2.feature[3] = (char)old_ns;
                f2.feature[4] = '=';
                strcpy(f2.feature+5, f->feature);

                me->audit_features[neighbor_namespace].push_back(f2);
              }

            }
            me->sum_feat_sq[neighbor_namespace] += you->sum_feat_sq[old_ns];
            me->total_sum_feat_sq += you->sum_feat_sq[old_ns];
            me->num_features += you_size;
          }
        } else if ((n + offset == -1) || (n + offset == (int32_t)sch.priv->ec_seq.size())) { // handle <s> and </s>
          size_t bias  = constant * ((n + offset < 0) ? 2 : 3);
          uint32_t fid = ((uint32_t)(( bias * neighbor_constant + enc_offset))) & sch.priv->all->reg.weight_mask;

          if (me->atomics[neighbor_namespace].size() == 0)
            me->indices.push_back(neighbor_namespace);

          feature f = { 1., fid };
          me->atomics[neighbor_namespace].push_back(f);

          if (all->audit && (all->current_pass==0)) {
            audit_data f2 = { NULL, NULL, fid, 1., true };

            f2.space = (char*) calloc_or_die(neighbor_feature_space.length()+1, sizeof(char));
            strcpy(f2.space, neighbor_feature_space.c_str());

            f2.feature = (char*) calloc_or_die(4, sizeof(char) );
            f2.feature[0] = 'b';
            f2.feature[1] = '@';
            f2.feature[2] = (offset > 0) ? '+' : '-';
            f2.feature[3] = 0;

            me->audit_features[neighbor_namespace].push_back(f2);
          }

          me->sum_feat_sq[neighbor_namespace] += 1.;
          me->total_sum_feat_sq += 1.;
          me->num_features += 1;
        }
      }
    }
  }

  void del_neighbor_features(search& sch) {
    if (sch.priv->neighbor_features.size() == 0) return;
    vw*all = sch.priv->all;

    for (int32_t n=0; n<(int32_t)sch.priv->ec_seq.size(); n++) {
      example*me = sch.priv->ec_seq[n];
      size_t total_size = 0;
      float total_sfs = 0.;

      for (int32_t*enc=sch.priv->neighbor_features.begin; enc!=sch.priv->neighbor_features.end; ++enc) {
        int32_t offset = (*enc) >> 24;
        size_t  old_ns = (*enc) & 0xFF;

        if ((n + offset >= 0) && (n + offset < (int32_t)sch.priv->ec_seq.size())) { // we're okay on position
          example*you = sch.priv->ec_seq[n+offset];
          total_size += you->atomics[old_ns].size();
          total_sfs  += you->sum_feat_sq[old_ns];
        } else if ((n + offset == -1) || (n + offset == (int32_t)sch.priv->ec_seq.size())) {
          total_size += 1;
          total_sfs += 1;
        }
      }

      if (total_size > 0) {
        if (me->atomics[neighbor_namespace].size() == total_size) {
          char last_idx = me->indices.pop();
          if (last_idx != (char)neighbor_namespace) {
            cerr << "error: some namespace was added after the neighbor namespace" << endl;
            throw exception();
          }
          cdbg << "erasing new ns '" << (char)neighbor_namespace << "' of size " << me->atomics[neighbor_namespace].size() << endl;
          me->atomics[neighbor_namespace].erase();
        } else {
          cerr << "warning: neighbor namespace seems to be the wrong size? (total_size=" << total_size << " but ns.size=" << me->atomics[neighbor_namespace].size() << ")" << endl;
          assert(false);
          me->atomics[neighbor_namespace].end -= total_size;
          cdbg << "erasing " << total_size << " features" << endl;
        }

        if (all->audit && (all->current_pass == 0)) {
          assert(total_size == me->audit_features[neighbor_namespace].size());

          for (audit_data*ad = me->audit_features[neighbor_namespace].begin; ad != me->audit_features[neighbor_namespace].end; ++ad)
            if (ad->alloced) {
              free(ad->space);
              free(ad->feature);
            }

          me->audit_features[neighbor_namespace].end -= total_size;
        }

        me->sum_feat_sq[neighbor_namespace] -= total_sfs;
        me->total_sum_feat_sq -= total_sfs;
        me->num_features -= total_size;
      } else {
        // TODO: add dummy features for <s> or </s>
      }
    }
  }

  void reset_search_structure(search_private& priv) {
    // NOTE: make sure do NOT reset priv.learn_a_idx
    priv.t = 0;
    priv.loss_declared = false;
    priv.done_with_all_actions = false;
    priv.test_loss = 0.;
    priv.learn_loss = 0.;
    priv.train_loss = 0.;
    priv.num_features = 0;
    priv.should_produce_string = false;
    if (priv.adaptive_beta)
      priv.beta = 1.f - powf(1.f - priv.alpha, (float)priv.total_examples_generated);
    priv.ptag_to_action.erase();
  }

  void search_declare_loss(search_private& priv, float loss) {
    priv.loss_declared = true;
    switch (priv.state) {
      case INIT_TEST:  priv.test_loss  += loss; break;
      case INIT_TRAIN: priv.train_loss += loss; break;
      case LEARN:      priv.learn_loss += loss; break;
      default: break; // get rid of the warning about missing cases (danger!)
    }
  }

  size_t random(size_t max) { return (size_t)(frand48() * (float)max); }
  
  action choose_oracle_action(search_private& priv, size_t ec_cnt, const action* oracle_actions, size_t oracle_actions_cnt, const action* allowed_actions, size_t allowed_actions_cnt) {
    return ( oracle_actions_cnt > 0) ?  oracle_actions[random(oracle_actions_cnt )] :
           (allowed_actions_cnt > 0) ? allowed_actions[random(allowed_actions_cnt)] :
           random(ec_cnt);
  }

  void add_conditioning_feature(search_private& priv, const uint32_t idx, const float val, float& cur_weight) {
    //bool found = priv.ec_conditioning_audit_ss.str().find("n=") != string::npos;
    //if (!found) return;
    
    size_t wpp  = priv.all->wpp << priv.all->reg.stride_shift;
    size_t mask = priv.all->reg.weight_mask;
    feature f = { val * priv.acset.feature_value,
                  (uint32_t) (((priv.ec_conditioning_idx * quadratic_constant + idx) * wpp) & mask) };
    priv.ec_conditioning_ec->atomics[conditioning_namespace].push_back(f);

    if (priv.all->audit) {
      audit_data a = { NULL, NULL, f.weight_index, f.x, true };
      a.space   = (char*)calloc_or_die(condition_feature_space.length()+1, sizeof(char));
      a.feature = (char*)calloc_or_die(priv.ec_conditioning_audit_ss.str().length() + 32, sizeof(char));
      strcpy(a.space, condition_feature_space.c_str());
      int num = sprintf(a.feature, "fid=%lu_", (idx & mask) >> priv.all->reg.stride_shift);
      strcpy(a.feature+num, priv.ec_conditioning_audit_ss.str().c_str());
      priv.ec_conditioning_ec->audit_features[conditioning_namespace].push_back(a);
    }
  }
  
  void add_example_conditioning(search_private& priv, example& ec, const ptag* condition_on, size_t condition_on_cnt, const char* condition_on_names, const action* condition_on_actions) {
    float _ignored = 0.;
    // if override_values is non-null then we use those as the values for condition_on, rather than
    // the ones stored in priv. this is used by generate_training_example because it needs to
    // "remember" the old actions, in case some have been overwritten
    if (condition_on_cnt == 0) return;

    size_t I = condition_on_cnt;
    size_t N = max(priv.acset.max_bias_ngram_length, priv.acset.max_quad_ngram_length);
    for (size_t i=0; i<I; i++) { // position in conditioning
      uint32_t fid = 71933;
      if (priv.all->audit) {
        priv.ec_conditioning_audit_ss.str("");
        priv.ec_conditioning_audit_ss.clear();
      }

      for (size_t n=0; n<N; n++) { // length of ngram
        if (i + n >= I) break; // no more ngrams
        // we're going to add features for the ngram condition_on_actions[i .. i+N]
        char name = condition_on_names ? condition_on_names[i+n] : i;
        fid = fid * 328901 + 71933 * ((condition_on_actions[i+n] + 349101) * (name + 38490137));

        priv.ec_conditioning_ec  = &ec;
        priv.ec_conditioning_idx = fid;

        if (priv.all->audit) {
          if (n > 0) priv.ec_conditioning_audit_ss << ',';
          if ((33 <= name) && (name <= 126)) priv.ec_conditioning_audit_ss << name;
          else priv.ec_conditioning_audit_ss << '#' << (int)name;
          priv.ec_conditioning_audit_ss << '=' << condition_on_actions[i+n];
        }
        
        // add the single bias feature
        if (n < priv.acset.max_bias_ngram_length)
          add_conditioning_feature(priv, 1, 1., _ignored);

        // add the quadratic features
        if (n < priv.acset.max_quad_ngram_length)
          GD::foreach_feature<search_private,add_conditioning_feature>(*priv.all, ec, priv);
      }
    }

    ec.indices.push_back(conditioning_namespace);
    ec.sum_feat_sq[conditioning_namespace] = priv.acset.feature_value * priv.acset.feature_value * (float)ec.atomics[conditioning_namespace].size();
    ec.total_sum_feat_sq += ec.sum_feat_sq[conditioning_namespace];
    ec.num_features += ec.atomics[conditioning_namespace].size();
  }
  
  void del_example_conditioning(search_private& priv, example& ec) {
    if ((ec.indices.size() == 0) || (ec.indices.last() != conditioning_namespace)) throw exception();
    ec.num_features -= ec.atomics[conditioning_namespace].size();
    ec.total_sum_feat_sq -= ec.sum_feat_sq[conditioning_namespace];
    ec.sum_feat_sq[conditioning_namespace] = 0;
    ec.indices.decr();
    ec.atomics[conditioning_namespace].erase();
    if (priv.all->audit) {
      for (size_t i=0; i<ec.audit_features[conditioning_namespace].size(); i++)
        if (ec.audit_features[conditioning_namespace][i].alloced) {
          free(ec.audit_features[conditioning_namespace][i].space);
          free(ec.audit_features[conditioning_namespace][i].feature);
        }
      ec.audit_features[conditioning_namespace].erase();
    }
  }

  uint32_t cs_get_prediction(bool isCB, void* ld) {
    return isCB ? ((CB::label*)ld)->prediction
                : ((CS::label*)ld)->prediction;
  }
  
  size_t cs_get_costs_size(bool isCB, void* ld) {
    return isCB ? ((CB::label*)ld)->costs.size()
                : ((CS::label*)ld)->costs.size();
  }

  uint32_t cs_get_cost_index(bool isCB, void* ld, size_t k) {
    return isCB ? ((CB::label*)ld)->costs[k].action
                : ((CS::label*)ld)->costs[k].class_index;
  }

  uint32_t cs_get_cost_partial_prediction(bool isCB, void* ld, size_t k) {
    return isCB ? ((CB::label*)ld)->costs[k].partial_prediction
                : ((CS::label*)ld)->costs[k].partial_prediction;
  }

  void cs_costs_erase(bool isCB, void* ld) {
    if (isCB) ((CB::label*)ld)->costs.erase();
    else      ((CS::label*)ld)->costs.erase();
  }

  void cs_costs_resize(bool isCB, void* ld, size_t new_size) {
    if (isCB) ((CB::label*)ld)->costs.resize(new_size);
    else      ((CS::label*)ld)->costs.resize(new_size);
  }

  void cs_cost_push_back(bool isCB, void* ld, uint32_t index, float value) {
    if (isCB) { CB::cb_class cost = { value, index, 0., 0. }; ((CB::label*)ld)->costs.push_back(cost); }
    else      { CS::wclass   cost = { value, index, 0., 0. }; ((CS::label*)ld)->costs.push_back(cost); }
  }
  
  void* allowed_actions_to_ld(search_private& priv, size_t ec_cnt, const action* allowed_actions, size_t allowed_actions_cnt) {
    bool isCB = priv.cb_learner;
    void* ld  = priv.allowed_actions_cache;
    size_t num_costs = cs_get_costs_size(isCB, ld);

    if (priv.is_ldf) {  // LDF version easier
      if (num_costs > ec_cnt)
        cs_costs_resize(isCB, ld, ec_cnt);
      else if (num_costs < ec_cnt)
        for (action k = num_costs; k < ec_cnt; k++)
          cs_cost_push_back(isCB, ld, k, FLT_MAX);
      
    } else { // non-LDF version
      if ((allowed_actions == NULL) || (allowed_actions_cnt == 0)) { // any action is allowed
        if (num_costs != priv.A) {  // if there are already A-many actions, they must be the right ones, unless the user did something stupid like putting duplicate allowed_actions...
          cs_costs_erase(isCB, ld);
          for (action k = 0; k < priv.A; k++)
            cs_cost_push_back(isCB, ld, k+1, FLT_MAX);  //+1 because MC is 1-based
        }
      } else { // we need to peek at allowed_actions
        cs_costs_erase(isCB, ld);
        for (size_t i = 0; i < allowed_actions_cnt; i++)
          cs_cost_push_back(isCB, ld, allowed_actions[i], FLT_MAX);
      }
    }

    return ld;
  }
  
  action single_prediction_notLDF(search_private& priv, example& ec, int policy, const action* allowed_actions, size_t allowed_actions_cnt) {
    vw& all = *priv.all;
    
    void* old_label = ec.ld;
    ec.ld = allowed_actions_to_ld(priv, 1, allowed_actions, allowed_actions_cnt);
    priv.base_learner->predict(ec, policy);
    uint32_t act = cs_get_prediction(priv.cb_learner, ec.ld);

    // generate raw predictions if necessary
    if ((priv.state == INIT_TEST) && (all.raw_prediction > 0)) {
      string outputString; // TODO put this in the structure somewhere for speed's sake
      stringstream outputStringStream(outputString);
      for (size_t k = 0; k < cs_get_costs_size(priv.cb_learner, ec.ld); k++) {
        if (k > 0) outputStringStream << ' ';
        outputStringStream << cs_get_cost_index(priv.cb_learner, ec.ld, k) << ':' << cs_get_cost_partial_prediction(priv.cb_learner, ec.ld, k);
      }
      all.print_text(all.raw_prediction, outputStringStream.str(), ec.tag);
    }
    
    ec.ld = old_label;

    priv.total_predictions_made++;
    priv.num_features += ec.num_features;

    return act;
  }

  action single_prediction_LDF(search_private& priv, example* ecs, size_t ec_cnt, int policy) {
    CS::label test_label; CS::cs_label.default_label(&test_label);    // TODO: move to structure

    // keep track of best (aka chosen) action
    float  best_prediction = 0.;
    action best_action = 0;

    for (action a=0; a<ec_cnt; a++) {
      void* old_label = ecs[a].ld;
      ecs[a].ld = &test_label;
      priv.base_learner->predict(ecs[a], policy);
      ecs[a].ld = old_label;

      priv.empty_example->in_use = true;
      priv.base_learner->predict(*priv.empty_example);

      if ((a == 0) || (ecs[a].partial_prediction < best_prediction)) {
        best_prediction = ecs[a].partial_prediction;
        best_action     = a;
      }
      
      priv.num_features += ecs[a].num_features;
    }
    
    priv.total_predictions_made++;
    return best_action;
  }

  int choose_policy(search_private& priv) {
    RollMethod method = (priv.state == INIT_TEST ) ? POLICY :
                        (priv.state == LEARN     ) ? priv.rollout_method :
                        (priv.state == INIT_TRAIN) ? priv.rollin_method :
                        NO_ROLLOUT;   // this should never happen
    switch (method) {
      case POLICY:
        return random_policy(priv, priv.allow_current_policy || (priv.state == INIT_TEST), false);

      case ORACLE:
        return -1;

      case MIX_PER_STATE:
        return random_policy(priv, priv.allow_current_policy, true);

      case MIX_PER_ROLL:
        if (priv.mix_per_roll_policy == -2) // then we have to choose one!
          priv.mix_per_roll_policy = random_policy(priv, priv.allow_current_policy, true);
        return priv.mix_per_roll_policy;

      case NO_ROLLOUT:
        // TODO: handle this case correctly! for now we just revert to policy
        return -1;
    }
  }

  template<class T>
  void ensure_size(v_array<T>& A, size_t sz) {
    if (A.end_array - A.begin < sz) A.resize(sz*2+1, true);
    else A.end = A.begin + sz;
  }


  template<class T> void push_at(v_array<T>& v, T item, size_t pos) {
    if (v.size() > pos)
      v.begin[pos] = item;
    else {
      if (v.end_array > v.begin + pos) {
        // there's enough memory, just not enough filler
        v.begin[pos] = item;
        v.end = v.begin + pos + 1;
      } else {
        // there's not enough memory
        v.resize(2 * pos + 3, true);
        v.begin[pos] = item;
        v.end = v.begin + pos + 1;
      }
    }
  }
  
  void record_action(search_private& priv, ptag mytag, action a) {
    if (mytag == 0) return;
    push_at(priv.ptag_to_action, a, mytag);
  }
  

  bool cached_item_equivalent(unsigned char*& A, unsigned char*& B) {
    size_t sz_A = *A;
    size_t sz_B = *B;
    if (sz_A != sz_B) return false;
    return memcmp(A, B, sz_A) == 0;
  }

  void free_key(unsigned char* mem, action a) { free(mem); }
  void clear_cache_hash_map(search_private& priv) {
    priv.cache_hash_map.iter(free_key);
    priv.cache_hash_map.clear();
  }
  
  // returns true if found and do_store is false. if do_store is true, always returns true.
  bool cached_action_store_or_find(search_private& priv, ptag mytag, const ptag* condition_on, const char* condition_on_names, const action* condition_on_actions, size_t condition_on_cnt, int policy, action &a, bool do_store) {
    if (mytag == 0) return do_store; // don't attempt to cache when tag is zero
    for (size_t i=0; i<condition_on_cnt; i++)
      if (condition_on[i] == 0) return do_store; // don't attempt to cache when conditioning on a zero tag

    size_t sz  = sizeof(size_t) + sizeof(ptag) + sizeof(int) + sizeof(size_t) + condition_on_cnt * (sizeof(ptag) + sizeof(action) + sizeof(char));
    if (sz % 4 != 0) sz = 4 * (sz / 4 + 1); // make sure sz aligns to 4 so that uniform_hash does the right thing

    unsigned char* item = (unsigned char*)calloc(sz, 1);
    unsigned char* here = item;
    *here = sz;                here += sizeof(size_t);
    *here = mytag;             here += sizeof(ptag);
    *here = policy;            here += sizeof(int);
    *here = condition_on_cnt;  here += sizeof(size_t);
    for (size_t i=0; i<condition_on_cnt; i++) {
      *here = condition_on[i];         here += sizeof(ptag);
      *here = condition_on_actions[i]; here += sizeof(action);
      *here = condition_on_names[i];   here += sizeof(char);  // TODO: should we align this at 4?
    }
    uint32_t hash = uniform_hash(item, sz, 3419);

    if (do_store) {
      priv.cache_hash_map.put(item, hash, a);
      return true;
    } else { // its a find
      a = priv.cache_hash_map.get(item, hash);
      free(item);
      return a != (action)-1;
    }
  }
  
  // note: ec_cnt should be 1 if we are not LDF
  action search_predict(search_private& priv, example* ecs, size_t ec_cnt, ptag mytag, const action* oracle_actions, size_t oracle_actions_cnt, const ptag* condition_on, const char* condition_on_names, const action* allowed_actions, size_t allowed_actions_cnt, size_t learner_id) {
    size_t condition_on_cnt = condition_on_names ? strlen(condition_on_names) : 0;
    size_t t = priv.t;
    priv.t++;

    // make sure parameters come in pairs correctly
    assert((oracle_actions  == NULL) == (oracle_actions_cnt  == 0));
    assert((condition_on    == NULL) == (condition_on_names  == NULL));
    assert((allowed_actions == NULL) == (allowed_actions_cnt == 0));
    
    // if we're just after the string, choose an oracle action
    if (priv.state == GET_TRUTH_STRING)
      return choose_oracle_action(priv, ec_cnt, oracle_actions, oracle_actions_cnt, allowed_actions, allowed_actions_cnt);

    // if we're in LEARN mode and before learn_t, return the train action
    if ((priv.state == LEARN) && (t < priv.learn_t)) {
      assert(t < priv.train_trajectory.size());
      return priv.train_trajectory[t];
    }

    // for LDF, # of valid actions is ec_cnt; otherwise it's either allowed_actions_cnt or A
    size_t valid_action_cnt = priv.is_ldf ? ec_cnt :
                              (allowed_actions_cnt > 0) ? allowed_actions_cnt : priv.A;
    
    // if we're in LEARN mode and _at_ learn_t, then:
    //   - choose the next action
    //   - decide if we're done
    //   - if we are, then copy/mark the example ref
    if ((priv.state == LEARN) && (t == priv.learn_t)) {
      action a = priv.learn_a_idx;
      priv.learn_a_idx++;

      // check to see if we're done with available actions
      if (priv.learn_a_idx >= valid_action_cnt) {
        priv.done_with_all_actions = true;

        // set reference or copy example(s)
        priv.learn_ec_ref_cnt = ec_cnt;
        if (priv.examples_dont_change)
          priv.learn_ec_ref = ecs;
        else {
          size_t label_size = priv.is_ldf ? sizeof(CS::label) : sizeof(MC::multiclass);
          void (*label_copy_fn)(void*&,void*) = priv.is_ldf ? CS::cs_label.copy_label : NULL;
          
          ensure_size(priv.learn_ec_copy, ec_cnt);
          // current bug: we need to make sure ld is valid so we can copy to it, but the memset above might lead to leaks too
          for (size_t i=0; i<ec_cnt; i++) {
            if (!priv.learn_ec_copy[i].ld)
              priv.learn_ec_copy[i].ld = calloc_or_die(1, label_size);
            VW::copy_example_data(priv.all->audit, priv.learn_ec_copy.begin+i, ecs+i, label_size, label_copy_fn);
          }

          priv.learn_ec_ref = priv.learn_ec_copy.begin;
        }

        // copy conditioning stuff and allowed actions
        if (priv.auto_condition_features) {
          ensure_size(priv.learn_condition_on,     condition_on_cnt);
          ensure_size(priv.learn_condition_on_act, condition_on_cnt);

          priv.learn_condition_on.end = priv.learn_condition_on.begin + condition_on_cnt;   // allow .size() to be used in lieu of _cnt

          memcpy(priv.learn_condition_on.begin, condition_on, condition_on_cnt * sizeof(ptag));
          
          for (size_t i=0; i<condition_on_cnt; i++)
            push_at(priv.learn_condition_on_act, ((1 <= condition_on[i]) && (condition_on[i] < priv.ptag_to_action.size())) ? priv.ptag_to_action[condition_on[i]] : 0, i);

          // TODO: make this faster
          free(priv.learn_condition_on_names);
          priv.learn_condition_on_names = NULL;
          if (condition_on_names != 0) {
            priv.learn_condition_on_names = (char*)calloc_or_die(strlen(condition_on_names)+1, sizeof(char));
            strcpy(priv.learn_condition_on_names, condition_on_names);
          }
        }

        ensure_size(priv.learn_allowed_actions, allowed_actions_cnt);
        memcpy(priv.learn_allowed_actions.begin, allowed_actions, allowed_actions_cnt);
      }

      assert((allowed_actions_cnt == 0) || (a < allowed_actions_cnt));
      return (allowed_actions_cnt > 0) ? allowed_actions[a] : (a+1);
    }
    
    if ((priv.state == INIT_TRAIN) ||
        (priv.state == INIT_TEST) ||
        ((priv.state == LEARN) && (t > priv.learn_t))) {
      // we actually need to run the policy
      
      int policy = choose_policy(priv);
      action a;

      if (policy == -1)
        a = choose_oracle_action(priv, ec_cnt, oracle_actions, oracle_actions_cnt, allowed_actions, allowed_actions_cnt);
      else {
        // if we're caching, we need to know what we're conditioning on
        // TODO: if caching is turned off, and there's no auto-conditioning, we can skip this step
        action* condition_on_actions = (action*)calloc_or_die(condition_on_cnt, sizeof(action));
        for (size_t i=0; i<condition_on_cnt; i++)
          if ((1 <= condition_on[i]) && (condition_on[i] < priv.ptag_to_action.size()))
            condition_on_actions[i] = priv.ptag_to_action[condition_on[i]];

        // TODO: test equiv, which means we must store size for memcmp!
        if (cached_action_store_or_find(priv, mytag, condition_on, condition_on_names, condition_on_actions, condition_on_cnt, policy, a, false))
          // if this succeeded, 'a' has the right action
          priv.total_cache_hits++;
        else { // we need to predict, and then cache
          if (priv.auto_condition_features)
            for (size_t n=0; n<ec_cnt; n++)
              add_example_conditioning(priv, ecs[n], condition_on, condition_on_cnt, condition_on_names, condition_on_actions);
        
          a = priv.is_ldf
              ? single_prediction_LDF(priv, ecs, ec_cnt, policy)
              : single_prediction_notLDF(priv, *ecs, policy, allowed_actions, allowed_actions_cnt);

          if (priv.auto_condition_features)
            for (size_t n=0; n<ec_cnt; n++)
              del_example_conditioning(priv, ecs[n]);

          cached_action_store_or_find(priv, mytag, condition_on, condition_on_names, condition_on_actions, condition_on_cnt, policy, a, true);
        }

        free(condition_on_actions);
      }

      if (priv.state == INIT_TRAIN)
        priv.train_trajectory.push_back(a); // note the action for future reference
      
      return a;
    }

    cerr << "error: predict called in unknown state" << endl;
    throw exception();
  }
  
  inline bool cmp_size_t(const size_t a, const size_t b) { return a < b; }
  v_array<size_t> get_training_timesteps(search_private& priv) {
    v_array<size_t> timesteps;
    
    // if there's no subsampling to do, just return [0,T)
    if (priv.subsample_timesteps <= 0)
      for (size_t t=0; t<priv.T; t++)
        timesteps.push_back(t);

    // if subsample in (0,1) then pick steps with that probability, but ensuring there's at least one!
    else if (priv.subsample_timesteps < 1) {
      for (size_t t=0; t<priv.T; t++)
        if (frand48() <= priv.subsample_timesteps)
          timesteps.push_back(t);

      if (timesteps.size() == 0) // ensure at least one
        timesteps.push_back((size_t)(frand48() * priv.T));
    }

    // finally, if subsample >= 1, then pick (int) that many uniformly at random without replacement; could use an LFSR but why? :P
    else {
      while ((timesteps.size() < (size_t)priv.subsample_timesteps) &&
             (timesteps.size() < priv.T)) {
        size_t t = (size_t)(frand48() * (float)priv.T);
        if (! v_array_contains(timesteps, t))
          timesteps.push_back(t);
      }
      std::sort(timesteps.begin, timesteps.end, cmp_size_t);
    }

    return timesteps;
  }

  void generate_training_example(search_private& priv, v_array<float>& losses) {
    // TODO: should we really subtract out min-loss?
    float min_loss = FLT_MAX;
    for (size_t i=0; i<losses.size(); i++)
      if (losses[i] < min_loss) min_loss = losses[i];

    void* labels = allowed_actions_to_ld(priv, priv.learn_ec_ref_cnt, priv.learn_allowed_actions.begin, priv.learn_allowed_actions.size());
    for (size_t i=0; i<losses.size(); i++)
      if (priv.cb_learner)   ((CB::label*)labels)->costs[i].cost = losses[i] - min_loss;
      else                   ((CS::label*)labels)->costs[i].x    = losses[i] - min_loss;

    if (!priv.is_ldf) {   // not LDF
      // since we're not LDF, it should be the case that ec_ref_cnt == 1
      // and learn_ec_ref[0] is a pointer to a single example
      assert(priv.learn_ec_ref_cnt == 1);
      assert(priv.learn_ec_ref != NULL);

      // replace the label, add conditioning, and learn!
      int learner = select_learner(priv, priv.current_policy, priv.learn_learner_id);
      example& ec = priv.learn_ec_ref[0];
      void* old_label = ec.ld;
      ec.ld = labels;
      ec.in_use = true;
      add_example_conditioning(priv, ec, priv.learn_condition_on.begin, priv.learn_condition_on.size(), priv.learn_condition_on_names, priv.learn_condition_on_act.begin);
      priv.base_learner->learn(ec, learner);
      del_example_conditioning(priv, ec);
      ec.ld = old_label;
      priv.total_examples_generated++;
    } else {              // is  LDF
      // TODO
      throw exception();
    }
  }

  template<class T> void cdbg_print_array(string str, v_array<T> A) {
    cdbg << str << " = [";
    for (size_t i=0; i<A.size(); i++) cdbg << " " << A[i];
    cdbg << " ]" << endl;
  }
  
  template <bool is_learn>
  void train_single_example(search& sch) {
    search_private& priv = *sch.priv;
    vw&all = *priv.all;
    bool ran_test = false;  // we must keep track so that even if we skip test, we still update # of examples seen
    // do an initial test pass to compute output (and loss)

    clear_cache_hash_map(priv);
    
    if (must_run_test(all, priv.ec_seq)) {
      cdbg << "======================================== INIT TEST (" << priv.current_policy << "," << priv.read_example_last_pass << ") ========================================" << endl;

      ran_test = true;
      reset_search_structure(priv);
      priv.state = INIT_TEST;
      priv.should_produce_string = might_print_update(all) || (all.final_prediction_sink.size() > 0) || (all.raw_prediction > 0);
      priv.pred_string->str("");

      // do the prediction
      priv.task->run(sch, priv.ec_seq);

      // accumulate loss
      if (priv.ec_seq[0]->test_only) {
        all.sd->weighted_holdout_examples += 1.f;//test weight seen
        all.sd->weighted_holdout_examples_since_last_dump += 1.f;
        all.sd->weighted_holdout_examples_since_last_pass += 1.f;
        all.sd->holdout_sum_loss += priv.test_loss;
        all.sd->holdout_sum_loss_since_last_dump += priv.test_loss;
        all.sd->holdout_sum_loss_since_last_pass += priv.test_loss;//since last pass
      } else {
        all.sd->weighted_examples += 1.f;
        all.sd->total_features += priv.num_features;
        all.sd->sum_loss += priv.test_loss;
        all.sd->sum_loss_since_last_dump += priv.test_loss;
        all.sd->example_number++;
      }
      
      // generate output
      for (int* sink = all.final_prediction_sink.begin; sink != all.final_prediction_sink.end; ++sink)
        all.print_text((int)*sink, priv.pred_string->str(), priv.ec_seq[0]->tag);

      if (all.raw_prediction > 0) // TODO: this used to check that we weren't CB... why?
        all.print_text(all.raw_prediction, "", priv.ec_seq[0]->tag);
    }

    // if we're not training, then we're done!
    if ((!is_learn) || priv.ec_seq[0]->test_only)
      return;

    // TODO: if the oracle was never called, we can skip this!
    
    // do a pass over the data allowing oracle
    cdbg << "======================================== INIT TRAIN (" << priv.current_policy << "," << priv.read_example_last_pass << ") ========================================" << endl;
    reset_search_structure(priv);
    priv.state = INIT_TRAIN;
    priv.train_trajectory.erase();  // this is where we'll store the training sequence
    priv.task->run(sch, priv.ec_seq);

    if (!ran_test && !priv.ec_seq[0]->test_only) {
      all.sd->weighted_examples += 1.f;
      all.sd->total_features += priv.num_features;
      all.sd->sum_loss += priv.test_loss;
      all.sd->sum_loss_since_last_dump += priv.test_loss;
      all.sd->example_number++;
    }
    
    // if there's nothing to train on, we're done!
    if ((!priv.loss_declared) || (priv.t == 0))
      return;

    // TODO: special case NO_ROLLOUT so that we don't have to do tons of work
    
    // otherwise, we have some learn'in to do!
    cdbg << "======================================== LEARN (" << priv.current_policy << "," << priv.read_example_last_pass << ") ========================================" << endl;
    priv.T = priv.t;
    v_array<size_t> timesteps = get_training_timesteps(priv);  // TODO: avoid memory allocation
    v_array<float>  learn_losses; // TODO: avoid memory allocation
    cdbg_print_array("timesteps", timesteps);
    for (size_t tid=0; tid<timesteps.size(); tid++) {
      priv.learn_a_idx = 0;
      priv.done_with_all_actions = false;
      // for each action, roll out to get a loss
      while (! priv.done_with_all_actions) {
        reset_search_structure(priv); // should set mix_per_roll_policy to -2
        priv.state = LEARN;
        priv.learn_t = timesteps[tid];
        cdbg << "learn_t = " << priv.learn_t << ", learn_a_idx = " << priv.learn_a_idx << endl;
        priv.task->run(sch, priv.ec_seq);
        learn_losses.push_back( priv.learn_loss );  // TODO: should we just put this in a CS structure from the get-go?
        cdbg_print_array("learn_losses", learn_losses);
      }
      // now we can make a training example
      generate_training_example(priv, learn_losses);
      if (! priv.examples_dont_change)
        for (size_t n=0; n<priv.learn_ec_copy.size(); n++) {
          if (sch.priv->is_ldf) CS::cs_label.delete_label(priv.learn_ec_copy[n].ld);
          else                  MC::mc_label.delete_label(priv.learn_ec_copy[n].ld);
        }
      learn_losses.erase();
    }
    learn_losses.delete_v();
    timesteps.delete_v();
  }
    
  
  template <bool is_learn>
  void do_actual_learning(vw&all, search& sch) {
    search_private& priv = *sch.priv;
    if (priv.ec_seq.size() == 0)
      return;  // nothing to do :)

    if (priv.task->run_setup) priv.task->run_setup(sch, priv.ec_seq);
    
    // if we're going to have to print to the screen, generate the "truth" string
    cdbg << "======================================== GET TRUTH STRING (" << priv.current_policy << "," << priv.read_example_last_pass << ") ========================================" << endl;
    if (might_print_update(all)) {
      reset_search_structure(*sch.priv);
      priv.state = GET_TRUTH_STRING;
      priv.should_produce_string = true;
      priv.truth_string->str("");
      priv.task->run(sch, priv.ec_seq);
    }

    add_neighbor_features(sch);
    train_single_example<is_learn>(sch);
    del_neighbor_features(sch);

    if (priv.task->run_takedown) priv.task->run_takedown(sch, priv.ec_seq);
  }

  template <bool is_learn>
  void search_predict_or_learn(search& sch, learner& base, example& ec) {
    search_private& priv = *sch.priv;
    vw* all = priv.all;
    priv.base_learner = &base;
    bool is_real_example = true;

    cdbg << "search_predict_or_learn, einl=" << example_is_newline(ec) << endl;
    
    if (example_is_newline(ec) || priv.ec_seq.size() >= all->p->ring_size - 2) {
      if (priv.ec_seq.size() >= all->p->ring_size - 2) // -2 to give some wiggle room
        std::cerr << "warning: length of sequence at " << ec.example_counter << " exceeds ring size; breaking apart" << std::endl;

      do_actual_learning<is_learn>(*all, sch);

      priv.hit_new_pass = false;
      priv.last_example_was_newline = true;
      is_real_example = false;
    } else {
      if (priv.last_example_was_newline)
        priv.ec_seq.clear();
      priv.ec_seq.push_back(&ec);
      priv.last_example_was_newline = false;
    }

    if (is_real_example)
      priv.read_example_last_id = ec.example_counter;
  }

  void end_pass(search& sch) {
    search_private& priv = *sch.priv;
    vw* all = priv.all;
    priv.hit_new_pass = true;
    priv.read_example_last_pass++;
    priv.passes_since_new_policy++;

    if (priv.passes_since_new_policy >= priv.passes_per_policy) {
      priv.passes_since_new_policy = 0;
      if(all->training)
        priv.current_policy++;
      if (priv.current_policy > priv.total_number_of_policies) {
        std::cerr << "internal error (bug): too many policies; not advancing" << std::endl;
        priv.current_policy = priv.total_number_of_policies;
      }
      //reset search_trained_nb_policies in options_from_file so it is saved to regressor file later
      std::stringstream ss;
      ss << priv.current_policy;
      VW::cmd_string_replace_value(all->file_options,"--search_trained_nb_policies", ss.str());
    }
  }

  void finish_example(vw& all, search& sch, example& ec) {
    if (ec.end_pass || example_is_newline(ec) || sch.priv->ec_seq.size() >= all.p->ring_size - 2) {
      print_update(*sch.priv);
      VW::finish_example(all, &ec);
      clear_seq(all, *sch.priv);
    }
  }

  void end_examples(search& sch) {
    search_private& priv = *sch.priv;
    vw* all    = priv.all;

    do_actual_learning<true>(*all, sch);

    if( all->training ) {
      std::stringstream ss1;
      std::stringstream ss2;
      ss1 << ((priv.passes_since_new_policy == 0) ? priv.current_policy : (priv.current_policy+1));
      //use cmd_string_replace_value in case we already loaded a predictor which had a value stored for --search_trained_nb_policies
      VW::cmd_string_replace_value(all->file_options,"--search_trained_nb_policies", ss1.str());
      ss2 << priv.total_number_of_policies;
      //use cmd_string_replace_value in case we already loaded a predictor which had a value stored for --search_total_nb_policies
      VW::cmd_string_replace_value(all->file_options,"--search_total_nb_policies", ss2.str());
    }
  }
  
  void search_initialize(vw* all, search& sch) {
    search_private& priv = *sch.priv;
    priv.all = all;
    
    priv.auto_condition_features = false;
    priv.auto_hamming_loss = false;
    priv.examples_dont_change = false;
    priv.is_ldf = false;

    priv.A = 1;
    priv.num_learners = 1;
    priv.cb_learner = false;
    priv.state = INITIALIZE;
    priv.learn_learner_id = 0;
    priv.mix_per_roll_policy = -2;

    priv.t = 0;
    priv.T = 0;
    priv.learn_ec_ref = NULL;
    priv.learn_ec_ref_cnt = 0;
    priv.allowed_actions_cache = NULL;
    
    priv.loss_declared = false;
    priv.learn_t = 0;
    priv.learn_a_idx = 0;
    priv.done_with_all_actions = false;

    priv.test_loss = 0.;
    priv.learn_loss = 0.;
    priv.train_loss = 0.;
    priv.learn_condition_on_names = NULL;
    
    priv.last_example_was_newline = false;
    priv.hit_new_pass = false;

    priv.printed_output_header = false;

    priv.should_produce_string = false;
    priv.pred_string  = new stringstream();
    priv.truth_string = new stringstream();
    priv.bad_string_stream = new stringstream();
    priv.bad_string_stream->clear(priv.bad_string_stream->badbit);
        
    priv.beta = 0.5;
    priv.alpha = 1e-10f;
    priv.gamma = 0.5;

    priv.rollout_method = MIX_PER_ROLL;
    priv.rollin_method  = MIX_PER_ROLL;
    priv.subsample_timesteps = 0.;
    
    priv.allow_current_policy = true;
    priv.adaptive_beta = true;
    priv.passes_per_policy = 1;     //this should be set to the same value as --passes for dagger

    priv.current_policy = 0;

    priv.num_features = 0;
    priv.total_number_of_policies = 1;
    priv.read_example_last_id = 0;
    priv.passes_per_policy = 0;
    priv.read_example_last_pass = 0;
    priv.total_examples_generated = 0;
    priv.total_predictions_made = 0;
    priv.total_cache_hits = 0;
    
    priv.acset.max_bias_ngram_length = 1;
    priv.acset.max_quad_ngram_length = 0;
    priv.acset.feature_value = 1.;

    priv.cache_hash_map.set_default_value((action)-1);
    priv.cache_hash_map.set_equivalent(cached_item_equivalent);
    
    priv.task = NULL;
    sch.task_data = NULL;

    priv.empty_example = alloc_examples(sizeof(CS::label), 1);
    CS::cs_label.default_label(priv.empty_example->ld);
    priv.empty_example->in_use = true;
  }

  void search_finish(search& sch) {
    search_private& priv = *sch.priv;
    cdbg << "search_finish" << endl;

    clear_cache_hash_map(priv);

    delete priv.truth_string;
    delete priv.pred_string;
    delete priv.bad_string_stream;
    priv.neighbor_features.delete_v();

    if (priv.cb_learner) {
      ((CB::label*)priv.allowed_actions_cache)->costs.delete_v();
      delete (CB::label*)priv.allowed_actions_cache;
    } else {
      ((CS::label*)priv.allowed_actions_cache)->costs.delete_v();
      delete (CS::label*)priv.allowed_actions_cache;
    }

    priv.train_trajectory.delete_v();
    priv.ptag_to_action.delete_v();
    
    dealloc_example(CS::cs_label.delete_label, *(priv.empty_example));
    free(priv.empty_example);

    priv.ec_seq.clear();

    // destroy copied examples if we needed them
    if (! priv.examples_dont_change) {
      void (*delete_label)(void*) = priv.is_ldf ? CS::cs_label.delete_label : MC::mc_label.delete_label;
      for(example*ec = priv.learn_ec_copy.begin; ec!=priv.learn_ec_copy.end; ++ec)
        dealloc_example(delete_label, *ec);
      priv.learn_ec_copy.delete_v();
    }
    free(priv.learn_condition_on_names);
    priv.learn_condition_on.delete_v();
    priv.learn_condition_on_act.delete_v();
    
    if (priv.task->finish != NULL) {
      priv.task->finish(sch);
    }

    delete sch.priv;
  }

  void ensure_param(float &v, float lo, float hi, float def, const char* string) {
    if ((v < lo) || (v > hi)) {
      cerr << string << endl;
      v = def;
    }
  }

  bool string_equal(string a, string b) { return a.compare(b) == 0; }
  bool float_equal(float a, float b) { return fabs(a-b) < 1e-6; }
  bool uint32_equal(uint32_t a, uint32_t b) { return a==b; }
  bool size_equal(size_t a, size_t b) { return a==b; }

  template<class T> void check_option(T& ret, vw&all, po::variables_map& vm, const char* opt_name, bool default_to_cmdline, bool(*equal)(T,T), const char* mismatch_error_string, const char* required_error_string) {
    if (vm.count(opt_name)) {
      ret = vm[opt_name].as<T>();
      stringstream ss;
      ss << " --" << opt_name << " " << ret;
      all.file_options.append(ss.str());
    } else if (strlen(required_error_string)>0) {
      std::cerr << required_error_string << endl;
      if (! vm.count("help"))
        throw exception();
    }
  }

  void check_option(bool& ret, vw&all, po::variables_map& vm, const char* opt_name, bool default_to_cmdline, const char* mismatch_error_string) {
    if (vm.count(opt_name)) {
      ret = true;
      stringstream ss;
      ss << " --" << opt_name;
      all.file_options.append(ss.str());
    } else
      ret = false;
  }

  void handle_condition_options(vw& vw, auto_condition_settings& acset, po::variables_map& vm) {
    po::options_description condition_options("Search Auto-conditioning Options");
    condition_options.add_options()
        ("search_max_bias_ngram_length",   po::value<size_t>(), "add a \"bias\" feature for each ngram up to and including this length. eg., if it's 1 (default), then you get a single feature for each conditional")
        ("search_max_quad_ngram_length",   po::value<size_t>(), "add bias *times* input features for each ngram up to and including this length (def: 0)")
        ("search_condition_feature_value", po::value<float> (), "how much weight should the conditional features get? (def: 1.)");

    vm = add_options(vw, condition_options);

    check_option<size_t>(acset.max_bias_ngram_length, vw, vm, "search_max_bias_ngram_length", false, size_equal,
                         "warning: you specified a different value for --search_max_bias_ngram_length than the one loaded from regressor. proceeding with loaded value: ", "");

    check_option<size_t>(acset.max_quad_ngram_length, vw, vm, "search_max_quad_ngram_length", false, size_equal,
                         "warning: you specified a different value for --search_max_quad_ngram_length than the one loaded from regressor. proceeding with loaded value: ", "");

    check_option<float> (acset.feature_value, vw, vm, "search_condition_feature_value", false, float_equal,
                         "warning: you specified a different value for --search_condition_feature_value than the one loaded from regressor. proceeding with loaded value: ", "");
  }

  v_array<CS::label> read_allowed_transitions(action A, const char* filename) {
    FILE *f = fopen(filename, "r");
    if (f == NULL) {
      cerr << "error: could not read file " << filename << " (" << strerror(errno) << "); assuming all transitions are valid" << endl;
      throw exception();
    }

    bool* bg = (bool*)malloc((A+1)*(A+1) * sizeof(bool));
    int rd,from,to,count=0;
    while ((rd = fscanf(f, "%d:%d", &from, &to)) > 0) {
      if ((from < 0) || (from > (int)A)) { cerr << "warning: ignoring transition from " << from << " because it's out of the range [0," << A << "]" << endl; }
      if ((to   < 0) || (to   > (int)A)) { cerr << "warning: ignoring transition to "   << to   << " because it's out of the range [0," << A << "]" << endl; }
      bg[from * (A+1) + to] = true;
      count++;
    }
    fclose(f);

    v_array<CS::label> allowed;

    for (size_t from=0; from<A; from++) {
      v_array<CS::wclass> costs;

      for (size_t to=0; to<A; to++)
        if (bg[from * (A+1) + to]) {
          CS::wclass c = { FLT_MAX, (action)to, 0., 0. };
          costs.push_back(c);
        }

      CS::label ld = { costs, 0 };
      allowed.push_back(ld);
    }
    free(bg);

    cerr << "read " << count << " allowed transitions from " << filename << endl;

    return allowed;
  }


  void parse_neighbor_features(string& nf_string, search&sch) {
    search_private& priv = *sch.priv;
    priv.neighbor_features.erase();
    size_t len = nf_string.length();
    if (len == 0) return;

    char * cstr = new char [len+1];
    strcpy(cstr, nf_string.c_str());

    char * p = strtok(cstr, ",");
    v_array<substring> cmd;
    while (p != 0) {
      cmd.erase();
      substring me = { p, p+strlen(p) };
      tokenize(':', me, cmd, true);

      int32_t posn = 0;
      char ns = ' ';
      if (cmd.size() == 1) {
        posn = int_of_substring(cmd[0]);
        ns   = ' ';
      } else if (cmd.size() == 2) {
        posn = int_of_substring(cmd[0]);
        ns   = (cmd[1].end > cmd[1].begin) ? cmd[1].begin[0] : ' ';
      } else {
        cerr << "warning: ignoring malformed neighbor specification: '" << p << "'" << endl;
      }
      int32_t enc = (posn << 24) | (ns & 0xFF);
      priv.neighbor_features.push_back(enc);

      p = strtok(NULL, ",");
    }
    cmd.delete_v();

    delete cstr;
  }

  learner* setup(vw&all, po::variables_map& vm) {
    search* sch = (search*)calloc_or_die(1,sizeof(search));
    sch->priv = new search_private();
    search_initialize(&all, *sch);
    search_private& priv = *sch->priv;

    po::options_description search_opts("Search Options");
    search_opts.add_options()
        ("search_task",              po::value<string>(), "the search task")
        ("search_interpolation",     po::value<string>(), "at what level should interpolation happen? [*data|policy]")
        ("search_rollout",           po::value<string>(), "how should rollouts be executed?           [policy|oracle|*mix_per_state|mix_per_roll|none]")
        ("search_rollin",            po::value<string>(), "how should past trajectories be generated? [policy|oracle|*mix_per_state|mix_per_roll]")

        ("search_passes_per_policy", po::value<size_t>(), "number of passes per policy (only valid for search_interpolation=policy)     [def=1]")
        ("search_exp_perturbation",  po::value<float>(),  "interpolation rate for policies in rollout (only valid for search_rollout=mix_per_state|mix_per_roll) [def=0.5]")
        ("search_gamma",             po::value<float>(),  "interpolation rate for policies in rollout (only valid for search_rollout=mix_per_state|mix_per_roll) [def=0.5]")
        ("search_beta",              po::value<float>(),  "interpolation rate for policies (only valid for search_interpolation=policy) [def=0.5]")

        ("search_alpha",             po::value<float>(),  "annealed beta = 1-(1-alpha)^t (only valid for search_interpolation=data)     [def=1e-10]")

        ("search_total_nb_policies", po::value<size_t>(), "if we are going to train the policies through multiple separate calls to vw, we need to specify this parameter and tell vw how many policies are eventually going to be trained")

        ("search_trained_nb_policies", po::value<size_t>(), "the number of trained policies in a file")

        ("search_allowed_transitions",po::value<string>(),"read file of allowed transitions [def: all transitions are allowed]")
        ("search_subsample_time",    po::value<float>(),  "instead of training at all timesteps, use a subset. if value in (0,1), train on a random v%. if v>=1, train on precisely v steps per example")
        ("search_neighbor_features", po::value<string>(), "copy features from neighboring lines. argument looks like: '-1:a,+2' meaning copy previous line namespace a and next next line from namespace _unnamed_, where ',' separates them")
        ;

    vm = add_options(all, search_opts);

    std::string task_string;
    std::string interpolation_string = "data";
    std::string rollout_string = "mix_per_state";
    std::string rollin_string = "mix_per_state";

    check_option<string>(task_string, all, vm, "search_task", false, string_equal,
                         "warning: specified --search_task different than the one loaded from regressor. using loaded value of: ",
                         "error: you must specify a task using --search_task");
    check_option<string>(interpolation_string, all, vm, "search_interpolation", false, string_equal,
                         "warning: specified --search_interpolation different than the one loaded from regressor. using loaded value of: ", "");
    check_option<string>(rollout_string, all, vm, "search_rollout", false, string_equal,
                         "warning: specified --search_rollout different than the one loaded from regressor. using loaded value of: ", "");
    check_option<string>(rollin_string, all, vm, "search_rollin", false, string_equal,
                         "warning: specified --search_trajectory different than the one loaded from regressor. using loaded value of: ", "");

    if (vm.count("search_passes_per_policy"))       priv.passes_per_policy    = vm["search_passes_per_policy"].as<size_t>();
    if (vm.count("search_beta"))                    priv.beta                 = vm["search_beta"             ].as<float>();

    if (vm.count("search_alpha"))                   priv.alpha                = vm["search_alpha"            ].as<float>();
    if (vm.count("search_beta"))                    priv.beta                 = vm["search_beta"             ].as<float>();
    if (vm.count("search_gamma"))                   priv.gamma                = vm["search_gamma"            ].as<float>();
    //TODO: what was this???
    //if (vm.count("search_exp_perturbation"))        priv.exp_perturbation     = vm["search_exp_perturbation" ].as<float>();

    if (vm.count("search_subsample_time"))          priv.subsample_timesteps  = vm["search_subsample_time"].as<float>();

    priv.A = vm["searchnew"].as<size_t>(); // TODO: change to "search"

    string neighbor_features_string;
    check_option<string>(neighbor_features_string, all, vm, "search_neighbor_features", false, string_equal,
                         "warning: you specified a different feature structure with --search_neighbor_features than the one loaded from predictor. using loaded value of: ", "");
    parse_neighbor_features(neighbor_features_string, *sch);

    if (interpolation_string.compare("data") == 0) { // run as dagger
      priv.adaptive_beta = true;
      priv.allow_current_policy = true;
      priv.passes_per_policy = all.numpasses;
      if (priv.current_policy > 1) priv.current_policy = 1;
    } else if (interpolation_string.compare("policy") == 0) {
    } else {
      cerr << "error: --search_interpolation must be 'data' or 'policy'" << endl;
      throw exception();
    }

    if      (rollout_string.compare("policy") == 0)          priv.rollout_method = POLICY;
    else if (rollout_string.compare("oracle") == 0)          priv.rollout_method = ORACLE;
    else if (rollout_string.compare("mix_per_state") == 0)   priv.rollout_method = MIX_PER_STATE;
    else if (rollout_string.compare("mix_per_roll") == 0)    priv.rollout_method = MIX_PER_ROLL;
    else if (rollout_string.compare("none") == 0)            priv.rollout_method = NO_ROLLOUT;
    else {
      cerr << "error: --search_rollout must be 'policy', 'oracle', 'mix_per_state', 'mix_per_roll' or 'none'" << endl;
      throw exception();
    }

    if      (rollin_string.compare("policy") == 0)         priv.rollin_method = POLICY;
    else if (rollin_string.compare("oracle") == 0)         priv.rollin_method = ORACLE;
    else if (rollin_string.compare("mix_per_state") == 0)  priv.rollin_method = MIX_PER_STATE;
    else if (rollin_string.compare("mix_per_roll") == 0)   priv.rollin_method = MIX_PER_ROLL;
    else {
      cerr << "error: --search_rollin must be 'policy', 'oracle', 'mix_per_state' or 'mix_per_roll'" << endl;
      throw exception();
    }
    
    check_option<size_t>(priv.A, all, vm, "search", false, size_equal,
                         "warning: you specified a different number of actions through --search than the one loaded from predictor. using loaded value of: ", "");

    //check if the base learner is contextual bandit, in which case, we dont rollout all actions.
    if (vm.count("cb")) {
      priv.cb_learner = true;
      priv.allowed_actions_cache = new CB::label();
    } else {
      priv.cb_learner = false;
      priv.allowed_actions_cache = new CS::label();
    }

    //if we loaded a regressor with -i option, --search_trained_nb_policies contains the number of trained policies in the file
    // and --search_total_nb_policies contains the total number of policies in the file
    if (vm.count("search_total_nb_policies"))
      priv.total_number_of_policies = (uint32_t)vm["search_total_nb_policies"].as<size_t>();

    ensure_param(priv.beta , 0.0, 1.0, 0.5, "warning: search_beta must be in (0,1); resetting to 0.5");
    ensure_param(priv.alpha, 0.0, 1.0, 1e-10f, "warning: search_alpha must be in (0,1); resetting to 1e-10");
    ensure_param(priv.gamma , 0.0, 1.0, 0.5, "warning: search_gamma must be in (0,1); resetting to 0.5");
    //ensure_param(priv.exp_perturbation , 0.0, 1.0, 0.5, "warning: search_exp_perturbation must be in (0,1); resetting to 0.5");

    //compute total number of policies we will have at end of training
    // we add current_policy for cases where we start from an initial set of policies loaded through -i option
    uint32_t tmp_number_of_policies = priv.current_policy;
    if( all.training )
      tmp_number_of_policies += (int)ceil(((float)all.numpasses) / ((float)priv.passes_per_policy));

    //the user might have specified the number of policies that will eventually be trained through multiple vw calls,
    //so only set total_number_of_policies to computed value if it is larger
    cdbg << "current_policy=" << priv.current_policy << " tmp_number_of_policies=" << tmp_number_of_policies << " total_number_of_policies=" << priv.total_number_of_policies << endl;
    if( tmp_number_of_policies > priv.total_number_of_policies ) {
      priv.total_number_of_policies = tmp_number_of_policies;
      if( priv.current_policy > 0 ) //we loaded a file but total number of policies didn't match what is needed for training
        std::cerr << "warning: you're attempting to train more classifiers than was allocated initially. Likely to cause bad performance." << endl;
    }

    //current policy currently points to a new policy we would train
    //if we are not training and loaded a bunch of policies for testing, we need to subtract 1 from current policy
    //so that we only use those loaded when testing (as run_prediction is called with allow_current to true)
    if( !all.training && priv.current_policy > 0 )
      priv.current_policy--;

    std::stringstream ss1, ss2;
    ss1 << priv.current_policy;           VW::cmd_string_replace_value(all.file_options,"--search_trained_nb_policies", ss1.str());
    ss2 << priv.total_number_of_policies; VW::cmd_string_replace_value(all.file_options,"--search_total_nb_policies",   ss2.str());

    cdbg << "search current_policy = " << priv.current_policy << " total_number_of_policies = " << priv.total_number_of_policies << endl;

    for (search_task** mytask = all_tasks; *mytask != NULL; mytask++)
      if (task_string.compare((*mytask)->task_name) == 0) {
        priv.task = *mytask;
        break;
      }
    if (priv.task == NULL) {
      if (! vm.count("help")) {
        cerr << "fail: unknown task for --search_task: " << task_string << endl;
        throw exception();
      }
    }
    all.p->emptylines_separate_examples = true;

    // default to OAA labels unless the task wants to override this!
    all.p->lp = MC::mc_label;
    if (priv.task)
      priv.task->initialize(*sch, priv.A, vm);

    if (vm.count("search_allowed_transitions"))     read_allowed_transitions((action)priv.A, vm["search_allowed_transitions"].as<string>().c_str());

    // set up auto-history if they want it
    if (priv.auto_condition_features) {
      handle_condition_options(all, priv.acset, vm);

      // turn off auto-condition if it's irrelevant
      if (((priv.acset.max_bias_ngram_length == 0) && (priv.acset.max_quad_ngram_length == 0)) ||
          (priv.acset.feature_value == 0.f)) {
        cerr << "warning: turning off AUTO_CONDITION_FEATURES because settings make it useless" << endl;
        priv.auto_condition_features = false;
      }
    }

    if (!priv.allow_current_policy) // if we're not dagger
      all.check_holdout_every_n_passes = priv.passes_per_policy;

    all.searnstr = sch; // TODO: rename this

    priv.start_clock_time = clock();

    learner* l = new learner(sch, all.l, priv.total_number_of_policies);
    l->set_learn<search, search_predict_or_learn<true> >();
    l->set_predict<search, search_predict_or_learn<true> >();  // TODO: bug. was <false>, but then the default driver calls predict rather than learn on test_only examples, which doesn't work becaues it's looking at the NEXT example rather than the previous block :(
    l->set_finish_example<search,finish_example>();
    l->set_end_examples<search,end_examples>();
    l->set_finish<search,search_finish>();
    l->set_end_pass<search,end_pass>();

    cerr << "set" << endl;
    
    return l;
  }

  float action_hamming_loss(action a, const action* A, size_t sz) {
    if (sz == 0) return 0.;   // latent variables have zero loss
    for (size_t i=0; i<sz; i++)
      if (a == A[i]) return 0.;
    return 1.;
  }
  
  // the interface:
  action search::predict(example& ec, ptag mytag, const action* oracle_actions, size_t oracle_actions_cnt, const ptag* condition_on, const char* condition_on_names, const action* allowed_actions, size_t allowed_actions_cnt, size_t learner_id) {
    action a = search_predict(*this->priv, &ec, 1, mytag, oracle_actions, oracle_actions_cnt, condition_on, condition_on_names, allowed_actions, allowed_actions_cnt, learner_id);
    record_action(*this->priv, mytag, a);
    if (this->priv->auto_hamming_loss)
      loss(action_hamming_loss(a, oracle_actions, oracle_actions_cnt));
    return a;
  }

  action search::predictLDF(example* ecs, size_t ec_cnt, ptag mytag, const action* oracle_actions, size_t oracle_actions_cnt, const ptag* condition_on, const char* condition_on_names, size_t learner_id) {
    action a = search_predict(*this->priv, ecs, ec_cnt, mytag, oracle_actions, oracle_actions_cnt, condition_on, condition_on_names, NULL, 0, learner_id);
    record_action(*this->priv, mytag, a);
    if (this->priv->auto_hamming_loss)
      loss(action_hamming_loss(a, oracle_actions, oracle_actions_cnt));
    return a;
  }

  void search::loss(float loss) { search_declare_loss(*this->priv, loss); }

  stringstream& search::output() {
    if      (!this->priv->should_produce_string    ) return *(this->priv->bad_string_stream);
    else if ( this->priv->state == GET_TRUTH_STRING) return *(this->priv->truth_string);
    else                                             return *(this->priv->pred_string);
  }

  void  search::set_options(uint32_t opts) {
    if (this->priv->state != INITIALIZE) {
      cerr << "error: task cannot set options except in initialize function!" << endl;
      throw exception();
    }
    if ((opts & AUTO_CONDITION_FEATURES) != 0) this->priv->auto_condition_features = true;
    if ((opts & AUTO_HAMMING_LOSS)       != 0) this->priv->auto_hamming_loss = true;
    if ((opts & EXAMPLES_DONT_CHANGE)    != 0) this->priv->examples_dont_change = true;
    if ((opts & IS_LDF)                  != 0) this->priv->is_ldf = true;
  }

  void search::set_num_learners(size_t num_learners) { this->priv->num_learners = num_learners; }
}

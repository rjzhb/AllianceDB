//
// Created by Shuhao Zhang on 1/11/19.
//

#include <iostream>
#include <set>
#include <vector>
#include <numeric>
#include "localjoiner.h"
#include "avxsort.h"
#include "sort_common.h"

/**
 * As an example of join execution, consider a join with join predicate T1.attr1 = T2.attr2.
 * The join operator will incrementally load a hash table H1 for T1 by hashing attr1 using hash function f1,
 * and another hash table H2 for T2 by hashing attr2 using hash function f2.
 * The symmetric hash join operator starts by
 * (1): getting a tuple from T1, hashing its attr1 field using f1, and inserting it into H1.
 * (2): it probes H2 by applying f2 to attr1 of the current T1 tuple, returning any matched tuple pairs.
 *
 * (3): it gets a tuple from T2, hashes it by applying f2 to attr2, and inserts it into H2.
 * (4): it probes H1 by applying f1 to attr2 of the current T2 tuple, and returns any matches.
 * (5): This continues until all tuples from T1 and T2 have been consumed.
 *
 * @param relR
 * @param relS
 * @param nthreads
 * @return
 */
long
shj(int32_t tid, relation_t *rel_R,
    relation_t *rel_S, void *pVoid,
    T_TIMER *timer) {

    //allocate two hashtables.
    hashtable_t *htR;
    hashtable_t *htS;

    uint32_t nbucketsR = (rel_R->num_tuples / BUCKET_SIZE);
    allocate_hashtable(&htR, nbucketsR);

    uint32_t nbucketsS = (rel_S->num_tuples / BUCKET_SIZE);
    allocate_hashtable(&htS, nbucketsS);

    uint32_t index_R = 0;//index of rel_R
    uint32_t index_S = 0;//index of rel_S

    int64_t matches = 0;//number of matches.

    SHJJoiner joiner;

    do {
        if (index_R < rel_R->num_tuples) {
            joiner.join(tid, &rel_R->tuples[index_R], true, htR, htS, &matches, pVoid, timer);
            index_R++;
        }
        if (index_S < rel_S->num_tuples) {
            joiner.join(tid, &rel_S->tuples[index_S], false, htR, htS, &matches, pVoid, timer);
            index_S++;
        }
    } while (index_R < rel_R->num_tuples || index_S < rel_S->num_tuples);

    destroy_hashtable(htR);
    destroy_hashtable(htS);
    return matches;
}


#define progressive_step 0.01 //percentile, 0.01 ~ 0.2.
#define merge_step 1 // number of ``runs" to merge in each round.


inline tuple_t *read(tuple_t *tuple, int length, int idx) {
    if (idx >= length) return nullptr;
    return &tuple[idx];
}

inline bool EqualPredicate(tuple_t *u, tuple_t *v) {
    return u->key == v->key;
}

inline bool LessPredicate(tuple_t *u, tuple_t *v) {
    return u->key < v->key;
}

struct run {//a pair of subsequence (mask position only)

    std::vector<int> *posR;//readable position of R. By default should be 0 to size of R/S.
    std::vector<int> *posS;//readable position of S.
    run(std::vector<int> *posR, std::vector<int> *posS) {
        this->posR = posR;
        this->posS = posS;
    }

};

struct sweepArea {

    std::set<tuple_t *> sx;

    void insert(tuple_t *tuple) {
        sx.insert(tuple);
    }

    void query(tuple_t *tuple, int *matches) {

        //clean elements that are less than the current element.
        for (auto it = sx.begin(); it != sx.end();) {
            if (LessPredicate(it.operator*(), tuple)) {
                it = sx.erase(it);
            } else {  //perform join.
                if (EqualPredicate(it.operator*(), tuple)) {
                    (*matches)++;
                }
                ++it;
            }
        }
    }
};


std::string print_relation(tuple_t *tuple, int length);

void earlyJoinInitialRuns(tuple_t *tupleR, tuple_t *tupleS, int length, int i, int *matches);


void earlyJoinInitialRuns(tuple_t *tupleR, tuple_t *tupleS, int lengthR, int lengthS, int *matches) {
    //in early join
    printf("Tuple R: %s\n", print_relation(tupleR, lengthR).c_str());
    printf("Tuple S: %s\n", print_relation(tupleS, lengthS).c_str());
    fflush(stdout);

    int r = 0;
    int s = 0;
    sweepArea RM;
    sweepArea SM;
    while (r < lengthR || s < lengthS) {
        tuple_t *tr = read(tupleR, lengthR, r);
        tuple_t *ts = read(tupleS, lengthS, s);
        if (s == lengthS || (r < lengthR && tr->key <= ts->key)) {
            RM.insert(tr);
            SM.query(tr, matches);
            r++;//remove tr from tupleR.
        } else {
            SM.insert(ts);
            RM.query(ts, matches);
            s++;//remove ts from tupleS.
        }
    }
}


void earlyJoinMergedRuns(tuple_t *tupleR, tuple_t *tupleS, std::vector<run> *Q, int merge_itr, int *matches,
                         std::vector<tuple_t *> sortedR, std::vector<tuple_t *> sortedS);

/**
 * Merges the input sequences into two larger sequences, which are then joined directly.
 *
 * @param tupleR
 * @param tupleS
 * @param matches
 */
void earlyJoinMergedRuns(tuple_t *tupleR, tuple_t *tupleS, std::vector<run> *Q, int merge_itr, int *matches,
                         std::vector<tuple_t *> sortedR, std::vector<tuple_t *> sortedS) {


    sweepArea RM;
    sweepArea SM;
    int m;

    //determine the smallest element of r and s from multiple (#merge_step) subsequences.
    auto *minR = new tuple_t();
    minR->key = INT32_MAX;
    auto i = -1;
    auto *minS = new tuple_t();
    minS->key = INT32_MAX;
    auto j = -1;

    auto run_itr = Q->begin() + merge_itr;
    for (m = 0; m < merge_step; m++) {

        auto posR = run_itr.operator*().posR;
        auto posS = run_itr.operator*().posS;

        for (auto it = posR->begin(); it != posR->end();) {
            tuple_t *readR = read(tupleR, 1, it.operator*());
            if (minR->key > readR->key) {
                minR = readR;
                i = m;
            }
        }

        for (auto it = posS->begin(); it != posS->end();) {
            tuple_t *readS = read(tupleS, 1, it.operator*());
            if (minS->key > readS->key) {
                minS = readS;
                j = m;
            }
        }
    }

    if (j == -1 || (i != -1 && LessPredicate(minR, minS))) {
        RM.insert(minR);
        SM.query(minR, matches);
        Q->at(i).posR->erase(Q->at(i).posR->begin());//remove the smallest element from subsequence.
        sortedR.push_back(minR);//merge multiple subsequences into a longer sorted one.
    } else {
        SM.insert(minS);
        RM.query(minS, matches);
        Q->at(j).posS->erase(Q->at(j).posS->begin());//remove the smallest element from subsequence.
        sortedS.push_back(minR);//merge multiple subsequences into a longer sorted one.
    }
}


void insert(std::vector<run> *Q, int startR, int lengthR, int startS, int lengthS);

void insert(std::vector<run> *Q, int startR, int lengthR, int startS, int lengthS) {
    std::vector<int> v(lengthR);
    std::iota(v.begin(), v.end(), startR);

    std::vector<int> u(lengthS);
    std::iota(u.begin(), u.end(), startS);
    Q->push_back(run(&v, &u));
}


/**
 *  The main idea of PMJ is to read as much data as can fit in memory.
 *  Then, in-memory data is sorted and is joined together, and then is flushed into disk.
 *  When all data is received, PMJ joins the disk-resident data using a refinement version
 *  of the sort-merge join that allows producing join results while merging.
 *
 *  We change it to read up to progressive_step of data. Then sort and join, and then push aside at rest.
 *  When all data is received, join the rest data using refinement version of SMJ.
 *
 * @param tid
 * @param rel_R
 * @param rel_S
 * @param pVoid
 * @param timer
 * @return
 */
long
pmj(int32_t tid, relation_t *rel_R, relation_t *rel_S, void *pVoid, T_TIMER *timer) {

    //Phase 1 ('Join during run creation')
    int sizeR = rel_R->num_tuples;
    int sizeS = rel_S->num_tuples;
    int i = 0;
    int j = 0;
    int matches = 0;
    int progressive_stepR = progressive_step * sizeR;
    int progressive_stepS = progressive_step * sizeS;

    std::vector<run> Q;//let Q be an empty set;

    std::vector<tuple_t *> sortedR;
    std::vector<tuple_t *> sortedS;

    MSG("Join during run creation")
    do {
        tuple_t *inptrR = nullptr;
        tuple_t *inptrS = nullptr;
        tuple_t *outptrR = nullptr;
        tuple_t *outptrS = nullptr;
        //take subset of R and S to sort and join.
        if (i < sizeR) {
            inptrR = (rel_R->tuples) + i;
#ifdef DEBUG
            printf("%s\n", print_relation(rel_R->tuples, progressive_stepR * (i + 1)).c_str());
#endif
            avxsort_tuples(&inptrR, &outptrR, progressive_stepR);// the method will swap input and output pointers.
#ifdef DEBUG
            if (!is_sorted_helper((int64_t *) outptrR, progressive_step)) {
                printf("===> %d-thread -> R is NOT sorted, size = %d\n", tid, progressive_step);
            }
#endif
        }
        if (j < sizeS) {
            inptrS = (rel_S->tuples) + j;
            avxsort_tuples(&inptrS, &outptrS, progressive_stepS);
#ifdef DEBUG
            if (!is_sorted_helper((int64_t *) outptrS, progressive_step)) {
                printf("===> %d-thread -> S is NOT sorted, size = %d\n", tid, progressive_step);
            }
#endif
        }
        earlyJoinInitialRuns(outptrR, outptrS, progressive_stepR, progressive_stepS, &matches);
        insert(&Q, i, progressive_stepR, j, progressive_stepS);
        i += progressive_stepR;
        j += progressive_stepS;
    } while (i < sizeR || j < sizeS);//while R!=null, S!=null.

    MSG("Join during run merge")
    int merge_itr = 0;
    do {
        earlyJoinMergedRuns(rel_R->tuples, rel_S->tuples, &Q, merge_itr, &matches, sortedR, sortedS);
        i += progressive_stepR * merge_step;
        j += progressive_stepS * merge_step;
        merge_itr += merge_step;
    } while (i < sizeR || j < sizeS);
    return 0;
}


std::string print_relation(tuple_t *tuple, int length) {
    std::string tmp = "";
    tmp.append("[");

    for (int i = 0; i < length; i++)
        tmp.append(std::to_string(tuple[i].key)).append(",");
    tmp.append("]\n");
    return tmp;
}


/**
 * SHJ algorithm to be used in each thread.
 * @param tid
 * @param tuple
 * @param tuple_R
 * @param htR
 * @param htS
 * @param matches
 * @param pVoid
 * @param timer
 * @return
 */
long SHJJoiner::join(int32_t tid, tuple_t *tuple, bool tuple_R,
                     hashtable_t *htR, hashtable_t *htS, int64_t *matches,
                     void *pVoid, T_TIMER *timer) {

    const uint32_t hashmask_R = htR->hash_mask;
    const uint32_t skipbits_R = htR->skip_bits;

    const uint32_t hashmask_S = htS->hash_mask;
    const uint32_t skipbits_S = htS->skip_bits;

//    DEBUGMSG(1, "JOINING: tid: %d, tuple: %d, R?%d\n", tid, tuple->key, tuple_R)
    if (tuple_R) {
        if (tid == 0) {
            BEGIN_MEASURE_BUILD_ACC((*timer))
//            window0.R_Window.push_back(tuple->key);
        } else {
//            window1.R_Window.push_back(tuple->key);
        }
        build_hashtable_single(htR, tuple, hashmask_R, skipbits_R);//(1)
//        DEBUGMSG(1, "tid %d add tuple r %d to R-window. \n", tid, tuple->key)

        if (tid == 0) {
            END_MEASURE_BUILD_ACC((*timer))
        }
        if (tid == 0) {
            proble_hashtable_single_measure(htS, tuple, hashmask_S, skipbits_S, matches,
                                            timer->progressivetimer);//(2)
            DEBUGMSG(1, "matches:%ld, T0: Join R %d with %s", *matches, tuple->key,
                     print_window(window0.S_Window).c_str());
        } else {
            proble_hashtable_single(htS, tuple, hashmask_S, skipbits_S, matches);//(4)
            DEBUGMSG(1, "matches:%ld, T1: Join R %d with %s", *matches, tuple->key,
                     print_window(window1.S_Window).c_str());

        }
    } else {
//        DEBUGMSG(1, "BUILD TABLE: tid: %d, tuple: %d, R?%d\n", tid, tuple->key, tuple_R)

        if (tid == 0) {
            BEGIN_MEASURE_BUILD_ACC((*timer))
//            window0.S_Window.push_back(tuple->key);
        } else {
//            window1.S_Window.push_back(tuple->key);
        }
        build_hashtable_single(htS, tuple, hashmask_S, skipbits_S);//(3)

//        DEBUGMSG(1, "tid %d add tuple s %d to S-window. \n", tid, tuple->key)

        if (tid == 0) {
            END_MEASURE_BUILD_ACC((*timer))
        }

//        DEBUGMSG(1, "BUILD TABLE FINISH: tid: %d, tuple: %d, R?%d\n", tid, tuple->key, tuple_R)

        if (tid == 0) {
            proble_hashtable_single_measure(htR, tuple, hashmask_R, skipbits_R, matches, timer->progressivetimer);//(4)
            DEBUGMSG(1, "matches:%ld, T0: Join S %d with %s", *matches, tuple->key,
                     print_window(window0.R_Window).c_str());
        } else {
            proble_hashtable_single(htR, tuple, hashmask_R, skipbits_R, matches);//(4)
            DEBUGMSG(1, "matches:%ld, T1: Join S %d with %s", *matches, tuple->key,
                     print_window(window1.R_Window).c_str());
        }
    }
//    DEBUGMSG(1, "JOINING FINISH: tid: %d, tuple: %d, R?%d\n", tid, tuple->key, tuple_R)
    return *matches;
}


/**
 * PMJ algorithm to be used in each thread.
 * @param tid
 * @param tuple
 * @param tuple_R
 * @param htR
 * @param htS
 * @param matches
 * @param pVoid
 * @param timer
 * @return
 */

long PMJJoiner::join(int32_t tid, tuple_t *tuple, bool tuple_R, hashtable_t *htR, hashtable_t *htS, int64_t *matches,
                     void *pVoid, T_TIMER *timer) {
    return 0;
}


/**
 * PMJ algorithm to be used in each thread.
 * @param tid
 * @param tuple
 * @param tuple_R
 * @param htR
 * @param htS
 * @param matches
 * @param pVoid
 * @param timer
 * @return
 */

long RippleJoiner::join(int32_t tid, tuple_t *tuple, bool tuple_R, hashtable_t *htR, hashtable_t *htS, int64_t *matches,
                        void *pVoid, T_TIMER *timer) {
    return 0;
}

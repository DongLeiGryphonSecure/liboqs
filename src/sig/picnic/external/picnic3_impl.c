/*! @file picnic3_impl.c
 *  @brief This is the main file of the signature scheme for the Picnic3
 *  parameter sets.
 *
 *  This file is part of the reference implementation of the Picnic signature scheme.
 *  See the accompanying documentation for complete details.
 *
 *  The code is provided under the MIT license, see LICENSE for
 *  more details.
 *  SPDX-License-Identifier: MIT
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#if !defined(NDEBUG)
#include <stdio.h>
#include <inttypes.h>
#endif

#include "io.h"
#include "kdf_shake.h"
#include "macros.h"
#include "picnic.h"
#include "picnic3_impl.h"
#include "picnic3_tree.h"
#include "picnic3_types.h"

/* Helper functions */

ATTR_CONST
static inline uint32_t numBytes(uint32_t numBits) {
  return (numBits + 7) >> 3;
}

static void createRandomTapes(randomTape_t* tapes, uint8_t* seeds, uint8_t* salt, size_t t,
                              const picnic_instance_t* params) {
  hash_context_x4 ctx;

  size_t tapeSizeBytes = 2 * params->view_size;

  allocateRandomTape(tapes, params);
  assert(params->num_MPC_parties % 4 == 0);
  for (size_t i = 0; i < params->num_MPC_parties; i += 4) {
    hash_init_x4(&ctx, params->digest_size);

    hash_update_x4_4(&ctx, &seeds[i * params->seed_size], &seeds[(i + 1) * params->seed_size],
                     &seeds[(i + 2) * params->seed_size], &seeds[(i + 3) * params->seed_size],
                     params->seed_size);
    hash_update_x4_1(&ctx, salt, SALT_SIZE);
    hash_update_x4_uint16_le(&ctx, t);
    const uint16_t i_arr[4] = {i + 0, i + 1, i + 2, i + 3};
    hash_update_x4_uint16s_le(&ctx, i_arr);
    hash_final_x4(&ctx);

    hash_squeeze_x4_4(&ctx, tapes->tape[i], tapes->tape[i + 1], tapes->tape[i + 2],
                      tapes->tape[i + 3], tapeSizeBytes);
    hash_clear_x4(&ctx);
  }
}

/* Input is the tapes for one parallel repitition; i.e., tapes[t]
 * Updates the random tapes of all players with the mask values for the output of
 * AND gates, and computes the N-th party's share such that the AND gate invariant
 * holds on the mask values.
 */
static void computeAuxTape(randomTape_t* tapes, uint8_t* input_masks,
                           const picnic_instance_t* params) {
  mzd_local_t lowmc_key[1];

  size_t tapeSizeBytes = 2 * params->view_size;

  // combine into key shares and calculate lowmc evaluation in plain
  for (size_t i = 0; i < params->num_MPC_parties; i++) {
    for (size_t j = 0; j < tapeSizeBytes; j++) {
      tapes->parity_tapes[j] ^= tapes->tape[i][j];
    }
  }
  mzd_from_char_array(lowmc_key, tapes->parity_tapes, params->input_output_size);
  tapes->pos     = params->lowmc.n;
  tapes->aux_pos = 0;
  memset(tapes->aux_bits, 0, params->view_size);

  // Perform LowMC evaluation and fix AND masks for all AND gates
  lowmc_compute_aux(&params->lowmc, lowmc_key, tapes);

  // write the key masks to the input
  if (input_masks != NULL) {
    mzd_to_char_array(input_masks, lowmc_key, params->input_output_size);
  }

  // Reset the random tape counter so that the online execution uses the
  // same random bits as when computing the aux shares
  tapes->pos = 0;
}

static void commit(uint8_t* digest, const uint8_t* seed, const uint8_t* aux, const uint8_t* salt,
                   size_t t, size_t j, const picnic_instance_t* params) {
  /* Compute C[t][j];  as digest = H(seed||[aux]) aux is optional */
  hash_context ctx;

  hash_init(&ctx, params->digest_size);
  hash_update(&ctx, seed, params->seed_size);
  if (aux != NULL) {
    hash_update(&ctx, aux, params->view_size);
  }
  hash_update(&ctx, salt, SALT_SIZE);
  hash_update_uint16_le(&ctx, t);
  hash_update_uint16_le(&ctx, j);
  hash_final(&ctx);
  hash_squeeze(&ctx, digest, params->digest_size);
  hash_clear(&ctx);
}

static void commit_x4(uint8_t** digest, const uint8_t** seed, const uint8_t* salt, size_t t,
                      size_t j, const picnic_instance_t* params) {
  /* Compute C[t][j];  as digest = H(seed||[aux]) aux is optional */
  hash_context_x4 ctx;

  hash_init_x4(&ctx, params->digest_size);
  hash_update_x4(&ctx, seed, params->seed_size);
  hash_update_x4_1(&ctx, salt, SALT_SIZE);
  hash_update_x4_uint16_le(&ctx, t);
  const uint16_t j_arr[4] = {j + 0, j + 1, j + 2, j + 3};
  hash_update_x4_uint16s_le(&ctx, j_arr);
  hash_final_x4(&ctx);
  hash_squeeze_x4(&ctx, digest, params->digest_size);
  hash_clear_x4(&ctx);
}

static void commit_h(uint8_t* digest, const commitments_t* C, const picnic_instance_t* params) {
  hash_context ctx;

  hash_init(&ctx, params->digest_size);
  for (size_t i = 0; i < params->num_MPC_parties; i++) {
    hash_update(&ctx, C->hashes[i], params->digest_size);
  }
  hash_final(&ctx);
  hash_squeeze(&ctx, digest, params->digest_size);
  hash_clear(&ctx);
}

static void commit_h_x4(uint8_t** digest, const commitments_t* C, const picnic_instance_t* params) {
  hash_context_x4 ctx;

  hash_init_x4(&ctx, params->digest_size);
  for (size_t i = 0; i < params->num_MPC_parties; i++) {
    hash_update_x4_4(&ctx, C[0].hashes[i], C[1].hashes[i], C[2].hashes[i], C[3].hashes[i],
                     params->digest_size);
  }
  hash_final_x4(&ctx);
  hash_squeeze_x4(&ctx, digest, params->digest_size);
  hash_clear_x4(&ctx);
}

// Commit to the views for one parallel rep
static void commit_v(uint8_t* digest, const uint8_t* input, const msgs_t* msgs,
                     const picnic_instance_t* params) {
  hash_context ctx;

  hash_init(&ctx, params->digest_size);
  hash_update(&ctx, input, params->input_output_size);
  for (size_t i = 0; i < params->num_MPC_parties; i++) {
    hash_update(&ctx, msgs->msgs[i], numBytes(msgs->pos));
  }
  hash_final(&ctx);
  hash_squeeze(&ctx, digest, params->digest_size);
  hash_clear(&ctx);
}

static void commit_v_x4(uint8_t** digest, const uint8_t** input, const msgs_t* msgs,
                        const picnic_instance_t* params) {
  hash_context_x4 ctx;

  hash_init_x4(&ctx, params->digest_size);
  hash_update_x4(&ctx, input, params->input_output_size);
  for (size_t i = 0; i < params->num_MPC_parties; i++) {
    assert(msgs[0].pos == msgs[1].pos && msgs[2].pos == msgs[3].pos && msgs[0].pos == msgs[2].pos);
    hash_update_x4_4(&ctx, msgs[0].msgs[i], msgs[1].msgs[i], msgs[2].msgs[i], msgs[3].msgs[i],
                     numBytes(msgs->pos));
  }
  hash_final_x4(&ctx);
  hash_squeeze_x4(&ctx, digest, params->digest_size);
  hash_clear_x4(&ctx);
}

static void xor_byte_array(uint8_t* out, const uint8_t* in1, const uint8_t* in2, uint32_t length) {
  for (uint32_t i = 0; i < length; i++) {
    out[i] = in1[i] ^ in2[i];
  }
}

static int contains(const uint16_t* list, size_t len, uint16_t value) {
  for (size_t i = 0; i < len; i++) {
    if (list[i] == value) {
      return 1;
    }
  }
  return 0;
}

static int indexOf(const uint16_t* list, size_t len, uint16_t value) {
  for (size_t i = 0; i < len; i++) {
    if (list[i] == value) {
      return i;
    }
  }
  assert(!"indexOf called on list where value is not found. (caller bug)");
  return -1;
}

static void setAuxBits(randomTape_t* tapes, uint8_t* input, const picnic_instance_t* params) {
  size_t last  = params->num_MPC_parties - 1;
  size_t inBit = 0;

  for (size_t j = 0; j < params->lowmc.r; j++) {
    for (size_t i = 0; i < params->lowmc.n; i++) {
      setBit(tapes->tape[last], params->lowmc.n + params->lowmc.n * 2 * (j) + i,
             getBit(input, inBit++));
    }
  }
}

static size_t bitsToChunks(size_t chunkLenBits, const uint8_t* input, size_t inputLen,
                           uint16_t* chunks) {
  if (!chunkLenBits || chunkLenBits > inputLen * 8) {
    assert(!"Invalid input to bitsToChunks: not enough input");
    return 0;
  }
  size_t chunkCount = ((inputLen * 8) / chunkLenBits);

  for (size_t i = 0; i < chunkCount; i++) {
    chunks[i] = 0;
    for (size_t j = 0; j < chunkLenBits; j++) {
      chunks[i] += getBit(input, i * chunkLenBits + j) << j;
      assert(chunks[i] < (1 << chunkLenBits));
    }
  }

  return chunkCount;
}

static size_t appendUnique(uint16_t* list, uint16_t value, size_t position) {
  if (position == 0) {
    list[position] = value;
    return position + 1;
  }

  for (size_t i = 0; i < position; i++) {
    if (list[i] == value) {
      return position;
    }
  }
  list[position] = value;
  return position + 1;
}

static void expandChallenge(uint16_t* challengeC, uint16_t* challengeP, const uint8_t* sigH,
                            const picnic_instance_t* params) {
  uint8_t h[MAX_DIGEST_SIZE] = {0};
  hash_context ctx;

  memcpy(h, sigH, params->digest_size);
  // Populate C
  uint32_t bitsPerChunkC = ceil_log2(params->num_rounds);
  uint32_t bitsPerChunkP = ceil_log2(params->num_MPC_parties);
  assert(bitsPerChunkC >= 4);
  assert(bitsPerChunkP >= 4);
  // chunks would only require digest_size * 8 / min(bitsPerChunkC, bitsPerChunkP), but
  // bitsPerChunkP is always 4 and the minimum.
  uint16_t chunks[MAX_DIGEST_SIZE * 8 / 4] = {0};

  size_t countC = 0;
  while (countC < params->num_opened_rounds) {
    size_t numChunks = bitsToChunks(bitsPerChunkC, h, params->digest_size, chunks);
    for (size_t i = 0; i < numChunks; i++) {
      if (chunks[i] < params->num_rounds) {
        countC = appendUnique(challengeC, chunks[i], countC);
      }
      if (countC == params->num_opened_rounds) {
        break;
      }
    }

    hash_init_prefix(&ctx, params->digest_size, HASH_PREFIX_1);
    hash_update(&ctx, h, params->digest_size);
    hash_final(&ctx);
    hash_squeeze(&ctx, h, params->digest_size);
    hash_clear(&ctx);
  }

  // Note that we always compute h = H(h) after setting C
  size_t countP = 0;

  while (countP < params->num_opened_rounds) {
    size_t numChunks = bitsToChunks(bitsPerChunkP, h, params->digest_size, chunks);
    for (size_t i = 0; i < numChunks; i++) {
      if (chunks[i] < params->num_MPC_parties) {
        challengeP[countP] = chunks[i];
        countP++;
      }
      if (countP == params->num_opened_rounds) {
        break;
      }
    }

    hash_init_prefix(&ctx, params->digest_size, HASH_PREFIX_1);
    hash_update(&ctx, h, params->digest_size);
    hash_final(&ctx);
    hash_squeeze(&ctx, h, params->digest_size);
    hash_clear(&ctx);
  }
}

static void HCP(uint8_t* sigH, uint16_t* challengeC, uint16_t* challengeP, const commitments_t* Ch,
                uint8_t* hCv, uint8_t* salt, const uint8_t* pubKey, const uint8_t* plaintext,
                const uint8_t* message, size_t messageByteLength, const picnic_instance_t* params) {
  hash_context ctx;

  assert(params->num_opened_rounds < params->num_rounds);

  hash_init(&ctx, params->digest_size);
  for (size_t t = 0; t < params->num_rounds; t++) {
    hash_update(&ctx, Ch->hashes[t], params->digest_size);
  }

  hash_update(&ctx, hCv, params->digest_size);
  hash_update(&ctx, salt, SALT_SIZE);
  hash_update(&ctx, pubKey, params->input_output_size);
  hash_update(&ctx, plaintext, params->input_output_size);
  hash_update(&ctx, message, messageByteLength);
  hash_final(&ctx);
  hash_squeeze(&ctx, sigH, params->digest_size);
  hash_clear(&ctx);
  /* parts of this hash will be published as challenge so is public anyway */
  picnic_declassify(sigH, params->digest_size);

  expandChallenge(challengeC, challengeP, sigH, params);
}

static uint16_t* getMissingLeavesList(uint16_t* challengeC, const picnic_instance_t* params) {
  size_t missingLeavesSize = params->num_rounds - params->num_opened_rounds;
  uint16_t* missingLeaves  = calloc(missingLeavesSize, sizeof(uint16_t));
  size_t pos               = 0;

  for (size_t i = 0; i < params->num_rounds; i++) {
    if (!contains(challengeC, params->num_opened_rounds, i)) {
      missingLeaves[pos] = i;
      pos++;
    }
  }

  return missingLeaves;
}

static int verify_picnic3(signature2_t* sig, const uint8_t* pubKey, const uint8_t* plaintext,
                          const uint8_t* message, size_t messageByteLength,
                          const picnic_instance_t* params) {
  int ret = -1;

  commitments_t C[4];
  allocateCommitments2(&C[0], params, params->num_MPC_parties);
  allocateCommitments2(&C[1], params, params->num_MPC_parties);
  allocateCommitments2(&C[2], params, params->num_MPC_parties);
  allocateCommitments2(&C[3], params, params->num_MPC_parties);

  msgs_t* msgs = allocateMsgsVerify(params);
  if (!msgs) {
    goto free_commitmentsC;
  }

  tree_t treeCv;
  if (!createTree(&treeCv, params->num_rounds, params->digest_size)) {
    goto free_msgs;
  }

  const size_t challengeSizeBytes = params->num_opened_rounds * sizeof(uint16_t);
  uint16_t* challengeC            = malloc(challengeSizeBytes);
  uint16_t* challengeP            = malloc(challengeSizeBytes);
  randomTape_t* tapes             = malloc(params->num_rounds * sizeof(randomTape_t));
  if (!challengeC || !challengeP || !tapes) {
    goto free_tapes;
  }

  tree_t iSeedsTree;
  if (!createTree(&iSeedsTree, params->num_rounds, params->seed_size)) {
    goto free_tapes;
  }

  ret = reconstructSeeds(&iSeedsTree, sig->challengeC, params->num_opened_rounds, sig->iSeedInfo,
                         sig->iSeedInfoLen, sig->salt, 0, params);
  if (ret) {
    ret = -1;
    goto free_seedstree;
  }

  commitments_t Ch;
  allocateCommitments2(&Ch, params, params->num_rounds);
  commitments_t Cv;
  allocateCommitments2(&Cv, params, params->num_rounds);
  mzd_local_t m_plaintext[1];
  mzd_local_t m_maskedKey[1];
  mzd_from_char_array(m_plaintext, plaintext, params->input_output_size);

  const size_t last                      = params->num_MPC_parties - 1;
  lowmc_simulate_online_f simulateOnline = lowmc_simulate_online_get_implementation(&params->lowmc);

  /* Populate seeds with values from the signature */
  for (uint16_t t = 0; t < params->num_rounds; t++) {
    tree_t seed;
    if (!contains(sig->challengeC, params->num_opened_rounds, t)) {
      /* Expand iSeed[t] to seeds for each parties, using a seed tree */
      if (!generateSeeds(&seed, params->num_MPC_parties, getLeaf(&iSeedsTree, t), sig->salt, t,
                         params)) {
        ret = -1;
        goto Exit;
      }
    } else {
      /* We don't have the initial seed for the round, but instead a seed
       * for each unopened party */
      if (!createTree(&seed, params->num_MPC_parties, params->seed_size)) {
        ret = -1;
        goto Exit;
      }

      uint16_t hideList[1] = {
          sig->challengeP[indexOf(sig->challengeC, params->num_opened_rounds, t)]};
      ret = reconstructSeeds(&seed, hideList, 1, sig->proofs[t].seedInfo,
                             sig->proofs[t].seedInfoLen, sig->salt, t, params);
      if (ret != 0) {
#if !defined(NDEBUG)
        printf("Failed to reconstruct seeds for round %" PRIu16 "\n", t);
#endif
        clearTree(&seed);
        ret = -1;
        goto Exit;
      }
    }
    /* Commit */

    /* Compute random tapes for all parties.  One party for each repetition
     * challengeC will have a bogus seed; but we won't use that party's
     * random tape. */
    createRandomTapes(&tapes[t], getLeaves(&seed), sig->salt, t, params);

    if (!contains(sig->challengeC, params->num_opened_rounds, t)) {
      /* We're given iSeed, have expanded the seeds, compute aux from scratch so we can compute
       * Com[t] */
      computeAuxTape(&tapes[t], NULL, params);
      for (size_t j = 0; j < params->num_MPC_parties; j += 4) {
        const uint8_t* seed_ptr[4] = {getLeaf(&seed, j + 0), getLeaf(&seed, j + 1),
                                      getLeaf(&seed, j + 2), getLeaf(&seed, j + 3)};
        commit_x4(C[t % 4].hashes + j, seed_ptr, sig->salt, t, j, params);
      }
      commit(C[t % 4].hashes[last], getLeaf(&seed, last), tapes[t].aux_bits, sig->salt, t, last,
             params);
      /* after we have checked the tape, we do not need it anymore for this opened iteration */
    } else {
      /* We're given all seeds and aux bits, except for the unopened
       * party, we get their commitment */
      size_t unopened = sig->challengeP[indexOf(sig->challengeC, params->num_opened_rounds, t)];
      for (size_t j = 0; j < params->num_MPC_parties; j += 4) {
        const uint8_t* seed_ptr[4] = {getLeaf(&seed, j + 0), getLeaf(&seed, j + 1),
                                      getLeaf(&seed, j + 2), getLeaf(&seed, j + 3)};
        commit_x4(C[t % 4].hashes + j, seed_ptr, sig->salt, t, j, params);
      }
      if (last != unopened) {
        commit(C[t % 4].hashes[last], getLeaf(&seed, last), sig->proofs[t].aux, sig->salt, t, last,
               params);
      }

      memcpy(C[t % 4].hashes[unopened], sig->proofs[t].C, params->digest_size);
    }
    /* hash commitments every four iterations if possible, for the last few do single commitments
     */
    if (t >= params->num_rounds / 4 * 4) {
      commit_h(Ch.hashes[t], &C[t % 4], params);
    } else if ((t + 1) % 4 == 0) {
      size_t t4 = t / 4 * 4;
      commit_h_x4(&Ch.hashes[t4], &C[0], params);
    }
    clearTree(&seed);
  }

  /* Commit to the views */
  for (uint16_t t = 0; t < params->num_rounds; t++) {
    if (!contains(sig->challengeC, params->num_opened_rounds, t)) {
      Cv.hashes[t] = NULL;
    }
  }

  for (uint8_t i = 0; i < params->num_opened_rounds; i++) {
    /* 2. When t is in C, we have everything we need to re-compute the view, as an honest signer
     * would.
     * We simulate the MPC with one fewer party; the unopened party's values are all set to zero.
     */
    uint16_t t     = sig->challengeC[i];
    int unopened   = sig->challengeP[i];
    uint8_t* input = sig->proofs[t].input;
    setAuxBits(&tapes[t], sig->proofs[t].aux, params);
    memset(tapes[t].tape[unopened], 0, 2 * params->view_size);
    memcpy(msgs->msgs[unopened], sig->proofs[t].msgs, params->view_size);
    mzd_from_char_array(m_maskedKey, input, params->input_output_size);
    msgs->unopened = unopened;
    msgs->pos      = 0;
    ret            = simulateOnline(m_maskedKey, &tapes[t], msgs, m_plaintext, pubKey, params);

    if (ret != 0) {
#if !defined(NDEBUG)
      printf("MPC simulation failed for round %" PRIu8 ", signature invalid\n", i);
#endif
      ret = -1;
      goto Exit;
    }
    commit_v(Cv.hashes[t], sig->proofs[t].input, msgs, params);
  }

  size_t missingLeavesSize = params->num_rounds - params->num_opened_rounds;
  uint16_t* missingLeaves  = getMissingLeavesList(sig->challengeC, params);
  ret = addMerkleNodes(&treeCv, missingLeaves, missingLeavesSize, sig->cvInfo, sig->cvInfoLen);
  free(missingLeaves);
  if (ret != 0) {
    ret = -1;
    goto Exit;
  }

  ret = verifyMerkleTree(&treeCv, Cv.hashes, sig->salt, params);
  if (ret != 0) {
    ret = -1;
    goto Exit;
  }

  /* Compute the challenge; two lists of integers */
  uint8_t challenge[MAX_DIGEST_SIZE];
  HCP(challenge, challengeC, challengeP, &Ch, treeCv.nodes, sig->salt, pubKey, plaintext, message,
      messageByteLength, params);

  /* Compare to challenge from signature */
  ret = memcmp(sig->challenge, challenge, params->digest_size);
  if (ret) {
#if !defined(NDEBUG)
    printf("Challenge does not match, signature invalid\n");
#endif
    ret = -1;
  }

Exit:
  for (size_t t = 0; t < params->num_rounds; t++) {
    freeRandomTape(&tapes[t]);
  }

  freeCommitments2(&Cv);
  freeCommitments2(&Ch);

free_seedstree:
  clearTree(&iSeedsTree);

free_tapes:
  free(tapes);
  free(challengeP);
  free(challengeC);

  /* free_cvtree: */
  clearTree(&treeCv);

free_msgs:
  freeMsgs(msgs);

free_commitmentsC:
  freeCommitments2(&C[3]);
  freeCommitments2(&C[2]);
  freeCommitments2(&C[1]);
  freeCommitments2(&C[0]);

  return ret;
}

static void computeSaltAndRootSeed(uint8_t* saltAndRoot, size_t saltAndRootLength,
                                   const uint8_t* privateKey, const uint8_t* pubKey,
                                   const uint8_t* plaintext, const uint8_t* message,
                                   size_t messageByteLength, const picnic_instance_t* params) {
  hash_context ctx;

  hash_init(&ctx, params->digest_size);
  hash_update(&ctx, privateKey, params->input_output_size);
  hash_update(&ctx, message, messageByteLength);
  hash_update(&ctx, pubKey, params->input_output_size);
  hash_update(&ctx, plaintext, params->input_output_size);
  hash_update_uint16_le(&ctx, (uint16_t)params->lowmc.n);
  hash_final(&ctx);
  hash_squeeze(&ctx, saltAndRoot, saltAndRootLength);
  hash_clear(&ctx);
}

static bool initialize_seeds_tree(tree_t* tree, const uint8_t* privateKey, const uint8_t* pubKey,
                                  const uint8_t* plaintext, const uint8_t* message,
                                  size_t messageByteLength, signature2_t* sig,
                                  const picnic_instance_t* params) {
  uint8_t saltAndRoot[MAX_SEED_SIZE + SALT_SIZE];
  computeSaltAndRootSeed(saltAndRoot, params->seed_size + SALT_SIZE, privateKey, pubKey, plaintext,
                         message, messageByteLength, params);
  memcpy(sig->salt, saltAndRoot, SALT_SIZE);
  return generateSeeds(tree, params->num_rounds, &saltAndRoot[SALT_SIZE], sig->salt, 0, params);
}

static int sign_picnic3(const uint8_t* privateKey, const uint8_t* pubKey, const uint8_t* plaintext,
                        const uint8_t* message, size_t messageByteLength, signature2_t* sig,
                        const picnic_instance_t* params) {
  assert(params->num_MPC_parties % 4 == 0);

  tree_t iSeedsTree;
  if (!initialize_seeds_tree(&iSeedsTree, privateKey, pubKey, plaintext, message, messageByteLength,
                             sig, params)) {
    return -1;
  }

  int ret         = -1;
  uint8_t* iSeeds = getLeaves(&iSeedsTree);
  if (!iSeeds) {
    goto free_seedstree;
  }

  randomTape_t* tapes = calloc(params->num_rounds, sizeof(randomTape_t));
  tree_t* seeds       = calloc(params->num_rounds, sizeof(tree_t));
  commitments_t* C    = allocateCommitments(params, 0);

  inputs_t inputs = allocateInputs(params);
  msgs_t* msgs    = allocateMsgs(params);
  if (!tapes || !seeds || !C || !inputs || !msgs) {
    goto free_msgs;
  }

  /* Commitments to the commitments and views */
  commitments_t Ch;
  allocateCommitments2(&Ch, params, params->num_rounds);
  commitments_t Cv;
  allocateCommitments2(&Cv, params, params->num_rounds);

  mzd_local_t m_plaintext[1];
  mzd_local_t m_maskedKey[1];

  mzd_from_char_array(m_plaintext, plaintext, params->input_output_size);

  lowmc_simulate_online_f simulateOnline = lowmc_simulate_online_get_implementation(&params->lowmc);

  for (size_t t = 0; t < params->num_rounds; t++) {
    if (!generateSeeds(&seeds[t], params->num_MPC_parties, &iSeeds[t * params->seed_size],
                       sig->salt, t, params)) {
      goto Exit;
    }
    createRandomTapes(&tapes[t], getLeaves(&seeds[t]), sig->salt, t, params);
    /* Preprocessing; compute aux tape for the N-th player, for each parallel rep */
    computeAuxTape(&tapes[t], inputs[t], params);
    /* Commit to seeds and aux bits */
    for (size_t j = 0; j < params->num_MPC_parties; j += 4) {
      const uint8_t* seed_ptr[4] = {getLeaf(&seeds[t], j + 0), getLeaf(&seeds[t], j + 1),
                                    getLeaf(&seeds[t], j + 2), getLeaf(&seeds[t], j + 3)};
      commit_x4(C[t].hashes + j, seed_ptr, sig->salt, t, j, params);
    }
    const size_t last = params->num_MPC_parties - 1;
    commit(C[t].hashes[last], getLeaf(&seeds[t], last), tapes[t].aux_bits, sig->salt, t, last,
           params);
  }

  for (uint16_t t = 0; t < params->num_rounds; t++) {
    /* Simulate the online phase of the MPC */
    uint8_t* maskedKey = inputs[t];

    xor_byte_array(maskedKey, maskedKey, privateKey,
                   params->input_output_size); // maskedKey += privateKey
    for (size_t i = params->lowmc.n; i < params->input_output_size * 8; i++) {
      setBit(maskedKey, i, 0);
    }
    mzd_from_char_array(m_maskedKey, maskedKey, params->input_output_size);

    int rv = simulateOnline(m_maskedKey, &tapes[t], &msgs[t], m_plaintext, pubKey, params);
    if (rv != 0) {
#if !defined(NDEBUG)
      printf("MPC simulation failed in round %" PRIu16 ", aborting signature\n", t);
#endif
      goto Exit;
    }
  }

  /* Commit to the commitments and views */
  {
    size_t t = 0;
    for (; t < params->num_rounds / 4 * 4; t += 4) {
      commit_h_x4(&Ch.hashes[t], &C[t], params);
      commit_v_x4(&Cv.hashes[t], (const uint8_t**)&inputs[t], &msgs[t], params);
    }
    for (; t < params->num_rounds; t++) {
      commit_h(Ch.hashes[t], &C[t], params);
      commit_v(Cv.hashes[t], inputs[t], &msgs[t], params);
    }
  }
  /* Create a Merkle tree with Cv as the leaves */
  tree_t treeCv;
  if (!createTree(&treeCv, params->num_rounds, params->digest_size)) {
    goto Exit;
  }
  buildMerkleTree(&treeCv, Cv.hashes, sig->salt, params);

  /* Compute the challenge; two lists of integers */
  uint16_t* challengeC = sig->challengeC;
  uint16_t* challengeP = sig->challengeP;
  HCP(sig->challenge, challengeC, challengeP, &Ch, treeCv.nodes, sig->salt, pubKey, plaintext,
      message, messageByteLength, params);

  /* Send information required for checking commitments with Merkle tree.
   * The commitments the verifier will be missing are those not in challengeC. */
  size_t missingLeavesSize = params->num_rounds - params->num_opened_rounds;
  uint16_t* missingLeaves  = getMissingLeavesList(challengeC, params);
  size_t cvInfoLen         = 0;
  uint8_t* cvInfo          = openMerkleTree(&treeCv, missingLeaves, missingLeavesSize, &cvInfoLen);
  free(missingLeaves);
  clearTree(&treeCv);

  if (!cvInfo) {
    goto Exit;
  }
  sig->cvInfo    = cvInfo;
  sig->cvInfoLen = cvInfoLen;

  /* Reveal iSeeds for unopened rounds, those in {0..T-1} \ ChallengeC. */
  sig->iSeedInfo    = malloc(params->num_rounds * params->seed_size);
  sig->iSeedInfoLen = revealSeeds(&iSeedsTree, challengeC, params->num_opened_rounds,
                                  sig->iSeedInfo, params->num_rounds * params->seed_size, params);
  sig->iSeedInfo    = realloc(sig->iSeedInfo, sig->iSeedInfoLen);

  /* Assemble the proof */
  proof2_t* proofs = sig->proofs;
  for (size_t t = 0; t < params->num_rounds; t++) {
    if (contains(challengeC, params->num_opened_rounds, t)) {
      allocateProof2(&proofs[t], params);
      size_t P_index          = indexOf(challengeC, params->num_opened_rounds, t);
      proofs[t].unOpenedIndex = challengeP[P_index];

      uint16_t hideList[1];
      hideList[0]           = challengeP[P_index];
      proofs[t].seedInfo    = malloc(params->num_MPC_parties * params->seed_size);
      proofs[t].seedInfoLen = revealSeeds(&seeds[t], hideList, 1, proofs[t].seedInfo,
                                          params->num_MPC_parties * params->seed_size, params);
      proofs[t].seedInfo    = realloc(proofs[t].seedInfo, proofs[t].seedInfoLen);

      size_t last = params->num_MPC_parties - 1;
      if (challengeP[P_index] != last) {
        memcpy(proofs[t].aux, tapes[t].aux_bits, params->view_size);
      }

      memcpy(proofs[t].input, inputs[t], params->input_output_size);
      memcpy(proofs[t].msgs, msgs[t].msgs[challengeP[P_index]], params->view_size);

      /* recompute commitment of unopened party since we did not store it for memory optimization
       */
      if (proofs[t].unOpenedIndex == params->num_MPC_parties - 1) {
        commit(proofs[t].C, getLeaf(&seeds[t], proofs[t].unOpenedIndex), tapes[t].aux_bits,
               sig->salt, t, proofs[t].unOpenedIndex, params);
      } else {
        commit(proofs[t].C, getLeaf(&seeds[t], proofs[t].unOpenedIndex), NULL, sig->salt, t,
               proofs[t].unOpenedIndex, params);
      }
    }
  }
  ret = 0;

Exit:
  for (size_t t = 0; t < params->num_rounds; t++) {
    freeRandomTape(&tapes[t]);
    clearTree(&seeds[t]);
  }
  freeCommitments2(&Cv);
  freeCommitments2(&Ch);

free_msgs:
  freeMsgs(msgs);
  freeInputs(inputs);
  freeCommitments(C);
  free(seeds);
  free(tapes);

free_seedstree:
  clearTree(&iSeedsTree);

  return ret;
}

static int arePaddingBitsZero(uint8_t* data, size_t byteLength, size_t bitLength) {
  return !check_padding_bits(data[byteLength - 1], byteLength * 8 - bitLength);
}

static int deserializeSignature2(signature2_t* sig, const uint8_t* sigBytes, size_t sigBytesLen,
                                 const picnic_instance_t* params) {
  /* Read the challenge and salt */
  size_t bytesRequired = params->digest_size + SALT_SIZE;

  if (sigBytesLen < bytesRequired) {
    return EXIT_FAILURE;
  }

  memcpy(sig->challenge, sigBytes, params->digest_size);
  sigBytes += params->digest_size;
  memcpy(sig->salt, sigBytes, SALT_SIZE);
  sigBytes += SALT_SIZE;

  expandChallenge(sig->challengeC, sig->challengeP, sig->challenge, params);

  /* Add size of iSeeds tree data */
  sig->iSeedInfoLen =
      revealSeedsSize(params->num_rounds, sig->challengeC, params->num_opened_rounds, params);
  if (sig->iSeedInfoLen == SIZE_MAX) {
    return EXIT_FAILURE;
  }
  bytesRequired += sig->iSeedInfoLen;

  /* Add the size of the Cv Merkle tree data */
  size_t missingLeavesSize = params->num_rounds - params->num_opened_rounds;
  uint16_t* missingLeaves  = getMissingLeavesList(sig->challengeC, params);
  sig->cvInfoLen = openMerkleTreeSize(params->num_rounds, missingLeaves, missingLeavesSize, params);
  bytesRequired += sig->cvInfoLen;
  free(missingLeaves);
  if (sig->cvInfoLen == SIZE_MAX) {
    return EXIT_FAILURE;
  }

  /* Compute the number of bytes required for the proofs */
  uint16_t hideList[1] = {0};
  size_t seedInfoLen   = revealSeedsSize(params->num_MPC_parties, hideList, 1, params);
  if (seedInfoLen == SIZE_MAX) {
    return EXIT_FAILURE;
  }

  for (size_t t = 0; t < params->num_rounds; t++) {
    if (contains(sig->challengeC, params->num_opened_rounds, t)) {
      uint16_t P_t = sig->challengeP[indexOf(sig->challengeC, params->num_opened_rounds, t)];
      if (P_t != (params->num_MPC_parties - 1u)) {
        bytesRequired += params->view_size;
      }
      bytesRequired += params->digest_size;
      bytesRequired += params->input_output_size;
      bytesRequired += params->view_size;
      bytesRequired += seedInfoLen;
    }
  }

  /* Fail if the signature does not have the exact number of bytes we expect */
  if (sigBytesLen != bytesRequired) {
#if !defined(NDEBUG)
    printf("%s: sigBytesLen = " SIZET_FMT ", expected bytesRequired = " SIZET_FMT "\n", __func__,
           sigBytesLen, bytesRequired);
#endif
    return EXIT_FAILURE;
  }

  sig->iSeedInfo = malloc(sig->iSeedInfoLen);
  memcpy(sig->iSeedInfo, sigBytes, sig->iSeedInfoLen);
  sigBytes += sig->iSeedInfoLen;

  sig->cvInfo = malloc(sig->cvInfoLen);
  memcpy(sig->cvInfo, sigBytes, sig->cvInfoLen);
  sigBytes += sig->cvInfoLen;

  /* Read the proofs */
  for (size_t t = 0; t < params->num_rounds; t++) {
    if (contains(sig->challengeC, params->num_opened_rounds, t)) {
      allocateProof2(&sig->proofs[t], params);
      sig->proofs[t].seedInfoLen = seedInfoLen;
      sig->proofs[t].seedInfo    = malloc(sig->proofs[t].seedInfoLen);
      memcpy(sig->proofs[t].seedInfo, sigBytes, sig->proofs[t].seedInfoLen);
      sigBytes += sig->proofs[t].seedInfoLen;

      uint16_t P_t = sig->challengeP[indexOf(sig->challengeC, params->num_opened_rounds, t)];
      if (P_t != (params->num_MPC_parties - 1u)) {
        memcpy(sig->proofs[t].aux, sigBytes, params->view_size);
        sigBytes += params->view_size;
        if (!arePaddingBitsZero(sig->proofs[t].aux, params->view_size,
                                3 * params->lowmc.r * params->lowmc.m)) {
#if !defined(NDEBUG)
          printf("%s: failed while deserializing aux bits\n", __func__);
#endif
          return -1;
        }
      }

      memcpy(sig->proofs[t].input, sigBytes, params->input_output_size);
      if (!arePaddingBitsZero(sig->proofs[t].input, params->input_output_size, params->lowmc.n)) {
#if !defined(NDEBUG)
        printf("%s: failed while deserializing input bits\n", __func__);
#endif
        return -1;
      }
      sigBytes += params->input_output_size;

      size_t msgsByteLength = params->view_size;
      memcpy(sig->proofs[t].msgs, sigBytes, msgsByteLength);
      sigBytes += msgsByteLength;
      size_t msgsBitLength = 3 * params->lowmc.r * params->lowmc.m;
      if (!arePaddingBitsZero(sig->proofs[t].msgs, msgsByteLength, msgsBitLength)) {
#if !defined(NDEBUG)
        printf("%s: failed while deserializing msgs bits\n", __func__);
#endif
        return -1;
      }

      memcpy(sig->proofs[t].C, sigBytes, params->digest_size);
      sigBytes += params->digest_size;
    }
  }

  return EXIT_SUCCESS;
}

static size_t required_signature_size(const signature2_t* sig, const picnic_instance_t* params) {
  /* Compute the number of bytes required for the signature */
  size_t bytesRequired = params->digest_size + SALT_SIZE; /* challenge and salt */

  /* Encode only iSeedInfo, the length will be recomputed by deserialize */
  bytesRequired += sig->iSeedInfoLen;
  bytesRequired += sig->cvInfoLen;

  const size_t per_round = params->digest_size + params->input_output_size + params->view_size;
  for (uint16_t t = 0; t < params->num_rounds; t++) { /* proofs */
    if (contains(sig->challengeC, params->num_opened_rounds, t)) {
      uint16_t P_t = sig->challengeP[indexOf(sig->challengeC, params->num_opened_rounds, t)];
      bytesRequired += sig->proofs[t].seedInfoLen;
      if (P_t != (params->num_MPC_parties - 1u)) {
        bytesRequired += params->view_size;
      }
      bytesRequired += per_round;
    }
  }

  return bytesRequired;
}

static int serializeSignature2(const signature2_t* sig, uint8_t* sigBytes, size_t sigBytesLen,
                               const picnic_instance_t* params) {
  size_t required_bytes = required_signature_size(sig, params);
  if (sigBytesLen < required_bytes) {
    return -1;
  }

  memcpy(sigBytes, sig->challenge, params->digest_size);
  sigBytes += params->digest_size;

  memcpy(sigBytes, sig->salt, SALT_SIZE);
  sigBytes += SALT_SIZE;

  memcpy(sigBytes, sig->iSeedInfo, sig->iSeedInfoLen);
  sigBytes += sig->iSeedInfoLen;
  memcpy(sigBytes, sig->cvInfo, sig->cvInfoLen);
  sigBytes += sig->cvInfoLen;

  /* Write the proofs */
  for (uint16_t t = 0; t < params->num_rounds; t++) {
    if (contains(sig->challengeC, params->num_opened_rounds, t)) {
      memcpy(sigBytes, sig->proofs[t].seedInfo, sig->proofs[t].seedInfoLen);
      sigBytes += sig->proofs[t].seedInfoLen;

      uint16_t P_t = sig->challengeP[indexOf(sig->challengeC, params->num_opened_rounds, t)];
      if (P_t != (params->num_MPC_parties - 1u)) {
        memcpy(sigBytes, sig->proofs[t].aux, params->view_size);
        sigBytes += params->view_size;
      }

      memcpy(sigBytes, sig->proofs[t].input, params->input_output_size);
      sigBytes += params->input_output_size;

      memcpy(sigBytes, sig->proofs[t].msgs, params->view_size);
      sigBytes += params->view_size;

      memcpy(sigBytes, sig->proofs[t].C, params->digest_size);
      sigBytes += params->digest_size;
    }
  }

  return required_bytes;
}

int impl_sign_picnic3(const picnic_instance_t* instance, const uint8_t* plaintext,
                      const uint8_t* private_key, const uint8_t* public_key, const uint8_t* msg,
                      size_t msglen, uint8_t* signature, size_t* signature_len) {
  signature2_t sig;
  int ret = -1;
  if (!allocateSignature2(&sig, instance)) {
    goto Exit;
  }

  ret = sign_picnic3(private_key, public_key, plaintext, msg, msglen, &sig, instance);
  picnic_declassify(&ret, sizeof(ret));
  if (ret != EXIT_SUCCESS) {
#if !defined(NDEBUG)
    fprintf(stderr, "Failed to create signature\n");
    fflush(stderr);
#endif
    goto Exit;
  }

  ret = serializeSignature2(&sig, signature, *signature_len, instance);
  if (ret == -1) {
#if !defined(NDEBUG)
    fprintf(stderr, "Failed to serialize signature\n");
    fflush(stderr);
#endif
    goto Exit;
  }
  *signature_len = ret;
  ret            = 0;

Exit:
  freeSignature2(&sig, instance);
  return ret;
}

int impl_verify_picnic3(const picnic_instance_t* instance, const uint8_t* plaintext,
                        const uint8_t* public_key, const uint8_t* msg, size_t msglen,
                        const uint8_t* signature, size_t signature_len) {
  int ret = -1;
  signature2_t sig;
  if (!allocateSignature2(&sig, instance)) {
    goto Exit;
  }

  ret = deserializeSignature2(&sig, signature, signature_len, instance);
  if (ret != EXIT_SUCCESS) {
#if !defined(NDEBUG)
    fprintf(stderr, "Failed to deserialize signature\n");
    fflush(stderr);
#endif
    goto Exit;
  }

  ret = verify_picnic3(&sig, public_key, plaintext, msg, msglen, instance);

Exit:
  freeSignature2(&sig, instance);
  return ret;
}

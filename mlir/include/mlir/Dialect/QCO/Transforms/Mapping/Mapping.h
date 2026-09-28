/*
 * Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
 * Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
 * All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Licensed under the MIT License
 */

#pragma once

#include "mlir/Dialect/QCO/Transforms/Passes.h"

#include <mlir/Pass/Pass.h>
#include <mlir/Support/LLVM.h>

#include <cstddef>
#include <memory>

namespace mlir {

class CompilerTarget;

namespace qco {

/**
 * @brief Create a mapping pass instance for a compiler target.
 * @returns a pass object.
 */
std::unique_ptr<Pass> createMappingPass(const CompilerTarget& target,
                                        MappingPassOptions options);

/**
 * @brief Return whether a SWAP between hardware sites `a` and `b` acts on
 * exactly the same pair of sites that a two-qubit gate has just acted on,
 * with nothing in between (including a single-qubit gate, `qco.reset`,
 * `qco.measure`, or a barrier) touching either site.
 * @details `lastTwoQubitGate` and `lastAnyGate` are indexed by hardware
 * site: the most recent two-qubit gate, respectively the most recent
 * operation of any arity, to have touched that site, or null if none has.
 * The pair qualifies when the most recent two-qubit gate on site `a` is a
 * real operation, is the same operation as the most recent two-qubit gate
 * on site `b`, and is also the most recent operation of any arity on each
 * of the two sites --- that last condition is what enforces "nothing in
 * between", since any later operation on either site would have
 * overwritten its `lastAnyGate` entry.
 *
 * Exposed as a free function, independent of `MappingPass`'s internal
 * routing state, so this predicate can be unit-tested directly against
 * hand-constructed inputs rather than only indirectly through a full,
 * randomized routing run. See
 * `.agent/plans/gate-adjacent-swap-bias.md`, Milestone 1.
 */
[[nodiscard]] bool isGateAdjacentSwap(ArrayRef<Operation*> lastTwoQubitGate,
                                      ArrayRef<Operation*> lastAnyGate,
                                      size_t a, size_t b);

} // namespace qco
} // namespace mlir

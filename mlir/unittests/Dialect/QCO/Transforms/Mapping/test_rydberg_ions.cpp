/*
 * Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
 * Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
 * All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Licensed under the MIT License
 */

#include "mlir/Compiler/Target.h"
#include "mlir/Dialect/QCO/Builder/QCOProgramBuilder.h"
#include "mlir/Dialect/QCO/IR/QCODialect.h"
#include "mlir/Dialect/QCO/IR/QCOInterfaces.h"
#include "mlir/Dialect/QCO/IR/QCOOps.h"
#include "mlir/Dialect/QCO/Transforms/Mapping/Mapping.h"
#include "mlir/Dialect/QCO/Transforms/Passes.h"
#include "mlir/Dialect/QTensor/IR/QTensorDialect.h"
#include "mlir/Dialect/QTensor/IR/QTensorOps.h"
#include "mlir/Dialect/Utils/Utils.h"
#include "mlir/Support/Passes.h"

#include <gtest/gtest.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/Sequence.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/TypeSwitch.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/LogicalResult.h>
#include <mlir/Dialect/Arith/IR/Arith.h>
#include <mlir/Dialect/Func/IR/FuncOps.h>
#include <mlir/Dialect/SCF/IR/SCF.h>
#include <mlir/IR/BuiltinOps.h>
#include <mlir/IR/Diagnostics.h>
#include <mlir/IR/DialectRegistry.h>
#include <mlir/IR/Location.h>
#include <mlir/IR/OwningOpRef.h>
#include <mlir/IR/PatternMatch.h>
#include <mlir/IR/Types.h>
#include <mlir/IR/Value.h>
#include <mlir/IR/ValueRange.h>
#include <mlir/IR/Verifier.h>
#include <mlir/Parser/Parser.h>
#include <mlir/Pass/PassManager.h>
#include <mlir/Support/LLVM.h>
#include <mlir/Transforms/GreedyPatternRewriteDriver.h>
#include <mlir/Transforms/Passes.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace mlir;
using namespace mlir::qco;
using namespace mlir::utils;

static SmallVector<Value> getQubitValues(ValueRange values) {
  return to_vector(llvm::make_filter_range(
      values, [](Value value) { return isa<QubitType>(value.getType()); }));
}

namespace {

class RydbergIonMappingPassFixture : public testing::Test {
protected:
  void SetUp() override {
    DialectRegistry registry;
    registry.insert<QCODialect, qtensor::QTensorDialect, scf::SCFDialect,
                    arith::ArithDialect, func::FuncDialect>();
    context = std::make_unique<MLIRContext>();
    context->appendDialectRegistry(registry);
    context->loadAllAvailableDialects();
  }

  static LogicalResult runPass(ModuleOp m, const CompilerTarget& target,
                               const MappingPassOptions& options) {
    PassManager pm(m->getContext());
    pm.addPass(createMappingPass(target, options));
    if (failed(pm.run(m))) {
      return failure();
    }

    RewritePatternSet patterns(m.getContext());
    SinkOp::getCanonicalizationPatterns(patterns, m.getContext());
    return applyPatternsGreedily(m, std::move(patterns));
  }

  std::unique_ptr<MLIRContext> context;
};

class RydbergIonMappingPassTest
    : public RydbergIonMappingPassFixture,
      public testing::WithParamInterface<CompilerTarget> {};

}; // namespace

/// Return true, if the operations within a region fulfill the given coupling
/// constraints.
///
/// Unlike `test_mapping.cpp`'s own `isExecutable`, this generalizes the
/// two-qubit-gate check to any arity: it gathers every input qubit's mapped
/// hardware site and requires every pair among them to be mutually adjacent
/// (a single pair for a two-qubit gate; a "clique"/triangle check for a
/// native three-qubit gate). This circuit never produces nested regions
/// (`scf.for`/`scf.while`/`qco.if`/`qco.index_switch`), so unlike
/// `test_mapping.cpp`'s version, this one does not need to handle them.
static bool isExecutable(Region& body,
                         DenseMap<Value, CompilerTarget::SiteId>& m,
                         const CompilerTarget& target) {
  for (Operation& op : body.getOps()) {
    if (auto staticOp = dyn_cast<StaticOp>(op)) {
      m.try_emplace(staticOp.getQubit(), staticOp.getIndex());
      continue;
    }

    if (auto unitaryOp = dyn_cast<UnitaryOpInterface>(op)) {
      if (!isa<BarrierOp>(op) && unitaryOp.getNumQubits() > 1) {
        SmallVector<CompilerTarget::SiteId, 3> sites;
        sites.reserve(unitaryOp.getNumQubits());
        for (const Value qubit : unitaryOp.getInputQubits()) {
          sites.push_back(m.at(qubit));
        }

        for (size_t i = 0; i < sites.size(); ++i) {
          for (size_t j = i + 1; j < sites.size(); ++j) {
            const auto vertexA = target.vertexForSite(sites[i]);
            const auto vertexB = target.vertexForSite(sites[j]);
            if (!vertexA || !vertexB ||
                !target.areAdjacent(*vertexA, *vertexB)) {
              llvm::dbgs() << "The multi-qubit gate (" << sites[i] << ", "
                           << sites[j] << ") is not executable: \n";
              unitaryOp->dump();
              return false;
            }
          }
        }
      }

      for (const auto [pred, succ] : llvm::zip_equal(
               unitaryOp.getInputQubits(), unitaryOp.getOutputQubits())) {
        const auto hw = m.at(pred);
        m.try_emplace(succ, hw);
      }

      continue;
    }

    if (auto resetOp = dyn_cast<ResetOp>(op)) {
      const auto hw = m.at(resetOp.getQubitIn());
      m.try_emplace(resetOp.getQubitOut(), hw);
      continue;
    }

    if (auto measOp = dyn_cast<MeasureOp>(op)) {
      const auto hw = m.at(measOp.getQubitIn());
      m.try_emplace(measOp.getQubitOut(), hw);
      continue;
    }
  }

  return true;
}

/// Return true, if the entry point fulfills the given coupling constraints.
static bool isExecutable(func::FuncOp entry, const CompilerTarget& target) {
  DenseMap<Value, CompilerTarget::SiteId> m;
  return isExecutable(entry.getFunctionBody(), m, target);
}

/// Return the 12-qubit Rydberg-ion compiler target described by the task: a
/// mostly-linear chain of sites with three triangular "native-gate zones" at
/// {2, 3, 4}, {5, 6, 7}, and {8, 9, 10}, explicitly declaring CCX and CCZ as
/// native three-qubit operations.
///
/// Couplings: 0<>1, 1<>2, 2<>3, 2<>4, 3<>4, 4<>5, 5<>6, 5<>7, 6<>7, 7<>8,
/// 8<>9, 8<>10, 9<>10, 10<>11.
static CompilerTarget getRydbergIonTarget() {
  constexpr size_t numQubits = 12;
  const std::vector<CompilerTarget::Coupling> couplings{
      {0, 1}, {1, 2}, {2, 3}, {2, 4}, {3, 4},  {4, 5},  {5, 6},
      {5, 7}, {6, 7}, {7, 8}, {8, 9}, {8, 10}, {9, 10}, {10, 11},
  };
  const std::vector<CompilerTarget::Operation> operations{
      CompilerTarget::Operation("ccx", 3, 0),
      CompilerTarget::Operation("ccz", 3, 0),
  };
  return CompilerTarget(numQubits, couplings, operations);
}

//===----------------------------------------------------------------------===//
// The Bacon-Shor code QEC memory circuit, mirroring
// `find_acceptable_swaps.py`/`circuits.py`/`ftqc_circuits.py`'s
// `BaconShorCodeCircuitGenerator`: 9 data ("state") qubits encoding one
// logical qubit, and 3 QEC ancilla qubits used to extract X and Z syndromes
// and apply the corresponding corrections. Program qubits are allocated data
// qubits first, then ancilla qubits, so program index order matches the
// Python reference's `state_qubits + qec_ancillas` concatenation used for the
// canonical qubit ids there. The CCZ/CCX correction gates stay atomic
// three-qubit `qco.ctrl` operations all the way through routing; no
// decomposition pass runs anywhere in this file.
//
// This is a QEC memory round only: it never measures its own qubits (that is
// a separate concern, used only alongside some other, not-built-here
// state-preparation circuit to check data integrity before/after a memory
// round), so this file's qubits are always sunk directly, unmeasured.
//===----------------------------------------------------------------------===//

namespace {

/// The X-type gauge/stabilizer generators (eqn. Sx_1, Sx_2, Sx_3), indexed by
/// the ancilla each acts through, listing the data qubits it targets.
constexpr std::array<std::array<int64_t, 6>, 3> kSx{{
    {0, 1, 3, 4, 6, 7},
    {1, 2, 4, 5, 7, 8},
    {0, 2, 3, 5, 6, 8},
}};

/// The Z-type gauge/stabilizer generators (eqn. Sz_1, Sz_2, Sz_3), indexed by
/// the ancilla each acts through, listing the data qubits that control it.
constexpr std::array<std::array<int64_t, 6>, 3> kSz{{
    {0, 3, 1, 4, 2, 5},
    {3, 6, 4, 7, 5, 8},
    {0, 6, 1, 7, 2, 8},
}};

/// Tracks the current SSA values of the Bacon-Shor code's 9 data qubits and 3
/// QEC ancilla qubits as gates are appended.
struct BaconShorCircuit {
  SmallVector<Value> dataQubits = SmallVector<Value>(9);
  SmallVector<Value> ancillaQubits = SmallVector<Value>(3);
};

/// Mirrors `BaconShorCodeCircuitGenerator.x_syndrome_circuit`.
void buildXSyndromeCircuit(QCOProgramBuilder& builder,
                           BaconShorCircuit& circuit) {
  for (Value& anc : circuit.ancillaQubits) {
    anc = builder.reset(anc);
    anc = builder.h(anc);
  }

  for (size_t control = 0; control < kSx.size(); ++control) {
    for (const int64_t target : kSx[control]) {
      std::tie(circuit.ancillaQubits[control],
               circuit.dataQubits[static_cast<size_t>(target)]) =
          builder.cx(circuit.ancillaQubits[control],
                     circuit.dataQubits[static_cast<size_t>(target)]);
    }
  }

  for (Value& anc : circuit.ancillaQubits) {
    anc = builder.h(anc);
  }
}

/// Mirrors `BaconShorCodeCircuitGenerator.correct_x_circuit`: applies a CCZ
/// (as a two-controlled Z) with two ancilla controls and a data-qubit target
/// for each of the three X-correction triples. CCZ is symmetric under
/// permutation of its three qubits, so the control/target split below is
/// equivalent to the Python reference's unordered `ccz(...)` triples.
void buildXCorrectionCircuit(QCOProgramBuilder& builder,
                             BaconShorCircuit& circuit) {
  const auto ccz = [&](size_t dataIdx, size_t anc0, size_t anc1) {
    auto [controlsOut, targetOut] =
        builder.mcz({circuit.ancillaQubits[anc0], circuit.ancillaQubits[anc1]},
                    circuit.dataQubits[dataIdx]);
    circuit.ancillaQubits[anc0] = controlsOut[0];
    circuit.ancillaQubits[anc1] = controlsOut[1];
    circuit.dataQubits[dataIdx] = targetOut;
  };

  ccz(7, 0, 1);
  ccz(6, 0, 2);
  ccz(5, 1, 2);
}

/// Mirrors `BaconShorCodeCircuitGenerator.z_syndrome_circuit`.
void buildZSyndromeCircuit(QCOProgramBuilder& builder,
                           BaconShorCircuit& circuit) {
  for (Value& anc : circuit.ancillaQubits) {
    anc = builder.reset(anc);
  }

  for (size_t target = 0; target < kSz.size(); ++target) {
    for (const int64_t control : kSz[target]) {
      std::tie(circuit.dataQubits[static_cast<size_t>(control)],
               circuit.ancillaQubits[target]) =
          builder.cx(circuit.dataQubits[static_cast<size_t>(control)],
                     circuit.ancillaQubits[target]);
    }
  }
}

/// Mirrors `BaconShorCodeCircuitGenerator.correct_z_circuit`: applies a CCX
/// (Toffoli) with two ancilla controls and a data-qubit target for each of
/// the three Z-correction triples, matching the Python reference's
/// `ccx(ancilla, ancilla, data)` argument order exactly, since CCX
/// distinguishes its target from its controls.
void buildZCorrectionCircuit(QCOProgramBuilder& builder,
                             BaconShorCircuit& circuit) {
  const auto ccx = [&](size_t anc0, size_t anc1, size_t dataIdx) {
    auto [controlsOut, targetOut] =
        builder.mcx({circuit.ancillaQubits[anc0], circuit.ancillaQubits[anc1]},
                    circuit.dataQubits[dataIdx]);
    circuit.ancillaQubits[anc0] = controlsOut[0];
    circuit.ancillaQubits[anc1] = controlsOut[1];
    circuit.dataQubits[dataIdx] = targetOut;
  };

  ccx(0, 1, 5);
  ccx(0, 2, 2);
  ccx(1, 2, 8);

  for (Value& anc : circuit.ancillaQubits) {
    anc = builder.reset(anc);
  }
}

/// Builds the full Bacon-Shor QEC round: X-syndrome extraction, X-correction,
/// Z-syndrome extraction, Z-correction, mirroring
/// `find_acceptable_swaps.py`'s `full_circuit = x_syndrome_circuit +
/// correct_x_circuit + z_syndrome_circuit + correct_z_circuit`.
BaconShorCircuit buildBaconShorCircuit(QCOProgramBuilder& builder) {
  BaconShorCircuit circuit;
  for (Value& q : circuit.dataQubits) {
    q = builder.allocQubit();
  }
  for (Value& q : circuit.ancillaQubits) {
    q = builder.allocQubit();
  }

  buildXSyndromeCircuit(builder, circuit);
  buildXCorrectionCircuit(builder, circuit);
  buildZSyndromeCircuit(builder, circuit);
  buildZCorrectionCircuit(builder, circuit);

  return circuit;
}

/// Build the full Bacon-Shor QEC round on a fresh `QCOProgramBuilder`, then
/// measure and sink all 12 qubits, returning the finalized module together
/// with the classical bit values.
///
/// The measurement here is test instrumentation, not part of the Bacon-Shor
/// circuit's own semantics (`buildBaconShorCircuit` above never measures
/// anything, matching the Python reference exactly): without it, the whole
/// computation has no externally observable effect (QCO's gates are `Pure`,
/// and nothing else consumes the classical return value), so the fixture's
/// `runPass` helper's post-pass `SinkOp` canonicalization cleanup legitimately
/// treats the entire circuit — every gate, and any SWAPs the mapping pass
/// inserted — as dead code and erases all of it, silently turning every
/// assertion below into a vacuous pass over an empty function. Measuring
/// forces the routed gates to remain observable so the tests below actually
/// exercise them.
static OwningOpRef<ModuleOp>
buildAndFinalizeBaconShorProgram(MLIRContext* ctx) {
  QCOProgramBuilder builder(ctx);
  constexpr int64_t numQubits = 12;
  builder.initialize(SmallVector<Type>(numQubits, builder.getI1Type()));

  BaconShorCircuit circuit = buildBaconShorCircuit(builder);

  SmallVector<Value> bits(numQubits);
  for (size_t i = 0; i < circuit.dataQubits.size(); ++i) {
    std::tie(circuit.dataQubits[i], bits[i]) =
        builder.measure(circuit.dataQubits[i]);
  }
  for (size_t i = 0; i < circuit.ancillaQubits.size(); ++i) {
    std::tie(circuit.ancillaQubits[i], bits[9 + i]) =
        builder.measure(circuit.ancillaQubits[i]);
  }
  for (const Value q : circuit.dataQubits) {
    builder.sink(q);
  }
  for (const Value q : circuit.ancillaQubits) {
    builder.sink(q);
  }

  return builder.finalize(bits);
}

} // namespace

TEST_F(RydbergIonMappingPassFixture, MapBaconShorCodeOnRydbergIonTarget) {
  const auto target = getRydbergIonTarget();

  auto m = buildAndFinalizeBaconShorProgram(context.get());
  ASSERT_TRUE(succeeded(verify(*m)));

  // Label the 9 data qubits "B" (data) and the 3 QEC ancillas "A"
  // (auxiliary), in program-allocation order, and route with the
  // state-dependent A/B swap heuristic enabled. The CCZ/CCX gates are native
  // on this target (see getRydbergIonTarget), so no decomposition pass runs
  // here at all.
  const std::string qubitTypeLabels = std::string(9, 'B') + std::string(3, 'A');
  ASSERT_TRUE(runPass(m.get(), target,
                      MappingPassOptions{.ntrials = 1,
                                         .qubitTypeLabels = qubitTypeLabels})
                  .succeeded());
  ASSERT_TRUE(succeeded(verify(*m)));
  EXPECT_TRUE(isExecutable(getEntryPoint(m.get()), target));

  size_t numSwaps = 0;
  m->walk([&](SWAPOp) { ++numSwaps; });
  EXPECT_GT(numSwaps, 0U);

  // `builder.cx`/`cz` are themselves built as a `CtrlOp` with one control
  // (see `CompilerTarget::supports`'s one-control special case), so only
  // count `CtrlOp`s with two controls (the CCX/CCZ shape) here, not every
  // `CtrlOp` in the module.
  size_t numNativeMultiQubitGates = 0;
  m->walk([&](CtrlOp ctrl) {
    if (ctrl.getNumControls() != 2) {
      return;
    }
    ++numNativeMultiQubitGates;
    EXPECT_EQ(ctrl.getNumQubits(), 3U);
  });
  EXPECT_EQ(numNativeMultiQubitGates, 6U)
      << "expected all 6 CCZ/CCX gates to survive routing as undecomposed, "
         "two-control CtrlOps";
}

TEST_F(RydbergIonMappingPassFixture,
       MapBaconShorCodeOnRydbergIonTargetWithDefaultCost) {
  const auto target = getRydbergIonTarget();

  auto m = buildAndFinalizeBaconShorProgram(context.get());
  ASSERT_TRUE(succeeded(verify(*m)));

  // Leaving `qubitTypeLabels` unset (the default) must still compile the
  // same circuit successfully, reproducing the pass's previous behavior.
  ASSERT_TRUE(
      runPass(m.get(), target, MappingPassOptions{.ntrials = 1}).succeeded());
  ASSERT_TRUE(succeeded(verify(*m)));
  EXPECT_TRUE(isExecutable(getEntryPoint(m.get()), target));
}

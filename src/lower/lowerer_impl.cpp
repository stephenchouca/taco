#include "taco/lower/lowerer_impl.h"

#include "taco/index_notation/index_notation.h"
#include "taco/index_notation/index_notation_nodes.h"
#include "taco/index_notation/index_notation_visitor.h"
#include "taco/ir/ir.h"
#include "ir/ir_generators.h"
#include "taco/ir/ir_visitor.h"
#include "taco/ir/ir_rewriter.h"
#include "taco/ir/simplify.h"
#include "taco/lower/iterator.h"
#include "taco/lower/merge_lattice.h"
#include "mode_access.h"
#include "taco/util/collections.h"
#include "taco/util/name_generator.h"
#include "taco/lower/mode_format_impl.h"

#include <algorithm>

using namespace std;
using namespace taco::ir;
using taco::util::combine;

namespace taco {

class LowererImpl::Visitor : public IndexNotationVisitorStrict {
public:
  Visitor(LowererImpl* impl) : impl(impl) {}
  Stmt lower(IndexStmt stmt) {
    this->stmt = Stmt();
    impl->accessibleIterators.scope();
    IndexStmtVisitorStrict::visit(stmt);
    impl->accessibleIterators.unscope();
    return this->stmt;
  }
  Expr lower(IndexExpr expr) {
    this->expr = Expr();
    IndexExprVisitorStrict::visit(expr);
    return this->expr;
  }
private:
  LowererImpl* impl;
  Expr expr;
  Stmt stmt;
  using IndexNotationVisitorStrict::visit;
  void visit(const AssignmentNode* node)    { stmt = impl->lowerAssignment(node); }
  void visit(const YieldNode* node)         { stmt = impl->lowerYield(node); }
  void visit(const ForallNode* node)        { stmt = impl->lowerForall(node); }
  void visit(const WhereNode* node)         { stmt = impl->lowerWhere(node); }
  void visit(const MultiNode* node)         { stmt = impl->lowerMulti(node); }
  void visit(const SequenceNode* node)      { stmt = impl->lowerSequence(node); }
  void visit(const AccessNode* node)        { expr = impl->lowerAccess(node); }
  void visit(const LiteralNode* node)       { expr = impl->lowerLiteral(node); }
  void visit(const NegNode* node)           { expr = impl->lowerNeg(node); }
  void visit(const AddNode* node)           { expr = impl->lowerAdd(node); }
  void visit(const SubNode* node)           { expr = impl->lowerSub(node); }
  void visit(const MulNode* node)           { expr = impl->lowerMul(node); }
  void visit(const DivNode* node)           { expr = impl->lowerDiv(node); }
  void visit(const MaxNode* node)           { expr = impl->lowerMax(node); }
  void visit(const MinNode* node)           { expr = impl->lowerMin(node); }
  void visit(const SqrtNode* node)          { expr = impl->lowerSqrt(node); }
  void visit(const CastNode* node)          { expr = impl->lowerCast(node); }
  void visit(const MapNode* node)           { expr = impl->lowerMap(node); }
  void visit(const CallIntrinsicNode* node) { expr = impl->lowerCallIntrinsic(node); }
  void visit(const SlicedAccessNode* node) { 
    taco_ierror << "Sliced access nodes should not need to be explicitly lowered";
  }
  void visit(const ReductionNode* node)  {
    taco_ierror << "Reduction nodes not supported in concrete index notation";
  }
};

class LowererImpl::IndexVisitor : public IndexVarExprVisitorStrict {
public:
  IndexVisitor(LowererImpl* impl) : impl(impl) {}
  std::pair<Stmt,Expr> lower(IndexVarExpr expr) {
    this->expr = std::pair<Stmt,Expr>();
    IndexVarExprVisitorStrict::visit(expr);
    return this->expr;
  }
private:
  LowererImpl* impl;
  std::pair<Stmt,Expr> expr;
  using IndexVarExprVisitorStrict::visit;
  void visit(const IndexVarAccessNode* node)  { expr = impl->lowerIndexVarAccess(node); }
  void visit(const IndexVarLiteralNode* node) { expr = impl->lowerIndexVarLiteral(node); }
  void visit(const IndexVarAddNode* node)     { expr = impl->lowerIndexVarAdd(node); }
  void visit(const IndexVarSubNode* node)     { expr = impl->lowerIndexVarSub(node); }
  void visit(const IndexVarDivNode* node)     { expr = impl->lowerIndexVarDiv(node); }
  void visit(const IndexVarCountNode* node)   { expr = impl->lowerIndexVarCount(node); }
};

LowererImpl::LowererImpl() : 
    visitor(new Visitor(this)), 
    ivisitor(new IndexVisitor(this)) {
}


static void createCapacityVars(const map<TensorVar, Expr>& tensorVars,
                               map<Expr, Expr>* capacityVars) {
  for (auto& tensorVar : tensorVars) {
    Expr tensor = tensorVar.second;
    Expr capacityVar = Var::make(util::toString(tensor) + "_capacity", Int());
    capacityVars->insert({tensor, capacityVar});
  }
}

static void createReducedValueVars(const vector<Access>& inputAccesses,
                                   map<Access, Expr>* reducedValueVars) {
  for (const auto& access : inputAccesses) {
    const TensorVar inputTensor = access.getTensorVar();
    const std::string name = inputTensor.getName() + "_val";
    const Datatype type = inputTensor.getType().getDataType();
    reducedValueVars->insert({access, Var::make(name, type)});
  }
}

/// Returns true iff `stmt` modifies an array
static bool hasStores(Stmt stmt) {
  struct FindStores : IRVisitor {
    bool hasStore;

    using IRVisitor::visit;

    void visit(const Store* stmt) {
      hasStore = true;
    }

    bool hasStores(Stmt stmt) {
      hasStore = false;
      stmt.accept(this);
      return hasStore;
    }
  };
  return stmt.defined() && FindStores().hasStores(stmt);
}

static std::vector<IndexVar> getRhsFreeVars(IndexStmt stmt) {
  std::vector<IndexVar> result;
  match(stmt,
    function<void(const AssignmentNode*, Matcher*)>([&](
        const AssignmentNode* n, Matcher* m) {
      const auto rhsVars = getIndexVars(n->rhs);
      const auto reductionVars = Assignment(n).getReductionVars();
      std::vector<IndexVar> freeVarsInRHS;
      for (const auto ivar : result) {
        if (util::contains(rhsVars, ivar) && 
            !util::contains(reductionVars, ivar)) {
          freeVarsInRHS.push_back(ivar);
        }
      }
      result = freeVarsInRHS;
    }),
    function<void(const ForallNode*, Matcher*)>([&](
        const ForallNode* n, Matcher* m) {
      result.push_back(n->indexVar);
      m->match(n->stmt);
    }),
    function<void(const WhereNode*, Matcher*)>([&](
        const WhereNode* n, Matcher* m) {
      m->match(n->consumer);
    })
  );
  return result; 
}

static std::map<Iterator,std::map<std::string,AttrQueryResult>>
splitAttrQueryResults(const std::vector<Iterator>& resultIterators,
                      const std::map<std::string,AttrQueryResult>& queryResults) {
  std::map<Iterator,std::map<std::string,AttrQueryResult>> ret;
  for (const auto& it : resultIterators) {
    ret[it];
    const auto prefix = it.getMode().getName() + "_attr_";
    for (const auto& result : queryResults) {
      if (result.first.find(prefix) != std::string::npos) {
        ret[it][result.first.substr(prefix.length())] = result.second;
      }
    }
  }
  return ret;
}

static Expr getPrevSize(const Iterator iter) {
  if (iter.isRoot()) {
    return 0;
  }
  
  Iterator it = iter;
  while (!it.isRoot()) {
    it = it.getParent();
  }
  it = it.getChild();

  Expr prevSize = 1;
  while (it != iter) {
    prevSize = it.getSizeNew(prevSize);
    it = it.getChild();
  }
  return prevSize;
}

Stmt
LowererImpl::lower(IndexStmt stmt, string name, bool assemble, bool compute)
{
  std::cout << stmt << std::endl;
  this->assemble = assemble;
  this->compute = compute;

  // Create result and parameter variables
  vector<TensorVar> results = getResults(stmt);
  vector<TensorVar> arguments = getArguments(stmt);
  vector<TensorVar> temporaries = getTemporaries(stmt);

  // Convert tensor results and arguments IR variables
  map<TensorVar, Expr> resultVars;
  vector<Expr> resultsIR = createVars(results, &resultVars);
  tensorVars.insert(resultVars.begin(), resultVars.end());
  vector<Expr> argumentsIR = createVars(arguments, &tensorVars);

  // Create variables for temporaries
  // TODO Remove this
  for (auto& temp : temporaries) {
    std::cout << "tmp: " << temp << std::endl;
    ir::Expr irVar = ir::Var::make(temp.getName(), temp.getType().getDataType(),
                                   true, true);
    tensorVars.insert({temp, irVar});
    resultVars.insert({temp, irVar});
    TemporaryArrays arrays;
    arrays.values = ir::Var::make(temp.getName(), temp.getType().getDataType(), true);
    this->temporaryArrays.insert({temp, arrays});
    header.push_back(VarDecl::make(arrays.values, 0));
  }

  // Create variables for keeping track of result values array capacity
  createCapacityVars(resultVars, &capacityVars);

  // Create iterators
  iterators = Iterators(stmt, tensorVars);

  vector<Access> inputAccesses, resultAccesses;
  set<Access> reducedAccesses;
  inputAccesses = getArgumentAccesses(stmt);
  std::tie(resultAccesses, reducedAccesses) = getResultAccesses(stmt);

  // Create variables that represent the reduced values of duplicated tensor 
  // components
  createReducedValueVars(inputAccesses, &reducedValueVars);

  map<TensorVar, Expr> scalars;

  // Define and initialize dimension variables
  vector<IndexVar> indexVars = getIndexVars(stmt);
  for (auto& indexVar : indexVars) {
    Expr dimension;
    match(stmt,
      function<void(const AssignmentNode*, Matcher*)>([&](
          const AssignmentNode* n, Matcher* m) {
        m->match(n->rhs);
        if (!dimension.defined()) {
          for (size_t i = 0; i < n->lhs.getIndices().size(); ++i) {
            if (isa<IndexVarAccess>(n->lhs.getIndices()[i])) {
              const auto ivar = to<IndexVarAccess>(n->lhs.getIndices()[i]).getIndexVar();
              if (ivar == indexVar) {
                dimension = GetProperty::make(tensorVars.at(n->lhs.getTensorVar()),
                                              TensorProperty::Dimension, i);
              }
            }
          }
        }
      }),
      function<void(const AccessNode*)>([&](const AccessNode* n) {
        const auto indexVars = Access(n).getIndexVars();
        if (util::contains(indexVars, indexVar)) {
          int loc = (int)distance(indexVars.begin(),
                                  find(indexVars.begin(),indexVars.end(),
                                       indexVar));
          dimension = GetProperty::make(tensorVars.at(n->tensorVar),
                                        TensorProperty::Dimension, loc);
        }
      })
    );
    taco_iassert(dimension.defined());
    dimensions.insert({indexVar, dimension});
  }

  std::map<IndexVarExpr,std::pair<Expr,size_t>> resultDims;
  std::map<std::pair<Expr,size_t>,std::pair<Expr,size_t>> queryDims;
  match(stmt,
    function<void(const AssignmentNode*, Matcher*)>([&](
        const AssignmentNode* n, Matcher* m) {
      const auto p = n->lhs.getTensorVar().getName().find("_attr_");
      if (p == std::string::npos) {
        for (size_t i = 0; i < n->lhs.getIndices().size(); ++i) {
          resultDims[n->lhs.getIndices()[i]] = 
              std::make_pair(tensorVars.at(n->lhs.getTensorVar()), i);
        }
        std::cout << "here2" << IndexStmt(n) << std::endl;
      }
    })
  );
  match(stmt,
    function<void(const AssignmentNode*, Matcher*)>([&](
        const AssignmentNode* n, Matcher* m) {
      const auto p = n->lhs.getTensorVar().getName().find("_attr_");
      if (p != std::string::npos) {
        for (size_t i = 0; i < n->lhs.getIndices().size(); ++i) {
          for (const auto resultDim : resultDims) {
            std::cout << resultDim.first << " " << n->lhs.getIndices()[i] << std::endl;
            if (equals(resultDim.first, n->lhs.getIndices()[i])) {
              std::cout << "here3" << std::endl;
              queryDims[std::make_pair(tensorVars.at(n->lhs.getTensorVar()), i)] = resultDim.second;
              break;
            }
          }
        }
      }
    })
  );
  for (const auto it : queryDims) {
    std::cout << it.first.first << " " << it.first.second << " " << it.second.first << " " << it.second.second << std::endl;
  }

  struct GenerateCounters : public IndexVarExprVisitor {
    GenerateCounters(std::map<std::vector<IndexVar>,Counter>& counters,
                     const std::vector<IndexVar>& rhsFreeVars) : 
        counters(counters), rhsFreeVars(rhsFreeVars) {}

    using IndexVarExprVisitor::visit;

    void visit(const IndexVarCountNode* expr) {
      // Get index variables used to index into counter, in the same order 
      // index variables are nested.
      std::vector<IndexVar> indexVars;
      std::copy_if(rhsFreeVars.begin(), rhsFreeVars.end(), 
                   std::back_inserter(indexVars), 
                   [&](IndexVar ivar) { return util::contains(expr->indexVars, ivar); });

      const auto initPoint = std::mismatch(rhsFreeVars.begin(), 
                                           rhsFreeVars.end(), 
                                           indexVars.begin());
      const bool useArrayForCounter = (indexVars.end() - initPoint.second);
      Counter counter;
      if (useArrayForCounter) {
        counter.array = Var::make(util::uniqueName("counters"), Int(), true);
      }
      counter.count = Var::make(util::uniqueName("count"), Int());
      counter.initPoint = *(initPoint.first);
      counter.indices = std::vector<IndexVar>(initPoint.second, indexVars.end());
      counters[expr->indexVars] = counter;
    }

    std::map<std::vector<IndexVar>,Counter>& counters;
    const std::vector<IndexVar>& rhsFreeVars;
  };

  const auto rhsFreeVars = getRhsFreeVars(stmt);
  GenerateCounters counterGen(this->counters, rhsFreeVars);
  match(stmt,
    function<void(const AssignmentNode*, Matcher*)>([&](
        const AssignmentNode* n, Matcher* m) {
      m->match(n->rhs);
      for (const auto index : n->lhs.getIndices()) {
        index.accept(&counterGen);
      }
    })
  );

  // Define and initialize scalar results and arguments
  if (generateComputeCode()) {
    for (auto& result : results) {
      if (isScalar(result.getType())) {
        taco_iassert(!util::contains(scalars, result));
        taco_iassert(util::contains(tensorVars, result));
        scalars.insert({result, tensorVars.at(result)});
        header.push_back(defineScalarVariable(result, true));
      }
    }
    for (auto& argument : arguments) {
      if (isScalar(argument.getType())) {
        taco_iassert(!util::contains(scalars, argument));
        taco_iassert(util::contains(tensorVars, argument));
        scalars.insert({argument, tensorVars.at(argument)});
        header.push_back(defineScalarVariable(argument, false));
      }
    }
  }

  // Allocate memory for scalar results
  if (generateAssembleCode()) {
    for (auto& result : results) {
      if (result.getOrder() == 0) {
        Expr resultIR = resultVars.at(result);
        Expr vals = GetProperty::make(resultIR, TensorProperty::Values);
        header.push_back(Allocate::make(vals, 1));
      }
    }
  }

  // Allocate and free counter arrays
  std::vector<Stmt> allocCounterStmts;
  std::vector<Stmt> freeCounterStmts;
  for (const auto counter : this->counters) {
    const auto counterArray = counter.second.array;
    if (counterArray.defined()) {
      Expr counterArraySize = getCardinality(counter.second.indices);
      allocCounterStmts.push_back(VarDecl::make(counterArray, 0));
      allocCounterStmts.push_back(Allocate::make(counterArray, counterArraySize));
      freeCounterStmts.push_back(Free::make(counterArray));
    }
  }
  Stmt allocCounters = Block::make(allocCounterStmts);
  Stmt freeCounters = Block::make(freeCounterStmts);

  // Lower the index statement to compute and/or assemble
  // TODO: handle where statements without attribute queries
  std::vector<Stmt> body;
  std::map<std::string,AttrQueryResult> queryResults;
  while (isa<Where>(stmt)) {
    IndexStmt query = to<Where>(stmt).getProducer();
    for (const auto& queryAccess : getResultAccesses(query).first) {
      const auto queryVar = queryAccess.getTensorVar();
      queryResults[queryVar.getName()] = AttrQueryResult(queryVar, getTensorVar(queryVar), getValuesArray(queryVar));
      Stmt initializeResults = initResultArrays({queryAccess}, inputAccesses, 
                                              reducedAccesses);
      std::cout << "initResults: " << initializeResults << std::endl;
      body.push_back(initializeResults);
    }
    body.push_back(lower(query));
    stmt = to<Where>(stmt).getConsumer();
  }
  
  // TODO: fix assumption that there is only one output
  const auto resultAccess = getResultAccesses(stmt).first.front();
  const auto resultIterators = getIterators(resultAccess);
  const auto modeQueryResults = splitAttrQueryResults(resultIterators, queryResults);
  std::vector<Iterator> topResultIterators = {resultIterators.front().getParent()};
  for (const auto& resultIterator : resultIterators) {
    if (resultIterator.hasSeqInsertEdge() || 
        resultIterator.hasUnseqInsertEdge()) {
      topResultIterators.push_back(resultIterator);
    }
  }

  const auto resultTensor = resultAccess.getTensorVar();
  for (size_t i = 0; i < topResultIterators.size(); ++i) {
    this->topResultIterator = topResultIterators[i];
    this->nextTopResultIterator = Iterator();
    if (i + 1 < topResultIterators.size()) {
      this->nextTopResultIterator = topResultIterators[i+1];
    }

    Expr prevSize = this->topResultIterator.isRoot() 
                  ? 1 : getPrevSize(this->topResultIterator);

    if (!this->topResultIterator.isRoot()) {
      Iterator iter = this->topResultIterator.getParent();

      std::vector<Expr> coords;
      for (const auto it : resultIterators) {
        if (it == this->topResultIterator) {
          break;
        }
        coords.push_back(it.getCoordVar());
      }
      auto reverse = util::reverse(coords);
      coords = std::vector<Expr>(reverse.begin(), reverse.end());

      bool isSeqIter = iter.isCompact();
      for (Iterator it = iter; isSeqIter && !it.isRoot(); it = it.getParent()) {
        isSeqIter = iter.hasPosIter() || iter.isOrdered();
      }

      Stmt insertEdgeLoop;
      if (isSeqIter) {
        insertEdgeLoop = this->topResultIterator.getSeqInsertEdge(
            iter.getPosVar(), coords,
            modeQueryResults.at(this->topResultIterator));
      } else {
        taco_not_supported_yet;
      }

      while (!iter.isRoot()) {
        if (iter.hasPosIter()) {
          taco_not_supported_yet;
        } else if (iter.hasCoordIter()) {
          taco_not_supported_yet;
        } else {
          Expr dim = GetProperty::make(tensorVars.at(resultTensor),
                                       TensorProperty::Dimension, 
                                       resultTensor.getFormat().getModeOrdering()[iter.getMode().getLevel() - 1]);
          Expr pos = iter.getPosVar();
          const auto locate = iter.locate(coords);
          Stmt initPos = VarDecl::make(pos, locate[0]);
          insertEdgeLoop = For::make(coords.back(), 0, dim, 1, 
                                     Block::make(initPos, insertEdgeLoop));
        }
        iter = iter.getParent();
        coords.pop_back();
      }

      if (this->topResultIterator.hasSeqInsertEdge()) {
        Stmt initEdges = this->topResultIterator.getSeqInitEdges(prevSize,
            modeQueryResults.at(this->topResultIterator));
        body.push_back(Block::make(initEdges, insertEdgeLoop));
      } else {
        taco_not_supported_yet;
      }
    }

    for (Iterator it = this->topResultIterator; 
         it != this->nextTopResultIterator; it = it.getChild()) {
      if (it.isRoot()) {
        continue;
      }

      body.push_back(it.getInitCoords(prevSize, modeQueryResults.at(it)));
      body.push_back(it.getInitYieldPos(prevSize));
      prevSize = it.getSizeNew(prevSize);

      if (it.isLeaf()) {
        // Allocate and initialize append and insert mode indices
        Stmt initializeResults = initResultArrays(resultAccesses, inputAccesses, 
                                                  reducedAccesses);
        body.push_back(initializeResults);
        std::cout << "initialize: " << initializeResults << std::endl;
        break;
      }
    }

    body.push_back(lower(stmt));
  }

  for (const auto& resultIterator : resultIterators) {
    footer.push_back(resultIterator.getFinalizeLevel());
  }

  // Post-process result modes and allocate memory for values if necessary
  Stmt finalizeResults = finalizeResultArrays(resultAccesses);
  std::cout << "finalize: " << finalizeResults << std::endl;

  // Store scalar stack variables back to results
  if (generateComputeCode()) {
    for (auto& result : results) {
      if (isScalar(result.getType())) {
        taco_iassert(util::contains(scalars, result));
        taco_iassert(util::contains(tensorVars, result));
        Expr resultIR = scalars.at(result);
        Expr varValueIR = tensorVars.at(result);
        Expr valuesArrIR = GetProperty::make(resultIR, TensorProperty::Values);
        footer.push_back(Store::make(valuesArrIR, 0, varValueIR));
      }
    }
  }

  // Create function
  Stmt func = Function::make(name, resultsIR, argumentsIR,
                             Block::blanks(Block::make(header),
                                           allocCounters,
                                           Block::make(body),
                                           finalizeResults,
                                           freeCounters,
                                           Block::make(footer)));

            //modeQueryResults.at(this->topResultIterator));
  struct ReplaceDimensionVars : public IRRewriter {
    using IRRewriter::visit;

    ReplaceDimensionVars(const std::map<std::pair<Expr,size_t>,std::pair<Expr,size_t>>& queryDims) : 
        queryDims(queryDims) {}

    void visit(const GetProperty* op) {
      if (op->property == TensorProperty::Dimension) {
        const auto p = op->name.find("_attr_");
        if (p != std::string::npos) {
          const auto equivalentDim = queryDims.at(std::make_pair(op->tensor, (size_t)op->mode));
          std::cout << "here " << Expr(op) << equivalentDim.first << std::endl;
          expr = GetProperty::make(equivalentDim.first, TensorProperty::Dimension, equivalentDim.second);
          return;
        }
      }
      expr = op;
    }

    const std::map<std::pair<Expr,size_t>,std::pair<Expr,size_t>>& queryDims;
  };
  func = ReplaceDimensionVars(queryDims).rewrite(func);
  std::cout << func << std::endl;

  return func;
}


Stmt LowererImpl::lowerAssignment(Assignment assignment)
{
  TensorVar result = assignment.getLhs().getTensorVar();

  if (generateComputeCode()) {
    Expr var = getTensorVar(result);
    Expr rhs = lower(assignment.getRhs());

    // Assignment to scalar variables.
    if (isScalar(result.getType())) {
      if (!assignment.getOperator().defined()) {
        return Assign::make(var, rhs);
      }
      else {
        taco_iassert(isa<taco::Add>(assignment.getOperator()));
        return compoundAssign(var, rhs);
      }
    }
    // Assignments to tensor variables (non-scalar).
    else {
      Expr values = getValuesArray(result);
      Expr loc = generateValueLocExpr(assignment.getLhs());

      std::vector<Expr> coords;
      std::vector<Stmt> accessStmts;

      // TODO: emit this when only assembling as well
      Expr prevPos = 0;
      size_t i = 0;
      const auto resultIterators = getIterators(assignment.getLhs());
      const auto resultIndices = assignment.getLhs().getIndices();
      for (const auto it : util::zip(resultIterators, resultIndices)) {
        if (it.first == this->nextTopResultIterator) {
          break;
        }

        const auto coord = lower(it.second);
        accessStmts.push_back(coord.first);

        Expr ivar;
        if (!isa<IndexVarAccess>(it.second)) {
          ivar = it.first.getCoordVar();
          accessStmts.push_back(VarDecl::make(ivar, coord.second));
        } else {
          ivar = getCoordinateVar(it.first.getIndexVar());
        }
        coords.push_back(ivar);

        const auto yieldPos = it.first.getYieldPos(prevPos, coords);
        accessStmts.push_back(yieldPos.compute());
        Expr pos = it.first.getPosVar();
        accessStmts.push_back(VarDecl::make(pos, yieldPos[0]));
        accessStmts.push_back(it.first.getInsertCoord(prevPos, pos, coords));

        prevPos = pos;
        ++i;
      }

      if (!this->nextTopResultIterator.defined()) {
        if (!assignment.getOperator().defined() || (values.type() == Bool && 
            isa<ir::Literal>(rhs) && !to<ir::Literal>(rhs)->equalsScalar(0))) {
          accessStmts.push_back(Store::make(values, loc, rhs));
        }
        else {
          accessStmts.push_back(compoundStore(values, loc, rhs));
        }
      }

      //return IfThenElse::make(Neq::make(rhs, 0.0), Block::make(accessStmts));
      return Block::make(accessStmts);
    }
  }
  // We're only assembling so defer allocating value memory to the end when
  // we'll know exactly how much we need.
  else if (generateAssembleCode()) {
    // TODO
    return Stmt();
  }
  // We're neither assembling or computing so we emit nothing.
  else {
    return Stmt();
  }
  taco_unreachable;
  return Stmt();
}


Stmt LowererImpl::lowerYield(Yield yield) {
  std::vector<Expr> coords;
  for (auto& indexVar : yield.getIndexVars()) {
    coords.push_back(getCoordinateVar(indexVar));
  }
  Expr val = lower(yield.getExpr());
  return ir::Yield::make(coords, val);
}


static pair<vector<Iterator>, vector<Iterator>>
splitAppenderAndInserters(const vector<Iterator>& results) {
  vector<Iterator> appenders;
  vector<Iterator> inserters;

  // TODO: Choose insert when the current forall is nested inside a reduction
  for (auto& result : results) {
    taco_iassert(result.hasAppend() || result.hasInsert())
        << "Results must support append or insert";

    if (result.hasAppend()) {
      appenders.push_back(result);
    }
    else {
      taco_iassert(result.hasInsert());
      inserters.push_back(result);
    }
  }

  return {appenders, inserters};
}


Stmt LowererImpl::lowerForall(Forall forall)
{
  MergeLattice lattice = MergeLattice::make(forall, iterators);

  vector<Access> resultAccesses;
  set<Access> reducedAccesses;
  std::tie(resultAccesses, reducedAccesses) = getResultAccesses(forall);

  // Pre-allocate/initialize memory of value arrays that are full below this
  // loops index variable
  Stmt preInitValues = initResultArrays(forall.getIndexVar(), resultAccesses,
                                        getArgumentAccesses(forall), 
                                        reducedAccesses);

  std::vector<Stmt> initCounterStmts;
  for (const auto counter : this->counters) {
    if (counter.second.initPoint == forall.getIndexVar()) {
      const auto counterArray = counter.second.array;
      const auto counterCount = counter.second.count;
      if (counterArray.defined()) {
        Expr counterArraySize = getCardinality(counter.second.indices);
        Expr counterArrayIt = Var::make("it", Int());
        Stmt zeroCounter = Store::make(counterArray, counterArrayIt, 0);
        Stmt zeroLoop = For::make(counterArrayIt, 0, counterArraySize, 1, zeroCounter);
        initCounterStmts.push_back(zeroLoop);
      } else {
        initCounterStmts.push_back(VarDecl::make(counterCount, 0));
      }
    }
  }
  Stmt initCounters = Block::make(initCounterStmts);

  Stmt loops;
  // Emit a loop that iterates over over a single iterator (optimization)
  if (lattice.iterators().size() == 1 && lattice.iterators()[0].isUnique()) {
    taco_iassert(lattice.points().size() == 1);

    MergePoint point = lattice.points()[0];
    Iterator iterator = lattice.iterators()[0];

    vector<Iterator> locators = point.locators();
    vector<Iterator> appenders;
    vector<Iterator> inserters;
    tie(appenders, inserters) = splitAppenderAndInserters(point.results());

    // Emit dimension coordinate iteration loop
    if (iterator.isDimensionIterator()) {
      loops = lowerForallDimension(forall, point.locators(),
                                   inserters, appenders, reducedAccesses);
    }
    // Emit position iteration loop
    else if (iterator.hasPosIter()) {
      loops = lowerForallPosition(forall, iterator, locators,
                                 inserters, appenders, reducedAccesses);
    }
    // Emit coordinate iteration loop
    else {
      taco_iassert(iterator.hasCoordIter());
//      taco_not_supported_yet
      loops = Stmt();
    }
  }
  // Emit general loops to merge multiple iterators
  else {
    loops = lowerMergeLattice(lattice, getCoordinateVar(forall.getIndexVar()),
                              forall, reducedAccesses);
  }
//  taco_iassert(loops.defined());

  // TODO: fix scalar reductions
  //if (!generateComputeCode() && !hasStores(loops)) {
  if (!hasStores(loops)) {
    // If assembly loop does not modify output arrays, then it can be safely 
    // omitted.
    loops = Stmt();
  }

  return Block::make(preInitValues,
                     initCounters,
                     loops);
}


Stmt LowererImpl::lowerForallDimension(Forall forall,
                                       vector<Iterator> locators,
                                       vector<Iterator> inserters,
                                       vector<Iterator> appenders,
                                       set<Access> reducedAccesses)
{
  Expr coordinate = getCoordinateVar(forall.getIndexVar());

  Stmt body = lowerForallBody(coordinate, forall,
                              locators, inserters, appenders, reducedAccesses);

  Stmt posAppend = generateAppendPositions(appenders);

  // Emit loop with preamble and postamble
  Expr dimension = getDimension(forall.getIndexVar());
  bool parallelize = forall.getTags().count(Forall::PARALLELIZE);
  return Block::blanks(For::make(coordinate, 0, dimension, 1, body,
                                 parallelize ? LoopKind::Runtime : LoopKind::Serial, parallelize),
                       posAppend);
}


Stmt LowererImpl::lowerForallCoordinate(Forall forall, Iterator iterator,
                                        vector<Iterator> locators,
                                        vector<Iterator> inserters,
                                        vector<Iterator> appenders,
                                        set<Access> reducedAccesses) {
  taco_not_supported_yet;
  return Stmt();
}

Stmt LowererImpl::lowerForallPosition(Forall forall, Iterator iterator,
                                      vector<Iterator> locators,
                                      vector<Iterator> inserters,
                                      vector<Iterator> appenders,
                                      set<Access> reducedAccesses)
{
  Expr coordinate = getCoordinateVar(forall.getIndexVar());
  Expr coordinateArray= iterator.posAccess(iterator.getPosVar(), 
                                           coordinates(iterator)).getResults()[0];
  Stmt declareCoordinate = VarDecl::make(coordinate, coordinateArray);

  Stmt body = lowerForallBody(coordinate, forall,
                              locators, inserters, appenders, reducedAccesses);

  // Code to append positions
  Stmt posAppend = generateAppendPositions(appenders);

  // Code to compute iteration bounds
  Stmt boundsCompute;
  Expr startBound, endBound;
  Expr parentPos = iterator.getParent().getPosVar();
  if (iterator.getParent().isRoot() || iterator.getParent().isUnique()) {
    // E.g. a compressed mode without duplicates
    ModeFunction bounds = iterator.posBounds(parentPos);
    boundsCompute = bounds.compute();
    startBound = bounds[0];
    endBound = iterator.isBranchless() ? Expr() : bounds[1];
  } else {
    taco_iassert(iterator.isOrdered() && iterator.getParent().isOrdered());
    taco_iassert(iterator.isCompact() && iterator.getParent().isCompact());

    // E.g. a compressed mode with duplicates. Apply iterator chaining
    Expr parentSegend = iterator.getParent().getSegendVar();
    ModeFunction startBounds = iterator.posBounds(parentPos);
    ModeFunction endBounds = iterator.posBounds(ir::Sub::make(parentSegend, 1));
    boundsCompute = Block::make(startBounds.compute(), endBounds.compute());
    startBound = startBounds[0];
    endBound = endBounds[1];
  }
  bool parallelize = forall.getTags().count(Forall::PARALLELIZE);
  Stmt loop = endBound.defined()
            ? For::make(iterator.getPosVar(), startBound, endBound, 1,
                        Block::make(declareCoordinate, body),
                        parallelize ? LoopKind::Runtime : LoopKind::Serial, 
                        parallelize)
            : Block::make(VarDecl::make(iterator.getPosVar(), startBound),
                          declareCoordinate, body);
  // Loop with preamble and postamble
  return Block::blanks(boundsCompute, loop, posAppend);
}

Stmt LowererImpl::lowerMergeLattice(MergeLattice lattice, Expr coordinate,
                                    Forall statement, 
                                    const std::set<Access>& reducedAccesses)
{
  vector<Iterator> appenders = filter(lattice.results(),
                                      [](Iterator it){return it.hasAppend();});

  Stmt iteratorVarInits = codeToInitializeIteratorVars(lattice.iterators());

  vector<Stmt> mergeLoopsVec;
  for (MergePoint point : lattice.points()) {
    // Each iteration of this loop generates a while loop for one of the merge
    // points in the merge lattice.
    Forall zeroedStmt = to<Forall>(zero(statement, getExhaustedAccesses(point,lattice)));
    MergeLattice sublattice = lattice.subLattice(point);
    Stmt mergeLoop = lowerMergePoint(sublattice, coordinate, zeroedStmt, reducedAccesses);
    mergeLoopsVec.push_back(mergeLoop);
  }
  Stmt mergeLoops = Block::make(mergeLoopsVec);

  // Append position to the pos array
  Stmt appendPositions = generateAppendPositions(appenders);

  return Block::blanks(iteratorVarInits,
                       mergeLoops,
                       appendPositions);
}

Stmt LowererImpl::lowerMergePoint(MergeLattice pointLattice,
                                  ir::Expr coordinate, Forall statement,
                                  const std::set<Access>& reducedAccesses)
{
  MergePoint point = pointLattice.points().front();

  vector<Iterator> iterators = point.iterators();
  vector<Iterator> mergers = point.mergers();
  vector<Iterator> rangers = point.rangers();
  vector<Iterator> locators = point.locators();

  taco_iassert(iterators.size() > 0);
  taco_iassert(mergers.size() > 0);
  taco_iassert(rangers.size() > 0);

  // Load coordinates from position iterators
  Stmt loadPosIterCoordinates;
  if (iterators.size() > 1) {
    vector<Stmt> loadPosIterCoordinateStmts;
    auto posIters = filter(iterators, [](Iterator it){return it.hasPosIter();});
    for (auto& posIter : posIters) {
      taco_tassert(posIter.hasPosIter());
      ModeFunction posAccess = posIter.posAccess(posIter.getPosVar(), 
                                                 coordinates(posIter));
      loadPosIterCoordinateStmts.push_back(posAccess.compute());
      loadPosIterCoordinateStmts.push_back(VarDecl::make(posIter.getCoordVar(),
                                                          posAccess[0]));
    }
    loadPosIterCoordinates = Block::make(loadPosIterCoordinateStmts);
  }

  // Merge iterator coordinate variables
  Stmt resolveCoordinate;
  if (mergers.size() == 1) {
    Iterator merger = mergers[0];
    if (merger.hasPosIter()) {
      // Just one position iterator so it is the resolved coordinate
      ModeFunction posAccess = merger.posAccess(merger.getPosVar(), 
                                                coordinates(merger));
      resolveCoordinate = Block::make(posAccess.compute(),
                                          VarDecl::make(coordinate,
                                                        posAccess[0]));
    }
    else if (merger.hasCoordIter()) {
      taco_not_supported_yet;
    }
    else if (merger.isDimensionIterator()) {
      // Just one dimension iterator so resolved coordinate already exist and we
      // do nothing
    }
    else {
      taco_ierror << "Unexpected type of single iterator " << merger;
    }
  }
  else {
    // Multiple position iterators so the smallest is the resolved coordinate
    resolveCoordinate = VarDecl::make(coordinate,
                                      ir::Min::make(coordinates(mergers)));
  }

  // Locate positions
  Stmt loadLocatorPosVars = declLocatePosVars(locators);

  // Deduplication loops
  auto dupIters = filter(iterators, [](Iterator it){return !it.isUnique() && 
                                                           it.hasPosIter();});
  bool alwaysReduce = (mergers.size() == 1 && mergers[0].hasPosIter());
  Stmt deduplicationLoops = reduceDuplicateCoordinates(coordinate, dupIters, 
                                                       alwaysReduce);

  // One case for each child lattice point lp
  Stmt caseStmts = lowerMergeCases(coordinate, statement, pointLattice, 
                                   reducedAccesses);

  // Increment iterator position variables
  Stmt incIteratorVarStmts = codeToIncIteratorVars(coordinate, iterators);

  /// While loop over rangers
  return While::make(checkThatNoneAreExhausted(rangers),
                     Block::make(loadPosIterCoordinates,
                                 resolveCoordinate,
                                 loadLocatorPosVars,
                                 deduplicationLoops,
                                 caseStmts,
                                 incIteratorVarStmts));
}

Stmt LowererImpl::lowerMergeCases(ir::Expr coordinate, Forall stmt,
                                  MergeLattice lattice,
                                  const std::set<Access>& reducedAccesses)
{
  vector<Stmt> result;

  vector<Iterator> appenders;
  vector<Iterator> inserters;
  tie(appenders, inserters) = splitAppenderAndInserters(lattice.results());

  // Just one iterator so no conditionals
  if (lattice.iterators().size() == 1) {
    Stmt body = lowerForallBody(coordinate, stmt, {}, inserters, 
                                appenders, reducedAccesses);
    result.push_back(body);
  }
  else {
    vector<pair<Expr,Stmt>> cases;
    for (MergePoint point : lattice.points()) {

      // Construct case expression
      vector<Expr> coordComparisons;
      for (Iterator iterator : point.rangers()) {
        coordComparisons.push_back(Eq::make(iterator.getCoordVar(),coordinate));
      }

      // Construct case body
      Forall zeroedStmt = to<Forall>(zero(stmt, getExhaustedAccesses(point, lattice)));
      Stmt body = lowerForallBody(coordinate, zeroedStmt, {},
                                  inserters, appenders, reducedAccesses);

      cases.push_back({conjunction(coordComparisons), body});
    }
    result.push_back(Case::make(cases, lattice.exact()));
  }

  return Block::make(result);
}


Stmt LowererImpl::lowerForallBody(Expr coordinate, Forall stmt,
                                  vector<Iterator> locators,
                                  vector<Iterator> inserters,
                                  vector<Iterator> appenders,
                                  const set<Access>& reducedAccesses) {
  Stmt initVals = resizeAndInitValues(appenders, reducedAccesses);

  // Code to access counters
  Stmt getCounters = getCounterCounts(stmt);

  // Inserter positions
  Stmt declInserterPosVars = Stmt(); //declLocatePosVars(inserters);

  // Locate positions
  Stmt declLocatorPosVars = declLocatePosVars(locators);

  // Code of loop body statement
  Stmt body = lower(stmt.getStmt());

  // Code to append coordinates
  Stmt appendCoords = appendCoordinate(appenders, coordinate);

  // TODO: Emit code to insert coordinates

  // Code to increment counters
  Stmt incCounters = incrementCounters(stmt);

  return Block::make(initVals,
                     getCounters,
                     declInserterPosVars,
                     declLocatorPosVars,
                     body,
                     appendCoords,
                     incCounters);
}


Stmt LowererImpl::lowerWhere(Where where) {
  TensorVar temporary = where.getTemporary();

  // Declare and initialize the where statement's temporary
  Stmt initializeTemporary;
  if (isScalar(temporary.getType())) {
    initializeTemporary = defineScalarVariable(temporary, true);
  }
  else {
    if (generateComputeCode()) {
      Expr values = ir::Var::make(temporary.getName(),
                                  temporary.getType().getDataType(),
                                  true, false);

      Expr size = ir::Mul::make((uint64_t)3, Sizeof::make(values.type()));
      Stmt allocate = VarDecl::make(values, Malloc::make(size));
      this->header.push_back(allocate);

      Stmt free = Free::make(values);
      this->footer.push_back(free);

      /// Make a struct object that lowerAssignment and lowerAccess can read
      /// temporary value arrays from.
      TemporaryArrays arrays;
      arrays.values = values;
      this->temporaryArrays.insert({temporary, arrays});
    }
  }

  Stmt producer = lower(where.getProducer());
  Stmt consumer = lower(where.getConsumer());
  return Block::make(initializeTemporary, producer, consumer);
}


Stmt LowererImpl::lowerSequence(Sequence sequence) {
  Stmt definition = lower(sequence.getDefinition());
  Stmt mutation = lower(sequence.getMutation());
  return Block::make(definition, mutation);
}


Stmt LowererImpl::lowerMulti(Multi multi) {
  Stmt stmt1 = lower(multi.getStmt1());
  Stmt stmt2 = lower(multi.getStmt2());
  return Block::make(stmt1, stmt2);
}


Expr LowererImpl::lowerAccess(Access access) {
  TensorVar var = access.getTensorVar();

  if (isScalar(var.getType())) {
    return getTensorVar(var);
  }

  return getIterators(access).back().isUnique()
         ? Load::make(getValuesArray(var), generateValueLocExpr(access))
         : getReducedValueVar(access);
}


Expr LowererImpl::lowerLiteral(Literal literal) {
  switch (literal.getDataType().getKind()) {
    case Datatype::Bool:
      return ir::Literal::make(literal.getVal<bool>());
    case Datatype::UInt8:
      return ir::Literal::make((unsigned long long)literal.getVal<uint8_t>());
    case Datatype::UInt16:
      return ir::Literal::make((unsigned long long)literal.getVal<uint16_t>());
    case Datatype::UInt32:
      return ir::Literal::make((unsigned long long)literal.getVal<uint32_t>());
    case Datatype::UInt64:
      return ir::Literal::make((unsigned long long)literal.getVal<uint64_t>());
    case Datatype::UInt128:
      taco_not_supported_yet;
      break;
    case Datatype::Int8:
      return ir::Literal::make((int)literal.getVal<int8_t>());
    case Datatype::Int16:
      return ir::Literal::make((int)literal.getVal<int16_t>());
    case Datatype::Int32:
      return ir::Literal::make((int)literal.getVal<int32_t>());
    case Datatype::Int64:
      return ir::Literal::make((long long)literal.getVal<int64_t>());
    case Datatype::Int128:
      taco_not_supported_yet;
      break;
    case Datatype::Float32:
      return ir::Literal::make(literal.getVal<float>());
    case Datatype::Float64:
      return ir::Literal::make(literal.getVal<double>());
    case Datatype::Complex64:
      return ir::Literal::make(literal.getVal<std::complex<float>>());
    case Datatype::Complex128:
      return ir::Literal::make(literal.getVal<std::complex<double>>());
    case Datatype::Undefined:
      taco_unreachable;
      break;
  }
  return ir::Expr();
}


Expr LowererImpl::lowerNeg(Neg neg) {
  return ir::Neg::make(lower(neg.getA()));
}


Expr LowererImpl::lowerAdd(Add add) {
  Expr a = lower(add.getA());
  Expr b = lower(add.getB());
  return (add.getDataType().getKind() == Datatype::Bool)
         ? ir::Or::make(a, b) : ir::Add::make(a, b);
}


Expr LowererImpl::lowerSub(Sub sub) {
  return ir::Sub::make(lower(sub.getA()), lower(sub.getB()));
}


Expr LowererImpl::lowerMul(Mul mul) {
  Expr a = lower(mul.getA());
  Expr b = lower(mul.getB());
  return (mul.getDataType().getKind() == Datatype::Bool)
         ? ir::And::make(a, b) : ir::Mul::make(a, b);
}


Expr LowererImpl::lowerDiv(Div div) {
  return ir::Div::make(lower(div.getA()), lower(div.getB()));
}


Expr LowererImpl::lowerMax(Max max) {
  return ir::Max::make(lower(max.getA()), lower(max.getB()));
}


Expr LowererImpl::lowerMin(Min min) {
  return ir::Min::make({lower(min.getA()), lower(min.getB())});
}


Expr LowererImpl::lowerSqrt(Sqrt sqrt) {
  return ir::Sqrt::make(lower(sqrt.getA()));
}


Expr LowererImpl::lowerCast(Cast cast) {
  return ir::Cast::make(lower(cast.getA()), cast.getDataType());
}


Expr LowererImpl::lowerMap(Map map) {
  Expr out = lower(map.getOut());
  if (true) {  // TODO: check if in-expr can be zero
    return out;
  }
  Expr in = lower(map.getIn());
  Expr cmp = ir::Cast::make(Neq::make(in, 0.0), Int());
  return simplify(ir::Mul::make(cmp, out));
}


Expr LowererImpl::lowerCallIntrinsic(CallIntrinsic call) {
  std::vector<Expr> args;
  for (auto& arg : call.getArgs()) {
    args.push_back(lower(arg));
  }
  return call.getFunc().lower(args);
}


std::pair<Stmt,Expr> LowererImpl::lowerIndexVarAccess(IndexVarAccess access) {
  return std::make_pair(Stmt(), getCoordinateVar(access.getIndexVar()));
}


std::pair<Stmt,Expr> LowererImpl::lowerIndexVarLiteral(IndexVarLiteral lit) {
  return std::make_pair(Stmt(), ir::Literal::make((int)lit.getVal()));
}


std::pair<Stmt,Expr> LowererImpl::lowerIndexVarAdd(IndexVarAdd sub) {
  const auto a = lower(sub.getA());
  const auto b = lower(sub.getB());
  const auto body = Block::make({a.first, b.first});
  return std::make_pair(body, ir::Add::make(a.second, b.second));
}


std::pair<Stmt,Expr> LowererImpl::lowerIndexVarSub(IndexVarSub sub) {
  const auto a = lower(sub.getA());
  const auto b = lower(sub.getB());
  const auto body = Block::make({a.first, b.first});
  return std::make_pair(body, ir::Sub::make(a.second, b.second));
}


std::pair<Stmt,Expr> LowererImpl::lowerIndexVarDiv(IndexVarDiv div) {
  const auto a = lower(div.getA());
  const auto b = lower(div.getB());
  const auto body = Block::make({a.first, b.first});
  return std::make_pair(body, ir::Div::make(a.second, b.second));
}


std::pair<Stmt,Expr> LowererImpl::lowerIndexVarCount(IndexVarCount count) {
  return std::make_pair(Stmt(), getCounter(count.getIndexVars()).count);
}



Stmt LowererImpl::lower(IndexStmt stmt) {
  return visitor->lower(stmt);
}


Expr LowererImpl::lower(IndexExpr expr) {
  return visitor->lower(expr);
}


std::pair<Stmt,Expr> LowererImpl::lower(IndexVarExpr expr) {
  return ivisitor->lower(expr);
}


bool LowererImpl::generateAssembleCode() const {
  return this->assemble;
}


bool LowererImpl::generateComputeCode() const {
  return this->compute;
}


Expr LowererImpl::getTensorVar(TensorVar tensorVar) const {
  taco_iassert(util::contains(this->tensorVars, tensorVar)) << tensorVar;
  return this->tensorVars.at(tensorVar);
}


Expr LowererImpl::getCapacityVar(Expr tensor) const {
  taco_iassert(util::contains(this->capacityVars, tensor)) << tensor;
  return this->capacityVars.at(tensor);
}


ir::Expr LowererImpl::getValuesArray(TensorVar var) const
{
  return (util::contains(temporaryArrays, var))
         ? temporaryArrays.at(var).values
         : GetProperty::make(getTensorVar(var), TensorProperty::Values);
}


Expr LowererImpl::getDimension(IndexVar indexVar) const {
  taco_iassert(util::contains(this->dimensions, indexVar)) << indexVar;
  return this->dimensions.at(indexVar);
}
        

Expr LowererImpl::getCardinality(IndexVar indexVar) const {
  return getDimension(indexVar);
}


Expr LowererImpl::getCardinality(const std::vector<IndexVar>& indexVars) const {
  Expr size = 1;
  for (const auto index : indexVars) {
    const auto stride = getCardinality(index);
    size = ir::Mul::make(size, stride);
  }
  return size;
}


std::vector<Iterator> LowererImpl::getIterators(Access access) const {
  vector<Iterator> result;
  TensorVar tensor = access.getTensorVar();
  for (int i = 0; i < tensor.getOrder(); i++) {
    int mode = tensor.getFormat().getModeOrdering()[i];
    result.push_back(iterators.levelIterator(ModeAccess(access, mode+1)));
  }
  return result;
}


set<Access> LowererImpl::getExhaustedAccesses(MergePoint point,
                                              MergeLattice lattice) const
{
  set<Access> exhaustedAccesses;
  for (auto& iterator : lattice.exhausted(point)) {
    exhaustedAccesses.insert(iterators.modeAccess(iterator).getAccess());
  }
  return exhaustedAccesses;
}


Expr LowererImpl::getReducedValueVar(Access access) const {
  return this->reducedValueVars.at(access);
}


Expr LowererImpl::getCoordinateVar(IndexVar indexVar) const {
  return this->iterators.modeIterator(indexVar).getCoordVar();
}


Expr LowererImpl::getCoordinateVar(Iterator iterator) const {
  if (iterator.isDimensionIterator()) {
    return iterator.getCoordVar();
  }
  return this->getCoordinateVar(iterator.getIndexVar());
}


LowererImpl::Counter
LowererImpl::getCounter(const std::vector<IndexVar>& indexVars) const {
  return this->counters.at(indexVars);
}


vector<Expr> LowererImpl::coordinates(Iterator iterator) const {
  taco_iassert(iterator.defined());

  vector<Expr> coords;
  do {
    coords.push_back(getCoordinateVar(iterator));
    iterator = iterator.getParent();
  } while (!iterator.isRoot());
  auto reverse = util::reverse(coords);
  return vector<Expr>(reverse.begin(), reverse.end());
}


vector<Expr> LowererImpl::coordinates(vector<Iterator> iterators)
{
  taco_iassert(all(iterators, [](Iterator iter){ return iter.defined(); }));
  vector<Expr> result;
  for (auto& iterator : iterators) {
    result.push_back(iterator.getCoordVar());
  }
  return result;
}


/// Returns true iff a result mode is assembled by inserting a sparse set of 
/// result coordinates (e.g., compressed to dense).
static 
bool hasSparseInserts(const std::vector<Iterator>& resultIterators,
                      const std::multimap<IndexVar, Iterator>& inputIterators) {
  for (const auto& resultIterator : resultIterators) {
    if (resultIterator.hasInsert()) {
      const auto indexVar = resultIterator.getIndexVar();
      const auto accessedInputs = inputIterators.equal_range(indexVar);
      bool indexVarIndexesInputs = false;
      for (auto inputIterator = accessedInputs.first; 
           inputIterator != accessedInputs.second; ++inputIterator) {
        indexVarIndexesInputs = true;
        if (!inputIterator->second.isFull()) {
          return true;
        }
      }
      if (!indexVarIndexesInputs) {
        // TODO: Temporary hack that's overly pessimistic. Should distinguish 
        // between broadcasted dimensions and dimensions that are indexed by 
        // functions of index variables that also index the inputs.
        return true;
      }
    }
  }
  return false;
}


Stmt LowererImpl::initResultArrays(vector<Access> writes, 
                                   vector<Access> reads,
                                   set<Access> reducedAccesses) {
  multimap<IndexVar, Iterator> readIterators;
  for (auto& read : reads) {
    for (auto& readIterator : getIterators(read)) {
      readIterators.insert({readIterator.getIndexVar(), readIterator});
    }
  }

  std::vector<Stmt> result;
  for (auto& write : writes) {
    std::cout << "write: " << write << std::endl;
    if (write.getTensorVar().getOrder() == 0) continue;

    std::vector<Stmt> initArrays;

    const auto iterators = getIterators(write);
    taco_iassert(!iterators.empty());

    Expr tensor = getTensorVar(write.getTensorVar());
    Expr valuesArr = getValuesArray(write.getTensorVar());
    //Expr valuesArr = GetProperty::make(tensor, TensorProperty::Values);

    Expr parentSize = 1;
    if (generateAssembleCode()) {
      for (const auto& iterator : iterators) {
        Expr size;
        Stmt init;
        // Initialize data structures for storing levels
        if (iterator.hasAppend()) {
          size = 0;
          init = iterator.getAppendInitLevel(parentSize, size);
        } else if (iterator.hasInsert()) {
          size = simplify(ir::Mul::make(parentSize, iterator.getWidth()));
          init = iterator.getInsertInitLevel(parentSize, size);
        } else {
          size = simplify(iterator.getSizeNew(parentSize));
        }
        initArrays.push_back(init);

        // Declare position variable of append modes that are not above a 
        // branchless mode (if mode below is branchless, then can share same 
        // position variable)
        if (iterator.hasAppend() && (iterator.isLeaf() || 
            !iterator.getChild().isBranchless())) {
          initArrays.push_back(VarDecl::make(iterator.getPosVar(), 0));
        }

        parentSize = size;
      }

      // Pre-allocate memory for the value array if computing while assembling
      if (generateComputeCode()) {
        taco_iassert(!iterators.empty());
        
        Expr capacityVar = getCapacityVar(tensor);
        Expr allocSize = isValue(parentSize, 0) 
                         ? DEFAULT_ALLOC_SIZE : parentSize;
        initArrays.push_back(VarDecl::make(capacityVar, allocSize));
        // TODO: check for compactness and no zero-padding instead
        if (!iterators.back().hasInitYieldPos() && !isValue(parentSize, 0)) {
          initArrays.push_back(Allocate::make(valuesArr, capacityVar, false, Expr(), true));
        } else {
          initArrays.push_back(Allocate::make(valuesArr, capacityVar));
        }
      }

      taco_iassert(!initArrays.empty());
      result.push_back(Block::make(initArrays));
    }
    else if (generateComputeCode()) {
      Iterator lastAppendIterator;
      // Compute size of values array
      for (auto& iterator : iterators) {
        if (iterator.hasAppend()) {
          lastAppendIterator = iterator;
          parentSize = iterator.getSize(parentSize);
        } else if (iterator.hasInsert()) {
          parentSize = ir::Mul::make(parentSize, iterator.getWidth());
        } else {
          parentSize = iterator.getSizeNew(parentSize);
        }
        parentSize = simplify(parentSize);
      }

      // Declare position variable for the last append level
      if (lastAppendIterator.defined()) {
        result.push_back(VarDecl::make(lastAppendIterator.getPosVar(), 0));
      }
    }

    //if (generateComputeCode() && !iterators.back().hasAppend() && 
    //    !isValue(parentSize, 0) && 
    //    (hasSparseInserts(iterators, readIterators) || 
    //     util::contains(reducedAccesses, write))) {
    //  // Zero-initialize values array if size statically known and might not 
    //  // assign to every element in values array during compute
    //  Expr size = generateAssembleCode() ? getCapacityVar(tensor) : parentSize;
    //  result.push_back(zeroInitValues(tensor, 0, size));
    //}
  }
  return result.empty() ? Stmt() : Block::blanks(result);
}


ir::Stmt LowererImpl::finalizeResultArrays(std::vector<Access> writes) {
  if (!generateAssembleCode()) {
    return Stmt();
  }

  std::vector<Stmt> result;
  for (auto& write : writes) {
    if (write.getTensorVar().getOrder() == 0) continue;

    const auto iterators = getIterators(write);
    taco_iassert(!iterators.empty());
      
    Expr parentSize = 1;
    for (const auto& iterator : iterators) {
      Expr size;
      Stmt finalize;
      // Post-process data structures for storing levels
      if (iterator.hasAppend()) {
        size = iterator.getPosVar();
        finalize = iterator.getAppendFinalizeLevel(parentSize, size);
      } else if (iterator.hasInsert()) {
        size = simplify(ir::Mul::make(parentSize, iterator.getWidth()));
        finalize = iterator.getInsertFinalizeLevel(parentSize, size);
      } else {
        size = iterator.getSizeNew(parentSize);
      }
      result.push_back(finalize);
      parentSize = size;
    }

    if (!generateComputeCode()) {
      // Allocate memory for values array after assembly if not also computing
      Expr tensor = getTensorVar(write.getTensorVar());
      Expr valuesArr = GetProperty::make(tensor, TensorProperty::Values);
      result.push_back(Allocate::make(valuesArr, parentSize));
    }
  }
  return result.empty() ? Stmt() : Block::blanks(result);
}

Stmt LowererImpl::defineScalarVariable(TensorVar var, bool zero) {
  Datatype type = var.getType().getDataType();
  Expr varValueIR = Var::make(var.getName() + "_val", type, false, false);
  Expr init = (zero) ? ir::Literal::zero(type)
                     : Load::make(GetProperty::make(tensorVars.at(var),
                                                    TensorProperty::Values));
  tensorVars.find(var)->second = varValueIR;
  return VarDecl::make(varValueIR, init);
}

static
vector<Iterator> getIteratorsFrom(IndexVar var, 
                                  const vector<Iterator>& iterators) {
  vector<Iterator> result;
  bool found = false;
  for (Iterator iterator : iterators) {
    if (var == iterator.getIndexVar()) found = true;
    if (found) {
      result.push_back(iterator);
    }
  }
  return result;
}


Stmt LowererImpl::initResultArrays(IndexVar var, vector<Access> writes, 
                                   vector<Access> reads,
                                   set<Access> reducedAccesses) {
  return Stmt();
  if (!generateAssembleCode()) {
    return Stmt();
  }

  multimap<IndexVar, Iterator> readIterators;
  for (auto& read : reads) {
    for (auto& readIterator : getIteratorsFrom(var, getIterators(read))) {
      readIterators.insert({readIterator.getIndexVar(), readIterator});
    }
  }

  vector<Stmt> result;
  for (auto& write : writes) {
    Expr tensor = getTensorVar(write.getTensorVar());
    Expr values = GetProperty::make(tensor, TensorProperty::Values);

    vector<Iterator> iterators = getIteratorsFrom(var, getIterators(write));

    if (iterators.empty()) {
      continue;
    }

    Iterator resultIterator = iterators.front();

    // Initialize begin var
    if (resultIterator.hasAppend() && !resultIterator.isBranchless()) {
      Expr begin = resultIterator.getBeginVar();
      result.push_back(VarDecl::make(begin, resultIterator.getPosVar()));
    }

    const bool isTopLevel = (iterators.size() == write.getIndices().size());
    if (resultIterator.getParent().hasAppend() || isTopLevel) {
      Expr resultParentPos = resultIterator.getParent().getPosVar();
      Expr resultParentPosNext = simplify(ir::Add::make(resultParentPos, 1));
      Expr initBegin = resultParentPos;
      Expr initEnd = resultParentPosNext;
      Expr stride = 1;

      Iterator initIterator;
      for (Iterator iterator : iterators) {
        if (!iterator.hasInsert()) {
          initIterator = iterator;
          break;
        }

        stride = simplify(ir::Mul::make(stride, iterator.getWidth()));
        initBegin = simplify(ir::Mul::make(resultParentPos, stride));
        initEnd = simplify(ir::Mul::make(resultParentPosNext, stride));

        // Initialize data structures for storing insert mode
        result.push_back(iterator.getInsertInitCoords(initBegin, initEnd));
      }

      if (initIterator.defined()) {
        // Initialize data structures for storing edges of next append mode
        taco_iassert(initIterator.hasAppend());
        result.push_back(initIterator.getAppendInitEdges(initBegin, initEnd));
      } else if (generateComputeCode() && !isTopLevel) {
        if (isa<ir::Mul>(stride)) {
          Expr strideVar = Var::make(util::toString(tensor) + "_stride", Int());
          result.push_back(VarDecl::make(strideVar, stride));
          stride = strideVar;
        } 

        // Resize values array if not large enough
        Expr capacityVar = getCapacityVar(tensor);
        Expr size = simplify(ir::Mul::make(resultParentPosNext, stride));
        result.push_back(atLeastDoubleSizeIfFull(values, capacityVar, size));

        if (hasSparseInserts(iterators, readIterators) || 
            util::contains(reducedAccesses, write)) {
          // Zero-initialize values array if might not assign to every element 
          // in values array during compute
          result.push_back(zeroInitValues(tensor, resultParentPos, stride));
        }
      }
    }
  }
  return result.empty() ? Stmt() : Block::make(result);
}


Stmt LowererImpl::resizeAndInitValues(const std::vector<Iterator>& appenders, 
                                      const std::set<Access>& reducedAccesses) {
  if (!generateComputeCode()) {
    return Stmt();
  }

  std::function<Expr(Access)> getTensor = [&](Access access) {
    return getTensorVar(access.getTensorVar());
  };
  const auto reducedTensors = util::map(reducedAccesses, getTensor);

  std::vector<Stmt> result;

  for (auto& appender : appenders) {
    if (!appender.isLeaf()) {
      continue;
    }

    Expr tensor = appender.getTensor(); 
    Expr values = GetProperty::make(tensor, TensorProperty::Values);
    Expr capacity = getCapacityVar(appender.getTensor());
    Expr pos = appender.getIteratorVar();

    if (generateAssembleCode()) {
      result.push_back(doubleSizeIfFull(values, capacity, pos));
    }

    if (util::contains(reducedTensors, tensor)) {
      Expr zero = ir::Literal::zero(tensor.type());
      result.push_back(Store::make(values, pos, zero));
    }
  }

  return result.empty() ? Stmt() : Block::make(result);
}


Stmt LowererImpl::zeroInitValues(Expr tensor, Expr begin, Expr size) {
  Expr lower = simplify(ir::Mul::make(begin, size));
  Expr upper = simplify(ir::Mul::make(ir::Add::make(begin, 1), size));
  Expr p = Var::make("p" + util::toString(tensor), Int());
  Expr values = GetProperty::make(tensor, TensorProperty::Values);
  Stmt zeroInit = Store::make(values, p, ir::Literal::zero(tensor.type()));
  LoopKind parallel = (isa<ir::Literal>(size) && 
                       to<ir::Literal>(size)->getIntValue() < (1 << 10))
                      ? LoopKind::Serial : LoopKind::Static;
  return For::make(p, lower, upper, 1, zeroInit, parallel, false);
}


Stmt LowererImpl::declLocatePosVars(vector<Iterator> locators) {
  vector<Stmt> result;
  for (Iterator& locator : locators) {
    accessibleIterators.insert(locator);

    bool doLocate = true;
    for (Iterator ancestorIterator = locator.getParent();
         !ancestorIterator.isRoot() && ancestorIterator.hasLocate();
         ancestorIterator = ancestorIterator.getParent()) {
      if (!accessibleIterators.contains(ancestorIterator)) {
        doLocate = false;
      }
    }

    if (doLocate) {
      Iterator locateIterator = locator;
      do {
        ModeFunction locate = locateIterator.locate(coordinates(locateIterator));
        taco_iassert(isValue(locate.getResults()[1], true));
        Stmt declarePosVar = VarDecl::make(locateIterator.getPosVar(),
                                           locate.getResults()[0]);
        result.push_back(declarePosVar);

        if (locateIterator.isLeaf()) {
          break;
        }
        
        locateIterator = locateIterator.getChild();
      } while (accessibleIterators.contains(locateIterator));
    }
  }
  return result.empty() ? Stmt() : Block::make(result);
}


Stmt LowererImpl::reduceDuplicateCoordinates(Expr coordinate, 
                                             vector<Iterator> iterators,
                                             bool alwaysReduce) {
  vector<Stmt> result;
  for (Iterator& iterator : iterators) {
    taco_iassert(!iterator.isUnique() && iterator.hasPosIter());

    Access access = this->iterators.modeAccess(iterator).getAccess();
    Expr iterVar = iterator.getIteratorVar();
    Expr segendVar = iterator.getSegendVar();
    Expr reducedVal = iterator.isLeaf() ? getReducedValueVar(access) : Expr();
    Expr tensorVar = getTensorVar(access.getTensorVar());
    Expr tensorVals = GetProperty::make(tensorVar, TensorProperty::Values);

    // Initialize variable storing reduced component value.
    if (reducedVal.defined()) {
      Expr reducedValInit = alwaysReduce 
                          ? Load::make(tensorVals, iterVar)
                          : ir::Literal::zero(reducedVal.type());
      result.push_back(VarDecl::make(reducedVal, reducedValInit));
    }

    if (iterator.isLeaf()) {
      // If iterator is over bottommost coordinate hierarchy level and will 
      // always advance (i.e., not merging with another iterator), then we don't 
      // need a separate segend variable.
      segendVar = iterVar;
      if (alwaysReduce) {
        result.push_back(compoundAssign(segendVar, 1));
      }
    } else {
      Expr segendInit = alwaysReduce ? ir::Add::make(iterVar, 1) : iterVar;
      result.push_back(VarDecl::make(segendVar, segendInit));
    } 
    
    vector<Stmt> dedupStmts;
    if (reducedVal.defined()) {
      Expr partialVal = Load::make(tensorVals, segendVar);
      dedupStmts.push_back(compoundAssign(reducedVal, partialVal));
    }
    dedupStmts.push_back(compoundAssign(segendVar, 1));
    Stmt dedupBody = Block::make(dedupStmts);

    ModeFunction posAccess = iterator.posAccess(segendVar, 
                                                coordinates(iterator));
    // TODO: Support access functions that perform additional computations 
    //       and/or might access invalid positions.
    taco_iassert(!posAccess.compute().defined());
    taco_iassert(to<ir::Literal>(posAccess.getResults()[1])->getBoolValue());
    Expr nextCoord = posAccess.getResults()[0];
    Expr withinBounds = Lt::make(segendVar, iterator.getEndVar());
    Expr isDuplicate = Eq::make(posAccess.getResults()[0], coordinate);
    result.push_back(While::make(And::make(withinBounds, isDuplicate),
                                 Block::make(dedupStmts)));
  }
  return result.empty() ? Stmt() : Block::make(result);
}


Stmt LowererImpl::codeToInitializeIteratorVars(vector<Iterator> iterators) {
  vector<Stmt> result;
  for (Iterator iterator : iterators) {
    taco_iassert(iterator.hasPosIter() || iterator.hasCoordIter() ||
                 iterator.isDimensionIterator());

    Expr iterVar = iterator.getIteratorVar();
    Expr endVar = iterator.getEndVar();
    if (iterator.hasPosIter()) {
      Expr parentPos = iterator.getParent().getPosVar();
      if (iterator.getParent().isRoot() || iterator.getParent().isUnique()) {
        // E.g. a compressed mode without duplicates
        ModeFunction bounds = iterator.posBounds(parentPos);
        result.push_back(bounds.compute());
        result.push_back(VarDecl::make(iterVar, bounds[0]));
        result.push_back(VarDecl::make(endVar, bounds[1]));
      } else {
        taco_iassert(iterator.isOrdered() && iterator.getParent().isOrdered());
        taco_iassert(iterator.isCompact() && iterator.getParent().isCompact());

        // E.g. a compressed mode with duplicates. Apply iterator chaining
        Expr parentSegend = iterator.getParent().getSegendVar();
        ModeFunction startBounds = iterator.posBounds(parentPos);
        ModeFunction endBounds = iterator.posBounds(ir::Sub::make(parentSegend, 1));
        result.push_back(startBounds.compute());
        result.push_back(VarDecl::make(iterVar, startBounds[0]));
        result.push_back(endBounds.compute());
        result.push_back(VarDecl::make(endVar, endBounds[1]));
      }
    }
    else if (iterator.hasCoordIter()) {
      // E.g. a hasmap mode
      vector<Expr> coords = coordinates(iterator);
      coords.erase(coords.begin());
      ModeFunction bounds = iterator.coordBounds(coords);
      result.push_back(bounds.compute());
      result.push_back(VarDecl::make(iterVar, bounds[0]));
      result.push_back(VarDecl::make(endVar, bounds[1]));
    }
    else if (iterator.isDimensionIterator()) {
      // A dimension
      Expr coord = coordinates(vector<Iterator>({iterator}))[0];
      result.push_back(VarDecl::make(coord, 0));
    }
  }
  return result.empty() ? Stmt() : Block::make(result);
}


Stmt LowererImpl::codeToIncIteratorVars(Expr coordinate, vector<Iterator> iterators) {
  if (iterators.size() == 1) {
    Expr ivar = iterators[0].getIteratorVar();

    if (iterators[0].isUnique()) {
      return compoundAssign(ivar, 1); 
    }

    // If iterator is over bottommost coordinate hierarchy level with 
    // duplicates and iterator will always advance (i.e., not merging with 
    // another iterator), then deduplication loop will take care of 
    // incrementing iterator variable.
    return iterators[0].isLeaf() 
           ? Stmt()
           : Assign::make(ivar, iterators[0].getSegendVar());
  }

  vector<Stmt> result;

  // We emit the level iterators before the mode iterator because the coordinate
  // of the mode iterator is used to conditionally advance the level iterators.

  auto levelIterators =
      filter(iterators, [](Iterator it){return !it.isDimensionIterator();});
  for (auto& iterator : levelIterators) {
    Expr ivar = iterator.getIteratorVar();
    if (iterator.isUnique()) {
      Expr increment = iterator.isFull()
                     ? 1
                     : ir::Cast::make(Eq::make(iterator.getCoordVar(), 
                                               coordinate),
                                      ivar.type());
      result.push_back(compoundAssign(ivar, increment));
    } else if (!iterator.isLeaf()) {
      result.push_back(Assign::make(ivar, iterator.getSegendVar()));
    }
  }

  auto modeIterators =
      filter(iterators, [](Iterator it){return it.isDimensionIterator();});
  for (auto& iterator : modeIterators) {
    taco_iassert(iterator.isFull());
    Expr ivar = iterator.getIteratorVar();
    result.push_back(compoundAssign(ivar, 1));
  }

  return Block::make(result);
}


static
bool isLastAppender(Iterator iter) {
  taco_iassert(iter.hasAppend());
  while (!iter.isLeaf()) {
    iter = iter.getChild();
    if (iter.hasAppend()) {
      return false;
    }
  }
  return true;
}


Stmt LowererImpl::appendCoordinate(vector<Iterator> appenders, Expr coord) {
  vector<Stmt> result;
  for (auto& appender : appenders) {
    Expr pos = appender.getPosVar();
    Iterator appenderChild = appender.getChild();

    if (appenderChild.defined() && appenderChild.isBranchless()) {
      // Already emitted assembly code for current level when handling 
      // branchless child level, so don't emit code again.
      continue;
    }

    vector<Stmt> appendStmts;

    if (generateAssembleCode()) {
      appendStmts.push_back(appender.getAppendCoord(pos, coord));
      while (!appender.isRoot() && appender.isBranchless()) {
        // Need to append result coordinate to parent level as well if child 
        // level is branchless (so child coordinates will have unique parents).
        appender = appender.getParent();
        if (!appender.isRoot()) {
          taco_iassert(appender.hasAppend()) << "Parent level of branchless, "
              << "append-capable level must also be append-capable";
          taco_iassert(!appender.isUnique()) << "Need to be able to insert " 
              << "duplicate coordinates to level, but level is declared unique";

          Expr coord = getCoordinateVar(appender);
          appendStmts.push_back(appender.getAppendCoord(pos, coord));
        }
      }
    } 
    
    if (generateAssembleCode() || isLastAppender(appender)) {
      appendStmts.push_back(compoundAssign(pos, 1));

      Stmt appendCode = Block::make(appendStmts);
      if (appenderChild.defined() && appenderChild.hasAppend()) {
        // Emit guard to avoid appending empty slices to result.
        // TODO: Users should be able to configure whether to append zeroes.
        Expr shouldAppend = Lt::make(appenderChild.getBeginVar(), 
                                     appenderChild.getPosVar());
        appendCode = IfThenElse::make(shouldAppend, appendCode);
      }
      result.push_back(appendCode);
    }
  }
  return result.empty() ? Stmt() : Block::make(result);
}


Stmt LowererImpl::generateAppendPositions(vector<Iterator> appenders) {
  vector<Stmt> result;
  if (generateAssembleCode()) {
    for (Iterator appender : appenders) {
      if (!appender.isBranchless()) {
        Expr pos = [](Iterator appender) {
          // Get the position variable associated with the appender. If a mode 
          // is above a branchless mode, then the two modes can share the same 
          // position variable.
          while (!appender.isLeaf() && appender.getChild().isBranchless()) {
            appender = appender.getChild();
          }
          return appender.getPosVar();
        }(appender);
        Expr beginPos = appender.getBeginVar();
        Expr parentPos = appender.getParent().getPosVar();
        result.push_back(appender.getAppendEdges(parentPos, beginPos, pos));
      }
    }
  }
  return result.empty() ? Stmt() : Block::make(result);
}


Expr LowererImpl::generateValueLocExpr(Access access) const {
  if (isScalar(access.getTensorVar().getType())) {
    return ir::Literal::make(0);
  }
  Iterator it = getIterators(access).back();
  return it.getPosVar();
}


Expr LowererImpl::checkThatNoneAreExhausted(std::vector<Iterator> iterators)
{
  taco_iassert(!iterators.empty());
  if (iterators.size() == 1 && iterators[0].isFull()) {
    Expr dimension = getDimension(iterators[0].getIndexVar());
    return Lt::make(iterators[0].getIteratorVar(), dimension);
  }

  vector<Expr> result;
  for (const auto& iterator : iterators) {
    taco_iassert(!iterator.isFull()) << iterator
        << " - full iterators do not need to partake in merge loop bounds";
    Expr iterUnexhausted = Lt::make(iterator.getIteratorVar(),
                                    iterator.getEndVar());
    result.push_back(iterUnexhausted);
  }

  return (!result.empty())
         ? conjunction(result)
         : Lt::make(iterators[0].getIteratorVar(), iterators[0].getEndVar());
}


Stmt LowererImpl::getCounterCounts(Forall forall) const {
  std::vector<Stmt> accessStmts;
  for (const auto counter : this->counters) {
    const auto counterArray = counter.second.array;
    if (counterArray.defined()) {
      const auto counterCount = counter.second.count;
      Expr index = generateDenseArrayIndex(counter.second.indices);
      accessStmts.push_back(VarDecl::make(counterCount, Load::make(counterArray, index)));
    }
  }
  return Block::make(accessStmts);
}


Stmt LowererImpl::incrementCounters(Forall forall) const {
  const auto rhsFreeVars = getRhsFreeVars(forall);
  if (rhsFreeVars.size() != 1 || rhsFreeVars[0] != forall.getIndexVar()) {
    return Stmt();
  }

  std::vector<Stmt> incStmts;
  for (const auto counter : this->counters) {
    const auto counterArray = counter.second.array;
    const auto counterCount = counter.second.count;
    Expr incExpr = ir::Add::make(counterCount, 1);
    if (counterArray.defined()) {
      Expr index = generateDenseArrayIndex(counter.second.indices);
      incStmts.push_back(Store::make(counterArray, index, incExpr));
    } else {
      incStmts.push_back(Assign::make(counterCount, incExpr));
    }
  }
  return Block::make(incStmts);
}


Expr LowererImpl::generateDenseArrayIndex(const std::vector<IndexVar>& indexVars) const {
  Expr pos = 0;
  for (const auto ivar : indexVars) {
    pos = ir::Add::make(ir::Mul::make(pos, getCardinality(ivar)), getCoordinateVar(ivar));
  }
  return pos;
}

}

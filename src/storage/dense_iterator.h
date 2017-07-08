#ifndef TACO_STORAGE_DENSE_H
#define TACO_STORAGE_DENSE_H

#include <string>

#include "iterator.h"
#include "taco/ir/ir.h"

namespace taco {
namespace storage {

class DenseIterator : public IteratorImpl {
public:
  DenseIterator(std::string name, const ir::Expr& tensor, int level,
                size_t dimSize, Iterator previous);
  virtual ~DenseIterator() {};

  bool isDense() const;
  bool isFixedRange() const;

  bool isRandomAccess() const;
  bool isSequentialAccess() const;

  bool hasDuplicates() const;

  RangeType getRangeType() const;

  ir::Expr getPtrVar() const;
  ir::Expr getEndVar() const;
  ir::Expr getIdxVar() const;

  ir::Expr getIteratorVar() const;
  ir::Expr begin() const;
  ir::Expr end() const;

  ir::Expr getIdx(ir::Expr pos) const;

  ir::Stmt initDerivedVars() const;

  ir::Stmt storePtr() const;
  ir::Stmt storeIdx(ir::Expr idx) const;

  ir::Stmt initStorage(ir::Expr size) const;
  ir::Stmt resizePtrStorage(ir::Expr size) const;
  ir::Stmt resizeIdxStorage(ir::Expr size) const;

private:
  ir::Expr tensor;
  int level;

  ir::Expr ptrVar;
  ir::Expr endVar;
  ir::Expr idxVar;

  ir::Expr dimSize;

  ir::Expr getSizeArr() const;
};

}}
#endif

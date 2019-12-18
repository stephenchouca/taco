#include "taco/index_notation/index_notation_visitor.h"
#include "taco/index_notation/index_notation_nodes.h"

using namespace std;

namespace taco {

// class IndexVarExprVisitorStrict
IndexVarExprVisitorStrict::~IndexVarExprVisitorStrict() {
}

void IndexVarExprVisitorStrict::visit(const IndexVarExpr& expr) {
  expr.accept(this);
}


// class IndexVarExprVisitor
IndexVarExprVisitor::~IndexVarExprVisitor() {
}

void IndexVarExprVisitor::visit(const IndexVarAccessNode* op) {
}

void IndexVarExprVisitor::visit(const IndexVarLiteralNode* op) {
}

void IndexVarExprVisitor::visit(const IndexVarAddNode* op) {
  visit(static_cast<const BinaryIndexVarExprNode*>(op));
}

void IndexVarExprVisitor::visit(const IndexVarSubNode* op) {
  visit(static_cast<const BinaryIndexVarExprNode*>(op));
}

void IndexVarExprVisitor::visit(const IndexVarDivNode* op) {
  visit(static_cast<const BinaryIndexVarExprNode*>(op));
}

void IndexVarExprVisitor::visit(const IndexVarCountNode* op) {
}

void IndexVarExprVisitor::visit(const BinaryIndexVarExprNode* op) {
  op->a.accept(this);
  op->b.accept(this);
}

// class IndexExprVisitorStrict
IndexExprVisitorStrict::~IndexExprVisitorStrict() {
}

void IndexExprVisitorStrict::visit(const IndexExpr& expr) {
  expr.accept(this);
}


// class IndexStmtVisitorStrict
IndexStmtVisitorStrict::~IndexStmtVisitorStrict() {
}

void IndexStmtVisitorStrict::visit(const IndexStmt& stmt) {
  stmt.accept(this);
}


// class IndexNotationVisitorStrict
IndexNotationVisitorStrict::~IndexNotationVisitorStrict() {
}


// class IndexNotationVisitor
IndexNotationVisitor::~IndexNotationVisitor() {
}

void IndexNotationVisitor::visit(const AccessNode* op) {
}

void IndexNotationVisitor::visit(const SlicedAccessNode* op) {
}

void IndexNotationVisitor::visit(const LiteralNode* op) {
}

void IndexNotationVisitor::visit(const CoordNode* op) {
}

void IndexNotationVisitor::visit(const NegNode* op) {
  visit(static_cast<const UnaryExprNode*>(op));
}

void IndexNotationVisitor::visit(const SqrtNode* op) {
  visit(static_cast<const UnaryExprNode*>(op));
}

void IndexNotationVisitor::visit(const AddNode* op) {
  visit(static_cast<const BinaryExprNode*>(op));
}

void IndexNotationVisitor::visit(const SubNode* op) {
  visit(static_cast<const BinaryExprNode*>(op));
}

void IndexNotationVisitor::visit(const MulNode* op) {
  visit(static_cast<const BinaryExprNode*>(op));
}

void IndexNotationVisitor::visit(const DivNode* op) {
  visit(static_cast<const BinaryExprNode*>(op));
}

void IndexNotationVisitor::visit(const MaxNode* op) {
  visit(static_cast<const BinaryExprNode*>(op));
}

void IndexNotationVisitor::visit(const MinNode* op) {
  visit(static_cast<const BinaryExprNode*>(op));
}

void IndexNotationVisitor::visit(const CastNode* op) {
  op->a.accept(this);
}

void IndexNotationVisitor::visit(const MapNode* op) {
  op->in.accept(this);
  op->out.accept(this);
}

void IndexNotationVisitor::visit(const CallIntrinsicNode* op) {
  for (auto& arg : op->args) {
    arg.accept(this);
  }
}

void IndexNotationVisitor::visit(const UnaryExprNode* op) {
  op->a.accept(this);
}

void IndexNotationVisitor::visit(const BinaryExprNode* op) {
  op->a.accept(this);
  op->b.accept(this);
}

void IndexNotationVisitor::visit(const ReductionNode* op) {
  op->a.accept(this);
}

void IndexNotationVisitor::visit(const AssignmentNode* op) {
  op->rhs.accept(this);
}

void IndexNotationVisitor::visit(const YieldNode* op) {
  op->expr.accept(this);
}

void IndexNotationVisitor::visit(const ForallNode* op) {
  op->stmt.accept(this);
}

void IndexNotationVisitor::visit(const WhereNode* op) {
  op->producer.accept(this);
  op->consumer.accept(this);
}

void IndexNotationVisitor::visit(const SequenceNode* op) {
  op->definition.accept(this);
  op->mutation.accept(this);
}

void IndexNotationVisitor::visit(const MultiNode* op) {
  op->stmt1.accept(this);
  op->stmt2.accept(this);
}

}

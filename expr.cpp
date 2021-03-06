/*
  Copyright (c) 2010-2011, Intel Corporation
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are
  met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.

    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.

    * Neither the name of Intel Corporation nor the names of its
      contributors may be used to endorse or promote products derived from
      this software without specific prior written permission.


   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
   IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
   TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
   PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
   OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.  
*/

/** @file expr.cpp
    @brief Implementations of expression classes
*/

#include "expr.h"
#include "type.h"
#include "sym.h"
#include "ctx.h"
#include "module.h"
#include "util.h"
#include "llvmutil.h"

#include <list>
#include <set>
#include <stdio.h>
#include <llvm/Module.h>
#include <llvm/Function.h>
#include <llvm/Type.h>
#include <llvm/DerivedTypes.h>
#include <llvm/LLVMContext.h>
#include <llvm/Instructions.h>
#include <llvm/CallingConv.h>
#include <llvm/Target/TargetData.h>
#include <llvm/Support/IRBuilder.h>
#include <llvm/ExecutionEngine/GenericValue.h>
#include <llvm/Support/InstIterator.h>

/////////////////////////////////////////////////////////////////////////////////////
// Expr

llvm::Value *
Expr::GetLValue(FunctionEmitContext *ctx) const {
    // Expressions that can't provide an lvalue can just return NULL
    return NULL;
}


const Type *
Expr::GetLValueType() const {
    // This also only needs to be overrided by Exprs that implement the
    // GetLValue() method.
    return NULL;
}


llvm::Constant *
Expr::GetConstant(const Type *type) const {
    // The default is failure; just return NULL
    return NULL;
}


Symbol *
Expr::GetBaseSymbol() const {
    // Not all expressions can do this, so provide a generally-useful
    // default implementation.
    return NULL;
}


#if 0
/** If a conversion from 'fromAtomicType' to 'toAtomicType' may cause lost
    precision, issue a warning.  Don't warn for conversions to bool and
    conversions between signed and unsigned integers of the same size.
 */
static void
lMaybeIssuePrecisionWarning(const AtomicType *toAtomicType, 
                            const AtomicType *fromAtomicType, 
                            SourcePos pos, const char *errorMsgBase) {
    switch (toAtomicType->basicType) {
    case AtomicType::TYPE_BOOL:
    case AtomicType::TYPE_INT8:
    case AtomicType::TYPE_UINT8:
    case AtomicType::TYPE_INT16:
    case AtomicType::TYPE_UINT16:
    case AtomicType::TYPE_INT32:
    case AtomicType::TYPE_UINT32:
    case AtomicType::TYPE_FLOAT:
    case AtomicType::TYPE_INT64:
    case AtomicType::TYPE_UINT64:
    case AtomicType::TYPE_DOUBLE:
        if ((int)toAtomicType->basicType < (int)fromAtomicType->basicType &&
            toAtomicType->basicType != AtomicType::TYPE_BOOL &&
            !(toAtomicType->basicType == AtomicType::TYPE_INT8 && 
              fromAtomicType->basicType == AtomicType::TYPE_UINT8) &&
            !(toAtomicType->basicType == AtomicType::TYPE_INT16 && 
              fromAtomicType->basicType == AtomicType::TYPE_UINT16) &&
            !(toAtomicType->basicType == AtomicType::TYPE_INT32 && 
              fromAtomicType->basicType == AtomicType::TYPE_UINT32) &&
            !(toAtomicType->basicType == AtomicType::TYPE_INT64 && 
              fromAtomicType->basicType == AtomicType::TYPE_UINT64))
            Warning(pos, "Conversion from type \"%s\" to type \"%s\" for %s"
                    " may lose information.",
                    fromAtomicType->GetString().c_str(), toAtomicType->GetString().c_str(),
                    errorMsgBase);
        break;
    default:
        FATAL("logic error in lMaybeIssuePrecisionWarning()");
    }
}
#endif

///////////////////////////////////////////////////////////////////////////

static Expr *
lArrayToPointer(Expr *expr) {
    Assert(expr && dynamic_cast<const ArrayType *>(expr->GetType()));

    Expr *zero = new ConstExpr(AtomicType::UniformInt32, 0, expr->pos);
    Expr *index = new IndexExpr(expr, zero, expr->pos);
    Expr *addr = new AddressOfExpr(index, expr->pos);
    addr = TypeCheck(addr);
    Assert(addr != NULL);
    addr = Optimize(addr);
    Assert(addr != NULL);
    return addr;
}


static bool
lIsAllIntZeros(Expr *expr) {
    const Type *type = expr->GetType();
    if (type == NULL || type->IsIntType() == false)
        return false;

    ConstExpr *ce = dynamic_cast<ConstExpr *>(expr);
    if (ce == NULL)
        return false;

    uint64_t vals[ISPC_MAX_NVEC];
    int count = ce->AsUInt64(vals);
    if (count == 1) 
        return (vals[0] == 0);
    else {
        for (int i = 0; i < count; ++i)
            if (vals[i] != 0)
                return false;
    }
    return true;
}


static bool
lDoTypeConv(const Type *fromType, const Type *toType, Expr **expr,
            bool failureOk, const char *errorMsgBase, SourcePos pos) {
    /* This function is way too long and complex.  Is type conversion stuff
       always this messy, or can this be cleaned up somehow? */
    Assert(failureOk || errorMsgBase != NULL);

    if (toType == NULL || fromType == NULL)
        return false;

    // The types are equal; there's nothing to do
    if (Type::Equal(toType, fromType))
        return true;

    if (fromType == AtomicType::Void) {
        if (!failureOk)
            Error(pos, "Can't convert from \"void\" to \"%s\" for %s.",
                  toType->GetString().c_str(), errorMsgBase);
        return false;
    }

    if (toType == AtomicType::Void) {
        if (!failureOk)
            Error(pos, "Can't convert type \"%s\" to \"void\" for %s.",
                  fromType->GetString().c_str(), errorMsgBase);
        return false;
    }

    const ArrayType *toArrayType = dynamic_cast<const ArrayType *>(toType);
    const ArrayType *fromArrayType = dynamic_cast<const ArrayType *>(fromType);
    const VectorType *toVectorType = dynamic_cast<const VectorType *>(toType);
    const VectorType *fromVectorType = dynamic_cast<const VectorType *>(fromType);
    const StructType *toStructType = dynamic_cast<const StructType *>(toType);
    const StructType *fromStructType = dynamic_cast<const StructType *>(fromType);
    const EnumType *toEnumType = dynamic_cast<const EnumType *>(toType);
    const EnumType *fromEnumType = dynamic_cast<const EnumType *>(fromType);
    const AtomicType *toAtomicType = dynamic_cast<const AtomicType *>(toType);
    const AtomicType *fromAtomicType = dynamic_cast<const AtomicType *>(fromType);
    const PointerType *fromPointerType = dynamic_cast<const PointerType *>(fromType);
    const PointerType *toPointerType = dynamic_cast<const PointerType *>(toType);

    // Do this early, since for the case of a conversion like
    // "float foo[10]" -> "float * uniform foo", we have what's seemingly
    // a varying to uniform conversion (but not really)
    if (fromArrayType != NULL && toPointerType != NULL) {
        // array to pointer to array element type
        const Type *eltType = fromArrayType->GetElementType();
        if (toPointerType->GetBaseType()->IsConstType())
            eltType = eltType->GetAsConstType();
        if (Type::Equal(toPointerType, 
                        new PointerType(eltType,
                                        toPointerType->IsUniformType(),
                                        toPointerType->IsConstType())))
            goto typecast_ok;
        else {
            if (!failureOk)
                Error(pos, "Can't convert from incompatible array type \"%s\" "
                      "to pointer type \"%s\" for %s.", 
                      fromType->GetString().c_str(),
                      toType->GetString().c_str(), errorMsgBase);
            return false;
        }
    }

    if (toType->IsUniformType() && fromType->IsVaryingType()) {
        if (!failureOk)
            Error(pos, "Can't convert from varying type \"%s\" to uniform "
                  "type \"%s\" for %s.", fromType->GetString().c_str(), 
                  toType->GetString().c_str(), errorMsgBase);
        return false;
    }

    if (fromPointerType != NULL) {
        if (dynamic_cast<const AtomicType *>(toType) != NULL &&
            toType->IsBoolType())
            // Allow implicit conversion of pointers to bools
            goto typecast_ok;

        if (toArrayType != NULL &&
            Type::Equal(fromType->GetBaseType(), toArrayType->GetElementType())) {
            // Can convert pointers to arrays of the same type
            goto typecast_ok;
        }
        if (toPointerType == NULL) {
            if (!failureOk)
                Error(pos, "Can't convert between from pointer type "
                      "\"%s\" to non-pointer type \"%s\" for %s.", 
                      fromType->GetString().c_str(),
                      toType->GetString().c_str(), errorMsgBase);
            return false;
        }
        else if (PointerType::IsVoidPointer(toPointerType)) {
            // any pointer type can be converted to a void *
            goto typecast_ok;
        }
        else if (PointerType::IsVoidPointer(fromPointerType) &&
                 expr != NULL &&
                 dynamic_cast<NullPointerExpr *>(*expr) != NULL) {
            // and a NULL convert to any other pointer type
            goto typecast_ok;
        }
        else if (!Type::Equal(fromPointerType->GetBaseType(), 
                              toPointerType->GetBaseType()) &&
                 !Type::Equal(fromPointerType->GetBaseType()->GetAsConstType(), 
                              toPointerType->GetBaseType())) {
            if (!failureOk)
                Error(pos, "Can't convert between incompatible pointer types "
                      "\"%s\" and \"%s\" for %s.",
                      fromPointerType->GetString().c_str(),
                      toPointerType->GetString().c_str(), errorMsgBase);
            return false;
        }

        if (toType->IsVaryingType() && fromType->IsUniformType())
            goto typecast_ok;

        // Otherwise there's nothing to do
        return true;
    }

    if (toPointerType != NULL && fromAtomicType != NULL && 
        fromAtomicType->IsIntType() && expr != NULL &&
        lIsAllIntZeros(*expr)) {
        // We have a zero-valued integer expression, which can also be
        // treated as a NULL pointer that can be converted to any other
        // pointer type.
        Expr *npe = new NullPointerExpr(pos);
        if (lDoTypeConv(PointerType::Void, toType, &npe,
                        failureOk, errorMsgBase, pos)) {
            *expr = npe;
            return true;
        }
        return false;
    }
        
    // Convert from type T -> const T; just return a TypeCast expr, which
    // can handle this
    if (Type::Equal(toType, fromType->GetAsConstType()))
        goto typecast_ok;
    
    if (dynamic_cast<const ReferenceType *>(fromType)) {
        if (dynamic_cast<const ReferenceType *>(toType)) {
            // Convert from a reference to a type to a const reference to a type;
            // this is handled by TypeCastExpr
            if (Type::Equal(toType->GetReferenceTarget(),
                            fromType->GetReferenceTarget()->GetAsConstType()))
                goto typecast_ok;

            const ArrayType *atFrom = 
                dynamic_cast<const ArrayType *>(fromType->GetReferenceTarget());
            const ArrayType *atTo = 
                dynamic_cast<const ArrayType *>(toType->GetReferenceTarget());

            if (atFrom != NULL && atTo != NULL && 
                Type::Equal(atFrom->GetElementType(), atTo->GetElementType())) {
                goto typecast_ok;
            }
            else {
                if (!failureOk)
                    Error(pos, "Can't convert between incompatible reference types \"%s\" "
                          "and \"%s\" for %s.", fromType->GetString().c_str(),
                          toType->GetString().c_str(), errorMsgBase);
                return false;
            }
        }
        else {
            // convert from a reference T -> T
            if (expr != NULL) {
                Expr *drExpr = new DereferenceExpr(*expr, pos);
                if (lDoTypeConv(drExpr->GetType(), toType, &drExpr, failureOk, 
                                errorMsgBase, pos) == true) {
                    *expr = drExpr;
                    return true;
                }
                return false;
            }
            else
                return lDoTypeConv(fromType->GetReferenceTarget(), toType, NULL,
                                   failureOk, errorMsgBase, pos);
        }
    }
    else if (dynamic_cast<const ReferenceType *>(toType)) {
        // T -> reference T
        if (expr != NULL) {
            Expr *rExpr = new ReferenceExpr(*expr, pos);
            if (lDoTypeConv(rExpr->GetType(), toType, &rExpr, failureOk,
                            errorMsgBase, pos) == true) {
                *expr = rExpr;
                return true;
            }
            return false;
        }
        else
            return lDoTypeConv(new ReferenceType(fromType), toType, NULL, 
                               failureOk, errorMsgBase, pos);
    }
    else if (Type::Equal(toType, fromType->GetAsNonConstType()))
        // convert: const T -> T (as long as T isn't a reference)
        goto typecast_ok;

    fromType = fromType->GetReferenceTarget();
    toType = toType->GetReferenceTarget();
    if (toArrayType && fromArrayType) {
        if (Type::Equal(toArrayType->GetElementType(), 
                        fromArrayType->GetElementType())) {
            // the case of different element counts should have returned
            // successfully earlier, yes??
            Assert(toArrayType->GetElementCount() != fromArrayType->GetElementCount());
            goto typecast_ok;
        }
        else if (Type::Equal(toArrayType->GetElementType(), 
                             fromArrayType->GetElementType()->GetAsConstType())) {
            // T[x] -> const T[x]
            goto typecast_ok;
        }
        else {
            if (!failureOk)
                Error(pos, "Array type \"%s\" can't be converted to type \"%s\" for %s.",
                      fromType->GetString().c_str(), toType->GetString().c_str(),
                      errorMsgBase);
            return false;
        }
    }

    if (toVectorType && fromVectorType) {
        // converting e.g. int<n> -> float<n>
        if (fromVectorType->GetElementCount() != toVectorType->GetElementCount()) {
            if (!failureOk)
                Error(pos, "Can't convert between differently sized vector types "
                      "\"%s\" -> \"%s\" for %s.", fromType->GetString().c_str(),
                      toType->GetString().c_str(), errorMsgBase);
            return false;
        }
        goto typecast_ok;
    }

    if (toStructType && fromStructType) {
        if (!Type::Equal(toStructType->GetAsUniformType()->GetAsConstType(),
                         fromStructType->GetAsUniformType()->GetAsConstType())) {
            if (!failureOk)
                Error(pos, "Can't convert between different struct types "
                      "\"%s\" and \"%s\" for %s.", fromStructType->GetString().c_str(),
                      toStructType->GetString().c_str(), errorMsgBase);
            return false;
        }
        goto typecast_ok;
    }

    if (toEnumType != NULL && fromEnumType != NULL) {
        // No implicit conversions between different enum types
        if (!Type::EqualIgnoringConst(toEnumType->GetAsUniformType(),
                                     fromEnumType->GetAsUniformType())) {
            if (!failureOk)
                Error(pos, "Can't convert between different enum types "
                      "\"%s\" and \"%s\" for %s", fromEnumType->GetString().c_str(),
                      toEnumType->GetString().c_str(), errorMsgBase);
            return false;
        }
        goto typecast_ok;
    }

    // enum -> atomic (integer, generally...) is always ok
    if (fromEnumType != NULL) {
        Assert(toAtomicType != NULL || toVectorType != NULL);
        goto typecast_ok;
    }

    // from here on out, the from type can only be atomic something or
    // other...
    if (fromAtomicType == NULL) {
        if (!failureOk)
            Error(pos, "Type conversion only possible from atomic types, not "
                  "from \"%s\" to \"%s\", for %s.", fromType->GetString().c_str(), 
                  toType->GetString().c_str(), errorMsgBase);
        return false;
    }

    // scalar -> short-vector conversions
    if (toVectorType != NULL)
        goto typecast_ok;

    // ok, it better be a scalar->scalar conversion of some sort by now
    if (toAtomicType == NULL) {
        if (!failureOk)
            Error(pos, "Type conversion only possible to atomic types, not "
                  "from \"%s\" to \"%s\", for %s.",
                  fromType->GetString().c_str(), toType->GetString().c_str(), 
                  errorMsgBase);
        return false;
    }

 typecast_ok:
    if (expr != NULL)
        *expr = new TypeCastExpr(toType, *expr, false, pos);
    return true;
}


bool
CanConvertTypes(const Type *fromType, const Type *toType, 
                const char *errorMsgBase, SourcePos pos) {
    return lDoTypeConv(fromType, toType, NULL, errorMsgBase == NULL,
                       errorMsgBase, pos);
}


Expr *
TypeConvertExpr(Expr *expr, const Type *toType, const char *errorMsgBase) {
    if (expr == NULL)
        return NULL;

    const Type *fromType = expr->GetType();
    Expr *e = expr;
    if (lDoTypeConv(fromType, toType, &e, false, errorMsgBase, 
                    expr->pos))
        return e;
    else
        return NULL;
}


///////////////////////////////////////////////////////////////////////////

/** Given an atomic or vector type, this returns a boolean type with the
    same "shape".  In other words, if the given type is a vector type of
    three uniform ints, the returned type is a vector type of three uniform
    bools. */
static const Type *
lMatchingBoolType(const Type *type) {
    bool uniformTest = type->IsUniformType();
    const AtomicType *boolBase = uniformTest ? AtomicType::UniformBool : 
                                               AtomicType::VaryingBool;
    const VectorType *vt = dynamic_cast<const VectorType *>(type);
    if (vt != NULL)
        return new VectorType(boolBase, vt->GetElementCount());
    else {
        Assert(dynamic_cast<const AtomicType *>(type) != NULL ||
               dynamic_cast<const PointerType *>(type) != NULL);
        return boolBase;
    }
}

///////////////////////////////////////////////////////////////////////////
// UnaryExpr

static llvm::Constant *
lLLVMConstantValue(const Type *type, llvm::LLVMContext *ctx, double value) {
    const AtomicType *atomicType = dynamic_cast<const AtomicType *>(type);
    const EnumType *enumType = dynamic_cast<const EnumType *>(type);
    const VectorType *vectorType = dynamic_cast<const VectorType *>(type);
    const PointerType *pointerType = dynamic_cast<const PointerType *>(type);

    // This function is only called with, and only works for atomic, enum,
    // and vector types.
    Assert(atomicType != NULL || enumType != NULL || vectorType != NULL ||
           pointerType != NULL);

    if (atomicType != NULL || enumType != NULL) {
        // If it's an atomic or enuemrator type, then figure out which of
        // the llvmutil.h functions to call to get the corresponding
        // constant and then call it...
        bool isUniform = type->IsUniformType();
        AtomicType::BasicType basicType = (enumType != NULL) ? 
            AtomicType::TYPE_UINT32 : atomicType->basicType;

        switch (basicType) {
        case AtomicType::TYPE_VOID:
            FATAL("can't get constant value for void type");
            return NULL;
        case AtomicType::TYPE_BOOL:
            if (isUniform)
                return (value != 0.) ? LLVMTrue : LLVMFalse;
            else
                return LLVMBoolVector(value != 0.);
        case AtomicType::TYPE_INT8: {
            int i = (int)value;
            Assert((double)i == value);
            return isUniform ? LLVMInt8(i) : LLVMInt8Vector(i);
        }
        case AtomicType::TYPE_UINT8: {
            unsigned int i = (unsigned int)value;
            return isUniform ? LLVMUInt8(i) : LLVMUInt8Vector(i);
        }
        case AtomicType::TYPE_INT16: {
            int i = (int)value;
            Assert((double)i == value);
            return isUniform ? LLVMInt16(i) : LLVMInt16Vector(i);
        }
        case AtomicType::TYPE_UINT16: {
            unsigned int i = (unsigned int)value;
            return isUniform ? LLVMUInt16(i) : LLVMUInt16Vector(i);
        }
        case AtomicType::TYPE_INT32: {
            int i = (int)value;
            Assert((double)i == value);
            return isUniform ? LLVMInt32(i) : LLVMInt32Vector(i);
        }
        case AtomicType::TYPE_UINT32: {
            unsigned int i = (unsigned int)value;
            return isUniform ? LLVMUInt32(i) : LLVMUInt32Vector(i);
        }
        case AtomicType::TYPE_FLOAT:
            return isUniform ? LLVMFloat((float)value) : 
                               LLVMFloatVector((float)value);
        case AtomicType::TYPE_UINT64: {
            uint64_t i = (uint64_t)value;
            Assert(value == (int64_t)i);
            return isUniform ? LLVMUInt64(i) : LLVMUInt64Vector(i);
        }
        case AtomicType::TYPE_INT64: {
            int64_t i = (int64_t)value;
            Assert((double)i == value);
            return isUniform ? LLVMInt64(i) : LLVMInt64Vector(i);
        }
        case AtomicType::TYPE_DOUBLE:
            return isUniform ? LLVMDouble(value) : LLVMDoubleVector(value);
        default:
            FATAL("logic error in lLLVMConstantValue");
            return NULL;
        }
    }
    else if (pointerType != NULL) {
        Assert(value == 0);
        if (pointerType->IsUniformType())
            return llvm::Constant::getNullValue(LLVMTypes::VoidPointerType);
        else
            return llvm::Constant::getNullValue(LLVMTypes::VoidPointerVectorType);
    }
    else {
        // For vector types, first get the LLVM constant for the basetype with
        // a recursive call to lLLVMConstantValue().
        const Type *baseType = vectorType->GetBaseType();
        llvm::Constant *constElement = lLLVMConstantValue(baseType, ctx, value);
        LLVM_TYPE_CONST llvm::Type *llvmVectorType = vectorType->LLVMType(ctx);

        // Now create a constant version of the corresponding LLVM type that we
        // use to represent the VectorType.
        // FIXME: this is a little ugly in that the fact that ispc represents
        // uniform VectorTypes as LLVM VectorTypes and varying VectorTypes as
        // LLVM ArrayTypes leaks into the code here; it feels like this detail
        // should be better encapsulated?
        if (baseType->IsUniformType()) {
            LLVM_TYPE_CONST llvm::VectorType *lvt = 
                llvm::dyn_cast<LLVM_TYPE_CONST llvm::VectorType>(llvmVectorType);
            Assert(lvt != NULL);
            std::vector<llvm::Constant *> vals;
            for (unsigned int i = 0; i < lvt->getNumElements(); ++i)
                vals.push_back(constElement);
            return llvm::ConstantVector::get(vals);
        }
        else {
            LLVM_TYPE_CONST llvm::ArrayType *lat = 
                llvm::dyn_cast<LLVM_TYPE_CONST llvm::ArrayType>(llvmVectorType);
            Assert(lat != NULL);
            std::vector<llvm::Constant *> vals;
            for (unsigned int i = 0; i < lat->getNumElements(); ++i)
                vals.push_back(constElement);
            return llvm::ConstantArray::get(lat, vals);
        }
    }
}


static llvm::Value *
lMaskForSymbol(Symbol *baseSym, FunctionEmitContext *ctx) {
    if (dynamic_cast<const PointerType *>(baseSym->type) != NULL ||
        dynamic_cast<const ReferenceType *>(baseSym->type) != NULL)
        // FIXME: for pointers, we really only want to do this for
        // dereferencing the pointer, not for things like pointer
        // arithmetic, when we may be able to use the internal mask,
        // depending on context...
        return ctx->GetFullMask();

    llvm::Value *mask = (baseSym->parentFunction == ctx->GetFunction() && 
                         baseSym->storageClass != SC_STATIC) ? 
        ctx->GetInternalMask() : ctx->GetFullMask();
    return mask;
}


/** Store the result of an assignment to the given location. 
 */
static void
lStoreAssignResult(llvm::Value *value, llvm::Value *ptr, const Type *ptrType,
                   FunctionEmitContext *ctx, Symbol *baseSym) {
    Assert(baseSym != NULL &&
           baseSym->varyingCFDepth <= ctx->VaryingCFDepth());
    if (!g->opt.disableMaskedStoreToStore &&
        !g->opt.disableMaskAllOnOptimizations &&
        baseSym->varyingCFDepth == ctx->VaryingCFDepth() &&
        baseSym->storageClass != SC_STATIC &&
        dynamic_cast<const ReferenceType *>(baseSym->type) == NULL &&
        dynamic_cast<const PointerType *>(baseSym->type) == NULL) {
        // If the variable is declared at the same varying control flow
        // depth as where it's being assigned, then we don't need to do any
        // masking but can just do the assignment as if all the lanes were
        // known to be on.  While this may lead to random/garbage values
        // written into the lanes that are off, by definition they will
        // never be accessed, since those lanes aren't executing, and won't
        // be executing at this scope or any other one before the variable
        // goes out of scope.
        ctx->StoreInst(value, ptr, LLVMMaskAllOn, ptrType);
    }
    else {
        ctx->StoreInst(value, ptr, lMaskForSymbol(baseSym, ctx), ptrType);
    }
}


/** Utility routine to emit code to do a {pre,post}-{inc,dec}rement of the
    given expresion.
 */
static llvm::Value *
lEmitPrePostIncDec(UnaryExpr::Op op, Expr *expr, SourcePos pos,
                   FunctionEmitContext *ctx) {
    const Type *type = expr->GetType();
    if (type == NULL)
        return NULL;

    // Get both the lvalue and the rvalue of the given expression
    llvm::Value *lvalue = NULL, *rvalue = NULL;
    const Type *lvalueType = NULL;
    if (dynamic_cast<const ReferenceType *>(type) != NULL) {
        lvalueType = type;
        type = type->GetReferenceTarget();
        lvalue = expr->GetValue(ctx);

        Expr *deref = new DereferenceExpr(expr, expr->pos);
        rvalue = deref->GetValue(ctx);
    }
    else {
        lvalue = expr->GetLValue(ctx);
        lvalueType = expr->GetLValueType();
        rvalue = expr->GetValue(ctx);
    }

    if (lvalue == NULL) {
        // If we can't get a lvalue, then we have an error here 
        const char *prepost = (op == UnaryExpr::PreInc || 
                               op == UnaryExpr::PreDec) ? "pre" : "post";
        const char *incdec = (op == UnaryExpr::PreInc || 
                              op == UnaryExpr::PostInc) ? "increment" : "decrement";
        Error(pos, "Can't %s-%s non-lvalues.", prepost, incdec);
        return NULL;
    }

    // Emit code to do the appropriate addition/subtraction to the
    // expression's old value
    ctx->SetDebugPos(pos);
    llvm::Value *binop = NULL;
    int delta = (op == UnaryExpr::PreInc || op == UnaryExpr::PostInc) ? 1 : -1;

    if (dynamic_cast<const PointerType *>(type) != NULL) {
        const Type *incType = type->IsUniformType() ? AtomicType::UniformInt32 :
            AtomicType::VaryingInt32;
        llvm::Constant *dval = lLLVMConstantValue(incType, g->ctx, delta);
        binop = ctx->GetElementPtrInst(rvalue, dval, type, "ptr_inc_or_dec");
    }
    else {
        llvm::Constant *dval = lLLVMConstantValue(type, g->ctx, delta);
        if (type->IsFloatType())
            binop = ctx->BinaryOperator(llvm::Instruction::FAdd, rvalue, 
                                        dval, "val_inc_or_dec");
        else
            binop = ctx->BinaryOperator(llvm::Instruction::Add, rvalue, 
                                        dval, "val_inc_or_dec");
    }

    // And store the result out to the lvalue
    Symbol *baseSym = expr->GetBaseSymbol();
    lStoreAssignResult(binop, lvalue, lvalueType, ctx, baseSym);

    // And then if it's a pre increment/decrement, return the final
    // computed result; otherwise return the previously-grabbed expression
    // value.
    return (op == UnaryExpr::PreInc || op == UnaryExpr::PreDec) ? binop : rvalue;
}



/** Utility routine to emit code to negate the given expression.
 */
static llvm::Value *
lEmitNegate(Expr *arg, SourcePos pos, FunctionEmitContext *ctx) {
    const Type *type = arg->GetType();
    llvm::Value *argVal = arg->GetValue(ctx);
    if (type == NULL || argVal == NULL)
        return NULL;

    // Negate by subtracting from zero...
    llvm::Value *zero = lLLVMConstantValue(type, g->ctx, 0.);
    ctx->SetDebugPos(pos);
    if (type->IsFloatType())
        return ctx->BinaryOperator(llvm::Instruction::FSub, zero, argVal,
                                   "fnegate");
    else {
        Assert(type->IsIntType());
        return ctx->BinaryOperator(llvm::Instruction::Sub, zero, argVal,
                                   "inegate");
    }
}


UnaryExpr::UnaryExpr(Op o, Expr *e, SourcePos p) 
  : Expr(p), op(o) { 
    expr = e;
}


llvm::Value *
UnaryExpr::GetValue(FunctionEmitContext *ctx) const {
    if (expr == NULL)
        return NULL;

    ctx->SetDebugPos(pos);

    switch (op) {
    case PreInc:
    case PreDec:
    case PostInc:
    case PostDec:
        return lEmitPrePostIncDec(op, expr, pos, ctx);
    case Negate:
        return lEmitNegate(expr, pos, ctx);
    case LogicalNot: {
        llvm::Value *argVal = expr->GetValue(ctx);
        return ctx->NotOperator(argVal, "logicalnot");
    }
    case BitNot: {
        llvm::Value *argVal = expr->GetValue(ctx);
        return ctx->NotOperator(argVal, "bitnot");
    }
    default:
        FATAL("logic error");
        return NULL;
    }
}


const Type *
UnaryExpr::GetType() const {
    if (expr == NULL)
        return NULL;

    const Type *type = expr->GetType();
    if (type == NULL)
        return NULL;

    // For all unary expressions besides logical not, the returned type is
    // the same as the source type.  Logical not always returns a bool
    // type, with the same shape as the input type.
    switch (op) {
    case PreInc:
    case PreDec:
    case PostInc:
    case PostDec:
    case Negate:
    case BitNot: 
        return type;
    case LogicalNot:
        return lMatchingBoolType(type);
    default:
        FATAL("error");
        return NULL;
    }
}


Expr *
UnaryExpr::Optimize() {
    ConstExpr *constExpr = dynamic_cast<ConstExpr *>(expr);
    // If the operand isn't a constant, then we can't do any optimization
    // here...
    if (constExpr == NULL)
        return this;

    const Type *type = constExpr->GetType();
    bool isEnumType = dynamic_cast<const EnumType *>(type) != NULL;

    const Type *baseType = type->GetAsNonConstType()->GetAsUniformType();
    if (baseType == AtomicType::UniformInt8 ||
        baseType == AtomicType::UniformUInt8 ||
        baseType == AtomicType::UniformInt16 ||
        baseType == AtomicType::UniformUInt16 ||
        baseType == AtomicType::UniformInt64 ||
        baseType == AtomicType::UniformUInt64)
        // FIXME: should handle these at some point; for now we only do
        // constant folding for bool, int32 and float types...
        return this;

    switch (op) {
    case PreInc:
    case PreDec:
    case PostInc:
    case PostDec:
        // this shouldn't happen--it's illegal to modify a contant value..
        // An error will be issued elsewhere...
        return this;
    case Negate: {
        // Since we currently only handle int32, floats, and doubles here,
        // it's safe to stuff whatever we have into a double, do the negate
        // as a double, and then return a ConstExpr with the same type as
        // the original...
        double v[ISPC_MAX_NVEC];
        int count = constExpr->AsDouble(v);
        for (int i = 0; i < count; ++i)
            v[i] = -v[i];
        return new ConstExpr(constExpr, v);
    }
    case BitNot: {
        if (type == AtomicType::UniformInt32 || 
            type == AtomicType::VaryingInt32 ||
            type == AtomicType::UniformConstInt32 || 
            type == AtomicType::VaryingConstInt32) {
            int32_t v[ISPC_MAX_NVEC];
            int count = constExpr->AsInt32(v);
            for (int i = 0; i < count; ++i)
                v[i] = ~v[i];
            return new ConstExpr(type, v, pos);
        }
        else if (type == AtomicType::UniformUInt32 || 
                 type == AtomicType::VaryingUInt32 ||
                 type == AtomicType::UniformConstUInt32 || 
                 type == AtomicType::VaryingConstUInt32 ||
                 isEnumType == true) {
            uint32_t v[ISPC_MAX_NVEC];
            int count = constExpr->AsUInt32(v);
            for (int i = 0; i < count; ++i)
                v[i] = ~v[i];
            return new ConstExpr(type, v, pos);
        }
        else
            FATAL("unexpected type in UnaryExpr::Optimize() / BitNot case");
    }
    case LogicalNot: {
        Assert(type == AtomicType::UniformBool || 
               type == AtomicType::VaryingBool ||
               type == AtomicType::UniformConstBool || 
               type == AtomicType::VaryingConstBool);
        bool v[ISPC_MAX_NVEC];
        int count = constExpr->AsBool(v);
        for (int i = 0; i < count; ++i)
            v[i] = !v[i];
        return new ConstExpr(type, v, pos);
    }
    default:
        FATAL("unexpected op in UnaryExpr::Optimize()");
        return NULL;
    }
}


Expr *
UnaryExpr::TypeCheck() {
    const Type *type;
    if (expr == NULL || (type = expr->GetType()) == NULL)
        // something went wrong in type checking...
        return NULL;

    if (op == PreInc || op == PreDec || op == PostInc || op == PostDec) {
        if (type->IsConstType()) {
            Error(pos, "Can't assign to type \"%s\" on left-hand side of "
                  "expression.", type->GetString().c_str());
            return NULL;
        }

        if (type->IsNumericType())
            return this;

        if (dynamic_cast<const PointerType *>(type) == NULL) {
            Error(expr->pos, "Can only pre/post increment numeric and "
                  "pointer types, not \"%s\".", type->GetString().c_str());
            return NULL;
        }

        if (PointerType::IsVoidPointer(type)) {
            Error(expr->pos, "Illegal to pre/post increment \"%s\" type.",
                  type->GetString().c_str());
            return NULL;
        }

        return this;
    }

    // don't do this for pre/post increment/decrement
    if (dynamic_cast<const ReferenceType *>(type)) {
        expr = new DereferenceExpr(expr, pos);
        type = expr->GetType();
    }

    if (op == Negate) {
        if (!type->IsNumericType()) {
            Error(expr->pos, "Negate not allowed for non-numeric type \"%s\".", 
                  type->GetString().c_str());
            return NULL;
        }
    }
    else if (op == LogicalNot) {
        const Type *boolType = lMatchingBoolType(type);
        expr = TypeConvertExpr(expr, boolType, "logical not");
        if (expr == NULL)
            return NULL;
    }
    else if (op == BitNot) {
        if (!type->IsIntType()) {
            Error(expr->pos, "~ operator can only be used with integer types, "
                  "not \"%s\".", type->GetString().c_str());
            return NULL;
        }
    }
    return this;
}


int
UnaryExpr::EstimateCost() const {
    return COST_SIMPLE_ARITH_LOGIC_OP;
}


void
UnaryExpr::Print() const {
    if (!expr || !GetType())
        return;

    printf("[ %s ] (", GetType()->GetString().c_str());
    if (op == PreInc) printf("++");
    if (op == PreDec) printf("--");
    if (op == Negate) printf("-");
    if (op == LogicalNot) printf("!");
    if (op == BitNot) printf("~");
    printf("(");
    expr->Print();
    printf(")");
    if (op == PostInc) printf("++");
    if (op == PostDec) printf("--");
    printf(")");
    pos.Print();
}


///////////////////////////////////////////////////////////////////////////
// BinaryExpr

static const char *
lOpString(BinaryExpr::Op op) {
    switch (op) {
    case BinaryExpr::Add:        return "+";
    case BinaryExpr::Sub:        return "-";
    case BinaryExpr::Mul:        return "*";
    case BinaryExpr::Div:        return "/";
    case BinaryExpr::Mod:        return "%";
    case BinaryExpr::Shl:        return "<<";
    case BinaryExpr::Shr:        return ">>";
    case BinaryExpr::Lt:         return "<";
    case BinaryExpr::Gt:         return ">";
    case BinaryExpr::Le:         return "<=";
    case BinaryExpr::Ge:         return ">=";
    case BinaryExpr::Equal:      return "==";
    case BinaryExpr::NotEqual:   return "!=";
    case BinaryExpr::BitAnd:     return "&";
    case BinaryExpr::BitXor:     return "^";
    case BinaryExpr::BitOr:      return "|";
    case BinaryExpr::LogicalAnd: return "&&";
    case BinaryExpr::LogicalOr:  return "||";
    case BinaryExpr::Comma:      return ",";
    default:
        FATAL("unimplemented case in lOpString()");
        return "";
    }
}


/** Utility routine to emit the binary bitwise operator corresponding to
    the given BinaryExpr::Op. 
*/
static llvm::Value *
lEmitBinaryBitOp(BinaryExpr::Op op, llvm::Value *arg0Val,
                 llvm::Value *arg1Val, bool isUnsigned,
                 FunctionEmitContext *ctx) {
    llvm::Instruction::BinaryOps inst;
    switch (op) {
    case BinaryExpr::Shl:    inst = llvm::Instruction::Shl;  break;
    case BinaryExpr::Shr:
        if (isUnsigned)
            inst = llvm::Instruction::LShr; 
        else
            inst = llvm::Instruction::AShr; 
        break; 
    case BinaryExpr::BitAnd: inst = llvm::Instruction::And;  break;
    case BinaryExpr::BitXor: inst = llvm::Instruction::Xor;  break;
    case BinaryExpr::BitOr:  inst = llvm::Instruction::Or;   break;
    default:
        FATAL("logic error in lEmitBinaryBitOp()");
        return NULL;
    }

    return ctx->BinaryOperator(inst, arg0Val, arg1Val, "bitop");
}


/** Utility routine to emit binary arithmetic operator based on the given
    BinaryExpr::Op.
*/
static llvm::Value *
lEmitBinaryArith(BinaryExpr::Op op, llvm::Value *value0, llvm::Value *value1,
                 const Type *type0, const Type *type1,
                 FunctionEmitContext *ctx, SourcePos pos) {
    const PointerType *ptrType = dynamic_cast<const PointerType *>(type0);

    if (ptrType != NULL) {
        switch (op) {
        case BinaryExpr::Add:
            // ptr + integer
            return ctx->GetElementPtrInst(value0, value1, ptrType, "ptrmath");
            break;
        case BinaryExpr::Sub: {
            if (dynamic_cast<const PointerType *>(type1) != NULL) {
                // ptr - ptr
                if (ptrType->IsUniformType()) {
                    value0 = ctx->PtrToIntInst(value0);
                    value1 = ctx->PtrToIntInst(value1);
                }

                // Compute the difference in bytes
                llvm::Value *delta = 
                    ctx->BinaryOperator(llvm::Instruction::Sub, value0, value1,
                                        "ptr_diff");

                // Now divide by the size of the type that the pointer
                // points to in order to return the difference in elements.
                LLVM_TYPE_CONST llvm::Type *llvmElementType = 
                    ptrType->GetBaseType()->LLVMType(g->ctx);
                llvm::Value *size = g->target.SizeOf(llvmElementType);
                if (ptrType->IsVaryingType())
                    size = ctx->SmearUniform(size);

                if (g->target.is32Bit == false && 
                    g->opt.force32BitAddressing == true) {
                    // If we're doing 32-bit addressing math on a 64-bit
                    // target, then trunc the delta down to a 32-bit value.
                    // (Thus also matching what will be a 32-bit value
                    // returned from SizeOf above.)
                    if (ptrType->IsUniformType())
                        delta = ctx->TruncInst(delta, LLVMTypes::Int32Type,
                                               "trunc_ptr_delta");
                    else
                        delta = ctx->TruncInst(delta, LLVMTypes::Int32VectorType,
                                               "trunc_ptr_delta");
                }

                // And now do the actual division
                return ctx->BinaryOperator(llvm::Instruction::SDiv, delta, size,
                                           "element_diff");
            }
            else {
                // ptr - integer
                llvm::Value *zero = lLLVMConstantValue(type1, g->ctx, 0.);
                llvm::Value *negOffset = 
                    ctx->BinaryOperator(llvm::Instruction::Sub, zero, value1, 
                                        "negate");
                // Do a GEP as ptr + -integer
                return ctx->GetElementPtrInst(value0, negOffset, ptrType, 
                                              "ptrmath");
            }
        }
        default:
            FATAL("Logic error in lEmitBinaryArith() for pointer type case");
            return NULL;
        }
    }
    else {
        Assert(Type::EqualIgnoringConst(type0, type1));

        llvm::Instruction::BinaryOps inst;
        bool isFloatOp = type0->IsFloatType();
        bool isUnsignedOp = type0->IsUnsignedType();

        switch (op) {
        case BinaryExpr::Add:
            inst = isFloatOp ? llvm::Instruction::FAdd : llvm::Instruction::Add;
            break;
        case BinaryExpr::Sub:
            inst = isFloatOp ? llvm::Instruction::FSub : llvm::Instruction::Sub;
            break;
        case BinaryExpr::Mul:
            inst = isFloatOp ? llvm::Instruction::FMul : llvm::Instruction::Mul;
            break;
        case BinaryExpr::Div:
            if (type0->IsVaryingType() && !isFloatOp)
                PerformanceWarning(pos, "Division with varying integer types is "
                                   "very inefficient."); 
            inst = isFloatOp ? llvm::Instruction::FDiv : 
                (isUnsignedOp ? llvm::Instruction::UDiv : llvm::Instruction::SDiv);
            break;
        case BinaryExpr::Mod:
            if (type0->IsVaryingType() && !isFloatOp)
                PerformanceWarning(pos, "Modulus operator with varying types is "
                                   "very inefficient."); 
            inst = isFloatOp ? llvm::Instruction::FRem : 
                (isUnsignedOp ? llvm::Instruction::URem : llvm::Instruction::SRem);
            break;
        default:
            FATAL("Invalid op type passed to lEmitBinaryArith()");
            return NULL;
        }

        return ctx->BinaryOperator(inst, value0, value1, "binop");
    }
}


/** Utility routine to emit a binary comparison operator based on the given
    BinaryExpr::Op.
 */
static llvm::Value *
lEmitBinaryCmp(BinaryExpr::Op op, llvm::Value *e0Val, llvm::Value *e1Val,
               const Type *type, FunctionEmitContext *ctx, SourcePos pos) {
    bool isFloatOp = type->IsFloatType();
    bool isUnsignedOp = type->IsUnsignedType();

    llvm::CmpInst::Predicate pred;
    switch (op) {
    case BinaryExpr::Lt:
        pred = isFloatOp ? llvm::CmpInst::FCMP_OLT : 
            (isUnsignedOp ? llvm::CmpInst::ICMP_ULT : llvm::CmpInst::ICMP_SLT);
        break;
    case BinaryExpr::Gt:
        pred = isFloatOp ? llvm::CmpInst::FCMP_OGT : 
            (isUnsignedOp ? llvm::CmpInst::ICMP_UGT : llvm::CmpInst::ICMP_SGT);
        break;
    case BinaryExpr::Le:
        pred = isFloatOp ? llvm::CmpInst::FCMP_OLE : 
            (isUnsignedOp ? llvm::CmpInst::ICMP_ULE : llvm::CmpInst::ICMP_SLE);
        break;
    case BinaryExpr::Ge:
        pred = isFloatOp ? llvm::CmpInst::FCMP_OGE : 
            (isUnsignedOp ? llvm::CmpInst::ICMP_UGE : llvm::CmpInst::ICMP_SGE);
        break;
    case BinaryExpr::Equal:
        pred = isFloatOp ? llvm::CmpInst::FCMP_OEQ : llvm::CmpInst::ICMP_EQ;
        break;
    case BinaryExpr::NotEqual:
        pred = isFloatOp ? llvm::CmpInst::FCMP_ONE : llvm::CmpInst::ICMP_NE;
        break;
    default:
        FATAL("error in lEmitBinaryCmp()");
        return NULL;
    }

    llvm::Value *cmp = ctx->CmpInst(isFloatOp ? llvm::Instruction::FCmp : 
                                    llvm::Instruction::ICmp,
                                    pred, e0Val, e1Val, "bincmp");
    // This is a little ugly: CmpInst returns i1 values, but we use vectors
    // of i32s for varying bool values; type convert the result here if
    // needed.
    if (type->IsVaryingType())
        cmp = ctx->I1VecToBoolVec(cmp);

    return cmp;
}


BinaryExpr::BinaryExpr(Op o, Expr *a, Expr *b, SourcePos p) 
    : Expr(p), op(o) {
    arg0 = a;
    arg1 = b;
}


llvm::Value *
BinaryExpr::GetValue(FunctionEmitContext *ctx) const {
    if (!arg0 || !arg1)
        return NULL;

    llvm::Value *value0 = arg0->GetValue(ctx);
    llvm::Value *value1 = arg1->GetValue(ctx);
    ctx->SetDebugPos(pos);

    switch (op) {
    case Add:
    case Sub:
    case Mul:
    case Div:
    case Mod:
        return lEmitBinaryArith(op, value0, value1, arg0->GetType(), arg1->GetType(),
                                ctx, pos);
    case Lt:
    case Gt:
    case Le:
    case Ge:
    case Equal:
    case NotEqual:
        return lEmitBinaryCmp(op, value0, value1, arg0->GetType(), ctx, pos);
    case Shl:
    case Shr:
    case BitAnd:
    case BitXor:
    case BitOr: {
        if (op == Shr && arg1->GetType()->IsVaryingType() && 
            dynamic_cast<ConstExpr *>(arg1) == NULL)
            PerformanceWarning(pos, "Shift right is extremely inefficient for "
                               "varying shift amounts.");
        return lEmitBinaryBitOp(op, value0, value1, 
                                arg0->GetType()->IsUnsignedType(), ctx);
    }
    case LogicalAnd:
        return ctx->BinaryOperator(llvm::Instruction::And, value0, value1,
                                   "logical_and");
    case LogicalOr:
        return ctx->BinaryOperator(llvm::Instruction::Or, value0, value1, 
                                   "logical_or");
    case Comma:
        return value1;
    default:
        FATAL("logic error");
        return NULL;
    }
}


const Type *
BinaryExpr::GetType() const {
    if (arg0 == NULL || arg1 == NULL)
        return NULL;

    const Type *type0 = arg0->GetType(), *type1 = arg1->GetType();
    if (type0 == NULL || type1 == NULL)
        return NULL;

    // If this hits, it means that our TypeCheck() method hasn't been
    // called before GetType() was called; adding two pointers is illegal
    // and will fail type checking and (int + ptr) should be canonicalized
    // into (ptr + int) by type checking.
    if (op == Add)
        Assert(dynamic_cast<const PointerType *>(type1) == NULL);

    if (op == Comma)
        return arg1->GetType();

    if (dynamic_cast<const PointerType *>(type0) != NULL) {
        if (op == Add)
            // ptr + int -> ptr
            return type0;
        else if (op == Sub) {
            if (dynamic_cast<const PointerType *>(type1) != NULL) {
                // ptr - ptr -> ~ptrdiff_t
                const Type *diffType = (g->target.is32Bit || 
                                        g->opt.force32BitAddressing) ? 
                    AtomicType::UniformInt32 : AtomicType::UniformInt64;
                if (type0->IsVaryingType() || type1->IsVaryingType())
                    diffType = diffType->GetAsVaryingType();
                return diffType;
            }
            else
                // ptr - int -> ptr
                return type0;
        }

        // otherwise fall through for these...
        Assert(op == Lt || op == Gt || op == Le || op == Ge ||
               op == Equal || op == NotEqual);
    }

    const Type *exprType = Type::MoreGeneralType(type0, type1, pos, lOpString(op));
    // I don't think that MoreGeneralType should be able to fail after the
    // checks done in BinaryExpr::TypeCheck().
    Assert(exprType != NULL);

    switch (op) {
    case Add:
    case Sub:
    case Mul:
    case Div:
    case Mod:
        return exprType;
    case Lt:
    case Gt:
    case Le:
    case Ge:
    case Equal:
    case NotEqual:
    case LogicalAnd:
    case LogicalOr:
        return lMatchingBoolType(exprType);
    case Shl:
    case Shr:
        return type1->IsVaryingType() ? type0->GetAsVaryingType() : type0;
    case BitAnd:
    case BitXor:
    case BitOr:
        return exprType;
    case Comma:
        // handled above, so fall through here just in case
    default:
        FATAL("logic error in BinaryExpr::GetType()");
        return NULL;
    }
}


#define FOLD_OP(O, E)                           \
    case O:                                     \
        for (int i = 0; i < count; ++i)         \
            result[i] = (v0[i] E v1[i]);        \
        break

/** Constant fold the binary integer operations that aren't also applicable
    to floating-point types. 
*/
template <typename T> static ConstExpr *
lConstFoldBinIntOp(BinaryExpr::Op op, const T *v0, const T *v1, ConstExpr *carg0) {
    T result[ISPC_MAX_NVEC];
    int count = carg0->Count();
        
    switch (op) {
        FOLD_OP(BinaryExpr::Mod, %);
        FOLD_OP(BinaryExpr::Shl, <<);
        FOLD_OP(BinaryExpr::Shr, >>);
        FOLD_OP(BinaryExpr::BitAnd, &);
        FOLD_OP(BinaryExpr::BitXor, ^);
        FOLD_OP(BinaryExpr::BitOr, |);
    default:
        return NULL;
    }

    return new ConstExpr(carg0->GetType(), result, carg0->pos);
}


/** Constant fold the binary logical ops.
 */ 
template <typename T> static ConstExpr *
lConstFoldBinLogicalOp(BinaryExpr::Op op, const T *v0, const T *v1, ConstExpr *carg0) {
    bool result[ISPC_MAX_NVEC];
    int count = carg0->Count();

    switch (op) {
        FOLD_OP(BinaryExpr::Lt, <);
        FOLD_OP(BinaryExpr::Gt, >);
        FOLD_OP(BinaryExpr::Le, <=);
        FOLD_OP(BinaryExpr::Ge, >=);
        FOLD_OP(BinaryExpr::Equal, ==);
        FOLD_OP(BinaryExpr::NotEqual, !=);
        FOLD_OP(BinaryExpr::LogicalAnd, &&);
        FOLD_OP(BinaryExpr::LogicalOr, ||);
    default:
        return NULL;
    }

    const Type *rType = carg0->GetType()->IsUniformType() ? 
        AtomicType::UniformBool : AtomicType::VaryingBool;
    return new ConstExpr(rType, result, carg0->pos);
}


/** Constant fold binary arithmetic ops.
 */
template <typename T> static ConstExpr *
lConstFoldBinArithOp(BinaryExpr::Op op, const T *v0, const T *v1, ConstExpr *carg0) {
    T result[ISPC_MAX_NVEC];
    int count = carg0->Count();

    switch (op) {
        FOLD_OP(BinaryExpr::Add, +);
        FOLD_OP(BinaryExpr::Sub, -);
        FOLD_OP(BinaryExpr::Mul, *);
        FOLD_OP(BinaryExpr::Div, /);
    default:
        return NULL;
    }
    
    return new ConstExpr(carg0->GetType(), result, carg0->pos);
}


/** Constant fold the various boolean binary ops.
 */
static ConstExpr *
lConstFoldBoolBinOp(BinaryExpr::Op op, const bool *v0, const bool *v1, 
                    ConstExpr *carg0) {
    bool result[ISPC_MAX_NVEC];
    int count = carg0->Count();

    switch (op) {
        FOLD_OP(BinaryExpr::BitAnd, &);
        FOLD_OP(BinaryExpr::BitXor, ^);
        FOLD_OP(BinaryExpr::BitOr, |);
        FOLD_OP(BinaryExpr::Lt, <);
        FOLD_OP(BinaryExpr::Gt, >);
        FOLD_OP(BinaryExpr::Le, <=);
        FOLD_OP(BinaryExpr::Ge, >=);
        FOLD_OP(BinaryExpr::Equal, ==);
        FOLD_OP(BinaryExpr::NotEqual, !=);
        FOLD_OP(BinaryExpr::LogicalAnd, &&);
        FOLD_OP(BinaryExpr::LogicalOr, ||);
    default:
        return NULL;
    }

    return new ConstExpr(carg0->GetType(), result, carg0->pos);
}


Expr *
BinaryExpr::Optimize() {
    if (arg0 == NULL || arg1 == NULL)
        return NULL;

    ConstExpr *constArg0 = dynamic_cast<ConstExpr *>(arg0);
    ConstExpr *constArg1 = dynamic_cast<ConstExpr *>(arg1);

    if (g->opt.fastMath) {
        // optimizations related to division by floats..

        // transform x / const -> x * (1/const)
        if (op == Div && constArg1 != NULL) {
            const Type *type1 = constArg1->GetType();
            if (Type::Equal(type1, AtomicType::UniformFloat) ||
                Type::Equal(type1, AtomicType::VaryingFloat) ||
                Type::Equal(type1, AtomicType::UniformConstFloat) ||
                Type::Equal(type1, AtomicType::VaryingConstFloat)) {
                float inv[ISPC_MAX_NVEC];
                int count = constArg1->AsFloat(inv);
                for (int i = 0; i < count; ++i)
                    inv[i] = 1.f / inv[i];
                Expr *einv = new ConstExpr(type1, inv, constArg1->pos);
                Expr *e = new BinaryExpr(Mul, arg0, einv, pos);
                e = ::TypeCheck(e);
                if (e == NULL)
                    return NULL;
                return ::Optimize(e);
            }
        }

        // transform x / y -> x * rcp(y)
        if (op == Div) {
            const Type *type1 = arg1->GetType();
            if (Type::Equal(type1, AtomicType::UniformFloat) ||
                Type::Equal(type1, AtomicType::VaryingFloat) ||
                Type::Equal(type1, AtomicType::UniformConstFloat) ||
                Type::Equal(type1, AtomicType::VaryingConstFloat)) {
                // Get the symbol for the appropriate builtin
                std::vector<Symbol *> rcpFuns;
                m->symbolTable->LookupFunction("rcp", &rcpFuns);
                if (rcpFuns.size() > 0) {
                    Assert(rcpFuns.size() == 2);
                    Expr *rcpSymExpr = new FunctionSymbolExpr("rcp", rcpFuns, pos);
                    ExprList *args = new ExprList(arg1, arg1->pos);
                    Expr *rcpCall = new FunctionCallExpr(rcpSymExpr, args, 
                                                         arg1->pos);
                    rcpCall = ::TypeCheck(rcpCall);
                    if (rcpCall == NULL)
                        return NULL;
                    rcpCall = ::Optimize(rcpCall);
                    if (rcpCall == NULL)
                        return NULL;

                    Expr *ret = new BinaryExpr(Mul, arg0, rcpCall, pos);
                    ret = ::TypeCheck(ret);
                    if (ret == NULL)
                        return NULL;
                    return ::Optimize(ret);
                }
                else
                    Warning(pos, "rcp() not found from stdlib.  Can't apply "
                            "fast-math rcp optimization.");
            }
        }
    }

    // From here on out, we're just doing constant folding, so if both args
    // aren't constants then we're done...
    if (constArg0 == NULL || constArg1 == NULL)
        return this;

    Assert(Type::EqualIgnoringConst(arg0->GetType(), arg1->GetType()));
    const Type *type = arg0->GetType()->GetAsNonConstType();
    if (type == AtomicType::UniformFloat || type == AtomicType::VaryingFloat) {
        float v0[ISPC_MAX_NVEC], v1[ISPC_MAX_NVEC];
        constArg0->AsFloat(v0);
        constArg1->AsFloat(v1);
        ConstExpr *ret;
        if ((ret = lConstFoldBinArithOp(op, v0, v1, constArg0)) != NULL)
            return ret;
        else if ((ret = lConstFoldBinLogicalOp(op, v0, v1, constArg0)) != NULL)
            return ret;
        else 
            return this;
    }
    if (type == AtomicType::UniformDouble || type == AtomicType::VaryingDouble) {
        double v0[ISPC_MAX_NVEC], v1[ISPC_MAX_NVEC];
        constArg0->AsDouble(v0);
        constArg1->AsDouble(v1);
        ConstExpr *ret;
        if ((ret = lConstFoldBinArithOp(op, v0, v1, constArg0)) != NULL)
            return ret;
        else if ((ret = lConstFoldBinLogicalOp(op, v0, v1, constArg0)) != NULL)
            return ret;
        else 
            return this;
    }
    if (type == AtomicType::UniformInt32 || type == AtomicType::VaryingInt32) {
        int32_t v0[ISPC_MAX_NVEC], v1[ISPC_MAX_NVEC];
        constArg0->AsInt32(v0);
        constArg1->AsInt32(v1);
        ConstExpr *ret;
        if ((ret = lConstFoldBinArithOp(op, v0, v1, constArg0)) != NULL)
            return ret;
        else if ((ret = lConstFoldBinIntOp(op, v0, v1, constArg0)) != NULL)
            return ret;
        else if ((ret = lConstFoldBinLogicalOp(op, v0, v1, constArg0)) != NULL)
            return ret;
        else
            return this;
    }
    else if (type == AtomicType::UniformUInt32 || type == AtomicType::VaryingUInt32 ||
             dynamic_cast<const EnumType *>(type) != NULL) {
        uint32_t v0[ISPC_MAX_NVEC], v1[ISPC_MAX_NVEC];
        constArg0->AsUInt32(v0);
        constArg1->AsUInt32(v1);
        ConstExpr *ret;
        if ((ret = lConstFoldBinArithOp(op, v0, v1, constArg0)) != NULL)
            return ret;
        else if ((ret = lConstFoldBinIntOp(op, v0, v1, constArg0)) != NULL)
            return ret;
        else if ((ret = lConstFoldBinLogicalOp(op, v0, v1, constArg0)) != NULL)
            return ret;
        else
            return this;
    }
    else if (type == AtomicType::UniformBool || type == AtomicType::VaryingBool) {
        bool v0[ISPC_MAX_NVEC], v1[ISPC_MAX_NVEC];
        constArg0->AsBool(v0);
        constArg1->AsBool(v1);
        ConstExpr *ret;
        if ((ret = lConstFoldBoolBinOp(op, v0, v1, constArg0)) != NULL)
            return ret;
        else if ((ret = lConstFoldBinLogicalOp(op, v0, v1, constArg0)) != NULL)
            return ret;
        else 
            return this;
    }
    else
        return this;
}


Expr *
BinaryExpr::TypeCheck() {
    if (arg0 == NULL || arg1 == NULL)
        return NULL;

    const Type *type0 = arg0->GetType(), *type1 = arg1->GetType();
    if (type0 == NULL || type1 == NULL)
        return NULL;

    if (dynamic_cast<const ReferenceType *>(type0) != NULL) {
        arg0 = new DereferenceExpr(arg0, arg0->pos);
        type0 = arg0->GetType();
        Assert(type0 != NULL);
    }
    if (dynamic_cast<const ReferenceType *>(type1) != NULL) {
        arg1 = new DereferenceExpr(arg1, arg1->pos);
        type1 = arg1->GetType();
        Assert(type1 != NULL);
    }

    // Convert arrays to pointers to their first elements
    if (dynamic_cast<const ArrayType *>(type0) != NULL) {
        arg0 = lArrayToPointer(arg0);
        type0 = arg0->GetType();
    }
    if (dynamic_cast<const ArrayType *>(type1) != NULL) {
        arg1 = lArrayToPointer(arg1);
        type1 = arg1->GetType();
    }

    const PointerType *pt0 = dynamic_cast<const PointerType *>(type0);
    const PointerType *pt1 = dynamic_cast<const PointerType *>(type1);
    if (pt0 != NULL && pt1 != NULL && op == Sub) {
        if (PointerType::IsVoidPointer(type0)) {
            Error(pos, "Illegal to perform pointer arithmetic "
                  "on \"%s\" type.", type0->GetString().c_str());
            return NULL;
        }
        if (PointerType::IsVoidPointer(type1)) {
            Error(pos, "Illegal to perform pointer arithmetic "
                  "on \"%s\" type.", type1->GetString().c_str());
            return NULL;
        }

        const Type *t = Type::MoreGeneralType(type0, type1, pos, "-");
        if (t == NULL)
            return NULL;
        arg0 = TypeConvertExpr(arg0, t, "pointer subtraction");
        arg1 = TypeConvertExpr(arg1, t, "pointer subtraction");
        if (arg0 == NULL || arg1 == NULL)
            return NULL;

        return this;
    }
    else if (((pt0 != NULL || pt1 != NULL) && op == Add) ||
             (pt0 != NULL && op == Sub)) {
        // Handle ptr + int, int + ptr, ptr - int
        if (pt0 != NULL && pt1 != NULL) {
            Error(pos, "Illegal to add two pointer types \"%s\" and \"%s\".",
                  pt0->GetString().c_str(), pt1->GetString().c_str());
            return NULL;
        }
        else if (pt1 != NULL) {
            // put in canonical order with the pointer as the first operand
            // for GetValue()
            std::swap(arg0, arg1);
            std::swap(type0, type1);
            std::swap(pt0, pt1);
        }

        Assert(pt0 != NULL);

        if (PointerType::IsVoidPointer(pt0)) {
            Error(pos, "Illegal to perform pointer arithmetic "
                  "on \"%s\" type.", pt0->GetString().c_str());
            return NULL;
        }

        const Type *offsetType = g->target.is32Bit ? 
            AtomicType::UniformInt32 : AtomicType::UniformInt64;
        if (pt0->IsVaryingType())
            offsetType = offsetType->GetAsVaryingType();
        if (type1->IsVaryingType()) {
            arg0 = TypeConvertExpr(arg0, type0->GetAsVaryingType(), 
                                   "pointer addition");
            Assert(arg0 != NULL);
        }

        arg1 = TypeConvertExpr(arg1, offsetType, lOpString(op));
        if (arg1 == NULL)
            return NULL;

        return this;
    }

    switch (op) {
    case Shl:
    case Shr:
    case BitAnd:
    case BitXor:
    case BitOr: {
        // Must have integer or bool-typed operands for these bit-related
        // ops; don't do any implicit conversions from floats here...
        if (!type0->IsIntType() && !type0->IsBoolType()) {
            Error(arg0->pos, "First operand to binary operator \"%s\" must be "
                  "an integer or bool.", lOpString(op));
            return NULL;
        }
        if (!type1->IsIntType() && !type1->IsBoolType()) {
            Error(arg1->pos, "Second operand to binary operator \"%s\" must be "
                  "an integer or bool.", lOpString(op));
            return NULL;
        }

        if (op == Shl || op == Shr) {
            bool isVarying = (type0->IsVaryingType() ||
                              type1->IsVaryingType());
            if (isVarying) {
                arg0 = TypeConvertExpr(arg0, type0->GetAsVaryingType(), 
                                       "shift operator");
                if (arg0 == NULL)
                    return NULL;
                type0 = arg0->GetType();
            }
            arg1 = TypeConvertExpr(arg1, type0, "shift operator");
            if (arg1 == NULL)
                return NULL;
        }
        else {
            const Type *promotedType = Type::MoreGeneralType(type0, type1, arg0->pos,
                                                             "binary bit op");
            if (promotedType == NULL)
                return NULL;

            arg0 = TypeConvertExpr(arg0, promotedType, "binary bit op");
            arg1 = TypeConvertExpr(arg1, promotedType, "binary bit op");
            if (arg0 == NULL || arg1 == NULL)
                return NULL;
        }
        return this;
    }
    case Add:
    case Sub:
    case Mul:
    case Div:
    case Mod: {
        // Must be numeric type for these.  (And mod is special--can't be float)
        if (!type0->IsNumericType() || (op == Mod && type0->IsFloatType())) {
            Error(arg0->pos, "First operand to binary operator \"%s\" is of "
                  "invalid type \"%s\".", lOpString(op), 
                  type0->GetString().c_str());
            return NULL;
        }
        if (!type1->IsNumericType() || (op == Mod && type1->IsFloatType())) {
            Error(arg1->pos, "First operand to binary operator \"%s\" is of "
                  "invalid type \"%s\".", lOpString(op), 
                  type1->GetString().c_str());
            return NULL;
        }

        const Type *promotedType = Type::MoreGeneralType(type0, type1, arg0->pos,
                                                         lOpString(op));
        if (promotedType == NULL)
            return NULL;

        arg0 = TypeConvertExpr(arg0, promotedType, lOpString(op));
        arg1 = TypeConvertExpr(arg1, promotedType, lOpString(op));
        if (arg0 == NULL || arg1 == NULL)
            return NULL;
        return this;
    }
    case Lt:
    case Gt:
    case Le:
    case Ge:
    case Equal:
    case NotEqual: {
        const PointerType *pt0 = dynamic_cast<const PointerType *>(type0);
        const PointerType *pt1 = dynamic_cast<const PointerType *>(type1);

        // Convert '0' in expressions where the other expression is a
        // pointer type to a NULL pointer.
        if (pt0 != NULL && lIsAllIntZeros(arg1)) {
            arg1 = new NullPointerExpr(pos);
            type1 = arg1->GetType();
            pt1 = dynamic_cast<const PointerType *>(type1);
        }
        else if (pt1 != NULL && lIsAllIntZeros(arg0)) {
            arg0 = new NullPointerExpr(pos);
            type0 = arg1->GetType();
            pt0 = dynamic_cast<const PointerType *>(type0);
        }

        if (pt0 == NULL && pt1 == NULL) {
            if (!type0->IsBoolType() && !type0->IsNumericType()) {
                Error(arg0->pos,
                      "First operand to operator \"%s\" is of "
                      "non-comparable type \"%s\".", lOpString(op), 
                      type0->GetString().c_str());
                return NULL;
            }
            if (!type1->IsBoolType() && !type1->IsNumericType()) {
                Error(arg1->pos,
                      "Second operand to operator \"%s\" is of "
                      "non-comparable type \"%s\".", lOpString(op), 
                      type1->GetString().c_str());
                return NULL;
            }
        }

        const Type *promotedType = 
            Type::MoreGeneralType(type0, type1, arg0->pos, lOpString(op));
        if (promotedType == NULL)
            return NULL;

        arg0 = TypeConvertExpr(arg0, promotedType, lOpString(op));
        arg1 = TypeConvertExpr(arg1, promotedType, lOpString(op));
        if (arg0 == NULL || arg1 == NULL)
            return NULL;
        return this;
    }
    case LogicalAnd:
    case LogicalOr: {
        // We need to type convert to a boolean type of the more general
        // shape of the two types
        bool isUniform = (type0->IsUniformType() && type1->IsUniformType());
        const AtomicType *boolType = isUniform ? AtomicType::UniformBool : 
                                                 AtomicType::VaryingBool;
        const Type *destType = NULL;
        const VectorType *vtype0 = dynamic_cast<const VectorType *>(type0);
        const VectorType *vtype1 = dynamic_cast<const VectorType *>(type1);
        if (vtype0 && vtype1) {
            int sz0 = vtype0->GetElementCount(), sz1 = vtype1->GetElementCount();
            if (sz0 != sz1) {
                Error(pos, "Can't do logical operation \"%s\" between vector types of "
                      "different sizes (%d vs. %d).", lOpString(op), sz0, sz1);
                return NULL;
            }
            destType = new VectorType(boolType, sz0);
        }
        else if (vtype0)
            destType = new VectorType(boolType, vtype0->GetElementCount());
        else if (vtype1)
            destType = new VectorType(boolType, vtype1->GetElementCount());
        else
            destType = boolType;

        arg0 = TypeConvertExpr(arg0, destType, lOpString(op));
        arg1 = TypeConvertExpr(arg1, destType, lOpString(op));
        if (arg0 == NULL || arg1 == NULL)
            return NULL;
        return this;
    }
    case Comma:
        return this;
    default:
        FATAL("logic error");
        return NULL;
    }
}


int
BinaryExpr::EstimateCost() const {
    return (op == Div || op == Mod) ? COST_COMPLEX_ARITH_OP : 
                                      COST_SIMPLE_ARITH_LOGIC_OP;
}


void
BinaryExpr::Print() const {
    if (!arg0 || !arg1 || !GetType())
        return;

    printf("[ %s ] (", GetType()->GetString().c_str());
    arg0->Print();
    printf(" %s ", lOpString(op));
    arg1->Print();
    printf(")");
    pos.Print();
}


///////////////////////////////////////////////////////////////////////////
// AssignExpr

static const char *
lOpString(AssignExpr::Op op) {
    switch (op) {
    case AssignExpr::Assign:    return "=";
    case AssignExpr::MulAssign: return "*=";
    case AssignExpr::DivAssign: return "/=";
    case AssignExpr::ModAssign: return "%%=";
    case AssignExpr::AddAssign: return "+=";
    case AssignExpr::SubAssign: return "-=";
    case AssignExpr::ShlAssign: return "<<=";
    case AssignExpr::ShrAssign: return ">>=";
    case AssignExpr::AndAssign: return "&=";
    case AssignExpr::XorAssign: return "^=";
    case AssignExpr::OrAssign:  return "|=";
    default:
        FATAL("Missing op in lOpstring");
        return "";
    }
}

/** Emit code to do an "assignment + operation" operator, e.g. "+=".
 */
static llvm::Value *
lEmitOpAssign(AssignExpr::Op op, Expr *arg0, Expr *arg1, const Type *type, 
              Symbol *baseSym, SourcePos pos, FunctionEmitContext *ctx) {
    llvm::Value *lv = arg0->GetLValue(ctx);
    if (!lv) {
        // FIXME: I think this test is unnecessary and that this case
        // should be caught during typechecking
        Error(pos, "Can't assign to left-hand side of expression.");
        return NULL;
    }
    const Type *lvalueType = arg0->GetLValueType();
    if (lvalueType == NULL)
        return NULL;

    // Get the value on the right-hand side of the assignment+operation
    // operator and load the current value on the left-hand side.
    llvm::Value *rvalue = arg1->GetValue(ctx);
    ctx->SetDebugPos(pos);
    llvm::Value *mask = lMaskForSymbol(baseSym, ctx);
    llvm::Value *oldLHS = ctx->LoadInst(lv, mask, lvalueType, "opassign_load");

    // Map the operator to the corresponding BinaryExpr::Op operator
    BinaryExpr::Op basicop;
    switch (op) {
    case AssignExpr::MulAssign: basicop = BinaryExpr::Mul;    break;
    case AssignExpr::DivAssign: basicop = BinaryExpr::Div;    break;
    case AssignExpr::ModAssign: basicop = BinaryExpr::Mod;    break;
    case AssignExpr::AddAssign: basicop = BinaryExpr::Add;    break;
    case AssignExpr::SubAssign: basicop = BinaryExpr::Sub;    break;
    case AssignExpr::ShlAssign: basicop = BinaryExpr::Shl;    break;
    case AssignExpr::ShrAssign: basicop = BinaryExpr::Shr;    break;
    case AssignExpr::AndAssign: basicop = BinaryExpr::BitAnd; break;
    case AssignExpr::XorAssign: basicop = BinaryExpr::BitXor; break;
    case AssignExpr::OrAssign:  basicop = BinaryExpr::BitOr;  break;
    default:
        FATAL("logic error in lEmitOpAssign()");
        return NULL;
    }

    // Emit the code to compute the new value
    llvm::Value *newValue = NULL;
    switch (op) {
    case AssignExpr::MulAssign:
    case AssignExpr::DivAssign:
    case AssignExpr::ModAssign:
    case AssignExpr::AddAssign:
    case AssignExpr::SubAssign:
        newValue = lEmitBinaryArith(basicop, oldLHS, rvalue, type,
                                    arg1->GetType(), ctx, pos);
        break;
    case AssignExpr::ShlAssign:
    case AssignExpr::ShrAssign:
    case AssignExpr::AndAssign:
    case AssignExpr::XorAssign:
    case AssignExpr::OrAssign:
        newValue = lEmitBinaryBitOp(basicop, oldLHS, rvalue, 
                                    arg0->GetType()->IsUnsignedType(), ctx);
        break;
    default:
        FATAL("logic error in lEmitOpAssign");
        return NULL;
    }

    // And store the result back to the lvalue.
    lStoreAssignResult(newValue, lv, lvalueType, ctx, baseSym);

    return newValue;
}


AssignExpr::AssignExpr(AssignExpr::Op o, Expr *a, Expr *b, SourcePos p) 
    : Expr(p), op(o) {
    lvalue = a;
    rvalue = b;
}


llvm::Value *
AssignExpr::GetValue(FunctionEmitContext *ctx) const {
    const Type *type = NULL;
    if (lvalue == NULL || rvalue == NULL || (type = GetType()) == NULL)
        return NULL;

    ctx->SetDebugPos(pos);

    Symbol *baseSym = lvalue->GetBaseSymbol();
    // Should be caught during type-checking...
    assert(baseSym != NULL);

    switch (op) {
    case Assign: {
        llvm::Value *lv = lvalue->GetLValue(ctx);
        if (lv == NULL) {
            Assert(m->errorCount > 0);
            return NULL;
        }
        const Type *lvalueType = lvalue->GetLValueType();
        if (lvalueType == NULL) {
            Assert(m->errorCount > 0);
            return NULL;
        }

        llvm::Value *rv = rvalue->GetValue(ctx);
        if (rv == NULL) {
            Assert(m->errorCount > 0);
            return NULL;
        }

        ctx->SetDebugPos(pos);

        lStoreAssignResult(rv, lv, lvalueType, ctx, baseSym);

        return rv;
    }
    case MulAssign:
    case DivAssign:
    case ModAssign:
    case AddAssign:
    case SubAssign:
    case ShlAssign:
    case ShrAssign:
    case AndAssign:
    case XorAssign:
    case OrAssign: {
        // This should be caught during type checking
        Assert(!dynamic_cast<const ArrayType *>(type) &&
               !dynamic_cast<const StructType *>(type));
        return lEmitOpAssign(op, lvalue, rvalue, type, baseSym, pos, ctx);
    }
    default:
        FATAL("logic error in AssignExpr::GetValue()");
        return NULL;
    }
}


Expr *
AssignExpr::Optimize() {
    if (lvalue == NULL || rvalue == NULL)
        return NULL;
    return this;
}


const Type *
AssignExpr::GetType() const {
    return lvalue ? lvalue->GetType() : NULL;
}


/** Recursively checks a structure type to see if it (or any struct type
    that it holds) has a const-qualified member. */
static bool
lCheckForConstStructMember(SourcePos pos, const StructType *structType,
                           const StructType *initialType) {
    for (int i = 0; i < structType->GetElementCount(); ++i) {
        const Type *t = structType->GetElementType(i);
        if (t->IsConstType()) {
            if (structType == initialType)
                Error(pos, "Illegal to assign to type \"%s\" due to element "
                      "\"%s\" with type \"%s\".", structType->GetString().c_str(),
                      structType->GetElementName(i).c_str(),
                      t->GetString().c_str());
            else
                Error(pos, "Illegal to assign to type \"%s\" in type \"%s\" "
                      "due to element \"%s\" with type \"%s\".", 
                      structType->GetString().c_str(),
                      initialType->GetString().c_str(), 
                      structType->GetElementName(i).c_str(),
                      t->GetString().c_str());
            return true;
        }

        const StructType *st = dynamic_cast<const StructType *>(t);
        if (st != NULL && lCheckForConstStructMember(pos, st, initialType))
            return true;
    }
    return false;
}


Expr *
AssignExpr::TypeCheck() {
    if (lvalue == NULL || rvalue == NULL) 
        return NULL;

    bool lvalueIsReference = 
        dynamic_cast<const ReferenceType *>(lvalue->GetType()) != NULL;
    if (lvalueIsReference)
        lvalue = new DereferenceExpr(lvalue, lvalue->pos);

    FunctionSymbolExpr *fse;
    if ((fse = dynamic_cast<FunctionSymbolExpr *>(rvalue)) != NULL) {
        // Special case to use the type of the LHS to resolve function
        // overloads when we're assigning a function pointer where the
        // function is overloaded.
        const Type *lvalueType = lvalue->GetType();
        const FunctionType *ftype;
        if (dynamic_cast<const PointerType *>(lvalueType) == NULL ||
            (ftype = dynamic_cast<const FunctionType *>(lvalueType->GetBaseType())) == NULL) {
            Error(pos, "Can't assign function pointer to type \"%s\".",
                  lvalue->GetType()->GetString().c_str());
            return NULL;
        }

        std::vector<const Type *> paramTypes;
        for (int i = 0; i < ftype->GetNumParameters(); ++i)
            paramTypes.push_back(ftype->GetParameterType(i));

        if (!fse->ResolveOverloads(rvalue->pos, paramTypes)) {
            Error(pos, "Unable to find overloaded function for function "
                  "pointer assignment.");
            return NULL;
        }
    }

    if (lvalue->GetBaseSymbol() == NULL) {
        Error(lvalue->pos, "Left hand side of assignment statement can't be "
              "assigned to.");
        return NULL;
    }

    const Type *lhsType = lvalue->GetType();
    if (dynamic_cast<const PointerType *>(lhsType) != NULL) {
        if (op == AddAssign || op == SubAssign) {
            if (PointerType::IsVoidPointer(lhsType)) {
                Error(pos, "Illegal to perform pointer arithmetic on \"%s\" "
                      "type.", lhsType->GetString().c_str());
                return NULL;
            }

            const Type *deltaType = g->target.is32Bit ? AtomicType::UniformInt32 :
                AtomicType::UniformInt64;
            if (lhsType->IsVaryingType())
                deltaType = deltaType->GetAsVaryingType();
            rvalue = TypeConvertExpr(rvalue, deltaType, lOpString(op));
        }
        else if (op == Assign)
            rvalue = TypeConvertExpr(rvalue, lhsType, "assignment");
        else {
            Error(pos, "Assignment operator \"%s\" is illegal with pointer types.",
                  lOpString(op));
            return NULL;
        }
    }
    else if (dynamic_cast<const ArrayType *>(lhsType) != NULL) {
        Error(pos, "Illegal to assign to array type \"%s\".",
              lhsType->GetString().c_str());
        return NULL;
    }
    else
        rvalue = TypeConvertExpr(rvalue, lhsType, lOpString(op));

    if (rvalue == NULL)
        return NULL;

    if (lhsType->IsConstType()) {
        Error(pos, "Can't assign to type \"%s\" on left-hand side of "
              "expression.", lhsType->GetString().c_str());
        return NULL;
    }

    // Make sure we're not assigning to a struct that has a constant member
    const StructType *st = dynamic_cast<const StructType *>(lhsType);
    if (st != NULL && lCheckForConstStructMember(pos, st, st))
        return NULL;

    return this;
}


int
AssignExpr::EstimateCost() const {
    if (op == Assign)
        return COST_ASSIGN;
    if (op == DivAssign || op == ModAssign)
        return COST_ASSIGN + COST_COMPLEX_ARITH_OP;
    else
        return COST_ASSIGN + COST_SIMPLE_ARITH_LOGIC_OP;
}


void
AssignExpr::Print() const {
    if (!lvalue || !rvalue || !GetType())
        return;

    printf("[%s] assign (", GetType()->GetString().c_str());
    lvalue->Print();
    printf(" %s ", lOpString(op));
    rvalue->Print();
    printf(")");
    pos.Print();
}


///////////////////////////////////////////////////////////////////////////
// SelectExpr

SelectExpr::SelectExpr(Expr *t, Expr *e1, Expr *e2, SourcePos p) 
    : Expr(p) {
    test = t;
    expr1 = e1;
    expr2 = e2;
}


/** Emit code to select between two varying values based on a varying test
    value.
 */
static llvm::Value *
lEmitVaryingSelect(FunctionEmitContext *ctx, llvm::Value *test, 
                   llvm::Value *expr1, llvm::Value *expr2, 
                   const Type *type) {
    llvm::Value *resultPtr = ctx->AllocaInst(expr1->getType(), "selectexpr_tmp");
    // Don't need to worry about masking here
    ctx->StoreInst(expr2, resultPtr);
    // Use masking to conditionally store the expr1 values
    Assert(resultPtr->getType() ==
           PointerType::GetUniform(type)->LLVMType(g->ctx));
    ctx->StoreInst(expr1, resultPtr, test, PointerType::GetUniform(type));
    return ctx->LoadInst(resultPtr, "selectexpr_final");
}


llvm::Value *
SelectExpr::GetValue(FunctionEmitContext *ctx) const {
    if (!expr1 || !expr2 || !test)
        return NULL;

    ctx->SetDebugPos(pos);

    const Type *testType = test->GetType()->GetAsNonConstType();
    // This should be taken care of during typechecking
    Assert(testType->GetBaseType() == AtomicType::UniformBool ||
           testType->GetBaseType() == AtomicType::VaryingBool);

    const Type *type = expr1->GetType();

    if (testType == AtomicType::UniformBool) {
        // Simple case of a single uniform bool test expression; we just
        // want one of the two expressions.  In this case, we can be
        // careful to evaluate just the one of the expressions that we need
        // the value of so that if the other one has side-effects or
        // accesses invalid memory, it doesn't execute.
        llvm::Value *testVal = test->GetValue(ctx);
        llvm::BasicBlock *testTrue = ctx->CreateBasicBlock("select_true");
        llvm::BasicBlock *testFalse = ctx->CreateBasicBlock("select_false");
        llvm::BasicBlock *testDone = ctx->CreateBasicBlock("select_done");
        ctx->BranchInst(testTrue, testFalse, testVal);

        ctx->SetCurrentBasicBlock(testTrue);
        llvm::Value *expr1Val = expr1->GetValue(ctx);
        // Note that truePred won't be necessarily equal to testTrue, in
        // case the expr1->GetValue() call changes the current basic block.
        llvm::BasicBlock *truePred = ctx->GetCurrentBasicBlock();
        ctx->BranchInst(testDone);

        ctx->SetCurrentBasicBlock(testFalse);
        llvm::Value *expr2Val = expr2->GetValue(ctx);
        // See comment above truePred for why we can't just assume we're in
        // the testFalse basic block here.
        llvm::BasicBlock *falsePred = ctx->GetCurrentBasicBlock();
        ctx->BranchInst(testDone);

        ctx->SetCurrentBasicBlock(testDone);
        llvm::PHINode *ret = ctx->PhiNode(expr1Val->getType(), 2, "select");
        ret->addIncoming(expr1Val, truePred);
        ret->addIncoming(expr2Val, falsePred);
        return ret;
    }
    else if (dynamic_cast<const VectorType *>(testType) == NULL) {
        // if the test is a varying bool type, then evaluate both of the
        // value expressions with the mask set appropriately and then do an
        // element-wise select to get the result
        llvm::Value *testVal = test->GetValue(ctx);
        Assert(testVal->getType() == LLVMTypes::MaskType);
        llvm::Value *oldMask = ctx->GetInternalMask();
        ctx->SetInternalMaskAnd(oldMask, testVal);
        llvm::Value *expr1Val = expr1->GetValue(ctx);
        ctx->SetInternalMaskAndNot(oldMask, testVal);
        llvm::Value *expr2Val = expr2->GetValue(ctx);
        ctx->SetInternalMask(oldMask);

        return lEmitVaryingSelect(ctx, testVal, expr1Val, expr2Val, type);
    }
    else {
        // FIXME? Short-circuiting doesn't work in the case of
        // vector-valued test expressions.  (We could also just prohibit
        // these and place the issue in the user's hands...)
        llvm::Value *testVal = test->GetValue(ctx);
        llvm::Value *expr1Val = expr1->GetValue(ctx);
        llvm::Value *expr2Val = expr2->GetValue(ctx);

        ctx->SetDebugPos(pos);
        const VectorType *vt = dynamic_cast<const VectorType *>(type);
        // Things that typechecking should have caught
        Assert(vt != NULL);
        Assert(dynamic_cast<const VectorType *>(testType) != NULL &&
               (dynamic_cast<const VectorType *>(testType)->GetElementCount() == 
                vt->GetElementCount()));

        // Do an element-wise select  
        llvm::Value *result = llvm::UndefValue::get(type->LLVMType(g->ctx));
        for (int i = 0; i < vt->GetElementCount(); ++i) {
            llvm::Value *ti = ctx->ExtractInst(testVal, i);
            llvm::Value *e1i = ctx->ExtractInst(expr1Val, i);
            llvm::Value *e2i = ctx->ExtractInst(expr2Val, i);
            llvm::Value *sel = NULL;
            if (testType->IsUniformType())
                sel = ctx->SelectInst(ti, e1i, e2i);
            else
                sel = lEmitVaryingSelect(ctx, ti, e1i, e2i, vt->GetElementType());
            result = ctx->InsertInst(result, sel, i);
        }
        return result;
    }
}


const Type *
SelectExpr::GetType() const {
    if (!test || !expr1 || !expr2)
        return NULL;

    const Type *testType = test->GetType();
    const Type *expr1Type = expr1->GetType();
    const Type *expr2Type = expr2->GetType();

    if (!testType || !expr1Type || !expr2Type)
        return NULL;

    bool becomesVarying = (testType->IsVaryingType() || expr1Type->IsVaryingType() ||
                           expr2Type->IsVaryingType());
    // if expr1 and expr2 have different vector sizes, typechecking should fail...
    int testVecSize = dynamic_cast<const VectorType *>(testType) != NULL ?
        dynamic_cast<const VectorType *>(testType)->GetElementCount() : 0;
    int expr1VecSize = dynamic_cast<const VectorType *>(expr1Type) != NULL ?
        dynamic_cast<const VectorType *>(expr1Type)->GetElementCount() : 0;
    Assert(!(testVecSize != 0 && expr1VecSize != 0 && testVecSize != expr1VecSize));
    
    int vectorSize = std::max(testVecSize, expr1VecSize);
    return Type::MoreGeneralType(expr1Type, expr2Type, Union(expr1->pos, expr2->pos),
                                 "select expression", becomesVarying, vectorSize);
}


Expr *
SelectExpr::Optimize() {
    if (test == NULL || expr1 == NULL || expr2 == NULL)
        return NULL;
    return this;
}


Expr *
SelectExpr::TypeCheck() {
    if (test == NULL || expr1 == NULL || expr2 == NULL)
        return NULL;

    const Type *type1 = expr1->GetType(), *type2 = expr2->GetType();
    if (!type1 || !type2)
        return NULL;

    if (dynamic_cast<const ArrayType *>(type1)) {
        Error(pos, "Array type \"%s\" can't be used in select expression", 
              type1->GetString().c_str());
        return NULL;
    }
    if (dynamic_cast<const ArrayType *>(type2)) {
        Error(pos, "Array type \"%s\" can't be used in select expression", 
              type2->GetString().c_str());
        return NULL;
    }

    const Type *testType = test->GetType();
    if (testType == NULL)
        return NULL;
    test = TypeConvertExpr(test, lMatchingBoolType(testType), "select");
    if (test == NULL)
        return NULL;
    testType = test->GetType();

    int testVecSize = dynamic_cast<const VectorType *>(testType) ?
        dynamic_cast<const VectorType *>(testType)->GetElementCount() : 0;
    const Type *promotedType = 
        Type::MoreGeneralType(type1, type2, Union(expr1->pos, expr2->pos),
                              "select expression", testType->IsVaryingType(), testVecSize);
    if (promotedType == NULL)
        return NULL;

    expr1 = TypeConvertExpr(expr1, promotedType, "select");
    expr2 = TypeConvertExpr(expr2, promotedType, "select");
    if (expr1 == NULL || expr2 == NULL)
        return NULL;

    return this;
}


int
SelectExpr::EstimateCost() const {
    return COST_SELECT;
}


void
SelectExpr::Print() const {
    if (!test || !expr1 || !expr2 || !GetType())
        return;

    printf("[%s] (", GetType()->GetString().c_str());
    test->Print();
    printf(" ? ");
    expr1->Print();
    printf(" : ");
    expr2->Print();
    printf(")");
    pos.Print();
}


///////////////////////////////////////////////////////////////////////////
// FunctionCallExpr

FunctionCallExpr::FunctionCallExpr(Expr *f, ExprList *a, SourcePos p, 
                                   bool il, Expr *lce) 
    : Expr(p), isLaunch(il) {
    func = f;
    args = a;
    launchCountExpr = lce;
}


static const FunctionType *
lGetFunctionType(Expr *func) {
    if (func == NULL)
        return NULL;

    const Type *type = func->GetType();
    if (type == NULL)
        return NULL;

    const FunctionType *ftype = dynamic_cast<const FunctionType *>(type);
    if (ftype == NULL) {
        // Not a regular function symbol--is it a function pointer?
        if (dynamic_cast<const PointerType *>(type) != NULL)
            ftype = dynamic_cast<const FunctionType *>(type->GetBaseType());
    }
    return ftype;
}


llvm::Value *
FunctionCallExpr::GetValue(FunctionEmitContext *ctx) const {
    if (func == NULL || args == NULL)
        return NULL;

    ctx->SetDebugPos(pos);

    llvm::Value *callee = func->GetValue(ctx);

    if (callee == NULL) {
        Assert(m->errorCount > 0);
        return NULL;
    }

    const FunctionType *ft = lGetFunctionType(func);
    Assert(ft != NULL);
    bool isVoidFunc = (ft->GetReturnType() == AtomicType::Void);

    // Automatically convert function call args to references if needed.
    // FIXME: this should move to the TypeCheck() method... (but the
    // GetLValue call below needs a FunctionEmitContext, which is
    // problematic...)  
    std::vector<Expr *> callargs = args->exprs;
    bool err = false;

    // Specifically, this can happen if there's an error earlier during
    // overload resolution.
    if ((int)callargs.size() > ft->GetNumParameters()) {
        Assert(m->errorCount > 0);
        return NULL;
    }

    for (unsigned int i = 0; i < callargs.size(); ++i) {
        Expr *argExpr = callargs[i];
        if (argExpr == NULL)
            continue;

        const Type *paramType = ft->GetParameterType(i); 

        const Type *argLValueType = argExpr->GetLValueType();
        if (argLValueType != NULL &&
            dynamic_cast<const PointerType *>(argLValueType) != NULL &&
            argLValueType->IsVaryingType() &&
            dynamic_cast<const ReferenceType *>(paramType) != NULL) {
            Error(argExpr->pos, "Illegal to pass a \"varying\" lvalue to a "
                  "reference parameter of type \"%s\".",
                  paramType->GetString().c_str());
            return NULL;
        }

        // Do whatever type conversion is needed
        argExpr = TypeConvertExpr(argExpr, paramType,
                                  "function call argument");
        if (argExpr == NULL)
            return NULL;
        callargs[i] = argExpr;
    }
    if (err)
        return NULL;

    // Fill in any default argument values needed.
    // FIXME: should we do this during type checking?
    for (int i = callargs.size(); i < ft->GetNumParameters(); ++i) {
        Expr *paramDefault = ft->GetParameterDefault(i);
        const Type *paramType = ft->GetParameterType(i);
        // FIXME: this type conv should happen when we create the function
        // type!
        Expr *d = TypeConvertExpr(paramDefault, paramType,
                                  "function call default argument");
        if (d == NULL)
            return NULL;
        callargs.push_back(d);
    }

    // Now evaluate the values of all of the parameters being passed.
    std::vector<llvm::Value *> argVals;
    for (unsigned int i = 0; i < callargs.size(); ++i) {
        Expr *argExpr = callargs[i];
        if (argExpr == NULL)
            // give up; we hit an error earlier
            return NULL;

        llvm::Value *argValue = argExpr->GetValue(ctx);
        if (argValue == NULL)
            // something went wrong in evaluating the argument's
            // expression, so give up on this
            return NULL;

        argVals.push_back(argValue);
    }


    llvm::Value *retVal = NULL;
    ctx->SetDebugPos(pos);
    if (ft->isTask) {
        Assert(launchCountExpr != NULL);
        llvm::Value *launchCount = launchCountExpr->GetValue(ctx);
        if (launchCount != NULL)
            ctx->LaunchInst(callee, argVals, launchCount);
    }
    else
        retVal = ctx->CallInst(callee, ft, argVals, 
                               isVoidFunc ? "" : "calltmp");

    if (isVoidFunc)
        return NULL;
    else
        return retVal;
}


const Type *
FunctionCallExpr::GetType() const {
    const FunctionType *ftype = lGetFunctionType(func);
    return ftype ? ftype->GetReturnType() : NULL;
}


Expr *
FunctionCallExpr::Optimize() {
    if (func == NULL || args == NULL)
        return NULL;
    return this;
}


Expr *
FunctionCallExpr::TypeCheck() {
    if (func == NULL || args == NULL)
        return NULL;

    std::vector<const Type *> argTypes;
    std::vector<bool> argCouldBeNULL;
    for (unsigned int i = 0; i < args->exprs.size(); ++i) {
        if (args->exprs[i] == NULL)
            return NULL;
        const Type *t = args->exprs[i]->GetType();
        if (t == NULL)
            return NULL;
        argTypes.push_back(t);
        argCouldBeNULL.push_back(lIsAllIntZeros(args->exprs[i]));
    }

    FunctionSymbolExpr *fse = dynamic_cast<FunctionSymbolExpr *>(func);
    if (fse != NULL) {
        // Regular function call

        if (fse->ResolveOverloads(args->pos, argTypes, &argCouldBeNULL) == false)
            return NULL;

        func = ::TypeCheck(fse);
        if (func == NULL)
            return NULL;

        const PointerType *pt = 
            dynamic_cast<const PointerType *>(func->GetType());
        const FunctionType *ft = (pt == NULL) ? NULL : 
            dynamic_cast<const FunctionType *>(pt->GetBaseType());
        if (ft == NULL) {
            Error(pos, "Valid function name must be used for function call.");
            return NULL;
        }

        if (ft->isTask) {
            if (!isLaunch)
                Error(pos, "\"launch\" expression needed to call function "
                      "with \"task\" qualifier.");
            if (!launchCountExpr)
                return NULL;

            launchCountExpr = 
                TypeConvertExpr(launchCountExpr, AtomicType::UniformInt32,
                                "task launch count");
            if (launchCountExpr == NULL)
                return NULL;
        }
        else {
            if (isLaunch)
                Error(pos, "\"launch\" expression illegal with non-\"task\"-"
                      "qualified function.");
            Assert(launchCountExpr == NULL);
        }
    }
    else {
        // Call through a function pointer
        const Type *fptrType = func->GetType();
        if (fptrType == NULL)
            return NULL;
           
        Assert(dynamic_cast<const PointerType *>(fptrType) != NULL);
        const FunctionType *funcType = 
            dynamic_cast<const FunctionType *>(fptrType->GetBaseType());
        if (funcType == NULL) {
            Error(pos, "Must provide function name or function pointer for "
                  "function call expression.");
            return NULL;
        }
            
        // Make sure we don't have too many arguments for the function
        if ((int)argTypes.size() > funcType->GetNumParameters()) {
            Error(args->pos, "Too many parameter values provided in "
                  "function call (%d provided, %d expected).",
                  (int)argTypes.size(), funcType->GetNumParameters());
            return NULL;
        }
        // It's ok to have too few arguments, as long as the function's
        // default parameter values have started by the time we run out
        // of arguments
        if ((int)argTypes.size() < funcType->GetNumParameters() &&
            funcType->GetParameterDefault(argTypes.size()) == NULL) {
            Error(args->pos, "Too few parameter values provided in "
                  "function call (%d provided, %d expected).",
                  (int)argTypes.size(), funcType->GetNumParameters());
            return NULL;
        }

        // Now make sure they can all type convert to the corresponding
        // parameter types..
        for (int i = 0; i < (int)argTypes.size(); ++i) {
            if (i < funcType->GetNumParameters()) {
                // make sure it can type convert
                const Type *paramType = funcType->GetParameterType(i);
                if (CanConvertTypes(argTypes[i], paramType) == false &&
                    !(argCouldBeNULL[i] == true &&
                      dynamic_cast<const PointerType *>(paramType) != NULL)) {
                    Error(args->exprs[i]->pos, "Can't convert argument of "
                          "type \"%s\" to type \"%s\" for funcion call "
                          "argument.", argTypes[i]->GetString().c_str(),
                          paramType->GetString().c_str());
                    return NULL;
                }
            }
            else
                // Otherwise the parameter default saves us.  It should
                // be there for sure, given the check right above the
                // for loop.
                Assert(funcType->GetParameterDefault(i) != NULL);
        }

        if (fptrType->IsVaryingType() && 
            funcType->GetReturnType()->IsUniformType()) {
            Error(pos, "Illegal to call a varying function pointer that "
                  "points to a function with a uniform return type.");
            return NULL;
        }
    }

    if (func == NULL || args == NULL)
        return NULL;
    return this;
}


int
FunctionCallExpr::EstimateCost() const {
    if (isLaunch)
        return COST_TASK_LAUNCH;
    else if (dynamic_cast<FunctionSymbolExpr *>(func) == NULL) {
        // it's going through a function pointer
        const Type *fpType = func->GetType();
        if (fpType != NULL) {
            Assert(dynamic_cast<const PointerType *>(fpType) != NULL);
            if (fpType->IsUniformType())
                return COST_FUNPTR_UNIFORM;
            else 
                return COST_FUNPTR_VARYING;
        }
    }
    return COST_FUNCALL;
}


void
FunctionCallExpr::Print() const {
    if (!func || !args || !GetType())
        return;

    printf("[%s] funcall %s ", GetType()->GetString().c_str(),
           isLaunch ? "launch" : "");
    func->Print();
    printf(" args (");
    args->Print();
    printf(")");
    pos.Print();
}


///////////////////////////////////////////////////////////////////////////
// ExprList

llvm::Value *
ExprList::GetValue(FunctionEmitContext *ctx) const {
    FATAL("ExprList::GetValue() should never be called");
    return NULL;
}


const Type *
ExprList::GetType() const {
    FATAL("ExprList::GetType() should never be called");
    return NULL;
}


ExprList *
ExprList::Optimize() {
    return this;
}


ExprList *
ExprList::TypeCheck() {
    return this;
}


llvm::Constant *
ExprList::GetConstant(const Type *type) const {
    if (exprs.size() == 1 &&
        (dynamic_cast<const AtomicType *>(type) != NULL ||
         dynamic_cast<const EnumType *>(type) != NULL ||
         dynamic_cast<const PointerType *>(type) != NULL))
        return exprs[0]->GetConstant(type);

    const CollectionType *collectionType = 
        dynamic_cast<const CollectionType *>(type);
    if (collectionType == NULL)
        return NULL;

    std::string name;
    if (dynamic_cast<const StructType *>(type) != NULL)
        name = "struct";
    else if (dynamic_cast<const ArrayType *>(type) != NULL) 
        name = "array";
    else if (dynamic_cast<const VectorType *>(type) != NULL) 
        name = "vector";
    else 
        FATAL("Unexpected CollectionType in ExprList::GetConstant()");

    if ((int)exprs.size() != collectionType->GetElementCount()) {
        Error(pos, "Initializer list for %s \"%s\" must have %d elements "
              "(has %d).", name.c_str(), collectionType->GetString().c_str(),
              collectionType->GetElementCount(), (int)exprs.size());
        return NULL;
    }

    std::vector<llvm::Constant *> cv;
    for (unsigned int i = 0; i < exprs.size(); ++i) {
        if (exprs[i] == NULL)
            return NULL;
        const Type *elementType = collectionType->GetElementType(i);
        llvm::Constant *c = exprs[i]->GetConstant(elementType);
        if (c == NULL)
            // If this list element couldn't convert to the right constant
            // type for the corresponding collection member, then give up.
            return NULL;
        cv.push_back(c);
    }

    if (dynamic_cast<const StructType *>(type) != NULL) {
#if defined(LLVM_2_9)
        return llvm::ConstantStruct::get(*g->ctx, cv, false);
#else
        LLVM_TYPE_CONST llvm::StructType *llvmStructType =
            llvm::dyn_cast<LLVM_TYPE_CONST llvm::StructType>(collectionType->LLVMType(g->ctx));
        Assert(llvmStructType != NULL);
        return llvm::ConstantStruct::get(llvmStructType, cv);
#endif
    }
    else {
        LLVM_TYPE_CONST llvm::Type *lt = type->LLVMType(g->ctx);
        LLVM_TYPE_CONST llvm::ArrayType *lat = 
            llvm::dyn_cast<LLVM_TYPE_CONST llvm::ArrayType>(lt);
        // FIXME: should the assert below validly fail for uniform vectors
        // now?  Need a test case to reproduce it and then to be sure we
        // have the right fix; leave the assert until we can hit it...
        Assert(lat != NULL);
        return llvm::ConstantArray::get(lat, cv);
    }
    return NULL;
}


int
ExprList::EstimateCost() const {
    return 0;
}


void
ExprList::Print() const {
    printf("expr list (");
    for (unsigned int i = 0; i < exprs.size(); ++i) {
        if (exprs[i] != NULL)
            exprs[i]->Print();
        printf("%s", (i == exprs.size() - 1) ? ")" : ", ");
    }
    pos.Print();
}


///////////////////////////////////////////////////////////////////////////
// IndexExpr

IndexExpr::IndexExpr(Expr *a, Expr *i, SourcePos p) 
    : Expr(p) {
    baseExpr = a;
    index = i;
}


/** When computing pointer values, we need to apply a per-lane offset when
    we have a varying pointer that is itself indexing into varying data.
    Consdier the following ispc code:

    uniform float u[] = ...;
    float v[] = ...;
    int index = ...;
    float a = u[index];
    float b = v[index];

    To compute the varying pointer that holds the addresses to load from
    for u[index], we basically just need to multiply index element-wise by
    sizeof(float) before doing the memory load.  For v[index], we need to
    do the same scaling but also need to add per-lane offsets <0,
    sizeof(float), 2*sizeof(float), ...> so that the i'th lane loads the
    i'th of the varying values at its index value.  

    This function handles figuring out when this additional offset is
    needed and then incorporates it in the varying pointer value.
 */ 
static llvm::Value *
lAddVaryingOffsetsIfNeeded(FunctionEmitContext *ctx, llvm::Value *ptr, 
                           const Type *ptrType) {
    if (dynamic_cast<const ReferenceType *>(ptrType) != NULL)
        // References are uniform pointers, so no offsetting is needed
        return ptr;

    Assert(dynamic_cast<const PointerType *>(ptrType) != NULL);
    if (ptrType->IsUniformType())
        return ptr;

    const Type *baseType = ptrType->GetBaseType();
    if (baseType->IsUniformType())
        return ptr;

    // must be indexing into varying atomic, enum, or pointer types
    if (dynamic_cast<const AtomicType *>(baseType) == NULL &&
        dynamic_cast<const EnumType *>(baseType) == NULL &&
        dynamic_cast<const PointerType *>(baseType) == NULL)
        return ptr;

    // Onward: compute the per lane offsets.
    llvm::Value *varyingOffsets = 
        llvm::UndefValue::get(LLVMTypes::Int32VectorType);
    for (int i = 0; i < g->target.vectorWidth; ++i)
        varyingOffsets = ctx->InsertInst(varyingOffsets, LLVMInt32(i), i,
                                         "varying_delta");

    // And finally add the per-lane offsets.  Note that we lie to the GEP
    // call and tell it that the pointers are to uniform elements and not
    // varying elements, so that the offsets in terms of (0,1,2,...) will
    // end up turning into the correct step in bytes...
    const Type *uniformElementType = baseType->GetAsUniformType();
    const Type *ptrUnifType = PointerType::GetVarying(uniformElementType);
    return ctx->GetElementPtrInst(ptr, varyingOffsets, ptrUnifType);
}


llvm::Value *
IndexExpr::GetValue(FunctionEmitContext *ctx) const {
    const Type *baseExprType;
    if (baseExpr == NULL || index == NULL || 
        ((baseExprType = baseExpr->GetType()) == NULL))
        return NULL;

    ctx->SetDebugPos(pos);

    llvm::Value *lvalue = GetLValue(ctx);
    llvm::Value *mask = NULL;
    const Type *lvalueType = GetLValueType();
    if (lvalue == NULL) {
        // We may be indexing into a temporary that hasn't hit memory, so
        // get the full value and stuff it into temporary alloca'd space so
        // that we can index from there...
        llvm::Value *val = baseExpr->GetValue(ctx);
        if (val == NULL) {
            Assert(m->errorCount > 0);
            return NULL;
        }
        ctx->SetDebugPos(pos);
        llvm::Value *ptr = ctx->AllocaInst(baseExprType->LLVMType(g->ctx), 
                                           "array_tmp");
        ctx->StoreInst(val, ptr);

        lvalue = ctx->GetElementPtrInst(ptr, LLVMInt32(0), index->GetValue(ctx),
                                        PointerType::GetUniform(baseExprType));

        const SequentialType *st = 
            dynamic_cast<const SequentialType *>(baseExprType);
        if (st == NULL) {
            Assert(m->errorCount > 0);
            return NULL;
        }
        lvalueType = PointerType::GetUniform(st->GetElementType());

        lvalue = lAddVaryingOffsetsIfNeeded(ctx, lvalue, lvalueType);
                                            
        mask = LLVMMaskAllOn;
    }
    else {
        Symbol *baseSym = GetBaseSymbol();
        Assert(baseSym != NULL);
        mask = lMaskForSymbol(baseSym, ctx);
    }

    ctx->SetDebugPos(pos);
    return ctx->LoadInst(lvalue, mask, lvalueType, "index");
}


const Type *
IndexExpr::GetType() const {
    const Type *baseExprType, *indexType;
    if (!baseExpr || !index || 
        ((baseExprType = baseExpr->GetType()) == NULL) ||
        ((indexType = index->GetType()) == NULL))
        return NULL;

    const Type *elementType = NULL;
    const PointerType *pointerType = 
        dynamic_cast<const PointerType *>(baseExprType);
    if (pointerType != NULL)
        // ptr[index] -> type that the pointer points to
        elementType = pointerType->GetBaseType();
    else {
        // sequential type[index] -> element type of the sequential type
        const SequentialType *sequentialType = 
            dynamic_cast<const SequentialType *>(baseExprType->GetReferenceTarget());
        // Typechecking should have caught this...
        Assert(sequentialType != NULL);
        elementType = sequentialType->GetElementType();
    }

    if (indexType->IsUniformType())
        // If the index is uniform, the resulting type is just whatever the
        // element type is
        return elementType;
    else
        // A varying index into even a uniform base type -> varying type
        return elementType->GetAsVaryingType();
}


Symbol *
IndexExpr::GetBaseSymbol() const {
    return baseExpr ? baseExpr->GetBaseSymbol() : NULL;
}


llvm::Value *
IndexExpr::GetLValue(FunctionEmitContext *ctx) const {
    const Type *baseExprType;
    if (baseExpr == NULL || index == NULL || 
        ((baseExprType = baseExpr->GetType()) == NULL))
        return NULL;

    ctx->SetDebugPos(pos);
    if (dynamic_cast<const PointerType *>(baseExprType) != NULL) {
        // We're indexing off of a base pointer 
        llvm::Value *baseValue = baseExpr->GetValue(ctx);
        llvm::Value *indexValue = index->GetValue(ctx);
        if (baseValue == NULL || indexValue == NULL)
            return NULL;
        ctx->SetDebugPos(pos);
        return ctx->GetElementPtrInst(baseValue, indexValue,
                                      baseExprType, "ptr_offset");
    }

    // Otherwise it's an array or vector
    llvm::Value *basePtr = NULL;
    const Type *basePtrType = NULL;
    if (dynamic_cast<const ArrayType *>(baseExprType) ||
        dynamic_cast<const VectorType *>(baseExprType)) {
        basePtr = baseExpr->GetLValue(ctx);
        basePtrType = baseExpr->GetLValueType();
    }
    else {
        baseExprType = baseExprType->GetReferenceTarget();
        Assert(dynamic_cast<const ArrayType *>(baseExprType) ||
               dynamic_cast<const VectorType *>(baseExprType));
        basePtr = baseExpr->GetValue(ctx);
        basePtrType = baseExpr->GetType();
    }
    if (!basePtr)
        return NULL;

    // If the array index is a compile time constant, check to see if it
    // may lead to an out-of-bounds access.
    ConstExpr *ce = dynamic_cast<ConstExpr *>(index);
    const SequentialType *seqType = 
        dynamic_cast<const SequentialType *>(baseExprType);
    if (seqType != NULL) {
        int nElements = seqType->GetElementCount();
        if (ce != NULL && nElements > 0) {
            int32_t indices[ISPC_MAX_NVEC];
            int count = ce->AsInt32(indices);
            for (int i = 0; i < count; ++i) {
                if (indices[i] < 0 || indices[i] >= nElements)
                    Warning(index->pos, "Array index \"%d\" may be out of bounds for "
                            "%d element array.", indices[i], nElements);
            }
        }
    }

    ctx->SetDebugPos(pos);
    llvm::Value *ptr = 
        ctx->GetElementPtrInst(basePtr, LLVMInt32(0), index->GetValue(ctx),
                               basePtrType);
    ptr = lAddVaryingOffsetsIfNeeded(ctx, ptr, GetLValueType());
    return ptr;
}


const Type *
IndexExpr::GetLValueType() const {
    const Type *baseExprLValueType, *indexType;
    if (baseExpr == NULL || index == NULL || 
        ((baseExprLValueType = baseExpr->GetLValueType()) == NULL) ||
        ((indexType = index->GetType()) == NULL))
        return NULL;

    if (dynamic_cast<const ReferenceType *>(baseExprLValueType) != NULL)
        baseExprLValueType = PointerType::GetUniform(baseExprLValueType->GetReferenceTarget());
    Assert(dynamic_cast<const PointerType *>(baseExprLValueType) != NULL);

    // FIXME: can we do something in the type system that unifies the
    // concept of a sequential type's element type and a pointer type's
    // base type?  The code below is identical but for handling that
    // difference.  IndexableType?
    const SequentialType *st = 
        dynamic_cast<const SequentialType *>(baseExprLValueType->GetBaseType());
    if (st != NULL) {
        if (baseExprLValueType->IsUniformType() && indexType->IsUniformType())
            return PointerType::GetUniform(st->GetElementType());
        else
            return PointerType::GetVarying(st->GetElementType());
    }

    const PointerType *pt = 
        dynamic_cast<const PointerType *>(baseExprLValueType->GetBaseType());
    Assert(pt != NULL);
    if (baseExprLValueType->IsUniformType() && indexType->IsUniformType())
        return PointerType::GetUniform(pt->GetBaseType());
    else
        return PointerType::GetVarying(pt->GetBaseType());
}


Expr *
IndexExpr::Optimize() {
    if (baseExpr == NULL || index == NULL)
        return NULL;
    return this;
}


Expr *
IndexExpr::TypeCheck() {
    if (baseExpr == NULL || index == NULL || index->GetType() == NULL)
        return NULL;

    const Type *baseExprType = baseExpr->GetType();
    if (baseExprType == NULL)
        return NULL;

    if (!dynamic_cast<const SequentialType *>(baseExprType->GetReferenceTarget()) &&
        !dynamic_cast<const PointerType *>(baseExprType)) {
        Error(pos, "Trying to index into non-array, vector, or pointer "
              "type \"%s\".", baseExprType->GetString().c_str());
        return NULL;
    }

    bool isUniform = (index->GetType()->IsUniformType() && 
                      !g->opt.disableUniformMemoryOptimizations);
    const Type *indexType = isUniform ? AtomicType::UniformInt32 : 
                                        AtomicType::VaryingInt32;
    index = TypeConvertExpr(index, indexType, "array index");
    if (index == NULL)
        return NULL;

    return this;
}


int
IndexExpr::EstimateCost() const {
    if (index == NULL || baseExpr == NULL)
        return 0;

    const Type *indexType = index->GetType();
    const Type *baseExprType = baseExpr->GetType();
    
    if ((indexType != NULL && indexType->IsVaryingType()) ||
        (dynamic_cast<const PointerType *>(baseExprType) != NULL &&
         baseExprType->IsVaryingType()))
        // be pessimistic; some of these will later turn out to be vector
        // loads/stores, but it's too early for us to know that here.
        return COST_GATHER;
    else
        return COST_LOAD;
}


void
IndexExpr::Print() const {
    if (!baseExpr || !index || !GetType())
        return;

    printf("[%s] index ", GetType()->GetString().c_str());
    baseExpr->Print();
    printf("[");
    index->Print();
    printf("]");
    pos.Print();
}


///////////////////////////////////////////////////////////////////////////
// MemberExpr

/** Map one character ids to vector element numbers.  Allow a few different
    conventions--xyzw, rgba, uv.
*/
static int
lIdentifierToVectorElement(char id) {
    switch (id) {
    case 'x':
    case 'r':
    case 'u':
        return 0;
    case 'y':
    case 'g':
    case 'v':
        return 1;
    case 'z':
    case 'b':
        return 2;
    case 'w':
    case 'a':
        return 3;
    default:
        return -1;
    }
}

//////////////////////////////////////////////////
// StructMemberExpr

class StructMemberExpr : public MemberExpr
{
public:
    StructMemberExpr(Expr *e, const char *id, SourcePos p,
                     SourcePos idpos, bool derefLValue);

    const Type *GetType() const;
    int getElementNumber() const;
    const Type *getElementType() const;

private:
    const StructType *getStructType() const;
};


StructMemberExpr::StructMemberExpr(Expr *e, const char *id, SourcePos p,
                                   SourcePos idpos, bool derefLValue)
    : MemberExpr(e, id, p, idpos, derefLValue) {
}


const Type *
StructMemberExpr::GetType() const {
    // It's a struct, and the result type is the element type, possibly
    // promoted to varying if the struct type / lvalue is varying.
    const StructType *structType = getStructType();
    if (structType == NULL)
        return NULL;

    const Type *elementType = structType->GetElementType(identifier);
    if (elementType == NULL) {
        Error(identifierPos,
              "Element name \"%s\" not present in struct type \"%s\".%s",
              identifier.c_str(), structType->GetString().c_str(),
              getCandidateNearMatches().c_str());
        return NULL;
    }

    const PointerType *pt = dynamic_cast<const PointerType *>(expr->GetType());
    if (structType->IsVaryingType() ||
        (pt != NULL && pt->IsVaryingType()))
        return elementType->GetAsVaryingType();
    else
        return elementType;
}


int
StructMemberExpr::getElementNumber() const {
    const StructType *structType = getStructType();
    if (structType == NULL)
        return -1;

    int elementNumber = structType->GetElementNumber(identifier);
    if (elementNumber == -1)
        Error(identifierPos,
              "Element name \"%s\" not present in struct type \"%s\".%s",
              identifier.c_str(), structType->GetString().c_str(),
              getCandidateNearMatches().c_str());
    return elementNumber;
}


const Type *
StructMemberExpr::getElementType() const {
    const StructType *structType = getStructType();
    if (structType == NULL)
        return NULL;

    return structType->GetAsUniformType()->GetElementType(identifier);
}


const StructType *
StructMemberExpr::getStructType() const {
    const Type *exprType = expr->GetType();
    if (exprType == NULL)
        return NULL;
    
    const StructType *structType = dynamic_cast<const StructType *>(exprType);
    if (structType == NULL) {
        const PointerType *pt = dynamic_cast<const PointerType *>(exprType);
        if (pt != NULL)
            structType = dynamic_cast<const StructType *>(pt->GetBaseType());
        else {
            const ReferenceType *rt = 
                dynamic_cast<const ReferenceType *>(exprType);
            Assert(rt != NULL);
            structType = dynamic_cast<const StructType *>(rt->GetReferenceTarget());
        }
        Assert(structType != NULL);
    }
    return structType;
}


//////////////////////////////////////////////////
// VectorMemberExpr

class VectorMemberExpr : public MemberExpr
{
public:
    VectorMemberExpr(Expr *e, const char *id, SourcePos p,
                     SourcePos idpos, bool derefLValue);

    llvm::Value *GetValue(FunctionEmitContext* ctx) const;
    llvm::Value *GetLValue(FunctionEmitContext* ctx) const;
    const Type *GetType() const;
    const Type *GetLValueType() const;

    int getElementNumber() const;
    const Type *getElementType() const;

private:
    const VectorType *exprVectorType;
    const VectorType *memberType;
};


VectorMemberExpr::VectorMemberExpr(Expr *e, const char *id, SourcePos p,
                                   SourcePos idpos, bool derefLValue)
    : MemberExpr(e, id, p, idpos, derefLValue) {
    const Type *exprType = e->GetType();
    exprVectorType = dynamic_cast<const VectorType *>(exprType);
    if (exprVectorType == NULL) {
        const PointerType *pt = dynamic_cast<const PointerType *>(exprType);
        if (pt != NULL)
            exprVectorType = dynamic_cast<const VectorType *>(pt->GetBaseType());
        else {
            Assert(dynamic_cast<const ReferenceType *>(exprType) != NULL);
            exprVectorType = 
                dynamic_cast<const VectorType *>(exprType->GetReferenceTarget());
        }
        Assert(exprVectorType != NULL);
    }
    memberType = new VectorType(exprVectorType->GetElementType(),
                                identifier.length());
}


const Type *
VectorMemberExpr::GetType() const {
    // For 1-element expressions, we have the base vector element
    // type.  For n-element expressions, we have a shortvec type
    // with n > 1 elements.  This can be changed when we get
    // type<1> -> type conversions.
    const Type *type = (identifier.length() == 1) ? 
        (const Type *)exprVectorType->GetElementType() : 
        (const Type *)memberType;

    const Type *lvalueType = GetLValueType();
    if (lvalueType != NULL && lvalueType->IsVaryingType())
        type = type->GetAsVaryingType();
    return type;
}


llvm::Value *
VectorMemberExpr::GetLValue(FunctionEmitContext* ctx) const {
    if (identifier.length() == 1) {
        return MemberExpr::GetLValue(ctx);
    } else {
        return NULL;
    }
}


const Type *
VectorMemberExpr::GetLValueType() const {
    if (identifier.length() == 1) {
        if (expr == NULL)
            return NULL;

        const Type *exprLValueType = dereferenceExpr ? expr->GetType() :
            expr->GetLValueType();
        if (exprLValueType == NULL)
            return NULL;

        const VectorType *vt = NULL;
        if (dynamic_cast<const ReferenceType *>(exprLValueType) != NULL)
            vt = dynamic_cast<const VectorType *>(exprLValueType->GetReferenceTarget());
        else
            vt = dynamic_cast<const VectorType *>(exprLValueType->GetBaseType());
        Assert(vt != NULL);

        // we don't want to report that it's e.g. a pointer to a float<1>,
        // but ta pointer to a float, etc.
        const Type *elementType = vt->GetElementType();
        if (dynamic_cast<const ReferenceType *>(exprLValueType) != NULL)
            return new ReferenceType(elementType);
        else
            return exprLValueType->IsUniformType() ?
                PointerType::GetUniform(elementType) : 
                PointerType::GetVarying(elementType);
    }
    else
        return NULL;
}


llvm::Value *
VectorMemberExpr::GetValue(FunctionEmitContext *ctx) const {
    if (identifier.length() == 1) {
        return MemberExpr::GetValue(ctx);
    } 
    else {
        std::vector<int> indices;

        for (size_t i = 0; i < identifier.size(); ++i) {
            int idx = lIdentifierToVectorElement(identifier[i]);
            if (idx == -1)
                Error(pos,
                      "Invalid swizzle charcter '%c' in swizzle \"%s\".",
                      identifier[i], identifier.c_str());

            indices.push_back(idx);
        }

        llvm::Value *basePtr = NULL;
        const Type *basePtrType = NULL;
        if (dereferenceExpr) {
            basePtr = expr->GetValue(ctx);
            basePtrType = expr->GetType();
        }
        else {
            basePtr = expr->GetLValue(ctx);
            basePtrType = expr->GetLValueType();
        }

        if (basePtr == NULL || basePtrType == NULL) {
            Assert(m->errorCount > 0);
            return NULL;
        }

        // Allocate temporary memory to tore the result
        llvm::Value *resultPtr = ctx->AllocaInst(memberType->LLVMType(g->ctx), 
                                            "vector_tmp");

        // FIXME: we should be able to use the internal mask here according
        // to the same logic where it's used elsewhere
        llvm::Value *elementMask = ctx->GetFullMask();

        const Type *elementPtrType = basePtrType->IsUniformType() ? 
            PointerType::GetUniform(exprVectorType->GetElementType()) :
            PointerType::GetVarying(exprVectorType->GetElementType());

        ctx->SetDebugPos(pos);
        for (size_t i = 0; i < identifier.size(); ++i) {
            llvm::Value *elementPtr = ctx->AddElementOffset(basePtr, indices[i],
                                                            basePtrType);
            llvm::Value *elementValue = 
                ctx->LoadInst(elementPtr, elementMask, elementPtrType, 
                              "vec_element");

            llvm::Value *ptmp = ctx->AddElementOffset(resultPtr, i, NULL);
            ctx->StoreInst(elementValue, ptmp);
        }

        return ctx->LoadInst(resultPtr, "swizzle_vec");
    }
}


int
VectorMemberExpr::getElementNumber() const {
    int elementNumber = lIdentifierToVectorElement(identifier[0]);
    if (elementNumber == -1)
        Error(pos, "Vector element identifier \"%s\" unknown.", 
              identifier.c_str());
    return elementNumber;
}


const Type *
VectorMemberExpr::getElementType() const {
    return memberType;
}



MemberExpr *
MemberExpr::create(Expr *e, const char *id, SourcePos p, SourcePos idpos,
                   bool derefLValue) {
    const Type *exprType;
    if (e == NULL || (exprType = e->GetType()) == NULL)
        return NULL;

    const ReferenceType *referenceType =
        dynamic_cast<const ReferenceType *>(exprType);
    if (referenceType != NULL) {
        e = new DereferenceExpr(e, e->pos);
        exprType = e->GetType();
        Assert(exprType != NULL);
    }

    const PointerType *pointerType = dynamic_cast<const PointerType *>(exprType);
    if (pointerType != NULL)
        exprType = pointerType->GetBaseType();

    if (derefLValue == true && pointerType == NULL) {
        if (dynamic_cast<const StructType *>(exprType->GetReferenceTarget()) != NULL)
            Error(p, "Dereference operator \"->\" can't be applied to non-pointer "
                  "type \"%s\".  Did you mean to use \".\"?", 
                  exprType->GetString().c_str());
        else
            Error(p, "Dereference operator \"->\" can't be applied to non-struct "
                  "pointer type \"%s\".", exprType->GetString().c_str());
        return NULL;
    }
    if (derefLValue == false && pointerType != NULL &&
        dynamic_cast<const StructType *>(pointerType->GetBaseType()) != NULL) {
            Error(p, "Member operator \".\" can't be applied to pointer "
                  "type \"%s\".  Did you mean to use \"->\"?", 
                  exprType->GetString().c_str());
        return NULL;
    }

    if (dynamic_cast<const StructType *>(exprType) != NULL)
        return new StructMemberExpr(e, id, p, idpos, derefLValue);
    else if (dynamic_cast<const VectorType *>(exprType) != NULL)
        return new VectorMemberExpr(e, id, p, idpos, derefLValue);
    else {
        Error(p, "Member operator \"%s\" can't be used with expression of "
              "\"%s\" type.", derefLValue ? "->" : ".", 
              exprType->GetString().c_str());
        return NULL;
    }
}


MemberExpr::MemberExpr(Expr *e, const char *id, SourcePos p, SourcePos idpos,
                       bool derefLValue) 
    : Expr(p), identifierPos(idpos) {
    expr = e;
    identifier = id;
    dereferenceExpr = derefLValue;
}


llvm::Value *
MemberExpr::GetValue(FunctionEmitContext *ctx) const {
    if (!expr) 
        return NULL;

    llvm::Value *lvalue = GetLValue(ctx);
    const Type *lvalueType = GetLValueType();

    llvm::Value *mask = NULL;
    if (lvalue == NULL) {
        // As in the array case, this may be a temporary that hasn't hit
        // memory; get the full value and stuff it into a temporary array
        // so that we can index from there...
        llvm::Value *val = expr->GetValue(ctx);
        if (!val) {
            Assert(m->errorCount > 0);
            return NULL;
        }
        ctx->SetDebugPos(pos);
        const Type *exprType = expr->GetType();
        llvm::Value *ptr = ctx->AllocaInst(exprType->LLVMType(g->ctx), 
                                           "struct_tmp");
        ctx->StoreInst(val, ptr);

        int elementNumber = getElementNumber();
        if (elementNumber == -1)
            return NULL;

        lvalue = ctx->AddElementOffset(ptr, elementNumber, 
                                       PointerType::GetUniform(exprType));
        lvalueType = PointerType::GetUniform(GetType());
        mask = LLVMMaskAllOn;
    }
    else {
        Symbol *baseSym = GetBaseSymbol();
        Assert(baseSym != NULL);
        mask = lMaskForSymbol(baseSym, ctx);
    }

    ctx->SetDebugPos(pos);
    return ctx->LoadInst(lvalue, mask, lvalueType, "structelement");
}


const Type *
MemberExpr::GetType() const {
    return NULL;
}


Symbol *
MemberExpr::GetBaseSymbol() const {
    return expr ? expr->GetBaseSymbol() : NULL;
}


int
MemberExpr::getElementNumber() const {
    return -1;
}


llvm::Value *
MemberExpr::GetLValue(FunctionEmitContext *ctx) const {
    const Type *exprType;
    if (!expr || ((exprType = expr->GetType()) == NULL))
        return NULL;

    ctx->SetDebugPos(pos);
    llvm::Value *basePtr = dereferenceExpr ? expr->GetValue(ctx) :
        expr->GetLValue(ctx);
    if (!basePtr)
        return NULL;

    int elementNumber = getElementNumber();
    if (elementNumber == -1)
        return NULL;

    const Type *exprLValueType = dereferenceExpr ? expr->GetType() :
        expr->GetLValueType();
    ctx->SetDebugPos(pos);
    llvm::Value *ptr = ctx->AddElementOffset(basePtr, elementNumber,
                                             exprLValueType);

    ptr = lAddVaryingOffsetsIfNeeded(ctx, ptr, GetLValueType());

    return ptr;
}


const Type *
MemberExpr::GetLValueType() const {
    if (expr == NULL)
        return NULL;

    const Type *exprLValueType = dereferenceExpr ? expr->GetType() :
        expr->GetLValueType();
    if (exprLValueType == NULL)
        return NULL;

    return exprLValueType->IsUniformType() ?
        PointerType::GetUniform(getElementType()) : 
        PointerType::GetVarying(getElementType());
}


Expr *
MemberExpr::TypeCheck() {
    return expr ? this : NULL;
}


Expr *
MemberExpr::Optimize() {
    return expr ? this : NULL;
}


int
MemberExpr::EstimateCost() const {
    const Type *lvalueType = GetLValueType();
    if (lvalueType != NULL && lvalueType->IsVaryingType())
        return COST_GATHER + COST_SIMPLE_ARITH_LOGIC_OP;
    else
        return COST_SIMPLE_ARITH_LOGIC_OP;
}


void
MemberExpr::Print() const {
    if (!expr || !GetType())
        return;

    printf("[%s] member (", GetType()->GetString().c_str());
    expr->Print();
    printf(" . %s)", identifier.c_str());
    pos.Print();
}


/** There is no structure member with the name we've got in "identifier".
    Use the approximate string matching routine to see if the identifier is
    a minor misspelling of one of the ones that is there.
 */
std::string
MemberExpr::getCandidateNearMatches() const {
    const StructType *structType = 
        dynamic_cast<const StructType *>(expr->GetType());
    if (!structType)
        return "";

    std::vector<std::string> elementNames;
    for (int i = 0; i < structType->GetElementCount(); ++i)
        elementNames.push_back(structType->GetElementName(i));
    std::vector<std::string> alternates = MatchStrings(identifier, elementNames);
    if (!alternates.size())
        return "";

    std::string ret = " Did you mean ";
    for (unsigned int i = 0; i < alternates.size(); ++i) {
        ret += std::string("\"") + alternates[i] + std::string("\"");
        if (i < alternates.size() - 1) ret += ", or ";
    }
    ret += "?";
    return ret;
}


///////////////////////////////////////////////////////////////////////////
// ConstExpr

ConstExpr::ConstExpr(const Type *t, int8_t i, SourcePos p) 
  : Expr(p) {
    type = t;
    type = type->GetAsConstType();
    Assert(type == AtomicType::UniformConstInt8);
    int8Val[0] = i;
}


ConstExpr::ConstExpr(const Type *t, int8_t *i, SourcePos p) 
  : Expr(p) {
    type = t;
    type = type->GetAsConstType();
    Assert(type == AtomicType::UniformConstInt8 || 
           type == AtomicType::VaryingConstInt8);
    for (int j = 0; j < Count(); ++j)
        int8Val[j] = i[j];
}


ConstExpr::ConstExpr(const Type *t, uint8_t u, SourcePos p) 
  : Expr(p) {
    type = t;
    type = type->GetAsConstType();
    Assert(type == AtomicType::UniformConstUInt8);
    uint8Val[0] = u;
}


ConstExpr::ConstExpr(const Type *t, uint8_t *u, SourcePos p) 
  : Expr(p) {
    type = t;
    type = type->GetAsConstType();
    Assert(type == AtomicType::UniformConstUInt8 || 
           type == AtomicType::VaryingConstUInt8);
    for (int j = 0; j < Count(); ++j)
        uint8Val[j] = u[j];
}


ConstExpr::ConstExpr(const Type *t, int16_t i, SourcePos p) 
  : Expr(p) {
    type = t;
    type = type->GetAsConstType();
    Assert(type == AtomicType::UniformConstInt16);
    int16Val[0] = i;
}


ConstExpr::ConstExpr(const Type *t, int16_t *i, SourcePos p) 
  : Expr(p) {
    type = t;
    type = type->GetAsConstType();
    Assert(type == AtomicType::UniformConstInt16 || 
           type == AtomicType::VaryingConstInt16);
    for (int j = 0; j < Count(); ++j)
        int16Val[j] = i[j];
}


ConstExpr::ConstExpr(const Type *t, uint16_t u, SourcePos p) 
  : Expr(p) {
    type = t;
    type = type->GetAsConstType();
    Assert(type == AtomicType::UniformConstUInt16);
    uint16Val[0] = u;
}


ConstExpr::ConstExpr(const Type *t, uint16_t *u, SourcePos p) 
  : Expr(p) {
    type = t;
    type = type->GetAsConstType();
    Assert(type == AtomicType::UniformConstUInt16 || 
           type == AtomicType::VaryingConstUInt16);
    for (int j = 0; j < Count(); ++j)
        uint16Val[j] = u[j];
}


ConstExpr::ConstExpr(const Type *t, int32_t i, SourcePos p) 
  : Expr(p) {
    type = t;
    type = type->GetAsConstType();
    Assert(type == AtomicType::UniformConstInt32);
    int32Val[0] = i;
}


ConstExpr::ConstExpr(const Type *t, int32_t *i, SourcePos p) 
  : Expr(p) {
    type = t;
    type = type->GetAsConstType();
    Assert(type == AtomicType::UniformConstInt32 || 
           type == AtomicType::VaryingConstInt32);
    for (int j = 0; j < Count(); ++j)
        int32Val[j] = i[j];
}


ConstExpr::ConstExpr(const Type *t, uint32_t u, SourcePos p) 
  : Expr(p) {
    type = t;
    type = type->GetAsConstType();
    Assert(type == AtomicType::UniformConstUInt32 ||
           (dynamic_cast<const EnumType *>(type) != NULL &&
            type->IsUniformType()));
    uint32Val[0] = u;
}


ConstExpr::ConstExpr(const Type *t, uint32_t *u, SourcePos p) 
  : Expr(p) {
    type = t;
    type = type->GetAsConstType();
    Assert(type == AtomicType::UniformConstUInt32 || 
           type == AtomicType::VaryingConstUInt32 ||
           (dynamic_cast<const EnumType *>(type) != NULL));
    for (int j = 0; j < Count(); ++j)
        uint32Val[j] = u[j];
}


ConstExpr::ConstExpr(const Type *t, float f, SourcePos p)
    : Expr(p) {
    type = t;
    type = type->GetAsConstType();
    Assert(type == AtomicType::UniformConstFloat);
    floatVal[0] = f;
}


ConstExpr::ConstExpr(const Type *t, float *f, SourcePos p) 
  : Expr(p) {
    type = t;
    type = type->GetAsConstType();
    Assert(type == AtomicType::UniformConstFloat || 
           type == AtomicType::VaryingConstFloat);
    for (int j = 0; j < Count(); ++j)
        floatVal[j] = f[j];
}


ConstExpr::ConstExpr(const Type *t, int64_t i, SourcePos p) 
  : Expr(p) {
    type = t;
    type = type->GetAsConstType();
    Assert(type == AtomicType::UniformConstInt64);
    int64Val[0] = i;
}


ConstExpr::ConstExpr(const Type *t, int64_t *i, SourcePos p) 
  : Expr(p) {
    type = t;
    type = type->GetAsConstType();
    Assert(type == AtomicType::UniformConstInt64 || 
           type == AtomicType::VaryingConstInt64);
    for (int j = 0; j < Count(); ++j)
        int64Val[j] = i[j];
}


ConstExpr::ConstExpr(const Type *t, uint64_t u, SourcePos p) 
  : Expr(p) {
    type = t;
    type = type->GetAsConstType();
    Assert(type == AtomicType::UniformConstUInt64);
    uint64Val[0] = u;
}


ConstExpr::ConstExpr(const Type *t, uint64_t *u, SourcePos p) 
  : Expr(p) {
    type = t;
    type = type->GetAsConstType();
    Assert(type == AtomicType::UniformConstUInt64 || 
           type == AtomicType::VaryingConstUInt64);
    for (int j = 0; j < Count(); ++j)
        uint64Val[j] = u[j];
}


ConstExpr::ConstExpr(const Type *t, double f, SourcePos p)
    : Expr(p) {
    type = t;
    type = type->GetAsConstType();
    Assert(type == AtomicType::UniformConstDouble);
    doubleVal[0] = f;
}


ConstExpr::ConstExpr(const Type *t, double *f, SourcePos p) 
  : Expr(p) {
    type = t;
    type = type->GetAsConstType();
    Assert(type == AtomicType::UniformConstDouble || 
           type == AtomicType::VaryingConstDouble);
    for (int j = 0; j < Count(); ++j)
        doubleVal[j] = f[j];
}


ConstExpr::ConstExpr(const Type *t, bool b, SourcePos p) 
  : Expr(p) {
    type = t;
    type = type->GetAsConstType();
    Assert(type == AtomicType::UniformConstBool);
    boolVal[0] = b;
}


ConstExpr::ConstExpr(const Type *t, bool *b, SourcePos p) 
  : Expr(p) {
    type = t;
    type = type->GetAsConstType();
    Assert(type == AtomicType::UniformConstBool || 
           type == AtomicType::VaryingConstBool);
    for (int j = 0; j < Count(); ++j)
        boolVal[j] = b[j];
}


ConstExpr::ConstExpr(ConstExpr *old, double *v) 
    : Expr(old->pos) {
    type = old->type;

    AtomicType::BasicType basicType = getBasicType();

    switch (basicType) {
    case AtomicType::TYPE_BOOL:
        for (int i = 0; i < Count(); ++i)
            boolVal[i] = (v[i] != 0.);
        break;
    case AtomicType::TYPE_INT8:
        for (int i = 0; i < Count(); ++i)
            int8Val[i] = (int)v[i];
        break;
    case AtomicType::TYPE_UINT8:
        for (int i = 0; i < Count(); ++i)
            uint8Val[i] = (unsigned int)v[i];
        break;
    case AtomicType::TYPE_INT16:
        for (int i = 0; i < Count(); ++i)
            int16Val[i] = (int)v[i];
        break;
    case AtomicType::TYPE_UINT16:
        for (int i = 0; i < Count(); ++i)
            uint16Val[i] = (unsigned int)v[i];
        break;
    case AtomicType::TYPE_INT32:
        for (int i = 0; i < Count(); ++i)
            int32Val[i] = (int)v[i];
        break;
    case AtomicType::TYPE_UINT32:
        for (int i = 0; i < Count(); ++i)
            uint32Val[i] = (unsigned int)v[i];
        break;
    case AtomicType::TYPE_FLOAT:
        for (int i = 0; i < Count(); ++i)
            floatVal[i] = (float)v[i];
        break;
    case AtomicType::TYPE_DOUBLE:
        for (int i = 0; i < Count(); ++i)
            doubleVal[i] = v[i];
        break;
    case AtomicType::TYPE_INT64:
    case AtomicType::TYPE_UINT64:
        // For now, this should never be reached 
        FATAL("fixme; we need another constructor so that we're not trying to pass "
               "double values to init an int64 type...");
    default:
        FATAL("unimplemented const type");
    }
}


AtomicType::BasicType
ConstExpr::getBasicType() const {
    const AtomicType *at = dynamic_cast<const AtomicType *>(type);
    if (at != NULL)
        return at->basicType;
    else {
        Assert(dynamic_cast<const EnumType *>(type) != NULL);
        return AtomicType::TYPE_UINT32;
    }
}


const Type *
ConstExpr::GetType() const { 
    return type; 
}


llvm::Value *
ConstExpr::GetValue(FunctionEmitContext *ctx) const {
    ctx->SetDebugPos(pos);
    bool isVarying = type->IsVaryingType();

    AtomicType::BasicType basicType = getBasicType();

    switch (basicType) {
    case AtomicType::TYPE_BOOL:
        if (isVarying)
            return LLVMBoolVector(boolVal);
        else
            return boolVal[0] ? LLVMTrue : LLVMFalse;
    case AtomicType::TYPE_INT8:
        return isVarying ? LLVMInt8Vector(int8Val) : 
                           LLVMInt8(int8Val[0]);
    case AtomicType::TYPE_UINT8:
        return isVarying ? LLVMUInt8Vector(uint8Val) : 
                           LLVMUInt8(uint8Val[0]);
    case AtomicType::TYPE_INT16:
        return isVarying ? LLVMInt16Vector(int16Val) : 
                           LLVMInt16(int16Val[0]);
    case AtomicType::TYPE_UINT16:
        return isVarying ? LLVMUInt16Vector(uint16Val) : 
                           LLVMUInt16(uint16Val[0]);
    case AtomicType::TYPE_INT32:
        return isVarying ? LLVMInt32Vector(int32Val) : 
                           LLVMInt32(int32Val[0]);
    case AtomicType::TYPE_UINT32:
        return isVarying ? LLVMUInt32Vector(uint32Val) : 
                           LLVMUInt32(uint32Val[0]);
    case AtomicType::TYPE_FLOAT:
        return isVarying ? LLVMFloatVector(floatVal) : 
                           LLVMFloat(floatVal[0]);
    case AtomicType::TYPE_INT64:
        return isVarying ? LLVMInt64Vector(int64Val) : 
                           LLVMInt64(int64Val[0]);
    case AtomicType::TYPE_UINT64:
        return isVarying ? LLVMUInt64Vector(uint64Val) : 
                           LLVMUInt64(uint64Val[0]);
    case AtomicType::TYPE_DOUBLE:
        return isVarying ? LLVMDoubleVector(doubleVal) : 
                           LLVMDouble(doubleVal[0]);
    default:
        FATAL("unimplemented const type");
        return NULL;
    }
}


/* Type conversion templates: take advantage of C++ function overloading
   rules to get the one we want to match. */

/* First the most general case, just use C++ type conversion if nothing
   else matches */
template <typename From, typename To> static inline void
lConvertElement(From from, To *to) {
    *to = (To)from;
}


/** When converting from bool types to numeric types, make sure the result
    is one or zero.
 */ 
template <typename To> static inline void
lConvertElement(bool from, To *to) {
    *to = from ? (To)1 : (To)0;
}


/** When converting numeric types to bool, compare to zero.  (Do we
    actually need this one??) */
template <typename From> static inline void
lConvertElement(From from, bool *to) {
    *to = (from != 0);
}


/** And bool -> bool is just assignment */
static inline void
lConvertElement(bool from, bool *to) {
    *to = from;
}


/** Type conversion utility function
 */
template <typename From, typename To> static void
lConvert(const From *from, To *to, int count, bool forceVarying) {
    for (int i = 0; i < count; ++i)
        lConvertElement(from[i], &to[i]);

    if (forceVarying && count == 1)
        for (int i = 1; i < g->target.vectorWidth; ++i)
            to[i] = to[0];
}


int
ConstExpr::AsInt64(int64_t *ip, bool forceVarying) const {
    switch (getBasicType()) {
    case AtomicType::TYPE_BOOL:   lConvert(boolVal,   ip, Count(), forceVarying); break;
    case AtomicType::TYPE_INT8:   lConvert(int8Val,   ip, Count(), forceVarying); break;
    case AtomicType::TYPE_UINT8:  lConvert(uint8Val,  ip, Count(), forceVarying); break;
    case AtomicType::TYPE_INT16:  lConvert(int16Val,  ip, Count(), forceVarying); break;
    case AtomicType::TYPE_UINT16: lConvert(uint16Val, ip, Count(), forceVarying); break;
    case AtomicType::TYPE_INT32:  lConvert(int32Val,  ip, Count(), forceVarying); break;
    case AtomicType::TYPE_UINT32: lConvert(uint32Val, ip, Count(), forceVarying); break;
    case AtomicType::TYPE_FLOAT:  lConvert(floatVal,  ip, Count(), forceVarying); break;
    case AtomicType::TYPE_DOUBLE: lConvert(doubleVal, ip, Count(), forceVarying); break;
    case AtomicType::TYPE_INT64:  lConvert(int64Val,  ip, Count(), forceVarying); break;
    case AtomicType::TYPE_UINT64: lConvert(uint64Val, ip, Count(), forceVarying); break;
    default:
        FATAL("unimplemented const type");
    }
    return Count();
}


int
ConstExpr::AsUInt64(uint64_t *up, bool forceVarying) const {
    switch (getBasicType()) {
    case AtomicType::TYPE_BOOL:   lConvert(boolVal,   up, Count(), forceVarying); break;
    case AtomicType::TYPE_INT8:   lConvert(int8Val,   up, Count(), forceVarying); break;
    case AtomicType::TYPE_UINT8:  lConvert(uint8Val,  up, Count(), forceVarying); break;
    case AtomicType::TYPE_INT16:  lConvert(int16Val,  up, Count(), forceVarying); break;
    case AtomicType::TYPE_UINT16: lConvert(uint16Val, up, Count(), forceVarying); break;
    case AtomicType::TYPE_INT32:  lConvert(int32Val,  up, Count(), forceVarying); break;
    case AtomicType::TYPE_UINT32: lConvert(uint32Val, up, Count(), forceVarying); break;
    case AtomicType::TYPE_FLOAT:  lConvert(floatVal,  up, Count(), forceVarying); break;
    case AtomicType::TYPE_DOUBLE: lConvert(doubleVal, up, Count(), forceVarying); break;
    case AtomicType::TYPE_INT64:  lConvert(int64Val,  up, Count(), forceVarying); break;
    case AtomicType::TYPE_UINT64: lConvert(uint64Val, up, Count(), forceVarying); break;
    default:
        FATAL("unimplemented const type");
    }
    return Count();
}


int
ConstExpr::AsDouble(double *d, bool forceVarying) const {
    switch (getBasicType()) {
    case AtomicType::TYPE_BOOL:   lConvert(boolVal,   d, Count(), forceVarying); break;
    case AtomicType::TYPE_INT8:   lConvert(int8Val,   d, Count(), forceVarying); break;
    case AtomicType::TYPE_UINT8:  lConvert(uint8Val,  d, Count(), forceVarying); break;
    case AtomicType::TYPE_INT16:  lConvert(int16Val,  d, Count(), forceVarying); break;
    case AtomicType::TYPE_UINT16: lConvert(uint16Val, d, Count(), forceVarying); break;
    case AtomicType::TYPE_INT32:  lConvert(int32Val,  d, Count(), forceVarying); break;
    case AtomicType::TYPE_UINT32: lConvert(uint32Val, d, Count(), forceVarying); break;
    case AtomicType::TYPE_FLOAT:  lConvert(floatVal,  d, Count(), forceVarying); break;
    case AtomicType::TYPE_DOUBLE: lConvert(doubleVal, d, Count(), forceVarying); break;
    case AtomicType::TYPE_INT64:  lConvert(int64Val,  d, Count(), forceVarying); break;
    case AtomicType::TYPE_UINT64: lConvert(uint64Val, d, Count(), forceVarying); break;
    default:
        FATAL("unimplemented const type");
    }
    return Count();
}


int
ConstExpr::AsFloat(float *fp, bool forceVarying) const {
    switch (getBasicType()) {
    case AtomicType::TYPE_BOOL:   lConvert(boolVal,   fp, Count(), forceVarying); break;
    case AtomicType::TYPE_INT8:   lConvert(int8Val,   fp, Count(), forceVarying); break;
    case AtomicType::TYPE_UINT8:  lConvert(uint8Val,  fp, Count(), forceVarying); break;
    case AtomicType::TYPE_INT16:  lConvert(int16Val,  fp, Count(), forceVarying); break;
    case AtomicType::TYPE_UINT16: lConvert(uint16Val, fp, Count(), forceVarying); break;
    case AtomicType::TYPE_INT32:  lConvert(int32Val,  fp, Count(), forceVarying); break;
    case AtomicType::TYPE_UINT32: lConvert(uint32Val, fp, Count(), forceVarying); break;
    case AtomicType::TYPE_FLOAT:  lConvert(floatVal,  fp, Count(), forceVarying); break;
    case AtomicType::TYPE_DOUBLE: lConvert(doubleVal, fp, Count(), forceVarying); break;
    case AtomicType::TYPE_INT64:  lConvert(int64Val,  fp, Count(), forceVarying); break;
    case AtomicType::TYPE_UINT64: lConvert(uint64Val, fp, Count(), forceVarying); break;
    default:
        FATAL("unimplemented const type");
    }
    return Count();
}


int
ConstExpr::AsBool(bool *b, bool forceVarying) const {
    switch (getBasicType()) {
    case AtomicType::TYPE_BOOL:   lConvert(boolVal,   b, Count(), forceVarying); break;
    case AtomicType::TYPE_INT8:   lConvert(int8Val,   b, Count(), forceVarying); break;
    case AtomicType::TYPE_UINT8:  lConvert(uint8Val,  b, Count(), forceVarying); break;
    case AtomicType::TYPE_INT16:  lConvert(int16Val,  b, Count(), forceVarying); break;
    case AtomicType::TYPE_UINT16: lConvert(uint16Val, b, Count(), forceVarying); break;
    case AtomicType::TYPE_INT32:  lConvert(int32Val,  b, Count(), forceVarying); break;
    case AtomicType::TYPE_UINT32: lConvert(uint32Val, b, Count(), forceVarying); break;
    case AtomicType::TYPE_FLOAT:  lConvert(floatVal,  b, Count(), forceVarying); break;
    case AtomicType::TYPE_DOUBLE: lConvert(doubleVal, b, Count(), forceVarying); break;
    case AtomicType::TYPE_INT64:  lConvert(int64Val,  b, Count(), forceVarying); break;
    case AtomicType::TYPE_UINT64: lConvert(uint64Val, b, Count(), forceVarying); break;
    default:
        FATAL("unimplemented const type");
    }
    return Count();
}


int
ConstExpr::AsInt8(int8_t *ip, bool forceVarying) const {
    switch (getBasicType()) {
    case AtomicType::TYPE_BOOL:   lConvert(boolVal,   ip, Count(), forceVarying); break;
    case AtomicType::TYPE_INT8:   lConvert(int8Val,   ip, Count(), forceVarying); break;
    case AtomicType::TYPE_UINT8:  lConvert(uint8Val,  ip, Count(), forceVarying); break;
    case AtomicType::TYPE_INT16:  lConvert(int16Val,  ip, Count(), forceVarying); break;
    case AtomicType::TYPE_UINT16: lConvert(uint16Val, ip, Count(), forceVarying); break;
    case AtomicType::TYPE_INT32:  lConvert(int32Val,  ip, Count(), forceVarying); break;
    case AtomicType::TYPE_UINT32: lConvert(uint32Val, ip, Count(), forceVarying); break;
    case AtomicType::TYPE_FLOAT:  lConvert(floatVal,  ip, Count(), forceVarying); break;
    case AtomicType::TYPE_DOUBLE: lConvert(doubleVal, ip, Count(), forceVarying); break;
    case AtomicType::TYPE_INT64:  lConvert(int64Val,  ip, Count(), forceVarying); break;
    case AtomicType::TYPE_UINT64: lConvert(uint64Val, ip, Count(), forceVarying); break;
    default:
        FATAL("unimplemented const type");
    }
    return Count();
}


int
ConstExpr::AsUInt8(uint8_t *up, bool forceVarying) const {
    switch (getBasicType()) {
    case AtomicType::TYPE_BOOL:   lConvert(boolVal,   up, Count(), forceVarying); break;
    case AtomicType::TYPE_INT8:   lConvert(int8Val,   up, Count(), forceVarying); break;
    case AtomicType::TYPE_UINT8:  lConvert(uint8Val,  up, Count(), forceVarying); break;
    case AtomicType::TYPE_INT16:  lConvert(int16Val,  up, Count(), forceVarying); break;
    case AtomicType::TYPE_UINT16: lConvert(uint16Val, up, Count(), forceVarying); break;
    case AtomicType::TYPE_INT32:  lConvert(int32Val,  up, Count(), forceVarying); break;
    case AtomicType::TYPE_UINT32: lConvert(uint32Val, up, Count(), forceVarying); break;
    case AtomicType::TYPE_FLOAT:  lConvert(floatVal,  up, Count(), forceVarying); break;
    case AtomicType::TYPE_DOUBLE: lConvert(doubleVal, up, Count(), forceVarying); break;
    case AtomicType::TYPE_INT64:  lConvert(int64Val,  up, Count(), forceVarying); break;
    case AtomicType::TYPE_UINT64: lConvert(uint64Val, up, Count(), forceVarying); break;
    default:
        FATAL("unimplemented const type");
    }
    return Count();
}


int
ConstExpr::AsInt16(int16_t *ip, bool forceVarying) const {
    switch (getBasicType()) {
    case AtomicType::TYPE_BOOL:   lConvert(boolVal,   ip, Count(), forceVarying); break;
    case AtomicType::TYPE_INT8:   lConvert(int8Val,   ip, Count(), forceVarying); break;
    case AtomicType::TYPE_UINT8:  lConvert(uint8Val,  ip, Count(), forceVarying); break;
    case AtomicType::TYPE_INT16:  lConvert(int16Val,  ip, Count(), forceVarying); break;
    case AtomicType::TYPE_UINT16: lConvert(uint16Val, ip, Count(), forceVarying); break;
    case AtomicType::TYPE_INT32:  lConvert(int32Val,  ip, Count(), forceVarying); break;
    case AtomicType::TYPE_UINT32: lConvert(uint32Val, ip, Count(), forceVarying); break;
    case AtomicType::TYPE_FLOAT:  lConvert(floatVal,  ip, Count(), forceVarying); break;
    case AtomicType::TYPE_DOUBLE: lConvert(doubleVal, ip, Count(), forceVarying); break;
    case AtomicType::TYPE_INT64:  lConvert(int64Val,  ip, Count(), forceVarying); break;
    case AtomicType::TYPE_UINT64: lConvert(uint64Val, ip, Count(), forceVarying); break;
    default:
        FATAL("unimplemented const type");
    }
    return Count();
}


int
ConstExpr::AsUInt16(uint16_t *up, bool forceVarying) const {
    switch (getBasicType()) {
    case AtomicType::TYPE_BOOL:   lConvert(boolVal,   up, Count(), forceVarying); break;
    case AtomicType::TYPE_INT8:   lConvert(int8Val,   up, Count(), forceVarying); break;
    case AtomicType::TYPE_UINT8:  lConvert(uint8Val,  up, Count(), forceVarying); break;
    case AtomicType::TYPE_INT16:  lConvert(int16Val,  up, Count(), forceVarying); break;
    case AtomicType::TYPE_UINT16: lConvert(uint16Val, up, Count(), forceVarying); break;
    case AtomicType::TYPE_INT32:  lConvert(int32Val,  up, Count(), forceVarying); break;
    case AtomicType::TYPE_UINT32: lConvert(uint32Val, up, Count(), forceVarying); break;
    case AtomicType::TYPE_FLOAT:  lConvert(floatVal,  up, Count(), forceVarying); break;
    case AtomicType::TYPE_DOUBLE: lConvert(doubleVal, up, Count(), forceVarying); break;
    case AtomicType::TYPE_INT64:  lConvert(int64Val,  up, Count(), forceVarying); break;
    case AtomicType::TYPE_UINT64: lConvert(uint64Val, up, Count(), forceVarying); break;
    default:
        FATAL("unimplemented const type");
    }
    return Count();
}


int
ConstExpr::AsInt32(int32_t *ip, bool forceVarying) const {
    switch (getBasicType()) {
    case AtomicType::TYPE_BOOL:   lConvert(boolVal,   ip, Count(), forceVarying); break;
    case AtomicType::TYPE_INT8:   lConvert(int8Val,   ip, Count(), forceVarying); break;
    case AtomicType::TYPE_UINT8:  lConvert(uint8Val,  ip, Count(), forceVarying); break;
    case AtomicType::TYPE_INT16:  lConvert(int16Val,  ip, Count(), forceVarying); break;
    case AtomicType::TYPE_UINT16: lConvert(uint16Val, ip, Count(), forceVarying); break;
    case AtomicType::TYPE_INT32:  lConvert(int32Val,  ip, Count(), forceVarying); break;
    case AtomicType::TYPE_UINT32: lConvert(uint32Val, ip, Count(), forceVarying); break;
    case AtomicType::TYPE_FLOAT:  lConvert(floatVal,  ip, Count(), forceVarying); break;
    case AtomicType::TYPE_DOUBLE: lConvert(doubleVal, ip, Count(), forceVarying); break;
    case AtomicType::TYPE_INT64:  lConvert(int64Val,  ip, Count(), forceVarying); break;
    case AtomicType::TYPE_UINT64: lConvert(uint64Val, ip, Count(), forceVarying); break;
    default:
        FATAL("unimplemented const type");
    }
    return Count();
}


int
ConstExpr::AsUInt32(uint32_t *up, bool forceVarying) const {
    switch (getBasicType()) {
    case AtomicType::TYPE_BOOL:   lConvert(boolVal,   up, Count(), forceVarying); break;
    case AtomicType::TYPE_INT8:   lConvert(int8Val,   up, Count(), forceVarying); break;
    case AtomicType::TYPE_UINT8:  lConvert(uint8Val,  up, Count(), forceVarying); break;
    case AtomicType::TYPE_INT16:  lConvert(int16Val,  up, Count(), forceVarying); break;
    case AtomicType::TYPE_UINT16: lConvert(uint16Val, up, Count(), forceVarying); break;
    case AtomicType::TYPE_INT32:  lConvert(int32Val,  up, Count(), forceVarying); break;
    case AtomicType::TYPE_UINT32: lConvert(uint32Val, up, Count(), forceVarying); break;
    case AtomicType::TYPE_FLOAT:  lConvert(floatVal,  up, Count(), forceVarying); break;
    case AtomicType::TYPE_DOUBLE: lConvert(doubleVal, up, Count(), forceVarying); break;
    case AtomicType::TYPE_INT64:  lConvert(int64Val,  up, Count(), forceVarying); break;
    case AtomicType::TYPE_UINT64: lConvert(uint64Val, up, Count(), forceVarying); break;
    default:
        FATAL("unimplemented const type");
    }
    return Count();
}


int
ConstExpr::Count() const { 
    return GetType()->IsVaryingType() ? g->target.vectorWidth : 1; 
}


llvm::Constant *
ConstExpr::GetConstant(const Type *type) const {
    // Caller shouldn't be trying to stuff a varying value here into a
    // constant type.
    if (type->IsUniformType())
        Assert(Count() == 1);

    type = type->GetAsNonConstType();
    if (type == AtomicType::UniformBool || type == AtomicType::VaryingBool) {
        bool bv[ISPC_MAX_NVEC];
        AsBool(bv, type->IsVaryingType());
        if (type->IsUniformType())
            return bv[0] ? LLVMTrue : LLVMFalse;
        else
            return LLVMBoolVector(bv);
    }
    else if (type == AtomicType::UniformInt8 || type == AtomicType::VaryingInt8) {
        int8_t iv[ISPC_MAX_NVEC];
        AsInt8(iv, type->IsVaryingType());
        if (type->IsUniformType())
            return LLVMInt8(iv[0]);
        else
            return LLVMInt8Vector(iv);
    }
    else if (type == AtomicType::UniformUInt8 || type == AtomicType::VaryingUInt8 ||
             dynamic_cast<const EnumType *>(type) != NULL) {
        uint8_t uiv[ISPC_MAX_NVEC];
        AsUInt8(uiv, type->IsVaryingType());
        if (type->IsUniformType())
            return LLVMUInt8(uiv[0]);
        else
            return LLVMUInt8Vector(uiv);
    }
    else if (type == AtomicType::UniformInt16 || type == AtomicType::VaryingInt16) {
        int16_t iv[ISPC_MAX_NVEC];
        AsInt16(iv, type->IsVaryingType());
        if (type->IsUniformType())
            return LLVMInt16(iv[0]);
        else
            return LLVMInt16Vector(iv);
    }
    else if (type == AtomicType::UniformUInt16 || type == AtomicType::VaryingUInt16 ||
             dynamic_cast<const EnumType *>(type) != NULL) {
        uint16_t uiv[ISPC_MAX_NVEC];
        AsUInt16(uiv, type->IsVaryingType());
        if (type->IsUniformType())
            return LLVMUInt16(uiv[0]);
        else
            return LLVMUInt16Vector(uiv);
    }
    else if (type == AtomicType::UniformInt32 || type == AtomicType::VaryingInt32) {
        int32_t iv[ISPC_MAX_NVEC];
        AsInt32(iv, type->IsVaryingType());
        if (type->IsUniformType())
            return LLVMInt32(iv[0]);
        else
            return LLVMInt32Vector(iv);
    }
    else if (type == AtomicType::UniformUInt32 || type == AtomicType::VaryingUInt32 ||
             dynamic_cast<const EnumType *>(type) != NULL) {
        uint32_t uiv[ISPC_MAX_NVEC];
        AsUInt32(uiv, type->IsVaryingType());
        if (type->IsUniformType())
            return LLVMUInt32(uiv[0]);
        else
            return LLVMUInt32Vector(uiv);
    }
    else if (type == AtomicType::UniformFloat || type == AtomicType::VaryingFloat) {
        float fv[ISPC_MAX_NVEC];
        AsFloat(fv, type->IsVaryingType());
        if (type->IsUniformType())
            return LLVMFloat(fv[0]);
        else
            return LLVMFloatVector(fv);
    }
    else if (type == AtomicType::UniformInt64 || type == AtomicType::VaryingInt64) {
        int64_t iv[ISPC_MAX_NVEC];
        AsInt64(iv, type->IsVaryingType());
        if (type->IsUniformType())
            return LLVMInt64(iv[0]);
        else
            return LLVMInt64Vector(iv);
    }
    else if (type == AtomicType::UniformUInt64 || type == AtomicType::VaryingUInt64) {
        uint64_t uiv[ISPC_MAX_NVEC];
        AsUInt64(uiv, type->IsVaryingType());
        if (type->IsUniformType())
            return LLVMUInt64(uiv[0]);
        else
            return LLVMUInt64Vector(uiv);
    }
    else if (type == AtomicType::UniformDouble || type == AtomicType::VaryingDouble) {
        double dv[ISPC_MAX_NVEC];
        AsDouble(dv, type->IsVaryingType());
        if (type->IsUniformType())
            return LLVMDouble(dv[0]);
        else
            return LLVMDoubleVector(dv);
    }
    else {
        FATAL("unexpected type in ConstExpr::GetConstant()");
        return NULL;
    }
}


Expr *
ConstExpr::Optimize() {
    return this;
}


Expr *
ConstExpr::TypeCheck() {
    return this;
}


int
ConstExpr::EstimateCost() const {
    return 0;
}


void
ConstExpr::Print() const {
    printf("[%s] (", GetType()->GetString().c_str());
    for (int i = 0; i < Count(); ++i) {
        switch (getBasicType()) {
        case AtomicType::TYPE_BOOL:
            printf("%s", boolVal[i] ? "true" : "false");
            break;
        case AtomicType::TYPE_INT8:
            printf("%d", (int)int8Val[i]);
            break;
        case AtomicType::TYPE_UINT8:
            printf("%u", (int)uint8Val[i]);
            break;
        case AtomicType::TYPE_INT16:
            printf("%d", (int)int16Val[i]);
            break;
        case AtomicType::TYPE_UINT16:
            printf("%u", (int)uint16Val[i]);
            break;
        case AtomicType::TYPE_INT32:
            printf("%d", int32Val[i]);
            break;
        case AtomicType::TYPE_UINT32:
            printf("%u", uint32Val[i]);
            break;
        case AtomicType::TYPE_FLOAT:
            printf("%f", floatVal[i]);
            break;
        case AtomicType::TYPE_INT64:
#ifdef ISPC_IS_LINUX
            printf("%ld", int64Val[i]);
#else
            printf("%lld", int64Val[i]);
#endif
            break;
        case AtomicType::TYPE_UINT64:
#ifdef ISPC_IS_LINUX
            printf("%lu", uint64Val[i]);
#else
            printf("%llu", uint64Val[i]);
#endif
            break;
        case AtomicType::TYPE_DOUBLE:
            printf("%f", doubleVal[i]);
            break;
        default:
            FATAL("unimplemented const type");
        }
        if (i != Count() - 1)
            printf(", ");
    }
    printf(")");
    pos.Print();
}


///////////////////////////////////////////////////////////////////////////
// TypeCastExpr

TypeCastExpr::TypeCastExpr(const Type *t, Expr *e, bool pu, SourcePos p) 
  : Expr(p) {
    type = t;
    expr = e;
    preserveUniformity = pu;
}


/** Handle all the grungy details of type conversion between atomic types.
    Given an input value in exprVal of type fromType, convert it to the
    llvm::Value with type toType.
 */
static llvm::Value *
lTypeConvAtomic(FunctionEmitContext *ctx, llvm::Value *exprVal, 
                const AtomicType *toType, const AtomicType *fromType,
                SourcePos pos) {
    llvm::Value *cast = NULL;

    switch (toType->basicType) {
    case AtomicType::TYPE_FLOAT: {
        LLVM_TYPE_CONST llvm::Type *targetType = 
            fromType->IsUniformType() ? LLVMTypes::FloatType : 
                                        LLVMTypes::FloatVectorType;
        switch (fromType->basicType) {
        case AtomicType::TYPE_BOOL:
            if (fromType->IsVaryingType() && 
                LLVMTypes::BoolVectorType == LLVMTypes::Int32VectorType)
                // If we have a bool vector of i32 elements, first truncate
                // down to a single bit
                exprVal = ctx->TruncInst(exprVal, LLVMTypes::Int1VectorType, "bool_to_i1");
            // And then do an unisgned int->float cast
            cast = ctx->CastInst(llvm::Instruction::UIToFP, // unsigned int
                                 exprVal, targetType, "bool2float");
            break;
        case AtomicType::TYPE_INT8:
        case AtomicType::TYPE_INT16:
        case AtomicType::TYPE_INT32:
        case AtomicType::TYPE_INT64:
            cast = ctx->CastInst(llvm::Instruction::SIToFP, // signed int to float
                                 exprVal, targetType, "int2float");
            break;
        case AtomicType::TYPE_UINT8:
        case AtomicType::TYPE_UINT16:
        case AtomicType::TYPE_UINT32:
        case AtomicType::TYPE_UINT64:
            if (fromType->IsVaryingType())
                PerformanceWarning(pos, "Conversion from unsigned int to float is slow. "
                                   "Use \"int\" if possible");
            cast = ctx->CastInst(llvm::Instruction::UIToFP, // unsigned int to float
                                 exprVal, targetType, "uint2float");
            break;
        case AtomicType::TYPE_FLOAT:
            // No-op cast.
            cast = exprVal;
            break;
        case AtomicType::TYPE_DOUBLE:
            cast = ctx->FPCastInst(exprVal, targetType, "double2float");
            break;
        default:
            FATAL("unimplemented");
        }
        break;
    }
    case AtomicType::TYPE_DOUBLE: {
        LLVM_TYPE_CONST llvm::Type *targetType = 
            fromType->IsUniformType() ? LLVMTypes::DoubleType :
                                        LLVMTypes::DoubleVectorType;
        switch (fromType->basicType) {
        case AtomicType::TYPE_BOOL:
            if (fromType->IsVaryingType() && 
                LLVMTypes::BoolVectorType == LLVMTypes::Int32VectorType)
                // truncate i32 bool vector values to i1s
                exprVal = ctx->TruncInst(exprVal, LLVMTypes::Int1VectorType, "bool_to_i1");
            cast = ctx->CastInst(llvm::Instruction::UIToFP, // unsigned int to double
                                 exprVal, targetType, "bool2double");
            break;
        case AtomicType::TYPE_INT8:
        case AtomicType::TYPE_INT16:
        case AtomicType::TYPE_INT32:
        case AtomicType::TYPE_INT64:
            cast = ctx->CastInst(llvm::Instruction::SIToFP, // signed int
                                 exprVal, targetType, "int2double");
            break;
        case AtomicType::TYPE_UINT8:
        case AtomicType::TYPE_UINT16:
        case AtomicType::TYPE_UINT32:
        case AtomicType::TYPE_UINT64:
            cast = ctx->CastInst(llvm::Instruction::UIToFP, // unsigned int
                                 exprVal, targetType, "uint2double");
            break;
        case AtomicType::TYPE_FLOAT:
            cast = ctx->FPCastInst(exprVal, targetType, "float2double");
            break;
        case AtomicType::TYPE_DOUBLE:
            cast = exprVal;
            break;
        default:
            FATAL("unimplemented");
        }
        break;
    }
    case AtomicType::TYPE_INT8: {
        LLVM_TYPE_CONST llvm::Type *targetType = 
            fromType->IsUniformType() ? LLVMTypes::Int8Type :
                                        LLVMTypes::Int8VectorType;
        switch (fromType->basicType) {
        case AtomicType::TYPE_BOOL:
            if (fromType->IsVaryingType() && 
                LLVMTypes::BoolVectorType == LLVMTypes::Int32VectorType)
                exprVal = ctx->TruncInst(exprVal, LLVMTypes::Int1VectorType, "bool_to_i1");
            cast = ctx->ZExtInst(exprVal, targetType, "bool2int");
            break;
        case AtomicType::TYPE_INT8:
        case AtomicType::TYPE_UINT8:
            cast = exprVal;
            break;
        case AtomicType::TYPE_INT16:
        case AtomicType::TYPE_UINT16:
        case AtomicType::TYPE_INT32:
        case AtomicType::TYPE_UINT32:
        case AtomicType::TYPE_INT64:
        case AtomicType::TYPE_UINT64:
            cast = ctx->TruncInst(exprVal, targetType, "int64_to_int8");
            break;
        case AtomicType::TYPE_FLOAT:
            cast = ctx->CastInst(llvm::Instruction::FPToSI, // signed int
                                 exprVal, targetType, "float2int");
            break;
        case AtomicType::TYPE_DOUBLE:
            cast = ctx->CastInst(llvm::Instruction::FPToSI, // signed int
                                 exprVal, targetType, "double2int");
            break;
        default:
            FATAL("unimplemented");
        }
        break;
    }
    case AtomicType::TYPE_UINT8: {
        LLVM_TYPE_CONST llvm::Type *targetType = 
            fromType->IsUniformType() ? LLVMTypes::Int8Type :
                                        LLVMTypes::Int8VectorType;
        switch (fromType->basicType) {
        case AtomicType::TYPE_BOOL:
            if (fromType->IsVaryingType() && 
                LLVMTypes::BoolVectorType == LLVMTypes::Int32VectorType)
                exprVal = ctx->TruncInst(exprVal, LLVMTypes::Int1VectorType, "bool_to_i1");
            cast = ctx->ZExtInst(exprVal, targetType, "bool2uint");
            break;
        case AtomicType::TYPE_INT8:
        case AtomicType::TYPE_UINT8:
            cast = exprVal;
            break;
        case AtomicType::TYPE_INT16:
        case AtomicType::TYPE_UINT16:
        case AtomicType::TYPE_INT32:
        case AtomicType::TYPE_UINT32:
        case AtomicType::TYPE_INT64:
        case AtomicType::TYPE_UINT64:
            cast = ctx->TruncInst(exprVal, targetType, "int64_to_uint8");
            break;
        case AtomicType::TYPE_FLOAT:
            if (fromType->IsVaryingType())
                PerformanceWarning(pos, "Conversion from float to unsigned int is slow. "
                                   "Use \"int\" if possible");
            cast = ctx->CastInst(llvm::Instruction::FPToUI, // unsigned int
                                 exprVal, targetType, "float2uint");
            break;
        case AtomicType::TYPE_DOUBLE:
            if (fromType->IsVaryingType())
                PerformanceWarning(pos, "Conversion from double to unsigned int is slow. "
                                   "Use \"int\" if possible");
            cast = ctx->CastInst(llvm::Instruction::FPToUI, // unsigned int
                                 exprVal, targetType, "double2uint");
            break;
        default:
            FATAL("unimplemented");
        }
        break;
    }
    case AtomicType::TYPE_INT16: {
        LLVM_TYPE_CONST llvm::Type *targetType = 
            fromType->IsUniformType() ? LLVMTypes::Int16Type :
                                        LLVMTypes::Int16VectorType;
        switch (fromType->basicType) {
        case AtomicType::TYPE_BOOL:
            if (fromType->IsVaryingType() && 
                LLVMTypes::BoolVectorType == LLVMTypes::Int32VectorType)
                exprVal = ctx->TruncInst(exprVal, LLVMTypes::Int1VectorType, "bool_to_i1");
            cast = ctx->ZExtInst(exprVal, targetType, "bool2int");
            break;
        case AtomicType::TYPE_INT8:
            cast = ctx->SExtInst(exprVal, targetType, "int2int16");
            break;
        case AtomicType::TYPE_UINT8:
            cast = ctx->ZExtInst(exprVal, targetType, "uint2uint16");
            break;
        case AtomicType::TYPE_INT16:
        case AtomicType::TYPE_UINT16:
            cast = exprVal;
            break;
        case AtomicType::TYPE_FLOAT:
            cast = ctx->CastInst(llvm::Instruction::FPToSI, // signed int
                                 exprVal, targetType, "float2int");
            break;
        case AtomicType::TYPE_INT32:
        case AtomicType::TYPE_UINT32:
        case AtomicType::TYPE_INT64:
        case AtomicType::TYPE_UINT64:
            cast = ctx->TruncInst(exprVal, targetType, "int64_to_int16");
            break;
        case AtomicType::TYPE_DOUBLE:
            cast = ctx->CastInst(llvm::Instruction::FPToSI, // signed int
                                 exprVal, targetType, "double2int");
            break;
        default:
            FATAL("unimplemented");
        }
        break;
    }
    case AtomicType::TYPE_UINT16: {
        LLVM_TYPE_CONST llvm::Type *targetType = 
            fromType->IsUniformType() ? LLVMTypes::Int16Type :
                                        LLVMTypes::Int16VectorType;
        switch (fromType->basicType) {
        case AtomicType::TYPE_BOOL:
            if (fromType->IsVaryingType() && 
                LLVMTypes::BoolVectorType == LLVMTypes::Int32VectorType)
                exprVal = ctx->TruncInst(exprVal, LLVMTypes::Int1VectorType, "bool_to_i1");
            cast = ctx->ZExtInst(exprVal, targetType, "bool2uint16");
            break;
        case AtomicType::TYPE_INT8:
            cast = ctx->SExtInst(exprVal, targetType, "uint2uint16");
            break;
        case AtomicType::TYPE_UINT8:
            cast = ctx->ZExtInst(exprVal, targetType, "uint2uint16");
            break;            
        case AtomicType::TYPE_INT16:
        case AtomicType::TYPE_UINT16:
            cast = exprVal;
            break;
        case AtomicType::TYPE_FLOAT:
            if (fromType->IsVaryingType())
                PerformanceWarning(pos, "Conversion from float to unsigned int is slow. "
                                   "Use \"int\" if possible");
            cast = ctx->CastInst(llvm::Instruction::FPToUI, // unsigned int
                                 exprVal, targetType, "float2uint");
            break;
        case AtomicType::TYPE_INT32:
        case AtomicType::TYPE_UINT32:
        case AtomicType::TYPE_INT64:
        case AtomicType::TYPE_UINT64:
            cast = ctx->TruncInst(exprVal, targetType, "int64_to_uint16");
            break;
        case AtomicType::TYPE_DOUBLE:
            if (fromType->IsVaryingType())
                PerformanceWarning(pos, "Conversion from double to unsigned int is slow. "
                                   "Use \"int\" if possible");
            cast = ctx->CastInst(llvm::Instruction::FPToUI, // unsigned int
                                 exprVal, targetType, "double2uint");
            break;
        default:
            FATAL("unimplemented");
        }
        break;
    }
    case AtomicType::TYPE_INT32: {
        LLVM_TYPE_CONST llvm::Type *targetType = 
            fromType->IsUniformType() ? LLVMTypes::Int32Type :
                                        LLVMTypes::Int32VectorType;
        switch (fromType->basicType) {
        case AtomicType::TYPE_BOOL:
            if (fromType->IsVaryingType() && 
                LLVMTypes::BoolVectorType == LLVMTypes::Int32VectorType)
                exprVal = ctx->TruncInst(exprVal, LLVMTypes::Int1VectorType, "bool_to_i1");
            cast = ctx->ZExtInst(exprVal, targetType, "bool2int");
            break;
        case AtomicType::TYPE_INT8:
        case AtomicType::TYPE_INT16:
            cast = ctx->SExtInst(exprVal, targetType, "int2int32");
            break;
        case AtomicType::TYPE_UINT8:
        case AtomicType::TYPE_UINT16:
            cast = ctx->ZExtInst(exprVal, targetType, "uint2uint32");
            break;
        case AtomicType::TYPE_INT32:
        case AtomicType::TYPE_UINT32:
            cast = exprVal;
            break;
        case AtomicType::TYPE_FLOAT:
            cast = ctx->CastInst(llvm::Instruction::FPToSI, // signed int
                                 exprVal, targetType, "float2int");
            break;
        case AtomicType::TYPE_INT64:
        case AtomicType::TYPE_UINT64:
            cast = ctx->TruncInst(exprVal, targetType, "int64_to_int32");
            break;
        case AtomicType::TYPE_DOUBLE:
            cast = ctx->CastInst(llvm::Instruction::FPToSI, // signed int
                                 exprVal, targetType, "double2int");
            break;
        default:
            FATAL("unimplemented");
        }
        break;
    }
    case AtomicType::TYPE_UINT32: {
        LLVM_TYPE_CONST llvm::Type *targetType = 
            fromType->IsUniformType() ? LLVMTypes::Int32Type :
                                        LLVMTypes::Int32VectorType;
        switch (fromType->basicType) {
        case AtomicType::TYPE_BOOL:
            if (fromType->IsVaryingType() && 
                LLVMTypes::BoolVectorType == LLVMTypes::Int32VectorType)
                exprVal = ctx->TruncInst(exprVal, LLVMTypes::Int1VectorType, "bool_to_i1");
            cast = ctx->ZExtInst(exprVal, targetType, "bool2uint");
            break;
        case AtomicType::TYPE_INT8:
        case AtomicType::TYPE_INT16:
            cast = ctx->SExtInst(exprVal, targetType, "uint2uint");
            break;
        case AtomicType::TYPE_UINT8:
        case AtomicType::TYPE_UINT16:
            cast = ctx->ZExtInst(exprVal, targetType, "uint2uint");
            break;            
        case AtomicType::TYPE_INT32:
        case AtomicType::TYPE_UINT32:
            cast = exprVal;
            break;
        case AtomicType::TYPE_FLOAT:
            if (fromType->IsVaryingType())
                PerformanceWarning(pos, "Conversion from float to unsigned int is slow. "
                                   "Use \"int\" if possible");
            cast = ctx->CastInst(llvm::Instruction::FPToUI, // unsigned int
                                 exprVal, targetType, "float2uint");
            break;
        case AtomicType::TYPE_INT64:
        case AtomicType::TYPE_UINT64:
            cast = ctx->TruncInst(exprVal, targetType, "int64_to_uint32");
            break;
        case AtomicType::TYPE_DOUBLE:
            if (fromType->IsVaryingType())
                PerformanceWarning(pos, "Conversion from double to unsigned int is slow. "
                                   "Use \"int\" if possible");
            cast = ctx->CastInst(llvm::Instruction::FPToUI, // unsigned int
                                 exprVal, targetType, "double2uint");
            break;
        default:
            FATAL("unimplemented");
        }
        break;
    }
    case AtomicType::TYPE_INT64: {
        LLVM_TYPE_CONST llvm::Type *targetType = 
            fromType->IsUniformType() ? LLVMTypes::Int64Type : 
                                        LLVMTypes::Int64VectorType;
        switch (fromType->basicType) {
        case AtomicType::TYPE_BOOL:
            if (fromType->IsVaryingType() &&
                LLVMTypes::BoolVectorType == LLVMTypes::Int32VectorType)
                exprVal = ctx->TruncInst(exprVal, LLVMTypes::Int1VectorType, "bool_to_i1");
            cast = ctx->ZExtInst(exprVal, targetType, "bool2int64");
            break;
        case AtomicType::TYPE_INT8:
        case AtomicType::TYPE_INT16:
        case AtomicType::TYPE_INT32:
            cast = ctx->SExtInst(exprVal, targetType, "int_to_int64");
            break;
        case AtomicType::TYPE_UINT8:
        case AtomicType::TYPE_UINT16:
        case AtomicType::TYPE_UINT32:
            cast = ctx->ZExtInst(exprVal, targetType, "uint_to_int64");
            break;
        case AtomicType::TYPE_FLOAT:
            cast = ctx->CastInst(llvm::Instruction::FPToSI, // signed int
                                 exprVal, targetType, "float2int64");
            break;
        case AtomicType::TYPE_INT64:
        case AtomicType::TYPE_UINT64:
            cast = exprVal;
            break;
        case AtomicType::TYPE_DOUBLE:
            cast = ctx->CastInst(llvm::Instruction::FPToSI, // signed int
                                 exprVal, targetType, "double2int64");
            break;
        default:
            FATAL("unimplemented");
        }
        break;
    }
    case AtomicType::TYPE_UINT64: {
        LLVM_TYPE_CONST llvm::Type *targetType = 
            fromType->IsUniformType() ? LLVMTypes::Int64Type : 
                                        LLVMTypes::Int64VectorType;
        switch (fromType->basicType) {
        case AtomicType::TYPE_BOOL:
            if (fromType->IsVaryingType() && 
                LLVMTypes::BoolVectorType == LLVMTypes::Int32VectorType)
                exprVal = ctx->TruncInst(exprVal, LLVMTypes::Int1VectorType, "bool_to_i1");
            cast = ctx->ZExtInst(exprVal, targetType, "bool2uint");
            break;
        case AtomicType::TYPE_INT8:
        case AtomicType::TYPE_INT16:
        case AtomicType::TYPE_INT32:
            cast = ctx->SExtInst(exprVal, targetType, "int_to_uint64");
            break;
        case AtomicType::TYPE_UINT8:
        case AtomicType::TYPE_UINT16:
        case AtomicType::TYPE_UINT32:
            cast = ctx->ZExtInst(exprVal, targetType, "uint_to_uint64");
            break;
        case AtomicType::TYPE_FLOAT:
            if (fromType->IsVaryingType())
                PerformanceWarning(pos, "Conversion from float to unsigned int64 is slow. "
                                   "Use \"int64\" if possible");
            cast = ctx->CastInst(llvm::Instruction::FPToUI, // signed int
                                 exprVal, targetType, "float2uint");
            break;
        case AtomicType::TYPE_INT64:
        case AtomicType::TYPE_UINT64:
            cast = exprVal;
            break;
        case AtomicType::TYPE_DOUBLE:
            if (fromType->IsVaryingType())
                PerformanceWarning(pos, "Conversion from double to unsigned int64 is slow. "
                                   "Use \"int64\" if possible");
            cast = ctx->CastInst(llvm::Instruction::FPToUI, // signed int
                                 exprVal, targetType, "double2uint");
            break;
        default:
            FATAL("unimplemented");
        }
        break;
    }
    case AtomicType::TYPE_BOOL: {
        switch (fromType->basicType) {
        case AtomicType::TYPE_BOOL:
            cast = exprVal;
            break;
        case AtomicType::TYPE_INT8:
        case AtomicType::TYPE_UINT8: {
            llvm::Value *zero = fromType->IsUniformType() ? (llvm::Value *)LLVMInt8(0) : 
                (llvm::Value *)LLVMInt8Vector((int8_t)0);
            cast = ctx->CmpInst(llvm::Instruction::ICmp, llvm::CmpInst::ICMP_NE,
                                exprVal, zero, "cmpi0");
            break;
        }
        case AtomicType::TYPE_INT16:
        case AtomicType::TYPE_UINT16: {
            llvm::Value *zero = fromType->IsUniformType() ? (llvm::Value *)LLVMInt16(0) : 
                (llvm::Value *)LLVMInt16Vector((int16_t)0);
            cast = ctx->CmpInst(llvm::Instruction::ICmp, llvm::CmpInst::ICMP_NE,
                                exprVal, zero, "cmpi0");
            break;
        }
        case AtomicType::TYPE_INT32:
        case AtomicType::TYPE_UINT32: {
            llvm::Value *zero = fromType->IsUniformType() ? (llvm::Value *)LLVMInt32(0) : 
                (llvm::Value *)LLVMInt32Vector(0);
            cast = ctx->CmpInst(llvm::Instruction::ICmp, llvm::CmpInst::ICMP_NE,
                                exprVal, zero, "cmpi0");
            break;
        }
        case AtomicType::TYPE_FLOAT: {
            llvm::Value *zero = fromType->IsUniformType() ? (llvm::Value *)LLVMFloat(0.f) : 
                (llvm::Value *)LLVMFloatVector(0.f);
            cast = ctx->CmpInst(llvm::Instruction::FCmp, llvm::CmpInst::FCMP_ONE,
                                exprVal, zero, "cmpf0");
            break;
        }
        case AtomicType::TYPE_INT64:
        case AtomicType::TYPE_UINT64: {
            llvm::Value *zero = fromType->IsUniformType() ? (llvm::Value *)LLVMInt64(0) : 
                (llvm::Value *)LLVMInt64Vector((int64_t)0);
            cast = ctx->CmpInst(llvm::Instruction::ICmp, llvm::CmpInst::ICMP_NE,
                                exprVal, zero, "cmpi0");
            break;
        }
        case AtomicType::TYPE_DOUBLE: {
            llvm::Value *zero = fromType->IsUniformType() ? (llvm::Value *)LLVMDouble(0.) : 
                (llvm::Value *)LLVMDoubleVector(0.);
            cast = ctx->CmpInst(llvm::Instruction::FCmp, llvm::CmpInst::FCMP_ONE,
                                exprVal, zero, "cmpd0");
            break;
        }
        default:
            FATAL("unimplemented");
        }

        if (fromType->IsUniformType()) {
            if (toType->IsVaryingType() && 
                LLVMTypes::BoolVectorType == LLVMTypes::Int32VectorType) {
                // extend out to i32 bool values from i1 here.  then we'll
                // turn into a vector below, the way it does for everyone
                // else...
                cast = ctx->SExtInst(cast, LLVMTypes::BoolVectorType->getElementType(),
                                     "i1bool_to_i32bool");
            }
        }
        else
            // fromType->IsVaryingType())
            cast = ctx->I1VecToBoolVec(cast);

        break;
    }
    default:
        FATAL("unimplemented");
    }

    // If we also want to go from uniform to varying, replicate out the
    // value across the vector elements..
    if (toType->IsVaryingType() && fromType->IsUniformType())
        return ctx->SmearUniform(cast);
    else
        return cast;
}


// FIXME: fold this into the FunctionEmitContext::SmearUniform() method?

/** Converts the given value of the given type to be the varying
    equivalent, returning the resulting value.
 */
static llvm::Value *
lUniformValueToVarying(FunctionEmitContext *ctx, llvm::Value *value,
                       const Type *type) {
    // nothing to do if it's already varying
    if (type->IsVaryingType())
        return value;

    // for structs/arrays/vectors, just recursively make their elements
    // varying (if needed) and populate the return value.
    const CollectionType *collectionType = 
        dynamic_cast<const CollectionType *>(type);
    if (collectionType != NULL) {
        LLVM_TYPE_CONST llvm::Type *llvmType = 
            type->GetAsVaryingType()->LLVMType(g->ctx);
        llvm::Value *retValue = llvm::UndefValue::get(llvmType);

        for (int i = 0; i < collectionType->GetElementCount(); ++i) {
            llvm::Value *v = ctx->ExtractInst(value, i, "get_element");
            v = lUniformValueToVarying(ctx, v, collectionType->GetElementType(i));
            retValue = ctx->InsertInst(retValue, v, i, "set_element");
        }
        return retValue;
    }

    // Otherwise we must have a uniform AtomicType, so smear its value
    // across the vector lanes.
    Assert(dynamic_cast<const AtomicType *>(type) != NULL);
    return ctx->SmearUniform(value);
}


llvm::Value *
TypeCastExpr::GetValue(FunctionEmitContext *ctx) const {
    if (!expr)
        return NULL;

    ctx->SetDebugPos(pos);
    const Type *toType = GetType(), *fromType = expr->GetType();
    if (!toType || !fromType || toType == AtomicType::Void || 
        fromType == AtomicType::Void)
        // an error should have been issued elsewhere in this case
        return NULL;

    const PointerType *fromPointerType = dynamic_cast<const PointerType *>(fromType);
    const PointerType *toPointerType = dynamic_cast<const PointerType *>(toType);
    const ArrayType *toArrayType = dynamic_cast<const ArrayType *>(toType);
    const ArrayType *fromArrayType = dynamic_cast<const ArrayType *>(fromType);
    if (fromPointerType != NULL) {
        if (toArrayType != NULL) {
            return expr->GetValue(ctx);
        }
        else if (toPointerType != NULL) {
            llvm::Value *value = expr->GetValue(ctx);
            if (value == NULL)
                return NULL;

            if (fromType->IsUniformType() && toType->IsUniformType())
                // bitcast to the actual pointer type
                return ctx->BitCastInst(value, toType->LLVMType(g->ctx));
            else if (fromType->IsVaryingType() && toType->IsVaryingType()) {
                // both are vectors of ints already, nothing to do at the IR
                // level
                return value;
            }
            else {
                Assert(fromType->IsUniformType() && toType->IsVaryingType());
                value = ctx->PtrToIntInst(value);
                return ctx->SmearUniform(value);
            }
        }
        else {
            Assert(dynamic_cast<const AtomicType *>(toType) != NULL);
            if (toType->IsBoolType()) {
                // convert pointer to bool
                LLVM_TYPE_CONST llvm::Type *lfu = 
                    fromType->GetAsUniformType()->LLVMType(g->ctx);
                LLVM_TYPE_CONST llvm::PointerType *llvmFromUnifType = 
                    llvm::dyn_cast<LLVM_TYPE_CONST llvm::PointerType>(lfu);

                llvm::Value *nullPtrValue = 
                    llvm::ConstantPointerNull::get(llvmFromUnifType);
                if (fromType->IsVaryingType())
                    nullPtrValue = ctx->SmearUniform(nullPtrValue);

                llvm::Value *exprVal = expr->GetValue(ctx);
                llvm::Value *cmp = 
                    ctx->CmpInst(llvm::Instruction::ICmp, llvm::CmpInst::ICMP_NE,
                                 exprVal, nullPtrValue, "ptr_ne_NULL");

                if (toType->IsVaryingType()) {
                    if (fromType->IsUniformType())
                        cmp = ctx->SmearUniform(cmp);
                    cmp = ctx->I1VecToBoolVec(cmp);
                }

                return cmp;
            }
            else {
                // ptr -> int
                llvm::Value *value = expr->GetValue(ctx);
                if (value == NULL)
                    return NULL;

                if (toType->IsVaryingType() && fromType->IsUniformType())
                    value = ctx->SmearUniform(value);

                LLVM_TYPE_CONST llvm::Type *llvmToType = toType->LLVMType(g->ctx);
                if (llvmToType == NULL)
                    return NULL;
                return ctx->PtrToIntInst(value, llvmToType, "ptr_typecast");
            }
        }
    }

    if (Type::EqualIgnoringConst(toType, fromType))
        // There's nothing to do, just return the value.  (LLVM's type
        // system doesn't worry about constiness.)
        return expr->GetValue(ctx);

    if (fromArrayType != NULL && toPointerType != NULL) {
        // implicit array to pointer to first element
        Expr *arrayAsPtr = lArrayToPointer(expr);
        if (Type::EqualIgnoringConst(arrayAsPtr->GetType(), toPointerType) == false) {
            Assert(Type::EqualIgnoringConst(arrayAsPtr->GetType()->GetAsVaryingType(),
                                            toPointerType) == true);
            arrayAsPtr = new TypeCastExpr(toPointerType, arrayAsPtr, false, pos);
            arrayAsPtr = ::TypeCheck(arrayAsPtr);
            Assert(arrayAsPtr != NULL);
            arrayAsPtr = ::Optimize(arrayAsPtr);
            Assert(arrayAsPtr != NULL);
        }
        Assert(Type::EqualIgnoringConst(arrayAsPtr->GetType(), toPointerType));
        return arrayAsPtr->GetValue(ctx);
    }

    // This also should be caught during typechecking
    Assert(!(toType->IsUniformType() && fromType->IsVaryingType()));

    if (toArrayType != NULL && fromArrayType != NULL) {
        // cast array pointer from [n x foo] to [0 x foo] if needed to be able
        // to pass to a function that takes an unsized array as a parameter
        if (toArrayType->GetElementCount() != 0 && 
            (toArrayType->GetElementCount() != fromArrayType->GetElementCount()))
            Warning(pos, "Type-converting array of length %d to length %d",
                    fromArrayType->GetElementCount(), toArrayType->GetElementCount());
        Assert(Type::EqualIgnoringConst(toArrayType->GetBaseType(),
                                        fromArrayType->GetBaseType()));
        llvm::Value *v = expr->GetValue(ctx);
        LLVM_TYPE_CONST llvm::Type *ptype = toType->LLVMType(g->ctx);
        return ctx->BitCastInst(v, ptype); //, "array_cast_0size");
    }

    const ReferenceType *toReference = dynamic_cast<const ReferenceType *>(toType);
    const ReferenceType *fromReference = dynamic_cast<const ReferenceType *>(fromType);
    if (toReference && fromReference) {
        const Type *toTarget = toReference->GetReferenceTarget();
        const Type *fromTarget = fromReference->GetReferenceTarget();

        const ArrayType *toArray = dynamic_cast<const ArrayType *>(toTarget);
        const ArrayType *fromArray = dynamic_cast<const ArrayType *>(fromTarget);
        if (toArray && fromArray) {
            // cast array pointer from [n x foo] to [0 x foo] if needed to be able
            // to pass to a function that takes an unsized array as a parameter
            if(toArray->GetElementCount() != 0 && 
               (toArray->GetElementCount() != fromArray->GetElementCount()))
                Warning(pos, "Type-converting array of length %d to length %d",
                        fromArray->GetElementCount(), toArray->GetElementCount());
            Assert(Type::EqualIgnoringConst(toArray->GetBaseType(),
                                            fromArray->GetBaseType()));
            llvm::Value *v = expr->GetValue(ctx);
            LLVM_TYPE_CONST llvm::Type *ptype = toType->LLVMType(g->ctx);
            return ctx->BitCastInst(v, ptype); //, "array_cast_0size");
        }

        Assert(Type::Equal(toTarget, fromTarget) ||
               Type::Equal(toTarget, fromTarget->GetAsConstType()));
        return expr->GetValue(ctx);
    }

    const StructType *toStruct = dynamic_cast<const StructType *>(toType);
    const StructType *fromStruct = dynamic_cast<const StructType *>(fromType);
    if (toStruct && fromStruct) {
        // The only legal type conversions for structs are to go from a
        // uniform to a varying instance of the same struct type.
        Assert(toStruct->IsVaryingType() && fromStruct->IsUniformType() &&
               Type::Equal(toStruct, fromStruct->GetAsVaryingType()));

        llvm::Value *origValue = expr->GetValue(ctx);
        if (!origValue)
            return NULL;
        return lUniformValueToVarying(ctx, origValue, fromType);
    }

    const VectorType *toVector = dynamic_cast<const VectorType *>(toType);
    const VectorType *fromVector = dynamic_cast<const VectorType *>(fromType);
    if (toVector && fromVector) {
        // this should be caught during typechecking
        Assert(toVector->GetElementCount() == fromVector->GetElementCount());

        llvm::Value *exprVal = expr->GetValue(ctx);
        if (!exprVal)
            return NULL;

        // Emit instructions to do type conversion of each of the elements
        // of the vector.
        // FIXME: since uniform vectors are represented as
        // llvm::VectorTypes, we should just be able to issue the
        // corresponding vector type convert, which should be more
        // efficient by avoiding serialization!
        llvm::Value *cast = llvm::UndefValue::get(toType->LLVMType(g->ctx));
        for (int i = 0; i < toVector->GetElementCount(); ++i) {
            llvm::Value *ei = ctx->ExtractInst(exprVal, i);

            llvm::Value *conv = lTypeConvAtomic(ctx, ei, toVector->GetElementType(),
                                                fromVector->GetElementType(), pos);
            if (!conv) 
                return NULL;
            cast = ctx->InsertInst(cast, conv, i);
        }
        return cast;
    }

    llvm::Value *exprVal = expr->GetValue(ctx);
    if (!exprVal)
        return NULL;

    const EnumType *fromEnum = dynamic_cast<const EnumType *>(fromType);
    const EnumType *toEnum = dynamic_cast<const EnumType *>(toType);
    if (fromEnum)
        // treat it as an uint32 type for the below and all will be good.
        fromType = fromEnum->IsUniformType() ? AtomicType::UniformUInt32 :
            AtomicType::VaryingUInt32;
    if (toEnum)
        // treat it as an uint32 type for the below and all will be good.
        toType = toEnum->IsUniformType() ? AtomicType::UniformUInt32 :
            AtomicType::VaryingUInt32;

    const AtomicType *fromAtomic = dynamic_cast<const AtomicType *>(fromType);
    // at this point, coming from an atomic type is all that's left...
    Assert(fromAtomic != NULL);

    if (toVector) {
        // scalar -> short vector conversion
        llvm::Value *conv = lTypeConvAtomic(ctx, exprVal, toVector->GetElementType(),
                                            fromAtomic, pos);
        if (!conv) 
            return NULL;

        llvm::Value *cast = llvm::UndefValue::get(toType->LLVMType(g->ctx));
        for (int i = 0; i < toVector->GetElementCount(); ++i)
            cast = ctx->InsertInst(cast, conv, i);
        return cast;
    }
    else if (toPointerType != NULL) {
        // int -> ptr
        if (toType->IsVaryingType() && fromType->IsUniformType())
            exprVal = ctx->SmearUniform(exprVal);

        LLVM_TYPE_CONST llvm::Type *llvmToType = toType->LLVMType(g->ctx);
        if (llvmToType == NULL)
            return NULL;

        return ctx->IntToPtrInst(exprVal, llvmToType, "int_to_ptr");
    }
    else {
        const AtomicType *toAtomic = dynamic_cast<const AtomicType *>(toType);
        // typechecking should ensure this is the case
        Assert(toAtomic != NULL);

        return lTypeConvAtomic(ctx, exprVal, toAtomic, fromAtomic, pos);
    }
}


const Type *
TypeCastExpr::GetType() const { 
    return type; 
}


static const Type *
lDeconstifyType(const Type *t) {
    const PointerType *pt = dynamic_cast<const PointerType *>(t);
    if (pt != NULL)
        return new PointerType(lDeconstifyType(pt->GetBaseType()), 
                               pt->IsUniformType(), false);
    else
        return t->GetAsNonConstType();
}


Expr *
TypeCastExpr::TypeCheck() {
    if (expr == NULL)
        return NULL;

    const Type *toType = GetType(), *fromType = expr->GetType();
    if (toType == NULL || fromType == NULL)
        return NULL;

    if (preserveUniformity == true && fromType->IsUniformType() &&
        toType->IsVaryingType()) {
        TypeCastExpr *tce = new TypeCastExpr(toType->GetAsUniformType(),
                                             expr, false, pos);
        return ::TypeCheck(tce);
    }

    fromType = lDeconstifyType(fromType);
    toType = lDeconstifyType(toType);

    if (fromType->IsVaryingType() && toType->IsUniformType()) {
        Error(pos, "Can't type cast from varying type \"%s\" to uniform "
              "type \"%s\"", fromType->GetString().c_str(),
              toType->GetString().c_str());
        return NULL;
    }

    // First some special cases that we allow only with an explicit type cast
    const PointerType *fromPtr = dynamic_cast<const PointerType *>(fromType);
    const PointerType *toPtr = dynamic_cast<const PointerType *>(toType);
    if (fromPtr != NULL && toPtr != NULL)
        // allow explicit typecasts between any two different pointer types
        return this;

    const AtomicType *fromAtomic = dynamic_cast<const AtomicType *>(fromType);
    const AtomicType *toAtomic = dynamic_cast<const AtomicType *>(toType);
    const EnumType *fromEnum = dynamic_cast<const EnumType *>(fromType);
    const EnumType *toEnum = dynamic_cast<const EnumType *>(toType);
    if ((fromAtomic || fromEnum) && (toAtomic || toEnum))
        // Allow explicit casts between all of these
        return this;

    // ptr -> int type casts
    if (fromPtr != NULL && toAtomic != NULL && toAtomic->IsIntType()) {
        bool safeCast = (toAtomic->basicType == AtomicType::TYPE_INT64 ||
                         toAtomic->basicType == AtomicType::TYPE_UINT64);
        if (g->target.is32Bit)
            safeCast |= (toAtomic->basicType == AtomicType::TYPE_INT32 ||
                         toAtomic->basicType == AtomicType::TYPE_UINT32);
        if (safeCast == false)
            Warning(pos, "Pointer type cast of type \"%s\" to integer type "
                    "\"%s\" may lose information.", 
                    fromType->GetString().c_str(), 
                    toType->GetString().c_str());
        return this;
    }

    // int -> ptr
    if (fromAtomic != NULL && fromAtomic->IsIntType() && toPtr != NULL)
        return this;

    // And otherwise see if it's one of the conversions allowed to happen
    // implicitly.
    if (CanConvertTypes(fromType, toType, "type cast expression", pos) == false)
        return NULL;

    return this;
}


Expr *
TypeCastExpr::Optimize() {
    ConstExpr *constExpr = dynamic_cast<ConstExpr *>(expr);
    if (constExpr == NULL)
        // We can't do anything if this isn't a const expr
        return this;

    const Type *toType = GetType();
    const AtomicType *toAtomic = dynamic_cast<const AtomicType *>(toType);
    const EnumType *toEnum = dynamic_cast<const EnumType *>(toType);
    // If we're not casting to an atomic or enum type, we can't do anything
    // here, since ConstExprs can only represent those two types.  (So
    // e.g. we're casting from an int to an int<4>.)
    if (toAtomic == NULL && toEnum == NULL)
        return this;

    bool forceVarying = toType->IsVaryingType();

    // All of the type conversion smarts we need is already in the
    // ConstExpr::AsBool(), etc., methods, so we just need to call the
    // appropriate one for the type that this cast is converting to.
    AtomicType::BasicType basicType = toAtomic ? toAtomic->basicType :
        AtomicType::TYPE_UINT32;
    switch (basicType) {
    case AtomicType::TYPE_BOOL: {
        bool bv[ISPC_MAX_NVEC];
        constExpr->AsBool(bv, forceVarying);
        return new ConstExpr(toType, bv, pos);
    }
    case AtomicType::TYPE_INT8: {
        int8_t iv[ISPC_MAX_NVEC];
        constExpr->AsInt8(iv, forceVarying);
        return new ConstExpr(toType, iv, pos);
    }
    case AtomicType::TYPE_UINT8: {
        uint8_t uv[ISPC_MAX_NVEC];
        constExpr->AsUInt8(uv, forceVarying);
        return new ConstExpr(toType, uv, pos);
    }
    case AtomicType::TYPE_INT16: {
        int16_t iv[ISPC_MAX_NVEC];
        constExpr->AsInt16(iv, forceVarying);
        return new ConstExpr(toType, iv, pos);
    }
    case AtomicType::TYPE_UINT16: {
        uint16_t uv[ISPC_MAX_NVEC];
        constExpr->AsUInt16(uv, forceVarying);
        return new ConstExpr(toType, uv, pos);
    }
    case AtomicType::TYPE_INT32: {
        int32_t iv[ISPC_MAX_NVEC];
        constExpr->AsInt32(iv, forceVarying);
        return new ConstExpr(toType, iv, pos);
    }
    case AtomicType::TYPE_UINT32: {
        uint32_t uv[ISPC_MAX_NVEC];
        constExpr->AsUInt32(uv, forceVarying);
        return new ConstExpr(toType, uv, pos);
    }
    case AtomicType::TYPE_FLOAT: {
        float fv[ISPC_MAX_NVEC];
        constExpr->AsFloat(fv, forceVarying);
        return new ConstExpr(toType, fv, pos);
    }
    case AtomicType::TYPE_INT64: {
        int64_t iv[ISPC_MAX_NVEC];
        constExpr->AsInt64(iv, forceVarying);
        return new ConstExpr(toType, iv, pos);
    }
    case AtomicType::TYPE_UINT64: {
        uint64_t uv[ISPC_MAX_NVEC];
        constExpr->AsUInt64(uv, forceVarying);
        return new ConstExpr(toType, uv, pos);
    }
    case AtomicType::TYPE_DOUBLE: {
        double dv[ISPC_MAX_NVEC];
        constExpr->AsDouble(dv, forceVarying);
        return new ConstExpr(toType, dv, pos);
    }
    default:
        FATAL("unimplemented");
    }
    return this;
}


int
TypeCastExpr::EstimateCost() const {
    // FIXME: return COST_TYPECAST_COMPLEX when appropriate
    return COST_TYPECAST_SIMPLE;
}


void
TypeCastExpr::Print() const {
    printf("[%s] type cast (", GetType()->GetString().c_str());
    expr->Print();
    printf(")");
    pos.Print();
}


Symbol *
TypeCastExpr::GetBaseSymbol() const {
    return expr ? expr->GetBaseSymbol() : NULL;
}


llvm::Constant *
TypeCastExpr::GetConstant(const Type *constType) const {
    // We don't need to worry about most the basic cases where the type
    // cast can resolve to a constant here, since the
    // TypeCastExpr::Optimize() method ends up doing the type conversion
    // and returning a ConstExpr, which in turn will have its GetConstant()
    // method called.  Thus, the only case we do need to worry about here
    // is converting a uniform function pointer to a varying function
    // pointer of the same type.
    Assert(Type::Equal(constType, type));
    const FunctionType *ft = NULL;
    if (dynamic_cast<const PointerType *>(type) == NULL ||
        (ft = dynamic_cast<const FunctionType *>(type->GetBaseType())) == NULL)
        return NULL;

    llvm::Constant *ec = expr->GetConstant(expr->GetType());
    if (ec == NULL)
        return NULL;

    ec = llvm::ConstantExpr::getPtrToInt(ec, LLVMTypes::PointerIntType);

    Assert(type->IsVaryingType());
    std::vector<llvm::Constant *> smear;
    for (int i = 0; i < g->target.vectorWidth; ++i)
        smear.push_back(ec);
    return llvm::ConstantVector::get(smear);
}


///////////////////////////////////////////////////////////////////////////
// ReferenceExpr

ReferenceExpr::ReferenceExpr(Expr *e, SourcePos p)
    : Expr(p) {
    expr = e;
}


llvm::Value *
ReferenceExpr::GetValue(FunctionEmitContext *ctx) const {
    ctx->SetDebugPos(pos);
    return expr ? expr->GetLValue(ctx) : NULL;
}


Symbol *
ReferenceExpr::GetBaseSymbol() const {
    return expr ? expr->GetBaseSymbol() : NULL;
}


const Type *
ReferenceExpr::GetType() const {
    if (!expr) 
        return NULL;

    const Type *type = expr->GetType();
    if (!type) 
        return NULL;

    return new ReferenceType(type);
}


const Type *
ReferenceExpr::GetLValueType() const {
    if (!expr) 
        return NULL;

    const Type *type = expr->GetType();
    if (!type) 
        return NULL;

    return PointerType::GetUniform(type);
}


Expr *
ReferenceExpr::Optimize() {
    if (expr == NULL)
        return NULL;
    return this;
}


Expr *
ReferenceExpr::TypeCheck() {
    if (expr == NULL)
        return NULL;
    return this;
}


int
ReferenceExpr::EstimateCost() const {
    return 0;
}


void
ReferenceExpr::Print() const {
    if (expr == NULL || GetType() == NULL)
        return;

    printf("[%s] &(", GetType()->GetString().c_str());
    expr->Print();
    printf(")");
    pos.Print();
}


///////////////////////////////////////////////////////////////////////////
// DereferenceExpr

DereferenceExpr::DereferenceExpr(Expr *e, SourcePos p)
    : Expr(p) {
    expr = e;
}


llvm::Value *
DereferenceExpr::GetValue(FunctionEmitContext *ctx) const {
    if (expr == NULL) 
        return NULL;
    llvm::Value *ptr = expr->GetValue(ctx);
    if (ptr == NULL)
        return NULL;
    const Type *type = expr->GetType();
    if (type == NULL)
        return NULL;

    Symbol *baseSym = expr->GetBaseSymbol();
    llvm::Value *mask = baseSym ? lMaskForSymbol(baseSym, ctx) : 
        ctx->GetFullMask();

    ctx->SetDebugPos(pos);
    return ctx->LoadInst(ptr, mask, type, "deref_load");
}


llvm::Value *
DereferenceExpr::GetLValue(FunctionEmitContext *ctx) const {
    if (expr == NULL) 
        return NULL;
    return expr->GetValue(ctx);
}


const Type *
DereferenceExpr::GetLValueType() const {
    if (expr == NULL)
        return NULL;
    return expr->GetType();
}


Symbol *
DereferenceExpr::GetBaseSymbol() const {
    return expr ? expr->GetBaseSymbol() : NULL;
}


const Type *
DereferenceExpr::GetType() const {
    if (expr == NULL)
        return NULL;
    const Type *exprType = expr->GetType();
    if (exprType == NULL)
        return NULL;
    if (dynamic_cast<const ReferenceType *>(exprType) != NULL)
        return exprType->GetReferenceTarget();
    else {
        Assert(dynamic_cast<const PointerType *>(exprType) != NULL);
        if (exprType->IsUniformType())
            return exprType->GetBaseType();
        else
            return exprType->GetBaseType()->GetAsVaryingType();
    }
}


Expr *
DereferenceExpr::TypeCheck() {
    if (expr == NULL)
        return NULL;
    return this;
}


Expr *
DereferenceExpr::Optimize() {
    if (expr == NULL)
        return NULL;
    return this;
}


int
DereferenceExpr::EstimateCost() const {
    if (expr == NULL)
        return 0;

    const Type *exprType = expr->GetType();
    if (dynamic_cast<const PointerType *>(exprType) &&
        exprType->IsVaryingType())
        // Be pessimistic; some of these will later be optimized into
        // vector loads/stores..
        return COST_GATHER + COST_DEREF;
    else
        return COST_DEREF;
}


void
DereferenceExpr::Print() const {
    if (expr == NULL || GetType() == NULL)
        return;

    printf("[%s] *(", GetType()->GetString().c_str());
    expr->Print();
    printf(")");
    pos.Print();
}


///////////////////////////////////////////////////////////////////////////
// AddressOfExpr

AddressOfExpr::AddressOfExpr(Expr *e, SourcePos p)
    : Expr(p), expr(e) {
}


llvm::Value *
AddressOfExpr::GetValue(FunctionEmitContext *ctx) const {
    ctx->SetDebugPos(pos);
    if (expr == NULL)
        return NULL;

    const Type *exprType = expr->GetType();
    if (dynamic_cast<const ReferenceType *>(exprType) != NULL)
        return expr->GetValue(ctx);
    else
        return expr->GetLValue(ctx);
}


const Type *
AddressOfExpr::GetType() const {
    if (expr == NULL)
        return NULL;

    const Type *exprType = expr->GetType();
    if (dynamic_cast<const ReferenceType *>(exprType) != NULL)
        return PointerType::GetUniform(exprType->GetReferenceTarget());
    else
        return expr->GetLValueType();
}


Symbol *
AddressOfExpr::GetBaseSymbol() const {
    return expr ? expr->GetBaseSymbol() : NULL;
}


void
AddressOfExpr::Print() const {
    printf("&(");
    if (expr)
        expr->Print();
    else
        printf("NULL expr");
    printf(")");
    pos.Print();
}


Expr *
AddressOfExpr::TypeCheck() {
    return this;
}


Expr *
AddressOfExpr::Optimize() {
    return this;
}


int
AddressOfExpr::EstimateCost() const {
    return 0;
}


///////////////////////////////////////////////////////////////////////////
// SizeOfExpr

SizeOfExpr::SizeOfExpr(Expr *e, SourcePos p) 
    : Expr(p), expr(e), type(NULL) {
}


SizeOfExpr::SizeOfExpr(const Type *t, SourcePos p)
    : Expr(p), expr(NULL), type(t) {
}


llvm::Value *
SizeOfExpr::GetValue(FunctionEmitContext *ctx) const {
    ctx->SetDebugPos(pos);
    const Type *t = expr ? expr->GetType() : type;
    if (t == NULL)
        return NULL;

    LLVM_TYPE_CONST llvm::Type *llvmType = t->LLVMType(g->ctx);
    if (llvmType == NULL)
        return NULL;

    return g->target.SizeOf(llvmType);
}


const Type *
SizeOfExpr::GetType() const {
    return (g->target.is32Bit || g->opt.force32BitAddressing) ? 
        AtomicType::UniformUInt32 : AtomicType::UniformUInt64;
}


void
SizeOfExpr::Print() const {
    printf("Sizeof (");
    if (expr != NULL) 
        expr->Print();
    const Type *t = expr ? expr->GetType() : type;
    if (t != NULL)
        printf(" [type %s]", t->GetString().c_str());
    printf(")");
    pos.Print();
}


Expr *
SizeOfExpr::TypeCheck() {
    return this;
}


Expr *
SizeOfExpr::Optimize() {
    return this;
}


int
SizeOfExpr::EstimateCost() const {
    return 0;
}

///////////////////////////////////////////////////////////////////////////
// SymbolExpr

SymbolExpr::SymbolExpr(Symbol *s, SourcePos p) 
  : Expr(p) {
    symbol = s;
}


llvm::Value *
SymbolExpr::GetValue(FunctionEmitContext *ctx) const {
    // storagePtr may be NULL due to an earlier compilation error
    if (!symbol || !symbol->storagePtr)
        return NULL;
    ctx->SetDebugPos(pos);
    return ctx->LoadInst(symbol->storagePtr, symbol->name.c_str());
}


llvm::Value *
SymbolExpr::GetLValue(FunctionEmitContext *ctx) const {
    if (symbol == NULL)
        return NULL;
    ctx->SetDebugPos(pos);
    return symbol->storagePtr;
}


const Type *
SymbolExpr::GetLValueType() const {
    if (symbol == NULL)
        return NULL;

    return PointerType::GetUniform(symbol->type);
}


Symbol *
SymbolExpr::GetBaseSymbol() const {
    return symbol;
}


const Type *
SymbolExpr::GetType() const { 
    return symbol ? symbol->type : NULL;
}


Expr *
SymbolExpr::TypeCheck() {
    return this;
}


Expr *
SymbolExpr::Optimize() {
    if (symbol == NULL)
        return NULL;
    else if (symbol->constValue != NULL) {
        Assert(GetType()->IsConstType());
        return symbol->constValue;
    }
    else
        return this;
}


int
SymbolExpr::EstimateCost() const {
    // Be optimistic and assume it's in a register or can be used as a
    // memory operand..
    return 0;
}


void
SymbolExpr::Print() const {
    if (symbol == NULL || GetType() == NULL)
        return;

    printf("[%s] sym: (%s)", GetType()->GetString().c_str(), 
           symbol->name.c_str());
    pos.Print();
}


///////////////////////////////////////////////////////////////////////////
// FunctionSymbolExpr

FunctionSymbolExpr::FunctionSymbolExpr(const char *n,
                                       const std::vector<Symbol *> &candidates,
                                       SourcePos p) 
  : Expr(p) {
    name = n;
    candidateFunctions = candidates;
    matchingFunc = (candidates.size() == 1) ? candidates[0] : NULL;
    triedToResolve = false;
}


const Type *
FunctionSymbolExpr::GetType() const {
    if (triedToResolve == false && matchingFunc == NULL) {
        Error(pos, "Ambiguous use of overloaded function \"%s\".", 
              name.c_str());
        return NULL;
    }

    return matchingFunc ? new PointerType(matchingFunc->type, true, true) : NULL;
}


llvm::Value *
FunctionSymbolExpr::GetValue(FunctionEmitContext *ctx) const {
    return matchingFunc ? matchingFunc->function : NULL;
}


Symbol *
FunctionSymbolExpr::GetBaseSymbol() const {
    return matchingFunc;
}


Expr *
FunctionSymbolExpr::TypeCheck() {
    return this;
}


Expr *
FunctionSymbolExpr::Optimize() {
    return this;
}


int
FunctionSymbolExpr::EstimateCost() const {
    return 0;
}


void
FunctionSymbolExpr::Print() const {
    if (!matchingFunc || !GetType())
        return;

    printf("[%s] fun sym (%s)", GetType()->GetString().c_str(),
           matchingFunc->name.c_str());
    pos.Print();
}


llvm::Constant *
FunctionSymbolExpr::GetConstant(const Type *type) const {
    Assert(type->IsUniformType());
    Assert(GetType()->IsUniformType());

    if (Type::EqualIgnoringConst(type, GetType()) == false)
        return NULL;

    return matchingFunc ? matchingFunc->function : NULL;
}


static void
lPrintOverloadCandidates(SourcePos pos, const std::vector<Symbol *> &funcs, 
                         const std::vector<const Type *> &argTypes, 
                         const std::vector<bool> *argCouldBeNULL) {
    for (unsigned int i = 0; i < funcs.size(); ++i)
        Error(funcs[i]->pos, "Candidate function:");

    std::string passedTypes = "Passed types: (";
    for (unsigned int i = 0; i < argTypes.size(); ++i) {
        if (argTypes[i] != NULL)
            passedTypes += argTypes[i]->GetString();
        else
            passedTypes += "(unknown type)";
        passedTypes += (i < argTypes.size()-1) ? ", " : ")\n\n";
    }
    Error(pos, "%s", passedTypes.c_str());
}

             
/** Helper function used for function overload resolution: returns zero
    cost if the call argument's type exactly matches the function argument
    type (modulo a conversion to a const type if needed), otherwise reports
    failure.
 */ 
static int
lExactMatch(const Type *callType, const Type *funcArgType) {
    if (dynamic_cast<const ReferenceType *>(callType) == NULL)
        callType = callType->GetAsNonConstType();
    if (dynamic_cast<const ReferenceType *>(funcArgType) != NULL && 
        dynamic_cast<const ReferenceType *>(callType) == NULL)
        callType = new ReferenceType(callType);

    return Type::Equal(callType, funcArgType) ? 0 : -1;
}


/** Helper function used for function overload resolution: returns a cost
    of 1 if the call argument type and the function argument type match,
    modulo conversion to a reference type if needed.
 */
static int
lMatchIgnoringReferences(const Type *callType, const Type *funcArgType) {
    int prev = lExactMatch(callType, funcArgType);
    if (prev != -1)
        return prev;

    callType = callType->GetReferenceTarget();
    if (funcArgType->IsConstType())
        callType = callType->GetAsConstType();

    return Type::Equal(callType,
                       funcArgType->GetReferenceTarget()) ? 1 : -1;
}

/** Helper function used for function overload resolution: returns a cost
    of 1 if converting the argument to the call type only requires a type
    conversion that won't lose information.  Otherwise reports failure.
*/
static int
lMatchWithTypeWidening(const Type *callType, const Type *funcArgType) {
    int prev = lMatchIgnoringReferences(callType, funcArgType);
    if (prev != -1)
        return prev;

    const AtomicType *callAt = dynamic_cast<const AtomicType *>(callType);
    const AtomicType *funcAt = dynamic_cast<const AtomicType *>(funcArgType);
    if (callAt == NULL || funcAt == NULL)
        return -1;

    if (callAt->IsUniformType() != funcAt->IsUniformType())
        return -1;

    switch (callAt->basicType) {
    case AtomicType::TYPE_BOOL:
        return 1;
    case AtomicType::TYPE_INT8:
    case AtomicType::TYPE_UINT8:
        return (funcAt->basicType != AtomicType::TYPE_BOOL) ? 1 : -1;
    case AtomicType::TYPE_INT16:
    case AtomicType::TYPE_UINT16:
        return (funcAt->basicType != AtomicType::TYPE_BOOL &&
                funcAt->basicType != AtomicType::TYPE_INT8 &&
                funcAt->basicType != AtomicType::TYPE_UINT8) ? 1 : -1;
    case AtomicType::TYPE_INT32:
    case AtomicType::TYPE_UINT32:
        return (funcAt->basicType == AtomicType::TYPE_INT32 ||
                funcAt->basicType == AtomicType::TYPE_UINT32 ||
                funcAt->basicType == AtomicType::TYPE_INT64 ||
                funcAt->basicType == AtomicType::TYPE_UINT64) ? 1 : -1;
    case AtomicType::TYPE_FLOAT:
        return (funcAt->basicType == AtomicType::TYPE_DOUBLE) ? 1 : -1;
    case AtomicType::TYPE_INT64:
    case AtomicType::TYPE_UINT64:
        return (funcAt->basicType == AtomicType::TYPE_INT64 ||
                funcAt->basicType == AtomicType::TYPE_UINT64) ? 1 : -1;
    case AtomicType::TYPE_DOUBLE:
        return -1;
    default:
        FATAL("Unhandled atomic type");
        return -1;
    }
}


/** Helper function used for function overload resolution: returns a cost
    of 1 if the call argument type and the function argument type match if
    we only do a uniform -> varying type conversion but otherwise have
    exactly the same type.
 */
static int
lMatchIgnoringUniform(const Type *callType, const Type *funcArgType) {
    int prev = lMatchWithTypeWidening(callType, funcArgType);
    if (prev != -1)
        return prev;

    if (dynamic_cast<const ReferenceType *>(callType) == NULL)
        callType = callType->GetAsNonConstType();

    return (callType->IsUniformType() && 
            funcArgType->IsVaryingType() &&
            Type::Equal(callType->GetAsVaryingType(), funcArgType)) ? 1 : -1;
}


/** Helper function used for function overload resolution: returns a cost
    of 1 if we can type convert from the call argument type to the function
    argument type, but without doing a uniform -> varying conversion.
 */
static int
lMatchWithTypeConvSameVariability(const Type *callType,
                                  const Type *funcArgType) {
    int prev = lMatchIgnoringUniform(callType, funcArgType);
    if (prev != -1)
        return prev;

    if (CanConvertTypes(callType, funcArgType) &&
        (callType->IsUniformType() == funcArgType->IsUniformType()))
        return 1;
    else
        return -1;
}


/** Helper function used for function overload resolution: returns a cost
    of 1 if there is any type conversion that gets us from the caller
    argument type to the function argument type.
 */
static int
lMatchWithTypeConv(const Type *callType, const Type *funcArgType) {
    int prev = lMatchWithTypeConvSameVariability(callType, funcArgType);
    if (prev != -1)
        return prev;
        
    return CanConvertTypes(callType, funcArgType) ? 0 : -1;
}


/** Given a set of potential matching functions and their associated cost,
    return the one with the lowest cost, if unique.  Otherwise, if multiple
    functions match with the same cost, return NULL.
 */
static Symbol *
lGetBestMatch(std::vector<std::pair<int, Symbol *> > &matches) {
    Assert(matches.size() > 0);
    int minCost = matches[0].first;

    for (unsigned int i = 1; i < matches.size(); ++i)
        minCost = std::min(minCost, matches[i].first);

    Symbol *match = NULL;
    for (unsigned int i = 0; i < matches.size(); ++i) {
        if (matches[i].first == minCost) {
            if (match != NULL)
                // multiple things had the same cost
                return NULL;
            else
                match = matches[i].second;
        }
    }
    return match;
}


/** See if we can find a single function from the set of overload options
    based on the predicate function passed in.  Returns true if no more
    tries should be made to find a match, either due to success from
    finding a single overloaded function that matches or failure due to
    finding multiple ambiguous matches.
 */
bool
FunctionSymbolExpr::tryResolve(int (*matchFunc)(const Type *, const Type *),
                               SourcePos argPos,
                               const std::vector<const Type *> &callTypes,
                               const std::vector<bool> *argCouldBeNULL) {
    const char *funName = candidateFunctions.front()->name.c_str();

    std::vector<std::pair<int, Symbol *> > matches;
    std::vector<Symbol *>::iterator iter;
    for (iter = candidateFunctions.begin(); 
         iter != candidateFunctions.end(); ++iter) {
        // Loop over the set of candidate functions and try each one
        Symbol *candidateFunction = *iter;
        const FunctionType *ft = 
            dynamic_cast<const FunctionType *>(candidateFunction->type);
        Assert(ft != NULL);

        // There's no way to match if the caller is passing more arguments
        // than this function instance takes.
        if ((int)callTypes.size() > ft->GetNumParameters())
            continue;

        int i;
        // Note that we're looping over the caller arguments, not the
        // function arguments; it may be ok to have more arguments to the
        // function than are passed, if the function has default argument
        // values.  This case is handled below.
        int cost = 0;
        for (i = 0; i < (int)callTypes.size(); ++i) {
            // This may happen if there's an error earlier in compilation.
            // It's kind of a silly to redundantly discover this for each
            // potential match versus detecting this earlier in the
            // matching process and just giving up.
            const Type *paramType = ft->GetParameterType(i);

            if (callTypes[i] == NULL || paramType == NULL ||
                dynamic_cast<const FunctionType *>(callTypes[i]) != NULL)
                return false;

            int argCost = matchFunc(callTypes[i], paramType);
            if (argCost == -1) {
                if (argCouldBeNULL != NULL && (*argCouldBeNULL)[i] == true &&
                    dynamic_cast<const PointerType *>(paramType) != NULL)
                    // If the passed argument value is zero and this is a
                    // pointer type, then it can convert to a NULL value of
                    // that pointer type.
                    argCost = 0;
                else
                    // If the predicate function returns -1, we have failed no
                    // matter what else happens, so we stop trying
                    break;
            }
            cost += argCost;
        }
        if (i == (int)callTypes.size()) {
            // All of the arguments matched!
            if (i == ft->GetNumParameters())
                // And we have exactly as many arguments as the function
                // wants, so we're done.
                matches.push_back(std::make_pair(cost, candidateFunction));
            else if (i < ft->GetNumParameters() && 
                     ft->GetParameterDefault(i) != NULL)
                // Otherwise we can still make it if there are default
                // arguments for the rest of the arguments!  Because in
                // Module::AddFunction() we have verified that once the
                // default arguments start, then all of the following ones
                // have them as well.  Therefore, we just need to check if
                // the arg we stopped at has a default value and we're
                // done.
                matches.push_back(std::make_pair(cost, candidateFunction));
            // otherwise, we don't have a match
        }
    }

    if (matches.size() == 0)
        return false;
    else if ((matchingFunc = lGetBestMatch(matches)) != NULL)
        // We have a match!
        return true;
    else {
        Error(pos, "Multiple overloaded instances of function \"%s\" matched.",
              funName);

        // select the matches that have the lowest cost
        std::vector<Symbol *> bestMatches;
        int minCost = matches[0].first;
        for (unsigned int i = 1; i < matches.size(); ++i)
            minCost = std::min(minCost, matches[i].first);
        for (unsigned int i = 0; i < matches.size(); ++i)
            if (matches[i].first == minCost)
                bestMatches.push_back(matches[i].second);

        // And print a useful error message
        lPrintOverloadCandidates(argPos, bestMatches, callTypes, argCouldBeNULL);

        // Stop trying to find more matches after an ambigious set of
        // matches.
        return true;
    }
}


bool
FunctionSymbolExpr::ResolveOverloads(SourcePos argPos,
                                     const std::vector<const Type *> &argTypes,
                                     const std::vector<bool> *argCouldBeNULL) {
    triedToResolve = true;

    // Functions with names that start with "__" should only be various
    // builtins.  For those, we'll demand an exact match, since we'll
    // expect whichever function in stdlib.ispc is calling out to one of
    // those to be matching the argument types exactly; this is to be a bit
    // extra safe to be sure that the expected builtin is in fact being
    // called.
    bool exactMatchOnly = (name.substr(0,2) == "__");

    // Is there an exact match that doesn't require any argument type
    // conversion (other than converting type -> reference type)?
    if (tryResolve(lExactMatch, argPos, argTypes, argCouldBeNULL))
        return true;

    if (exactMatchOnly == false) {
        // Try to find a single match ignoring references
        if (tryResolve(lMatchIgnoringReferences, argPos, argTypes, 
                       argCouldBeNULL))
            return true;

        // Try to find an exact match via type widening--i.e. int8 ->
        // int16, etc.--things that don't lose data.
        if (tryResolve(lMatchWithTypeWidening, argPos, argTypes, argCouldBeNULL))
            return true;

        // Next try to see if there's a match via just uniform -> varying
        // promotions.
        if (tryResolve(lMatchIgnoringUniform, argPos, argTypes, argCouldBeNULL))
            return true;

        // Try to find a match via type conversion, but don't change
        // unif->varying
        if (tryResolve(lMatchWithTypeConvSameVariability, argPos, argTypes,
                       argCouldBeNULL))
            return true;
    
        // Last chance: try to find a match via arbitrary type conversion.
        if (tryResolve(lMatchWithTypeConv, argPos, argTypes, argCouldBeNULL))
            return true;
    }

    // failure :-(
    const char *funName = candidateFunctions.front()->name.c_str();
    Error(pos, "Unable to find matching overload for call to function \"%s\"%s.",
          funName, exactMatchOnly ? " only considering exact matches" : "");
    lPrintOverloadCandidates(argPos, candidateFunctions, argTypes, 
                             argCouldBeNULL);
    return false;
}


Symbol *
FunctionSymbolExpr::GetMatchingFunction() {
    return matchingFunc;
}


///////////////////////////////////////////////////////////////////////////
// SyncExpr

const Type *
SyncExpr::GetType() const {
    return AtomicType::Void;
}


llvm::Value *
SyncExpr::GetValue(FunctionEmitContext *ctx) const {
    ctx->SetDebugPos(pos);
    ctx->SyncInst();
    return NULL;
}


int
SyncExpr::EstimateCost() const {
    return COST_SYNC;
}


void
SyncExpr::Print() const {
    printf("sync");
    pos.Print();
}


Expr *
SyncExpr::TypeCheck() {
    return this;
}


Expr *
SyncExpr::Optimize() {
    return this;
}


///////////////////////////////////////////////////////////////////////////
// NullPointerExpr

llvm::Value *
NullPointerExpr::GetValue(FunctionEmitContext *ctx) const {
    return llvm::ConstantPointerNull::get(LLVMTypes::VoidPointerType);
}


const Type *
NullPointerExpr::GetType() const {
    return PointerType::Void;
}


Expr *
NullPointerExpr::TypeCheck() {
    return this;
}


Expr *
NullPointerExpr::Optimize() {
    return this;
}


void
NullPointerExpr::Print() const {
    printf("NULL");
    pos.Print();
}


int
NullPointerExpr::EstimateCost() const {
    return 0;
}


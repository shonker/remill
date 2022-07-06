/*
 * Copyright (c) 2022-present Trail of Bits, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <glog/logging.h>
#include <remill/BC/ABI.h>
#include <remill/BC/IntrinsicTable.h>
#include <remill/BC/SleighLifter.h>
#include <remill/BC/Util.h>

#include <cassert>
#include <unordered_map>
#include <unordered_set>

#include "../Arch/Sleigh/Arch.h"

namespace remill {


class SleighLifter::PcodeToLLVMEmitIntoBlock : public PcodeEmit {
 private:
  class Parameter {
   public:
    virtual ~Parameter(void) = default;

    virtual std::optional<llvm::Value *> LiftAsInParam(llvm::IRBuilder<> &bldr,
                                                       llvm::Type *ty) = 0;

    virtual LiftStatus StoreIntoParam(llvm::IRBuilder<> &bldr,
                                      llvm::Value *inner_lifted) = 0;
  };


  using ParamPtr = std::shared_ptr<Parameter>;


  class RegisterValue : public Parameter {
   private:
    llvm::Value *register_pointer;

   public:
    // TODO(Ian): allow this to be fallible and have better error handling
    std::optional<llvm::Value *> LiftAsInParam(llvm::IRBuilder<> &bldr,
                                               llvm::Type *ty) override {
      return bldr.CreateLoad(ty, register_pointer);
    }

    LiftStatus StoreIntoParam(llvm::IRBuilder<> &bldr,
                              llvm::Value *inner_lifted) override {
      bldr.CreateStore(inner_lifted, register_pointer);
      return LiftStatus::kLiftedInstruction;
    }

   public:
    RegisterValue(llvm::Value *register_pointer)
        : register_pointer(register_pointer) {}

    static ParamPtr CreatRegister(llvm::Value *register_pointer) {
      return std::make_shared<RegisterValue>(register_pointer);
    }

    virtual ~RegisterValue() {}
  };


  class Memory : public Parameter {
   public:
    virtual ~Memory() {}
    Memory(llvm::Value *memory_ref_ptr, llvm::Value *index,
           const IntrinsicTable *intrinsics, llvm::Type *memory_ptr_type)
        : memory_ref_ptr(memory_ref_ptr),
          index(index),
          intrinsics(intrinsics),
          memory_ptr_type(memory_ptr_type) {}

    static ParamPtr CreateMemory(llvm::Value *memory_ref_ptr,
                                 llvm::Value *index,
                                 const IntrinsicTable *intrinsics,
                                 llvm::Type *memory_ptr_type) {
      return std::make_shared<Memory>(memory_ref_ptr, index, intrinsics,
                                      memory_ptr_type);
    }

   private:
    llvm::Value *memory_ref_ptr;
    llvm::Value *index;
    const IntrinsicTable *intrinsics;
    llvm::Type *memory_ptr_type;

    std::optional<llvm::Value *> LiftAsInParam(llvm::IRBuilder<> &bldr,
                                               llvm::Type *ty) override {
      auto mem = bldr.CreateLoad(this->memory_ptr_type, this->memory_ref_ptr);
      auto res = remill::LoadFromMemory(
          *this->intrinsics, bldr.GetInsertBlock(), ty, mem, this->index);
      if (res) {
        return res;
      } else {
        return std::nullopt;
      }
    }

    LiftStatus StoreIntoParam(llvm::IRBuilder<> &bldr,
                              llvm::Value *inner_lifted) override {
      auto mem = bldr.CreateLoad(this->memory_ptr_type, this->memory_ref_ptr);
      auto new_mem =
          remill::StoreToMemory(*this->intrinsics, bldr.GetInsertBlock(),
                                inner_lifted, mem, this->index);
      if (new_mem) {
        bldr.CreateStore(new_mem, this->memory_ref_ptr);
        return LiftStatus::kLiftedInstruction;
      } else {
        return LiftStatus::kLiftedInvalidInstruction;
      }
    }
  };

  class ConstantValue : public Parameter {
   private:
    llvm::Value *cst;

   public:
    std::optional<llvm::Value *> LiftAsInParam(llvm::IRBuilder<> &bldr,
                                               llvm::Type *ty) override {
      if (ty != cst->getType()) {
        return std::nullopt;
      }
      return this->cst;
    }

    LiftStatus StoreIntoParam(llvm::IRBuilder<> &bldr,
                              llvm::Value *inner_lifted) override {
      return LiftStatus::kLiftedUnsupportedInstruction;
    }

    ConstantValue(llvm::Value *cst) : cst(cst) {}

    static ParamPtr CreatConstant(llvm::Value *cst) {
      return std::make_shared<ConstantValue>(cst);
    }
    virtual ~ConstantValue() {}
  };


  llvm::BasicBlock *target_block;
  llvm::Value *state_pointer;
  llvm::LLVMContext &context;
  const Instruction &insn;
  LiftStatus status;
  SleighLifter &insn_lifter_parent;


  class UniqueRegSpace {
   private:
    std::unordered_map<uint64_t, llvm::Value *> cached_unique_ptrs;
    llvm::LLVMContext &context;

   public:
    UniqueRegSpace(llvm::LLVMContext &context) : context(context) {}

    llvm::Value *GetUniquePtr(uint64_t offset, uint64_t size,
                              llvm::IRBuilder<> &bldr) {
      if (this->cached_unique_ptrs.find(offset) !=
          this->cached_unique_ptrs.end()) {
        return this->cached_unique_ptrs.find(offset)->second;
      }
      auto ptr = bldr.CreateAlloca(
          llvm::IntegerType::get(this->context, 8 * size), 0, nullptr);
      this->cached_unique_ptrs.insert({offset, ptr});
      return ptr;
    }
  };

  class ConstantReplacementContext {
   private:
    std::map<uint64_t, ParamPtr> current_replacements;
    std::set<uint64_t> used_values;

   public:
    void ApplyEqualityClaim(llvm::IRBuilder<> &bldr,
                            SleighLifter::PcodeToLLVMEmitIntoBlock &lifter,
                            VarnodeData lhs_constant,
                            VarnodeData rhs_unfolded_value) {
      assert(lhs_constant.space->getIndex() ==
             lhs_constant.space->constant_space_index);
      this->current_replacements.insert(
          {lhs_constant.offset, lifter.LiftParamPtr(bldr, rhs_unfolded_value)});
    }


    void ApplyNonEqualityClaim() {
      this->current_replacements.clear();
      this->used_values.clear();
    }

    llvm::Value *LiftOffsetOrReplace(llvm::IRBuilder<> &bldr,
                                     VarnodeData target,
                                     llvm::Type *target_type) {
      if (this->current_replacements.find(target.offset) !=
          this->current_replacements.end()) {

        if (this->used_values.find(target.offset) != this->used_values.end()) {
          LOG(FATAL) << "Ambigous value substitution via claim eq: "
                     << target.offset;
        }
        auto replacement = this->current_replacements.find(target.offset)
                               ->second->LiftAsInParam(bldr, target_type);
        if (!replacement.has_value()) {
          LOG(FATAL) << "Failure to lift replacement value for: "
                     << target.offset << " as "
                     << remill::LLVMThingToString(target_type);
        }
        this->used_values.insert(target.offset);
        return *replacement;
      }

      return llvm::ConstantInt::get(target_type, target.offset);
    }
  };

  UniqueRegSpace uniques;
  UniqueRegSpace unknown_regs;

  ConstantReplacementContext replacement_cont;
  // Generic sleigh arch
  std::vector<std::string> user_op_names;

  void UpdateStatus(LiftStatus new_status, OpCode opc) {
    if (new_status != LiftStatus::kLiftedInstruction) {
      LOG(ERROR) << "Failed to lift insn with opcode: " << get_opname(opc);
      this->status = new_status;
    }
  }

 public:
  PcodeToLLVMEmitIntoBlock(llvm::BasicBlock *target_block,
                           llvm::Value *state_pointer, const Instruction &insn,
                           SleighLifter &insn_lifter_parent,
                           std::vector<std::string> user_op_names_)
      : target_block(target_block),
        state_pointer(state_pointer),
        context(target_block->getContext()),
        insn(insn),
        status(remill::LiftStatus::kLiftedInstruction),
        insn_lifter_parent(insn_lifter_parent),
        uniques(target_block->getContext()),
        unknown_regs(target_block->getContext()),
        user_op_names(user_op_names_) {}


  ParamPtr CreateMemoryAddress(llvm::Value *offset) {
    const auto mem_ptr_ref = this->insn_lifter_parent.LoadRegAddress(
        this->target_block, this->state_pointer, kMemoryVariableName);
    // compute pointer into memory at offset


    return Memory::CreateMemory(mem_ptr_ref.first, offset,
                                this->insn_lifter_parent.GetIntrinsicTable(),
                                this->insn_lifter_parent.GetMemoryType());
  }

  std::optional<ParamPtr> LiftNormalRegister(llvm::IRBuilder<> &bldr,
                                             std::string reg_name) {
    for (auto &c : reg_name)
      c = toupper(c);


    if (this->insn_lifter_parent.ArchHasRegByName(reg_name)) {
      // TODO(Ian): will probably need to adjust the pointer here in certain circumstances
      auto reg_ptr = this->insn_lifter_parent.LoadRegAddress(
          bldr.GetInsertBlock(), this->state_pointer, reg_name);
      return RegisterValue::CreatRegister(reg_ptr.first);
    } else {
      return std::nullopt;
    }
  }

  //TODO(Ian): Maybe this should be a failable function that returns an unsupported insn in certain failures
  // So the times we need to replace an offset via a context are 3 fold.
  // 1. in Branches where the offset is retrieved directly from the varnode. This isnt handled here.
  // 2. In ram offsets
  // 3. In constant offsets
  ParamPtr LiftParamPtr(llvm::IRBuilder<> &bldr, VarnodeData vnode) {
    auto space_name = vnode.getAddr().getSpace()->getName();
    if (space_name == "ram") {
      // compute pointer into memory at offset

      auto constant_offset = this->replacement_cont.LiftOffsetOrReplace(
          bldr, vnode, this->insn_lifter_parent.GetWordType());

      return this->CreateMemoryAddress(constant_offset);
    } else if (space_name == "register") {
      auto reg_name = this->insn_lifter_parent.GetEngine().getRegisterName(
          vnode.space, vnode.offset, vnode.size);

      LOG(INFO) << "Looking for reg name " << reg_name << " from offset "
                << vnode.offset;


      auto res = this->LiftNormalRegister(bldr, reg_name);
      if (res.has_value()) {
        return *res;
      } else {
        auto reg_ptr =
            this->unknown_regs.GetUniquePtr(vnode.offset, vnode.size, bldr);
        return RegisterValue::CreatRegister(reg_ptr);
      }

    } else if (space_name == "const") {

      auto cst_v = this->replacement_cont.LiftOffsetOrReplace(
          bldr, vnode, llvm::IntegerType::get(this->context, vnode.size * 8));

      return ConstantValue::CreatConstant(cst_v);
    } else if (space_name == "unique") {
      auto reg_ptr = this->uniques.GetUniquePtr(vnode.offset, vnode.size, bldr);
      return RegisterValue::CreatRegister(reg_ptr);
    } else {
      LOG(FATAL) << "Unhandled memory space: " << space_name;
    }
  }


  llvm::Value *FixResultForOutVarnode(llvm::IRBuilder<> &bldr,
                                      llvm::Value *orig, VarnodeData outvnode) {
    assert(orig->getType()->isIntegerTy());
    auto out_bits = outvnode.size * 8;
    if (out_bits == orig->getType()->getIntegerBitWidth()) {
      return orig;
    }

    auto target_ty = llvm::IntegerType::get(bldr.getContext(), out_bits);

    return bldr.CreateZExtOrTrunc(orig, target_ty);
  }

  std::optional<llvm::Value *> LiftInParam(llvm::IRBuilder<> &bldr,
                                           VarnodeData vnode, llvm::Type *ty) {
    ParamPtr ptr = this->LiftParamPtr(bldr, vnode);

    return ptr->LiftAsInParam(bldr, ty);
  }

  std::optional<llvm::Value *> LiftIntegerInParam(llvm::IRBuilder<> &bldr,
                                                  VarnodeData vnode) {
    return this->LiftInParam(
        bldr, vnode, llvm::IntegerType::get(this->context, vnode.size * 8));
  }

  LiftStatus
  LiftRequireOutParam(std::function<LiftStatus(VarnodeData)> inner_lift,
                      VarnodeData *outvar) {
    if (outvar) {
      return inner_lift(*outvar);
    } else {
      return LiftStatus::kLiftedUnsupportedInstruction;
    }
  }


  LiftStatus LiftStoreIntoOutParam(llvm::IRBuilder<> &bldr,
                                   llvm::Value *inner_lifted,
                                   VarnodeData *outvar) {
    return this->LiftRequireOutParam(
        [&bldr, this, inner_lifted](VarnodeData out_param_data) {
          auto ptr = this->LiftParamPtr(bldr, out_param_data);
          return ptr->StoreIntoParam(bldr, inner_lifted);
        },
        outvar);
  }

  LiftStatus LiftUnOpWithFloatIntrinsic(
      llvm::IRBuilder<> &bldr,
      llvm::Intrinsic::IndependentIntrinsics intrinsic_id, VarnodeData *outvar,
      VarnodeData input_var) {
    auto inval = this->LiftInParam(bldr, input_var,
                                   llvm::Type::getFloatTy(this->context));
    if (inval.has_value()) {
      llvm::Function *intrinsic = llvm::Intrinsic::getDeclaration(
          bldr.GetInsertBlock()->getModule(), intrinsic_id);
      llvm::Value *intrinsic_args[] = {*inval};
      return this->LiftStoreIntoOutParam(
          bldr, bldr.CreateCall(intrinsic, intrinsic_args), outvar);
    }
    return LiftStatus::kLiftedUnsupportedInstruction;
  }

  LiftStatus LiftUnOp(llvm::IRBuilder<> &bldr, OpCode opc, VarnodeData *outvar,
                      VarnodeData input_var) {
    // TODO(Ian): when we lift a param we need to specify the type we want


    switch (opc) {
      case OpCode::CPUI_BOOL_NEGATE: {
        auto bneg_inval = this->LiftInParam(
            bldr, input_var, llvm::IntegerType::get(this->context, 1));
        if (bneg_inval.has_value()) {
          return this->LiftStoreIntoOutParam(
              bldr,
              bldr.CreateZExt(bldr.CreateNot(*bneg_inval),
                              llvm::IntegerType::get(this->context, 8)),
              outvar);
        }
        break;
      }
      case OpCode::CPUI_COPY:
      case OpCode::CPUI_CAST: {
        auto copy_inval = this->LiftInParam(
            bldr, input_var,
            llvm::IntegerType::get(this->context, input_var.size * 8));
        if (copy_inval.has_value()) {
          return this->LiftStoreIntoOutParam(bldr, *copy_inval, outvar);
        }
        break;
      }

      case OpCode::CPUI_BRANCH:
      case OpCode::CPUI_CALL: {
        // directs dont read the address of the variable, the offset is the jump
        // TODO(Ian): handle other address spaces


        auto input_val = this->replacement_cont.LiftOffsetOrReplace(
            bldr, input_var,
            llvm::IntegerType::get(this->context, input_var.size * 8));
        auto pc_reg = this->LiftNormalRegister(bldr, "PC");
        assert(pc_reg.has_value());
        return (*pc_reg)->StoreIntoParam(bldr, input_val);

        break;
      }
      case OpCode::CPUI_RETURN:
      case OpCode::CPUI_BRANCHIND:
      case OpCode::CPUI_CALLIND: {
        auto copy_inval = this->LiftInParam(
            bldr, input_var,
            llvm::IntegerType::get(this->context, input_var.size * 8));
        if (copy_inval.has_value()) {
          auto pc_reg = this->LiftNormalRegister(bldr, "PC");
          assert(pc_reg.has_value());
          return (*pc_reg)->StoreIntoParam(bldr, *copy_inval);
        }
        break;
      }
        // TODO(alex): Maybe extract this into a method like `LiftIntegerUnOp`?
        // Let's see how much duplication there is.
      case OpCode::CPUI_INT_ZEXT:
      case OpCode::CPUI_INT_SEXT: {
        auto zext_inval = this->LiftIntegerInParam(bldr, input_var);
        if (zext_inval.has_value()) {
          auto *zext_type =
              llvm::IntegerType::get(this->context, outvar->size * 8);
          auto *zext_op = (opc == OpCode::CPUI_INT_ZEXT)
                              ? bldr.CreateZExt(*zext_inval, zext_type)
                              : bldr.CreateSExt(*zext_inval, zext_type);
          return this->LiftStoreIntoOutParam(bldr, zext_op, outvar);
        }
        break;
      }
      case OpCode::CPUI_INT_2COMP: {
        auto two_comp_inval = this->LiftIntegerInParam(bldr, input_var);
        if (two_comp_inval.has_value()) {
          return this->LiftStoreIntoOutParam(
              bldr, bldr.CreateNeg(*two_comp_inval), outvar);
        }
        break;
      }
      case OpCode::CPUI_INT_NEGATE: {
        auto negate_inval = this->LiftIntegerInParam(bldr, input_var);
        if (negate_inval.has_value()) {
          return this->LiftStoreIntoOutParam(
              bldr, bldr.CreateNot(*negate_inval), outvar);
        }
        break;
      }
      case OpCode::CPUI_FLOAT_NEG: {
        auto negate_inval = this->LiftInParam(
            bldr, input_var, llvm::Type::getFloatTy(this->context));
        if (negate_inval.has_value()) {
          return this->LiftStoreIntoOutParam(
              bldr, bldr.CreateFNeg(*negate_inval), outvar);
        }
        break;
      }
      case OpCode::CPUI_FLOAT_ABS: {
        return this->LiftUnOpWithFloatIntrinsic(bldr, llvm::Intrinsic::fabs,
                                                outvar, input_var);
      }
      case OpCode::CPUI_FLOAT_SQRT: {
        return this->LiftUnOpWithFloatIntrinsic(bldr, llvm::Intrinsic::sqrt,
                                                outvar, input_var);
      }
      case OpCode::CPUI_FLOAT_CEIL: {
        return this->LiftUnOpWithFloatIntrinsic(bldr, llvm::Intrinsic::ceil,
                                                outvar, input_var);
      }
      case OpCode::CPUI_FLOAT_FLOOR: {
        return this->LiftUnOpWithFloatIntrinsic(bldr, llvm::Intrinsic::floor,
                                                outvar, input_var);
      }
      case OpCode::CPUI_FLOAT_ROUND: {
        return this->LiftUnOpWithFloatIntrinsic(bldr, llvm::Intrinsic::round,
                                                outvar, input_var);
      }
      case OpCode::CPUI_FLOAT_NAN: {
        auto nan_inval = this->LiftInParam(
            bldr, input_var, llvm::Type::getFloatTy(this->context));
        if (nan_inval.has_value()) {
          // LLVM trunk has an `isnan` intrinsic but to support older versions, I think we need to do this.
          auto *isnan_check = bldr.CreateZExt(
              bldr.CreateNot(bldr.CreateFCmpOEQ(*nan_inval, *nan_inval)),
              llvm::IntegerType::get(this->context, outvar->size * 8));
          return this->LiftStoreIntoOutParam(bldr, isnan_check, outvar);
        }
        break;
      }
      case OpCode::CPUI_FLOAT_INT2FLOAT: {
        auto int2float_inval = this->LiftIntegerInParam(bldr, input_var);
        if (int2float_inval.has_value()) {
          auto *converted = bldr.CreateSIToFP(
              *int2float_inval, llvm::Type::getFloatTy(this->context));
          return this->LiftStoreIntoOutParam(bldr, converted, outvar);
        }
        break;
      }
      case OpCode::CPUI_FLOAT_FLOAT2FLOAT: {
        auto float2float_inval = this->LiftInParam(
            bldr, input_var, llvm::Type::getFloatTy(this->context));
        if (float2float_inval.has_value()) {
          // This is a no-op until we make a helper to select an appropriate float type for a given node size.
          return this->LiftStoreIntoOutParam(
              bldr,
              bldr.CreateFPTrunc(*float2float_inval,
                                 llvm::Type::getFloatTy(this->context)),
              outvar);
        }
        break;
      }
      case OpCode::CPUI_FLOAT_TRUNC: {
        auto trunc_inval = this->LiftInParam(
            bldr, input_var, llvm::Type::getFloatTy(this->context));
        if (trunc_inval.has_value()) {
          auto *converted = bldr.CreateFPToSI(
              *trunc_inval,
              llvm::IntegerType::get(this->context, outvar->size * 8));
          return this->LiftStoreIntoOutParam(bldr, converted, outvar);
        }
        break;
      }
      case OpCode::CPUI_POPCOUNT: {
        auto ctpop_inval = this->LiftIntegerInParam(bldr, input_var);
        if (ctpop_inval.has_value()) {
          llvm::Type *overloaded_types[1] = {(*ctpop_inval)->getType()};
          llvm::Function *ctpop_intrinsic = llvm::Intrinsic::getDeclaration(
              bldr.GetInsertBlock()->getModule(), llvm::Intrinsic::ctpop,
              overloaded_types);

          llvm::Value *ctpop_args[] = {*ctpop_inval};
          llvm::Value *ctpop_val = this->FixResultForOutVarnode(
              bldr, bldr.CreateCall(ctpop_intrinsic, ctpop_args), *outvar);


          return this->LiftStoreIntoOutParam(bldr, ctpop_val, outvar);
        }
        break;
      }
      default: break;
    }
    return LiftStatus::kLiftedUnsupportedInstruction;
  }
  using BinaryOperator = std::function<llvm::Value *(
      llvm::Value *, llvm::Value *, llvm::IRBuilder<> &)>;
  static std::map<OpCode, BinaryOperator> INTEGER_BINARY_OPS;
  static std::unordered_set<OpCode> INTEGER_COMP_OPS;


  LiftStatus LiftIntegerBinOp(llvm::IRBuilder<> &bldr, OpCode opc,
                              VarnodeData *outvar, VarnodeData lhs,
                              VarnodeData rhs) {


    if (opc == OpCode::CPUI_CBRANCH) {
      auto should_branch = this->LiftInParam(
          bldr, rhs, llvm::IntegerType::get(this->context, rhs.size * 8));
      // directs dont read the address of the variable, the offset is the jump
      // TODO(Ian): handle other address spaces
      auto jump_addr = this->replacement_cont.LiftOffsetOrReplace(
          bldr, lhs, llvm::IntegerType::get(this->context, lhs.size * 8));
      if (should_branch.has_value()) {
        auto trunc_should_branch = bldr.CreateTrunc(
            *should_branch,
            llvm::IntegerType::get(this->context, rhs.size * 1));


        auto pc_reg_param = this->LiftNormalRegister(bldr, "PC");
        assert(pc_reg_param.has_value());
        auto pc_reg_ptr = *pc_reg_param;
        auto orig_pc_value = pc_reg_ptr->LiftAsInParam(
            bldr, this->insn_lifter_parent.GetWordType());

        if (this->insn.category ==
            remill::Instruction::Category::kCategoryConditionalBranch) {
          auto branch_taken_ref = LoadBranchTakenRef(bldr.GetInsertBlock());
          bldr.CreateStore(*should_branch, branch_taken_ref);
        }
        if (orig_pc_value.has_value()) {
          auto next_pc_value =
              bldr.CreateSelect(trunc_should_branch, jump_addr, *orig_pc_value);
          return pc_reg_ptr->StoreIntoParam(bldr, next_pc_value);
        }
      }
    }

    if (INTEGER_BINARY_OPS.find(opc) != INTEGER_BINARY_OPS.end()) {
      auto &op_func = INTEGER_BINARY_OPS.find(opc)->second;
      auto lifted_lhs = this->LiftIntegerInParam(bldr, lhs);
      auto lifted_rhs = this->LiftIntegerInParam(bldr, rhs);
      if (lifted_lhs.has_value() && lifted_rhs.has_value()) {
        LOG(INFO) << "Binop with lhs: "
                  << remill::LLVMThingToString(*lifted_lhs);
        LOG(INFO) << "Binop with rhs: "
                  << remill::LLVMThingToString(*lifted_rhs);
        auto orig_res = op_func(*lifted_lhs, *lifted_rhs, bldr);
        if (INTEGER_COMP_OPS.find(opc) != INTEGER_COMP_OPS.end()) {
          // Comparison operators always return a byte
          if (orig_res->getType()->getIntegerBitWidth() != 8) {
            orig_res = bldr.CreateZExt(
                orig_res, llvm::IntegerType::get(bldr.getContext(), 8));
          }
        }
        LOG(INFO) << "Res: " << remill::LLVMThingToString(orig_res);
        LOG(INFO) << "Res ty: "
                  << remill::LLVMThingToString(orig_res->getType());
        return this->LiftStoreIntoOutParam(bldr, orig_res, outvar);
      }
    }
    return LiftStatus::kLiftedUnsupportedInstruction;
  }

  LiftStatus LiftBoolBinOp(llvm::IRBuilder<> &bldr, OpCode opc,
                           VarnodeData *outvar, VarnodeData lhs,
                           VarnodeData rhs) {
    std::function<llvm::Value *(llvm::Value *, llvm::Value *,
                                llvm::IRBuilder<> &)>
        op_func;
    switch (opc) {
      case CPUI_BOOL_AND: {
        op_func = [](llvm::Value *lhs, llvm::Value *rhs,
                     llvm::IRBuilder<> &bldr) {
          return bldr.CreateAnd(lhs, rhs);
        };
        break;
      }
      case CPUI_BOOL_OR: {
        op_func = [](llvm::Value *lhs, llvm::Value *rhs,
                     llvm::IRBuilder<> &bldr) {
          return bldr.CreateOr(lhs, rhs);
        };
        break;
      }
      case CPUI_BOOL_XOR: {
        op_func = [](llvm::Value *lhs, llvm::Value *rhs,
                     llvm::IRBuilder<> &bldr) {
          return bldr.CreateXor(lhs, rhs);
        };
        break;
      }
      default: break;
    }
    if (op_func) {
      auto lifted_lhs = this->LiftInParam(
               bldr, lhs, llvm::IntegerType::get(this->context, 8)),
           lifted_rhs = this->LiftInParam(
               bldr, rhs, llvm::IntegerType::get(this->context, 8));
      if (lifted_lhs.has_value() && lifted_rhs.has_value()) {
        return this->LiftStoreIntoOutParam(
            bldr, op_func(*lifted_lhs, *lifted_rhs, bldr), outvar);
      }
    }
    return LiftStatus::kLiftedUnsupportedInstruction;
  }

  LiftStatus LiftFloatBinOp(llvm::IRBuilder<> &bldr, OpCode opc,
                            VarnodeData *outvar, VarnodeData lhs,
                            VarnodeData rhs) {
    std::function<llvm::Value *(llvm::Value *, llvm::Value *,
                                llvm::IRBuilder<> &)>
        op_func;
    switch (opc) {
      case CPUI_FLOAT_EQUAL: {
        op_func = [](llvm::Value *lhs, llvm::Value *rhs,
                     llvm::IRBuilder<> &bldr) {
          return bldr.CreateZExt(bldr.CreateFCmpOEQ(lhs, rhs),
                                 llvm::IntegerType::get(bldr.getContext(), 8));
        };
        break;
      }
      case CPUI_FLOAT_NOTEQUAL: {
        op_func = [](llvm::Value *lhs, llvm::Value *rhs,
                     llvm::IRBuilder<> &bldr) {
          return bldr.CreateZExt(bldr.CreateFCmpONE(lhs, rhs),
                                 llvm::IntegerType::get(bldr.getContext(), 8));
        };
        break;
      }
      case CPUI_FLOAT_LESS: {
        op_func = [](llvm::Value *lhs, llvm::Value *rhs,
                     llvm::IRBuilder<> &bldr) {
          return bldr.CreateZExt(bldr.CreateFCmpOLT(lhs, rhs),
                                 llvm::IntegerType::get(bldr.getContext(), 8));
        };
        break;
      }
      case CPUI_FLOAT_LESSEQUAL: {
        op_func = [](llvm::Value *lhs, llvm::Value *rhs,
                     llvm::IRBuilder<> &bldr) {
          return bldr.CreateZExt(bldr.CreateFCmpOLE(lhs, rhs),
                                 llvm::IntegerType::get(bldr.getContext(), 8));
        };
        break;
      }
      case CPUI_FLOAT_ADD: {
        op_func = [](llvm::Value *lhs, llvm::Value *rhs,
                     llvm::IRBuilder<> &bldr) {
          return bldr.CreateFAdd(lhs, rhs);
        };
        break;
      }
      case CPUI_FLOAT_SUB: {
        op_func = [](llvm::Value *lhs, llvm::Value *rhs,
                     llvm::IRBuilder<> &bldr) {
          return bldr.CreateFSub(lhs, rhs);
        };
        break;
      }
      case CPUI_FLOAT_MULT: {
        op_func = [](llvm::Value *lhs, llvm::Value *rhs,
                     llvm::IRBuilder<> &bldr) {
          return bldr.CreateFMul(lhs, rhs);
        };
        break;
      }
      case CPUI_FLOAT_DIV: {
        op_func = [](llvm::Value *lhs, llvm::Value *rhs,
                     llvm::IRBuilder<> &bldr) {
          return bldr.CreateFDiv(lhs, rhs);
        };
        break;
      }
      default: break;
    }
    if (op_func) {
      // TODO(alex): I think we need some helper here to achieve something similar to what `llvm::IntegerType::get`
      // gives us, except for floating point types.
      //
      // So we need to check the size of the node and return either a 32-bit float, brain float, double, etc.
      auto lifted_lhs =
          this->LiftInParam(bldr, lhs, llvm::Type::getFloatTy(this->context));
      auto lifted_rhs =
          this->LiftInParam(bldr, lhs, llvm::Type::getFloatTy(this->context));
      if (lifted_lhs.has_value() && lifted_rhs.has_value()) {
        return this->LiftStoreIntoOutParam(
            bldr, op_func(*lifted_lhs, *lifted_rhs, bldr), outvar);
      }
    }
    return LiftStatus::kLiftedUnsupportedInstruction;
  }


  LiftStatus LiftBinOp(llvm::IRBuilder<> &bldr, OpCode opc, VarnodeData *outvar,
                       VarnodeData lhs, VarnodeData rhs) {
    auto res = this->LiftIntegerBinOp(bldr, opc, outvar, lhs, rhs);
    if (res == LiftStatus::kLiftedInstruction) {
      return res;
    }

    auto sres = this->LiftBoolBinOp(bldr, opc, outvar, lhs, rhs);
    if (sres == LiftStatus::kLiftedInstruction) {
      return sres;
    }

    sres = this->LiftFloatBinOp(bldr, opc, outvar, lhs, rhs);
    if (sres == LiftStatus::kLiftedInstruction) {
      return sres;
    }

    if (opc == OpCode::CPUI_LOAD && outvar) {
      auto out_op = *outvar;
      auto addr_operand = rhs;
      auto lifted_addr_offset = this->LiftInParam(
          bldr, addr_operand, this->insn_lifter_parent.GetWordType());

      if (lifted_addr_offset) {

        auto out_type = llvm::IntegerType::get(this->context, out_op.size * 8);
        auto lifted_addr = this->CreateMemoryAddress(*lifted_addr_offset);

        auto loaded_value = lifted_addr->LiftAsInParam(bldr, out_type);
        if (loaded_value.has_value()) {
          auto lifted_out = this->LiftParamPtr(bldr, out_op);
          return lifted_out->StoreIntoParam(bldr, *loaded_value);
        }
      }
    }

    if (opc == OpCode::CPUI_PIECE && outvar) {
      assert(rhs.size + lhs.size == outvar->size);

      // Treat them as integers
      auto lifted_lhs = this->LiftInParam(
               bldr, lhs, llvm::IntegerType::get(this->context, lhs.size * 8)),
           lifted_rhs = this->LiftInParam(
               bldr, rhs, llvm::IntegerType::get(this->context, rhs.size * 8));

      if (lifted_lhs.has_value() && lifted_rhs.has_value()) {
        // Widen the most significant operand and then left shift it to make room for the least significant operand.
        auto *ms_operand = bldr.CreateZExt(
            *lifted_lhs, llvm::IntegerType::get(this->context, outvar->size));
        auto *shifted_ms_operand = bldr.CreateShl(
            ms_operand, llvm::ConstantInt::get(
                            llvm::Type::getInt8Ty(this->context), rhs.size));

        // Now concatenate them with an OR.
        auto *concat = bldr.CreateOr(shifted_ms_operand, *lifted_rhs);
        return this->LiftStoreIntoOutParam(bldr, concat, outvar);
      }
    }

    if (opc == OpCode::CPUI_SUBPIECE && outvar) {
      auto lifted_lhs = this->LiftInParam(
          bldr, lhs, llvm::IntegerType::get(this->context, lhs.size * 8));

      if (lifted_lhs.has_value()) {
        auto new_size = lhs.size - rhs.offset;
        auto *subpiece_lhs = bldr.CreateTrunc(
            *lifted_lhs, llvm::IntegerType::get(this->context, new_size * 8));

        if (new_size < outvar->size) {
          subpiece_lhs = bldr.CreateZExt(
              subpiece_lhs,
              llvm::IntegerType::get(this->context, 8 * outvar->size));
        } else if (new_size > outvar->size) {
          subpiece_lhs = bldr.CreateTrunc(
              subpiece_lhs,
              llvm::IntegerType::get(this->context, 8 * outvar->size));
        }

        return this->LiftStoreIntoOutParam(bldr, subpiece_lhs, outvar);
      }
    }

    if (opc == OpCode::CPUI_INDIRECT && outvar) {
      // TODO(alex): This isn't clear to me from the documentation.
      // I'll probably need to find some code that generates this op in order to understand how to handle it.
      return LiftStatus::kLiftedUnsupportedInstruction;
    }

    if (opc == OpCode::CPUI_NEW && outvar) {
      // NOTE(alex): We shouldn't encounter this op as it only get generated when lifting Java or
      // Dalvik bytecode
      return LiftStatus::kLiftedUnsupportedInstruction;
    }

    return LiftStatus::kLiftedUnsupportedInstruction;
  }

  LiftStatus LiftTernOp(llvm::IRBuilder<> &bldr, OpCode opc,
                        VarnodeData *outvar, VarnodeData param0,
                        VarnodeData param1, VarnodeData param2) {
    switch (opc) {
      case OpCode::CPUI_STORE: {
        auto addr_operand = param1;
        auto lifted_addr_offset = this->LiftInParam(
            bldr, addr_operand, this->insn_lifter_parent.GetWordType());

        if (lifted_addr_offset) {
          auto store_param = this->LiftInParam(
              bldr, param2,
              llvm::IntegerType::get(this->context, param2.size * 8));

          if (store_param.has_value()) {
            auto lifted_addr = this->CreateMemoryAddress(*lifted_addr_offset);
            return lifted_addr->StoreIntoParam(bldr, *store_param);
          }
        }
        break;
      }
      case OpCode::CPUI_PTRADD: {
        auto lifted_addr = this->LiftInParam(
                 bldr, param0, this->insn_lifter_parent.GetWordType()),
             lifted_index = this->LiftIntegerInParam(bldr, param1);
        auto *elem_size = llvm::ConstantInt::get(
            llvm::IntegerType::get(this->context, param2.size * 8),
            param2.offset);
        if (lifted_addr.has_value() && lifted_index.has_value()) {
          auto *offset = bldr.CreateMul(*lifted_index, elem_size),
               *ptr_add = bldr.CreateAdd(*lifted_addr, offset);
          return this->LiftStoreIntoOutParam(bldr, ptr_add, outvar);
        }
        break;
      }
      case OpCode::CPUI_PTRSUB: {
        auto lifted_addr = this->LiftInParam(
                 bldr, param0, this->insn_lifter_parent.GetWordType()),
             lifted_offset = this->LiftIntegerInParam(bldr, param1);
        if (lifted_addr.has_value() && lifted_offset.has_value()) {
          return this->LiftStoreIntoOutParam(
              bldr, bldr.CreateAdd(*lifted_addr, *lifted_offset), outvar);
        }
        break;
      }
      default: break;
    }

    return LiftStatus::kLiftedUnsupportedInstruction;
  }

  LiftStatus LiftVariadicOp(llvm::IRBuilder<> &bldr, OpCode opc,
                            VarnodeData *outvar, VarnodeData *vars,
                            int4 isize) {
    switch (opc) {
      // We shouldnt encounter this afaik MULTIEQUAL is a decompiler concept?
      case OpCode::CPUI_MULTIEQUAL: {
        llvm::Type *phi_type =
            llvm::IntegerType::get(this->context, vars[0].size * 8);
        llvm::PHINode *phi_node = bldr.CreatePHI(phi_type, isize);
        for (int4 i = 0; i < isize; ++i) {
          VarnodeData &var = vars[i];
          auto inval = this->LiftInParam(
              bldr, var, llvm::IntegerType::get(this->context, var.size * 8));
          if (!inval.has_value()) {
            return LiftStatus::kLiftedUnsupportedInstruction;
          }
          // TODO(alex): This isn't right, just using the current block to get things building.
          // We need to track the incoming basic blocks for each value.
          phi_node->addIncoming(*inval, bldr.GetInsertBlock());
        }
        return this->LiftStoreIntoOutParam(bldr, phi_node, outvar);
      }
      case OpCode::CPUI_CPOOLREF: {
        // NOTE(alex): We shouldn't encounter this op as it only get generated when lifting Java or
        // Dalvik bytecode
        return LiftStatus::kLiftedUnsupportedInstruction;
      }
      default: break;
    }

    return LiftStatus::kLiftedUnsupportedInstruction;
  }


  virtual void dump(const Address &addr, OpCode opc, VarnodeData *outvar,
                    VarnodeData *vars, int4 isize) final override {
    LOG(INFO) << "inner handle" << std::endl;
    llvm::IRBuilder bldr(this->target_block);

    // The MULTIEQUAL op has variadic operands
    if (opc == OpCode::CPUI_MULTIEQUAL || opc == OpCode::CPUI_CPOOLREF) {
      this->UpdateStatus(this->LiftVariadicOp(bldr, opc, outvar, vars, isize),
                         opc);
      return;
    }

    if (opc == OpCode::CPUI_CALLOTHER) {
      if (isize == 3 && vars[0].offset < this->user_op_names.size() &&
          this->user_op_names[vars[0].offset] == "claim_eq") {
        LOG(INFO) << "Applying eq claim";
        this->replacement_cont.ApplyEqualityClaim(bldr, *this, vars[1],
                                                  vars[2]);
        return;
      }


      this->UpdateStatus(LiftStatus::kLiftedUnsupportedInstruction, opc);
      return;
    }

    switch (isize) {
      case 1: {
        this->UpdateStatus(this->LiftUnOp(bldr, opc, outvar, vars[0]), opc);
        break;
      }
      case 2: {
        this->UpdateStatus(this->LiftBinOp(bldr, opc, outvar, vars[0], vars[1]),
                           opc);
        return;
      }
      case 3: {
        this->UpdateStatus(
            this->LiftTernOp(bldr, opc, outvar, vars[0], vars[1], vars[2]),
            opc);
        break;
      }
      default:
        //this->replacement_cont.ApplyNonEqualityClaim();
        this->UpdateStatus(LiftStatus::kLiftedUnsupportedInstruction, opc);
        return;
    }
    return;
    //this->replacement_cont.ApplyNonEqualityClaim();
  }

  LiftStatus GetStatus() {
    return this->status;
  }
};  // namespace remill

std::unordered_set<OpCode>
    SleighLifter::PcodeToLLVMEmitIntoBlock::INTEGER_COMP_OPS = {
        CPUI_INT_EQUAL,   CPUI_INT_NOTEQUAL,  CPUI_INT_LESS,
        CPUI_INT_SLESS,   CPUI_INT_LESSEQUAL, CPUI_INT_SLESSEQUAL,
        CPUI_INT_SBORROW, CPUI_INT_SCARRY,    CPUI_INT_CARRY};

std::map<OpCode, SleighLifter::PcodeToLLVMEmitIntoBlock::BinaryOperator>
    SleighLifter::PcodeToLLVMEmitIntoBlock::INTEGER_BINARY_OPS = {
        {OpCode::CPUI_INT_AND,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           return bldr.CreateAnd(lhs, rhs);
         }},
        {OpCode::CPUI_INT_OR,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           return bldr.CreateOr(lhs, rhs);
         }},
        {OpCode::CPUI_INT_XOR,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           return bldr.CreateXor(lhs, rhs);
         }},
        {OpCode::CPUI_INT_LEFT,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           auto new_rhs = rhs;
           if (lhs->getType() != rhs->getType()) {
             new_rhs = bldr.CreateZExtOrTrunc(rhs, lhs->getType());
           }
           return bldr.CreateShl(lhs, new_rhs);
         }},
        {OpCode::CPUI_INT_RIGHT,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           auto new_rhs = rhs;
           if (lhs->getType() != rhs->getType()) {
             new_rhs = bldr.CreateZExtOrTrunc(rhs, lhs->getType());
           }
           return bldr.CreateLShr(lhs, new_rhs);
         }},
        {OpCode::CPUI_INT_SRIGHT,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           return bldr.CreateAShr(lhs, rhs);
         }},
        {OpCode::CPUI_INT_ADD,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           return bldr.CreateAdd(lhs, rhs);
         }},
        {OpCode::CPUI_INT_SUB,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           return bldr.CreateSub(lhs, rhs);
         }},
        {OpCode::CPUI_INT_MULT,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           return bldr.CreateMul(lhs, rhs);
         }},
        {OpCode::CPUI_INT_DIV,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           return bldr.CreateUDiv(lhs, rhs);
         }},
        {OpCode::CPUI_INT_SDIV,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           return bldr.CreateSDiv(lhs, rhs);
         }},
        {OpCode::CPUI_INT_REM,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           return bldr.CreateURem(lhs, rhs);
         }},
        {OpCode::CPUI_INT_SREM,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           return bldr.CreateSRem(lhs, rhs);
         }},
        {OpCode::CPUI_INT_EQUAL,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           // TODO(alex): Should these by using `trunc`?
           // The docs seem to indicate that it's not ok to `zext` to a smaller type.
           return bldr.CreateZExt(bldr.CreateICmpEQ(lhs, rhs),
                                  llvm::IntegerType::get(bldr.getContext(), 8));
         }},
        {OpCode::CPUI_INT_NOTEQUAL,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           return bldr.CreateZExt(bldr.CreateICmpNE(lhs, rhs),
                                  llvm::IntegerType::get(bldr.getContext(), 8));
         }},
        {OpCode::CPUI_INT_LESS,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           return bldr.CreateZExt(bldr.CreateICmpULT(lhs, rhs),
                                  llvm::IntegerType::get(bldr.getContext(), 8));
         }},
        {OpCode::CPUI_INT_SLESS,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           return bldr.CreateZExt(bldr.CreateICmpSLT(lhs, rhs),
                                  llvm::IntegerType::get(bldr.getContext(), 8));
         }},
        {OpCode::CPUI_INT_LESSEQUAL,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           return bldr.CreateZExt(bldr.CreateICmpSLE(lhs, rhs),
                                  llvm::IntegerType::get(bldr.getContext(), 8));
         }},
        {OpCode::CPUI_INT_SLESSEQUAL,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           return bldr.CreateZExt(bldr.CreateICmpULE(lhs, rhs),
                                  llvm::IntegerType::get(bldr.getContext(), 8));
         }},
        {OpCode::CPUI_INT_CARRY,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           llvm::Type *overloaded_types[1] = {lhs->getType()};
           llvm::Function *uadd_intrinsic = llvm::Intrinsic::getDeclaration(
               bldr.GetInsertBlock()->getModule(),
               llvm::Intrinsic::uadd_with_overflow, overloaded_types);
           llvm::Value *uadd_args[] = {lhs, rhs};
           llvm::Value *uadd_val = bldr.CreateCall(uadd_intrinsic, uadd_args);
           // The value at index 1 is the overflow bit.
           return bldr.CreateExtractValue(uadd_val, {1});
         }},
        {OpCode::CPUI_INT_SCARRY,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           llvm::Type *overloaded_types[1] = {lhs->getType()};
           llvm::Function *sadd_intrinsic = llvm::Intrinsic::getDeclaration(
               bldr.GetInsertBlock()->getModule(),
               llvm::Intrinsic::sadd_with_overflow, overloaded_types);
           llvm::Value *sadd_args[] = {lhs, rhs};
           llvm::Value *sadd_val = bldr.CreateCall(sadd_intrinsic, sadd_args);
           // The value at index 1 is the overflow bit.
           return bldr.CreateExtractValue(sadd_val, {1});
         }},
        {OpCode::CPUI_INT_SBORROW,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           llvm::Type *overloaded_types[1] = {lhs->getType()};
           llvm::Function *ssub_intrinsic = llvm::Intrinsic::getDeclaration(
               bldr.GetInsertBlock()->getModule(),
               llvm::Intrinsic::ssub_with_overflow, overloaded_types);
           llvm::Value *ssub_args[] = {lhs, rhs};
           llvm::Value *ssub_val = bldr.CreateCall(ssub_intrinsic, ssub_args);
           // The value at index 1 is the overflow bit.
           return bldr.CreateExtractValue(ssub_val, {1});
         }},
};

SleighLifter::SleighLifter(const sleigh::SleighArch *arch_,
                           const IntrinsicTable &intrinsics_)
    : InstructionLifter(arch_, intrinsics_),
      sleigh_context(new sleigh::SingleInstructionSleighContext(
          arch_->GetSLAName(), arch_->GetPSpec())) {
  arch_->InitializeSleighContext(*sleigh_context);
}


const std::string_view SleighLifter::kInstructionFunctionPrefix =
    "sleigh_remill_instruction_function";


std::pair<LiftStatus, llvm::Function *>
SleighLifter::LiftIntoInternalBlock(Instruction &inst, llvm::Module *target_mod,
                                    bool is_delayed) {
  auto target_func = inst.arch->DefineLiftedFunction(
      SleighLifter::kInstructionFunctionPrefix, target_mod);

  llvm::BasicBlock *target_block = &target_func->getEntryBlock();
  llvm::IRBuilder<> ir(target_block);
  auto internal_state_pointer = remill::LoadStatePointer(target_block);
  const auto next_pc_ref =
      LoadRegAddress(target_block, internal_state_pointer, kNextPCVariableName);
  const auto next_pc = ir.CreateLoad(this->GetWordType(), next_pc_ref.first);
  auto pc_ref =
      this->LoadRegAddress(target_block, internal_state_pointer, "PC");

  auto curr_eip = ir.CreateAdd(
      next_pc, llvm::ConstantInt::get(this->GetWordType(), inst.bytes.size()));
  ir.CreateStore(curr_eip, next_pc_ref.first);
  ir.CreateStore(curr_eip, pc_ref.first);


  SleighLifter::PcodeToLLVMEmitIntoBlock lifter(
      target_block, internal_state_pointer, inst, *this,
      this->sleigh_context->getUserOpNames());
  sleigh_context->oneInstruction(inst.pc, lifter, inst.bytes);

  ir.CreateStore(ir.CreateLoad(this->GetWordType(), pc_ref.first),
                 next_pc_ref.first);

  ir.CreateRet(remill::LoadMemoryPointer(ir.GetInsertBlock(),
                                         *this->GetIntrinsicTable()));

  // Setup like an ISEL
  target_func->setLinkage(llvm::GlobalValue::InternalLinkage);
  target_func->removeFnAttr(llvm::Attribute::NoInline);
  target_func->addFnAttr(llvm::Attribute::InlineHint);
  target_func->addFnAttr(llvm::Attribute::AlwaysInline);

  return std::make_pair(lifter.GetStatus(), target_func);
}

LiftStatus
SleighLifter::LiftIntoBlock(Instruction &inst, llvm::BasicBlock *block,
                            llvm::Value *state_ptr, bool is_delayed) {

  if (!inst.IsValid()) {
    LOG(ERROR) << "Invalid function" << inst.Serialize();
    inst.operands.clear();
    return kLiftedInvalidInstruction;
  }


  // Call the instruction function

  auto res = this->LiftIntoInternalBlock(inst, block->getModule(), is_delayed);

  auto target_func = res.second;


  llvm::IRBuilder<> intoblock_builer(block);


  std::vector<llvm::Value *> args;
  args.reserve(3);

  args.push_back(remill::LoadStatePointer(block));
  args.push_back(remill::LoadProgramCounter(block, *this->GetIntrinsicTable()));
  args.push_back(remill::LoadMemoryPointer(block, *this->GetIntrinsicTable()));

  intoblock_builer.CreateStore(intoblock_builer.CreateCall(target_func, args),
                               remill::LoadMemoryPointerRef(block));

  // also store off the potentially updated pc into NEXT_PC to keep with traditional lifters
  intoblock_builer.CreateStore(
      remill::LoadProgramCounter(block, *this->GetIntrinsicTable()),
      remill::LoadNextProgramCounterRef(block));


  //NOTE(Ian): If we made it past decoding we should be able to decode the bytes again
  LOG(INFO) << res.first;

  return res.first;
}

Sleigh &SleighLifter::GetEngine(void) const {
  return this->sleigh_context->GetEngine();
}
}  // namespace remill

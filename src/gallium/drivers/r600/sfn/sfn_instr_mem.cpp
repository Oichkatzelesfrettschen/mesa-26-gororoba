/* -*- mesa-c++  -*-
 * Copyright 2022 Collabora LTD
 * Author: Gert Wollny <gert.wollny@collabora.com>
 * SPDX-License-Identifier: MIT
 */

#include "sfn_instr_mem.h"

#include "nir_intrinsics.h"
#include "nir_intrinsics_indices.h"
#include "sfn_alu_defines.h"
#include "sfn_debug.h"
#include "sfn_instr_alu.h"
#include "sfn_instr_fetch.h"
#include "sfn_instr_tex.h"
#include "sfn_shader.h"
#include "sfn_virtualvalues.h"

#include "util/format/u_format.h"

namespace r600 {

GDSInstr::GDSInstr(
   ESDOp op, Register *dest, const RegisterVec4& src, int uav_base, PRegister uav_id):
    Resource(this, uav_base, uav_id),
    m_op(op),
    m_dest(dest),
    m_src(src)
{
   set_always_keep();

   m_src.add_use(this);
   if (m_dest)
      m_dest->add_parent(this);
}

bool
GDSInstr::is_equal_to(const GDSInstr& rhs) const
{
#define NE(X) (X != rhs.X)

   if (NE(m_op) || NE(m_src))
      return false;

   sfn_value_equal(m_dest, rhs.m_dest);

   return resource_is_equal(rhs);
}

void
GDSInstr::accept(ConstInstrVisitor& visitor) const
{
   visitor.visit(*this);
}

void
GDSInstr::accept(InstrVisitor& visitor)
{
   visitor.visit(this);
}

bool
GDSInstr::do_ready() const
{
   return m_src.ready(block_id(), index()) && resource_ready(block_id(), index());
}

void
GDSInstr::do_print(std::ostream& os) const
{
   os << "GDS " << lds_ops.at(m_op).name;
   if (m_dest)
      os << *m_dest;
   else
      os << "___";
   os << " " << m_src;
   os << " BASE:" << resource_id();

   print_resource_offset(os);
}

bool
GDSInstr::emit_atomic_counter(nir_intrinsic_instr *intr, Shader& shader)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_atomic_counter_add:
   case nir_intrinsic_atomic_counter_and:
   case nir_intrinsic_atomic_counter_exchange:
   case nir_intrinsic_atomic_counter_max:
   case nir_intrinsic_atomic_counter_min:
   case nir_intrinsic_atomic_counter_or:
   case nir_intrinsic_atomic_counter_xor:
      return emit_atomic_op2(intr, shader);
   case nir_intrinsic_atomic_counter_comp_swap:
      return emit_atomic_counter_comp_swap(intr, shader);
   case nir_intrinsic_atomic_counter_read:
   case nir_intrinsic_atomic_counter_post_dec:
      return emit_atomic_read(intr, shader);
   case nir_intrinsic_atomic_counter_inc:
      return emit_atomic_inc(intr, shader);
   case nir_intrinsic_atomic_counter_pre_dec:
      return emit_atomic_pre_dec(intr, shader);
   default:
      return false;
   }
}

uint8_t GDSInstr::allowed_src_chan_mask() const
{
   return m_src.free_chan_mask();
}

static ESDOp
get_opcode(const nir_intrinsic_op opcode)
{
   switch (opcode) {
   case nir_intrinsic_atomic_counter_add:
      return DS_OP_ADD_RET;
   case nir_intrinsic_atomic_counter_and:
      return DS_OP_AND_RET;
   case nir_intrinsic_atomic_counter_exchange:
      return DS_OP_XCHG_RET;
   case nir_intrinsic_atomic_counter_inc:
      return DS_OP_INC_RET;
   case nir_intrinsic_atomic_counter_max:
      return DS_OP_MAX_UINT_RET;
   case nir_intrinsic_atomic_counter_min:
      return DS_OP_MIN_UINT_RET;
   case nir_intrinsic_atomic_counter_or:
      return DS_OP_OR_RET;
   case nir_intrinsic_atomic_counter_read:
      return DS_OP_READ_RET;
   case nir_intrinsic_atomic_counter_xor:
      return DS_OP_XOR_RET;
   case nir_intrinsic_atomic_counter_post_dec:
      return DS_OP_DEC_RET;
   case nir_intrinsic_atomic_counter_comp_swap:
      return DS_OP_CMP_XCHG_RET;
   case nir_intrinsic_atomic_counter_pre_dec:
   default:
      return DS_OP_INVALID;
   }
}

static ESDOp
get_opcode_wo(const nir_intrinsic_op opcode)
{
   switch (opcode) {
   case nir_intrinsic_atomic_counter_add:
      return DS_OP_ADD;
   case nir_intrinsic_atomic_counter_and:
      return DS_OP_AND;
   case nir_intrinsic_atomic_counter_inc:
      return DS_OP_INC;
   case nir_intrinsic_atomic_counter_max:
      return DS_OP_MAX_UINT;
   case nir_intrinsic_atomic_counter_min:
      return DS_OP_MIN_UINT;
   case nir_intrinsic_atomic_counter_or:
      return DS_OP_OR;
   case nir_intrinsic_atomic_counter_xor:
      return DS_OP_XOR;
   case nir_intrinsic_atomic_counter_post_dec:
      return DS_OP_DEC;
   case nir_intrinsic_atomic_counter_comp_swap:
      return DS_OP_CMP_XCHG_RET;
   case nir_intrinsic_atomic_counter_exchange:
      return DS_OP_XCHG_RET;
   case nir_intrinsic_atomic_counter_pre_dec:
   default:
      return DS_OP_INVALID;
   }
}

bool
GDSInstr::emit_atomic_op2(nir_intrinsic_instr *instr, Shader& shader)
{
   auto& vf = shader.value_factory();
   bool read_result = !list_is_empty(&instr->def.uses);
	
   ESDOp op =
      read_result ? get_opcode(instr->intrinsic) : get_opcode_wo(instr->intrinsic);

   if (DS_OP_INVALID == op)
      return false;

   auto [offset, uav_id] = shader.evaluate_resource_offset(instr, 0);
   {
   }
   offset += nir_intrinsic_base(instr);

   auto dest = read_result ? vf.dest(instr->def, 0, pin_free) : nullptr;

   PRegister src_as_register = nullptr;
   auto src_val = vf.src(instr->src[1], 0);
   if (!src_val->as_register()) {
      auto temp_src_val = vf.temp_register();
      shader.emit_instruction(
         new AluInstr(op1_mov, temp_src_val, src_val, AluInstr::write));
      src_as_register = temp_src_val;
   } else
      src_as_register = src_val->as_register();

   if (uav_id != nullptr)
      shader.set_flag(Shader::sh_indirect_atomic);

   GDSInstr *ir = nullptr;
   if (shader.chip_class() < ISA_CC_CAYMAN) {
      RegisterVec4 src(nullptr, src_as_register, nullptr, nullptr, pin_free);
      ir = new GDSInstr(op, dest, src, offset, uav_id);

   } else {
      auto dest = vf.dest(instr->def, 0, pin_free);
      auto tmp = vf.temp_vec4(pin_group, {0, 1, 7, 7});
      if (uav_id)
         shader.emit_instruction(new AluInstr(op3_muladd_uint24,
                                              tmp[0],
                                              uav_id,
                                              vf.literal(4),
                                              vf.literal(4 * offset),
                                              AluInstr::write));
      else
         shader.emit_instruction(
            new AluInstr(op1_mov, tmp[0], vf.literal(4 * offset), AluInstr::write));
      shader.emit_instruction(new AluInstr(op1_mov, tmp[1], src_val, AluInstr::write));
      ir = new GDSInstr(op, dest, tmp, 0, nullptr);
   }
   shader.emit_instruction(ir);
   return true;
}

bool
GDSInstr::emit_atomic_read(nir_intrinsic_instr *instr, Shader& shader)
{
   auto& vf = shader.value_factory();

   auto [offset, uav_id] = shader.evaluate_resource_offset(instr, 0);
   {
   }
   offset += shader.remap_atomic_base(nir_intrinsic_base(instr));

   auto dest = vf.dest(instr->def, 0, pin_free);

   GDSInstr *ir = nullptr;

   if (shader.chip_class() < ISA_CC_CAYMAN) {
      RegisterVec4 src = RegisterVec4(0, true, {7, 7, 7, 7});
      ir = new GDSInstr(DS_OP_READ_RET, dest, src, offset, uav_id);
   } else {
      auto tmp = vf.temp_vec4(pin_group, {0, 7, 7, 7});
      if (uav_id)
         shader.emit_instruction(new AluInstr(op3_muladd_uint24,
                                              tmp[0],
                                              uav_id,
                                              vf.literal(4),
                                              vf.literal(4 * offset),
                                              AluInstr::write));
      else
         shader.emit_instruction(
            new AluInstr(op1_mov, tmp[0], vf.literal(4 * offset), AluInstr::write));

      ir = new GDSInstr(DS_OP_READ_RET, dest, tmp, 0, nullptr);
   }

   shader.emit_instruction(ir);
   return true;
}

bool
GDSInstr::emit_atomic_inc(nir_intrinsic_instr *instr, Shader& shader)
{
   auto& vf = shader.value_factory();
   bool read_result = !list_is_empty(&instr->def.uses);

   auto [offset, uav_id] = shader.evaluate_resource_offset(instr, 0);
   {
   }
   offset += shader.remap_atomic_base(nir_intrinsic_base(instr));

   GDSInstr *ir = nullptr;
   auto dest = read_result ? vf.dest(instr->def, 0, pin_free) : nullptr;

   if (shader.chip_class() < ISA_CC_CAYMAN) {
            RegisterVec4 src(nullptr, shader.atomic_update(), nullptr, nullptr, pin_chan);
      ir =
         new GDSInstr(read_result ? DS_OP_ADD_RET : DS_OP_ADD, dest, src, offset, uav_id);
   } else {
      auto tmp = vf.temp_vec4(pin_group, {0, 1, 7, 7});

      if (uav_id)
         shader.emit_instruction(new AluInstr(op3_muladd_uint24,
                                              tmp[0],
                                              uav_id,
                                              vf.literal(4),
                                              vf.literal(4 * offset),
                                              AluInstr::write));
      else
         shader.emit_instruction(
            new AluInstr(op1_mov, tmp[0], vf.literal(4 * offset), AluInstr::write));

      shader.emit_instruction(
         new AluInstr(op1_mov, tmp[1], shader.atomic_update(), AluInstr::write));
      ir = new GDSInstr(read_result ? DS_OP_ADD_RET : DS_OP_ADD, dest, tmp, 0, nullptr);
   }
   shader.emit_instruction(ir);
   return true;
}

bool
GDSInstr::emit_atomic_pre_dec(nir_intrinsic_instr *instr, Shader& shader)
{
   auto& vf = shader.value_factory();

   bool read_result = !list_is_empty(&instr->def.uses);

   auto opcode = read_result ? DS_OP_SUB_RET : DS_OP_SUB;
	
   auto [offset, uav_id] = shader.evaluate_resource_offset(instr, 0);
   {
   }
   offset += shader.remap_atomic_base(nir_intrinsic_base(instr));


   auto *tmp_dest = read_result ? vf.temp_register() : nullptr;

   GDSInstr *ir = nullptr;

   if (shader.chip_class() < ISA_CC_CAYMAN) {
      RegisterVec4 src(nullptr, shader.atomic_update(), nullptr, nullptr, pin_chan);
      ir = new GDSInstr(opcode, tmp_dest, src, offset, uav_id);
   } else {
      auto tmp = vf.temp_vec4(pin_group, {0, 1, 7, 7});
      if (uav_id)
         shader.emit_instruction(new AluInstr(op3_muladd_uint24,
                                              tmp[0],
                                              uav_id,
                                              vf.literal(4),
                                              vf.literal(4 * offset),
                                              AluInstr::write));
      else
         shader.emit_instruction(
            new AluInstr(op1_mov, tmp[0], vf.literal(4 * offset), AluInstr::write));

      shader.emit_instruction(
         new AluInstr(op1_mov, tmp[1], shader.atomic_update(), AluInstr::write));
      ir = new GDSInstr(opcode, tmp_dest, tmp, 0, nullptr);
   }

   shader.emit_instruction(ir);
   if (read_result)
      shader.emit_instruction(new AluInstr(op2_sub_int,
                                           vf.dest(instr->def, 0, pin_free),
                                           tmp_dest,
                                           vf.one_i(),
                                           AluInstr::write));
   return true;
}

bool
GDSInstr::emit_atomic_counter_comp_swap(nir_intrinsic_instr *instr, Shader& shader)
{
   auto& vf = shader.value_factory();
   bool read_result = !list_is_empty(&instr->def.uses);

   ESDOp op =
      read_result ? get_opcode(instr->intrinsic) : get_opcode_wo(instr->intrinsic);

   if (DS_OP_INVALID == op)
      return false;

   auto [offset, uav_id] = shader.evaluate_resource_offset(instr, 0);

   offset += nir_intrinsic_base(instr);

   auto dest = read_result ? vf.dest(instr->def, 0, pin_free) : nullptr;

   if (uav_id != nullptr)
      shader.set_flag(Shader::sh_indirect_atomic);

   GDSInstr *ir = nullptr;

   if (shader.chip_class() < ISA_CC_CAYMAN) {
      auto tmp = vf.temp_vec4(pin_group, {4, 1, 2, 7});

      shader.emit_instruction(
         new AluInstr(op1_mov, tmp[1], vf.src(instr->src[1], 0), AluInstr::write));
      shader.emit_instruction(
         new AluInstr(op1_mov, tmp[2], vf.src(instr->src[2], 0), AluInstr::write));

      ir = new GDSInstr(op, dest, tmp, offset, uav_id);
   } else {
      auto tmp = vf.temp_vec4(pin_group, {0, 1, 2, 7});

      if (uav_id)
         shader.emit_instruction(new AluInstr(op3_muladd_uint24,
                                              tmp[0],
                                              uav_id,
                                              vf.literal(4),
                                              vf.literal(4 * offset),
                                              AluInstr::write));
      else
         shader.emit_instruction(
            new AluInstr(op1_mov, tmp[0], vf.literal(4 * offset), AluInstr::write));

      shader.emit_instruction(
         new AluInstr(op1_mov, tmp[1], vf.src(instr->src[1], 0), AluInstr::write));
      shader.emit_instruction(
         new AluInstr(op1_mov, tmp[2], vf.src(instr->src[2], 0), AluInstr::write));

      ir = new GDSInstr(op, dest, tmp, 0, nullptr);
   }
   shader.emit_instruction(ir);
   return true;
}

void GDSInstr::update_indirect_addr(PRegister old_reg, PRegister addr)
{
   (void)old_reg;
   set_resource_offset(addr);
}

RatInstr::RatInstr(ECFOpCode cf_opcode,
                   ERatOp rat_op,
                   const RegisterVec4& data,
                   const RegisterVec4& index,
                   int rat_id,
                   PRegister rat_id_offset,
                   int burst_count,
                   int comp_mask,
                   int element_size):
    Resource(this, rat_id, rat_id_offset),
    m_cf_opcode(cf_opcode),
    m_rat_op(rat_op),
    m_data(data),
    m_index(index),
    m_burst_count(burst_count),
    m_comp_mask(comp_mask),
    m_element_size(element_size)
{
   set_always_keep();
   m_data.add_use(this);
   m_index.add_use(this);
}

void
RatInstr::accept(ConstInstrVisitor& visitor) const
{
   visitor.visit(*this);
}

void
RatInstr::accept(InstrVisitor& visitor)
{
   visitor.visit(this);
}

bool
RatInstr::is_equal_to(const RatInstr& lhs) const
{
   (void)lhs;
   assert(0);
   return false;
}

bool
RatInstr::do_ready() const
{
   bool required_ready = true;

   if (m_rat_op != STORE_TYPED) {
      for (auto i : required_instr()) {
         if (!i->is_scheduled()) {
            required_ready = false;
            break;
         }
      }
   }

   const bool data_ready = m_data.ready(block_id(), index());
   const bool index_ready = m_index.ready(block_id(), index());
   const bool rat_resource_ready = resource_ready(block_id(), index());
   const bool ready = required_ready && data_ready && index_ready;

   if (!ready && sfn_log.has_debug_flag(SfnLog::schedule)) {
      sfn_log << SfnLog::schedule
              << "RAT readiness blocked: op=" << m_rat_op
              << " block=" << block_id()
              << " index=" << index()
              << " data_gpr=" << data_gpr()
              << " index_gpr=" << index_gpr()
              << " need_ack=" << need_ack()
              << " required_ready=" << required_ready
              << " data_ready=" << data_ready
              << " index_ready=" << index_ready
              << " resource_ready=" << rat_resource_ready
              << " required_count=" << required_instr().size() << "\n";

      for (auto i : required_instr()) {
         sfn_log << SfnLog::schedule
                 << "  RAT required: block=" << i->block_id()
                 << " index=" << i->index()
                 << " scheduled=" << i->is_scheduled()
                 << " dead=" << i->is_dead()
                 << " instr=" << *i << "\n";
      }

      for (int chan = 0; chan < 4; ++chan) {
         auto data_value = m_data[chan];
         auto index_value = m_index[chan];

         sfn_log << SfnLog::schedule
                 << "  RAT data[" << chan << "]=" << *data_value
                 << " ready=" << data_value->ready(block_id(), index())
                 << " parent_count=" << data_value->parents().size() << "\n";
         for (auto parent : data_value->parents()) {
            sfn_log << SfnLog::schedule
                    << "    data parent: block=" << parent->block_id()
                    << " index=" << parent->index()
                    << " scheduled=" << parent->is_scheduled()
                    << " dead=" << parent->is_dead()
                    << " instr=" << *parent << "\n";
         }

         sfn_log << SfnLog::schedule
                 << "  RAT index[" << chan << "]=" << *index_value
                 << " ready=" << index_value->ready(block_id(), index())
                 << " parent_count=" << index_value->parents().size() << "\n";
         for (auto parent : index_value->parents()) {
            sfn_log << SfnLog::schedule
                    << "    index parent: block=" << parent->block_id()
                    << " index=" << parent->index()
                    << " scheduled=" << parent->is_scheduled()
                    << " dead=" << parent->is_dead()
                    << " instr=" << *parent << "\n";
         }
      }
   }

   return ready;
}

void
RatInstr::do_print(std::ostream& os) const
{
   os << "MEM_RAT RAT " << resource_id();
   print_resource_offset(os);
   os << " @" << m_index;
   os << " OP:" << m_rat_op << " " << m_data;
   os << " BC:" << m_burst_count << " MASK:" << m_comp_mask << " ES:" << m_element_size;
   if (m_need_ack)
      os << " ACK";
}

void RatInstr::update_indirect_addr(UNUSED PRegister old_reg, PRegister addr)
{
   set_resource_offset(addr);
}

static RatInstr::ERatOp
get_rat_opcode(const nir_atomic_op opcode)
{
   switch (opcode) {
   case nir_atomic_op_iadd:
      return RatInstr::ADD_RTN;
   case nir_atomic_op_iand:
      return RatInstr::AND_RTN;
   case nir_atomic_op_ior:
      return RatInstr::OR_RTN;
   case nir_atomic_op_imin:
      return RatInstr::MIN_INT_RTN;
   case nir_atomic_op_imax:
      return RatInstr::MAX_INT_RTN;
   case nir_atomic_op_umin:
      return RatInstr::MIN_UINT_RTN;
   case nir_atomic_op_umax:
      return RatInstr::MAX_UINT_RTN;
   case nir_atomic_op_ixor:
      return RatInstr::XOR_RTN;
   case nir_atomic_op_cmpxchg:
      return RatInstr::CMPXCHG_INT_RTN;
   case nir_atomic_op_xchg:
      return RatInstr::XCHG_RTN;
   case nir_atomic_op_inc_wrap:
      return RatInstr::WRAP_INC_RTN;
   case nir_atomic_op_dec_wrap:
      return RatInstr::WRAP_DEC_RTN;
   default:
      /* Unsupported atomic op: bail to NOP and log so the pipeline
       * compile fails downstream with a VkResult error instead of
       * aborting the deqp-vk process via assert(). */
      R600_ERR("get_rat_opcode: unsupported atomic opcode %u; returning NOP\n",
               opcode);
      return RatInstr::NOP;
   }
}

static RatInstr::ERatOp
get_rat_opcode_wo(const nir_atomic_op opcode)
{
   switch (opcode) {
   case nir_atomic_op_iadd:
      return RatInstr::ADD;
   case nir_atomic_op_iand:
      return RatInstr::AND;
   case nir_atomic_op_ior:
      return RatInstr::OR;
   case nir_atomic_op_imin:
      return RatInstr::MIN_INT;
   case nir_atomic_op_imax:
      return RatInstr::MAX_INT;
   case nir_atomic_op_umin:
      return RatInstr::MIN_UINT;
   case nir_atomic_op_umax:
      return RatInstr::MAX_UINT;
   case nir_atomic_op_ixor:
      return RatInstr::XOR;
   case nir_atomic_op_cmpxchg:
      return RatInstr::CMPXCHG_INT;
   case nir_atomic_op_xchg:
      return RatInstr::XCHG_RTN;
   default:
      /* Same graceful-bail pattern as the _rtn variant above. */
      R600_ERR("get_rat_opcode_wo: unsupported atomic opcode %u; returning NOP\n",
               opcode);
      return RatInstr::NOP;
   }
}

bool
RatInstr::emit(nir_intrinsic_instr *intr, Shader& shader)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_load_ssbo:
      return emit_ssbo_load(intr, shader);
   case nir_intrinsic_store_ssbo:
      return emit_ssbo_store(intr, shader);
   case nir_intrinsic_ssbo_atomic:
   case nir_intrinsic_ssbo_atomic_swap:
      return emit_ssbo_atomic_op(intr, shader);
   case nir_intrinsic_global_atomic:
      return emit_global_atomic_op(intr, shader);
   case nir_intrinsic_global_atomic_swap:
      return emit_global_atomic_op(intr, shader);
   case nir_intrinsic_store_global:
      return emit_global_store(intr, shader);
   case nir_intrinsic_image_store:
      return emit_image_store(intr, shader);
   case nir_intrinsic_image_load:
   case nir_intrinsic_image_atomic:
   case nir_intrinsic_image_atomic_swap:
      return emit_image_load_or_atomic(intr, shader);
   case nir_intrinsic_uav_instr_r600:
      return emit_uav_store_r600(intr, shader);
   case nir_intrinsic_uav_returning_instr_r600:
      return emit_uav_returning_instr_r600(intr, shader);
   case nir_intrinsic_image_size:
      return emit_image_size(intr, shader);
   case nir_intrinsic_image_samples:
      return emit_image_samples(intr, shader);
   case nir_intrinsic_get_ssbo_size:
      return emit_ssbo_size(intr, shader);
   default:
      return false;
   }
}

bool
RatInstr::emit_ssbo_load(nir_intrinsic_instr *intr, Shader& shader)
{
   auto& vf = shader.value_factory();
   auto dest = vf.dest_vec4(intr->def, pin_group);

   int components = intr->def.bit_size / 32 * intr->def.num_components;

   /** src0 not used, should be some offset */
   auto addr = vf.src(intr->src[1], 0);
   auto addr_temp = vf.temp_register();

   /** Should be lowered in nir */
   shader.emit_instruction(
      new AluInstr(op2_lshr_int, addr_temp, addr, vf.literal(2), AluInstr::write));

   const EVTXDataFormat formats[4] = {fmt_32, fmt_32_32, fmt_32_32_32, fmt_32_32_32_32};

   RegisterVec4::Swizzle dest_swz[4] = {
      {0, 7, 7, 7},
      {0, 1, 7, 7},
      {0, 1, 2, 7},
      {0, 1, 2, 3}
   };

   int comp_idx = components - 1;

   auto [offset, res_offset] = shader.evaluate_resource_offset(intr, 0);

   auto res_id = R600_IMAGE_REAL_RESOURCE_OFFSET + offset + shader.ssbo_image_offset();

   auto ir = new LoadFromBuffer(
      dest, dest_swz[comp_idx], addr_temp, 0, res_id, res_offset, formats[comp_idx]);
   ir->set_fetch_flag(FetchInstr::use_tc);
   ir->set_num_format(vtx_nf_int);

   shader.emit_instruction(ir);
   return true;
}

bool
RatInstr::emit_global_store(nir_intrinsic_instr *intr, Shader& shader)
{
   auto& vf = shader.value_factory();
   auto addr_orig = vf.src(intr->src[1], 0);
   auto addr_vec = vf.temp_vec4(pin_chan, {0, 7, 7, 7});

   shader.emit_instruction(
      new AluInstr(op2_lshr_int, addr_vec[0], addr_orig, vf.literal(2), AluInstr::write));

   RegisterVec4::Swizzle value_swz = {0,7,7,7};
   auto mask = nir_intrinsic_write_mask(intr);
   for (int i = 0; i < 4; ++i) {
      if (mask & (1 << i))
         value_swz[i] = i;
   }

   auto value_vec = vf.temp_vec4(pin_chgr, value_swz);

   AluInstr *ir = nullptr;
   for (int i = 0; i < 4; ++i) {
      if (value_swz[i] < 4) {
         ir = new AluInstr(op1_mov, value_vec[i],
                           vf.src(intr->src[0], i), AluInstr::write);
         shader.emit_instruction(ir);
      }
   }

   auto store = new RatInstr(cf_mem_rat_cacheless,
                             RatInstr::STORE_RAW,
                             value_vec,
                             addr_vec,
                             shader.ssbo_image_offset(),
                             nullptr,
                             1,
                             mask,
                             0);
   shader.emit_instruction(store);
   /* Ensure at least one ACK-tracked store in this sequence so a later
    * WAIT_ACK can observe retirement on Evergreen compute tails. */
   store->set_ack();
   return true;
}

bool
RatInstr::emit_global_atomic_op(nir_intrinsic_instr *intr, Shader& shader)
{
   auto& vf = shader.value_factory();

   bool read_result = !list_is_empty(&intr->def.uses);
   auto opcode = read_result ? get_rat_opcode(nir_intrinsic_atomic_op(intr))
                             : get_rat_opcode_wo(nir_intrinsic_atomic_op(intr));

   auto coord_orig = vf.src(intr->src[0], 0);
   auto coord = vf.temp_register(0);

   auto data_vec4 = vf.temp_vec4(pin_chgr, {0, 1, 2, 3});

   shader.emit_instruction(
      new AluInstr(op2_lshr_int, coord, coord_orig, vf.literal(2), AluInstr::write));

   shader.emit_instruction(
      new AluInstr(op1_mov, data_vec4[1], shader.rat_return_address(), AluInstr::write));

   if (intr->intrinsic == nir_intrinsic_global_atomic_swap) {
      shader.emit_instruction(
         new AluInstr(op1_mov, data_vec4[0], vf.src(intr->src[2], 0), AluInstr::write));
      shader.emit_instruction(
         new AluInstr(op1_mov,
                      data_vec4[shader.chip_class() == ISA_CC_CAYMAN ? 2 : 3],
                      vf.src(intr->src[1], 0),
                      AluInstr::write));
   } else {
      shader.emit_instruction(
         new AluInstr(op1_mov, data_vec4[0], vf.src(intr->src[1], 0), AluInstr::write));
   }

   RegisterVec4 out_vec(coord, coord, coord, coord, pin_chgr);

   auto atomic = new RatInstr(cf_mem_rat,
                               opcode,
                               data_vec4,
                               out_vec,
                               shader.ssbo_image_offset(),
                               nullptr,
                               1,
                               0xf,
                               0);
   shader.emit_instruction(atomic);

   atomic->set_ack();
   if (read_result) {
      atomic->set_instr_flag(ack_rat_return_write);
      auto dest = vf.dest_vec4(intr->def, pin_group);

      auto wait = new ControlFlowInstr(ControlFlowInstr::cf_wait_ack);
      wait->add_required_instr(atomic);
      shader.emit_instruction(wait);

      auto fetch = new FetchInstr(vc_fetch,
                                  dest,
                                  {0, 1, 2, 3},
                                  shader.rat_return_address(),
                                  0,
                                  no_index_offset,
                                  fmt_32,
                                  vtx_nf_int,
                                  vtx_es_none,
                                  R600_IMAGE_IMMED_RESOURCE_OFFSET + shader.ssbo_image_offset(),
                                  nullptr);
      fetch->set_mfc(15);
      fetch->set_fetch_flag(FetchInstr::srf_mode);
      fetch->set_fetch_flag(FetchInstr::use_tc);
      fetch->set_fetch_flag(FetchInstr::vpm);
      fetch->add_required_instr(wait);
      shader.chain_ssbo_read(fetch);
      shader.emit_instruction(fetch);
   }

   return true;
}

bool
RatInstr::emit_uav_store_r600(nir_intrinsic_instr *intr, Shader& shader)
{
   /* uav_instr_r600 source layout:
    *   src[0] = uav_array_index (scalar, selects RAT slot)
    *   src[1] = coord (address, variable components)
    *   src[2] = value (data to write, variable components)
    *   src[3] = compare_value (for CAS, unused for stores)
    *   index: UAV_OP_R600 = operation type, ID_BASE = base resource
    */
   auto& vf = shader.value_factory();

   unsigned rat_id = nir_intrinsic_id_base(intr);
   PRegister rat_id_offset = nullptr;
   const nir_const_value * const uav_offset_const = nir_src_as_const_value(intr->src[0]);
   if (uav_offset_const != nullptr) {
      rat_id += uav_offset_const->u32;
   } else {
      rat_id_offset = vf.src(intr->src[0], 0)->as_register();
   }

   auto coord = vf.temp_vec4(pin_chgr, {0, 1, 2, 3});
   unsigned coord_components = MIN2(nir_src_num_components(intr->src[1]), 4u);
   for (unsigned i = 0; i < coord_components; ++i) {
      shader.emit_instruction(
         new AluInstr(op1_mov, coord[i], vf.src(intr->src[1], i), AluInstr::write));
   }
   for (unsigned i = coord_components; i < 4; ++i) {
      shader.emit_instruction(new AluInstr(op1_mov, coord[i], vf.zero(), AluInstr::write));
   }

   auto data_vec4 = vf.temp_vec4(pin_chgr, {0, 1, 2, 3});
   for (unsigned i = 0; i < 4; ++i) {
      shader.emit_instruction(new AluInstr(op1_mov, data_vec4[i], vf.zero(), AluInstr::write));
   }

   unsigned const uav_op = nir_intrinsic_uav_op_r600(intr);
   unsigned const uav_op_base = uav_op & 0x1F;
   unsigned const value_components = MIN2(nir_src_num_components(intr->src[2]), 4u);
   bool const cmpxchg_op = uav_op_base == RatInstr::CMPXCHG_INT ||
                           uav_op_base == RatInstr::CMPXCHG_FLT ||
                           uav_op_base == RatInstr::CMPXCHG_FDENORM;

   if (cmpxchg_op) {
      shader.emit_instruction(
         new AluInstr(op1_mov, data_vec4[0], vf.src(intr->src[2], 0), AluInstr::write));
      shader.emit_instruction(new AluInstr(op1_mov,
                                           data_vec4[shader.chip_class() == ISA_CC_CAYMAN ? 2 : 3],
                                           vf.src(intr->src[3], 0),
                                           AluInstr::write));
   } else if (uav_op_base == RatInstr::STORE_TYPED || uav_op_base == RatInstr::STORE_RAW) {
      for (unsigned i = 0; i < value_components; ++i) {
         shader.emit_instruction(
            new AluInstr(op1_mov, data_vec4[i], vf.src(intr->src[2], i), AluInstr::write));
      }
   } else if (uav_op_base != RatInstr::NOP) {
      shader.emit_instruction(
         new AluInstr(op1_mov, data_vec4[0], vf.src(intr->src[2], 0), AluInstr::write));
   }

   unsigned comp_mask = 1;
   if (uav_op_base == RatInstr::STORE_TYPED || uav_op_base == RatInstr::STORE_RAW) {
      comp_mask = value_components ? ((1u << value_components) - 1u) : 1u;
   }

   bool const scalar_buffer_store =
      uav_op_base == RatInstr::STORE_TYPED && coord_components == 1 && value_components == 1;
   /* Use uav_op_base (low 5 bits), not full uav_op, so my elem-size encoding
    * in bits [6:5] doesn't leak into the RAT opcode field which would
    * trigger the disassembler's bounds-check assertion. */
   unsigned const rat_opcode = scalar_buffer_store ? RatInstr::STORE_RAW : uav_op_base;

   /* ELEM_SIZE encodes "doublewords per array element" via bits {0,1,3}
    * for {1,2,4} dwords (Evergreen_ISA.pdf §10.18; "3 is not supported").
    * Hardcoded ELEM_SIZE=0 (= 1 dword) caused MEM_RAT_STORE_TYPED on
    * multi-dword UINT formats (notably r32g32_uint) to drop the FIRST
    * dword (R channel) in cold-context silicon state -- silicon strictly
    * honored "1 dword" while COMP_MASK said "all N channels".  In
    * warm-context, silicon's optional fallback cache wrote all dwords,
    * producing the previously-mysterious non-monotonic latch behavior.
    *
    * The terakan NIR lowering pass encodes the format-derived
    * elem_size_minus_one in uav_op high bits [6:5]; recover it here.
    * value_components from the (NIR-padded) value vector is too
    * coarse: shaders pass uvec4 to imageStore even for narrower
    * formats, so the NIR vec component count = always 4 for terakan.
    * The format-derived signal carried via uav_op is the only correct
    * source.  See steinmarder finding tranche-16 + CLAIMS C-2026-04-22-43.
    *
    * For non-terakan (gallium r600 OpenGL/CL) callers that don't encode
    * elem_size in uav_op, the high bits stay 0 -> elem_size_minus_one = 0
    * (preserves prior behavior). */
   unsigned elem_size_minus_one = (uav_op >> 5) & 0x3u;

   if (scalar_buffer_store)
      shader.start_new_block(0);

   sfn_log << SfnLog::trans
           << "RAT_ATOMIC_EMIT path=uav_store"
           << " intrinsic=" << nir_intrinsic_infos[intr->intrinsic].name
           << " uav_op=" << uav_op
           << " uav_op_base=" << uav_op_base
           << " rat_opcode=" << rat_opcode
           << " rat_id=" << rat_id
           << " returning=0"
           << " cmpxchg=" << cmpxchg_op
           << " coord=" << coord
           << " data=" << data_vec4
           << " comp_mask=" << CLAMP(comp_mask, 1u, 0xFu)
           << " burst_count=1"
           << " elem_size=" << elem_size_minus_one << "\n";
   if (cmpxchg_op) {
      sfn_log << SfnLog::trans
              << "RAT_CMPXCHG_MAP path=uav_store replacement=src2.x->data.x"
              << " compare=src3.x->data."
              << (shader.chip_class() == ISA_CC_CAYMAN ? "z" : "w") << "\n";
   }

   auto store = new RatInstr((scalar_buffer_store || uav_op_base == RatInstr::STORE_RAW)
                                 ? cf_mem_rat_cacheless
                                 : cf_mem_rat,
                             static_cast<RatInstr::ERatOp>(rat_opcode),
                             data_vec4,
                             coord,
                             rat_id,
                             rat_id_offset,
                             1,
                             CLAMP(comp_mask, 1u, 0xFu),
                             elem_size_minus_one);
   shader.emit_instruction(store);
   store->set_ack();
   return true;
}

bool
RatInstr::emit_uav_returning_instr_r600(nir_intrinsic_instr *intr, Shader& shader)
{
   auto& vf = shader.value_factory();

   unsigned rat_id = nir_intrinsic_id_base(intr);
   unsigned return_id = nir_intrinsic_uav_return_id_base_r600(intr);
   PRegister rat_id_offset = nullptr;
   PRegister return_id_offset = nullptr;
   const nir_const_value * const uav_offset_const = nir_src_as_const_value(intr->src[0]);
   if (uav_offset_const != nullptr) {
      rat_id += uav_offset_const->u32;
      return_id += uav_offset_const->u32;
   } else {
      rat_id_offset = vf.src(intr->src[0], 0)->as_register();
      return_id_offset = rat_id_offset;
   }

   auto coord = vf.temp_vec4(pin_chgr, {0, 1, 2, 3});
   unsigned coord_components = nir_src_num_components(intr->src[1]);
   coord_components = MIN2(coord_components, 4u);
   for (unsigned i = 0; i < coord_components; ++i) {
      shader.emit_instruction(
         new AluInstr(op1_mov, coord[i], vf.src(intr->src[1], i), AluInstr::write));
   }
   for (unsigned i = coord_components; i < 4; ++i) {
      shader.emit_instruction(new AluInstr(op1_mov, coord[i], vf.zero(), AluInstr::write));
   }

   auto data_vec4 = vf.temp_vec4(pin_chgr, {0, 1, 2, 3});
   shader.emit_instruction(
      new AluInstr(op1_mov, data_vec4[1], shader.rat_return_address(), AluInstr::write));

   unsigned const uav_op = nir_intrinsic_uav_op_r600(intr);
   unsigned const uav_op_base = uav_op & 0x1F;
   bool const cmpxchg_op = uav_op_base == RatInstr::CMPXCHG_INT ||
                           uav_op_base == RatInstr::CMPXCHG_FLT ||
                           uav_op_base == RatInstr::CMPXCHG_FDENORM;
   if (cmpxchg_op) {
      shader.emit_instruction(
         new AluInstr(op1_mov, data_vec4[0], vf.src(intr->src[2], 0), AluInstr::write));
      shader.emit_instruction(
         new AluInstr(op1_mov,
                      data_vec4[shader.chip_class() == ISA_CC_CAYMAN ? 2 : 3],
                      vf.src(intr->src[3], 0),
                      AluInstr::write));
   } else if (uav_op_base != RatInstr::NOP) {
      shader.emit_instruction(
         new AluInstr(op1_mov, data_vec4[0], vf.src(intr->src[2], 0), AluInstr::write));
      shader.emit_instruction(new AluInstr(op1_mov, data_vec4[2], vf.zero(), AluInstr::write));
   }

   sfn_log << SfnLog::trans
           << "RAT_ATOMIC_EMIT path=uav_returning"
           << " intrinsic=" << nir_intrinsic_infos[intr->intrinsic].name
           << " uav_op=" << uav_op
           << " uav_op_base=" << uav_op_base
           << " rat_opcode=" << uav_op
           << " rat_id=" << rat_id
           << " return_id=" << return_id
           << " returning=1"
           << " cmpxchg=" << cmpxchg_op
           << " coord=" << coord
           << " data=" << data_vec4
           << " comp_mask=15"
           << " burst_count=1"
           << " elem_size=0"
           << " return_address=" << *shader.rat_return_address() << "\n";
   if (cmpxchg_op) {
      sfn_log << SfnLog::trans
              << "RAT_CMPXCHG_MAP path=uav_returning replacement=src2.x->data.x"
              << " compare=src3.x->data."
              << (shader.chip_class() == ISA_CC_CAYMAN ? "z" : "w") << "\n";
   }

   auto rat = new RatInstr(cf_mem_rat,
                           static_cast<RatInstr::ERatOp>(uav_op),
                           data_vec4,
                           coord,
                           rat_id,
                           rat_id_offset,
                           1,
                           0xf,
                           0);
   shader.emit_instruction(rat);
   rat->set_ack();
   rat->set_instr_flag(ack_rat_return_write);

   auto dest = vf.dest_vec4(intr->def, pin_group);
   auto wait = new ControlFlowInstr(ControlFlowInstr::cf_wait_ack);
   wait->add_required_instr(rat);
   shader.chain_ssbo_read(wait);
   shader.emit_instruction(wait);

   auto fetch = new FetchInstr(vc_fetch,
                               dest,
                               {0, 1, 2, 3},
                               shader.rat_return_address(),
                               0,
                               no_index_offset,
                               fmt_32_32_32_32,
                               vtx_nf_int,
                               vtx_es_none,
                               return_id,
                               return_id_offset);
   unsigned mega_fetch_count = nir_intrinsic_mega_fetch_count_r600(intr);
   if (mega_fetch_count == 0)
      mega_fetch_count = sizeof(uint32_t) * intr->def.num_components;
   fetch->set_mfc(CLAMP(mega_fetch_count, 1u, 16u) - 1);
   sfn_log << SfnLog::trans
           << "RAT_RETURN_FETCH path=uav_returning"
           << " rat_opcode=" << uav_op
           << " rat_id=" << rat_id
           << " return_id=" << return_id
           << " return_address=" << *shader.rat_return_address()
           << " fetch_resource=" << return_id
           << " fetch_format=fmt_32_32_32_32"
           << " fetch_mfc=" << (CLAMP(mega_fetch_count, 1u, 16u) - 1)
           << " wait_ack=1"
           << " ack=1"
           << " ack_rat_return_write=1"
           << " dest=" << dest << "\n";
   if (return_id_offset)
      sfn_log << SfnLog::trans
              << "RAT_RETURN_FETCH_OFFSET path=uav_returning offset="
              << *return_id_offset << "\n";
   fetch->set_fetch_flag(FetchInstr::use_tc);
   fetch->set_fetch_flag(FetchInstr::vpm);
   fetch->add_required_instr(wait);
   shader.emit_instruction(fetch);
   shader.chain_ssbo_read(fetch);

   return true;
}

bool
RatInstr::emit_ssbo_store(nir_intrinsic_instr *instr, Shader& shader)
{
   auto& vf = shader.value_factory();
   auto orig_addr = vf.src(instr->src[2], 0);

   auto addr_base = vf.temp_register();

   auto [offset, rat_id] = shader.evaluate_resource_offset(instr, 1);
   const unsigned wrmask = nir_intrinsic_write_mask(instr);

   shader.emit_instruction(
      new AluInstr(op2_lshr_int, addr_base, orig_addr, vf.literal(2), AluInstr::write));

   for (unsigned i = 0; i < nir_src_num_components(instr->src[0]); ++i) {
      if (!(BITFIELD_BIT(i) & wrmask))
         continue;

      auto addr_vec = vf.temp_vec4(pin_group, {0, 1, 2, 7});
      if (i == 0) {
         shader.emit_instruction(
            new AluInstr(op1_mov, addr_vec[0], addr_base, AluInstr::write));
      } else {
         shader.emit_instruction(new AluInstr(op2_add_int,
                                              addr_vec[0],
                                              addr_base,
                                              vf.literal(i),
                                              AluInstr::write));
      }
      auto value = vf.src(instr->src[0], i);
      PRegister v = vf.temp_register(0);
      shader.emit_instruction(new AluInstr(op1_mov, v, value, AluInstr::write));
      auto value_vec = RegisterVec4(v, nullptr, nullptr, nullptr, pin_chan);
      auto store = new RatInstr(cf_mem_rat,
                                RatInstr::STORE_TYPED,
                                value_vec,
                                addr_vec,
                                offset + shader.ssbo_image_offset(),
                                rat_id,
                                1,
                                1,
                                0);
      shader.emit_instruction(store);
      /* Track store completion for explicit WAIT_ACK termination sequences. */
      store->set_ack();
   }

   return true;
}

bool
RatInstr::emit_ssbo_atomic_op(nir_intrinsic_instr *intr, Shader& shader)
{
   auto& vf = shader.value_factory();
   auto [imageid, image_offset] = shader.evaluate_resource_offset(intr, 0);
   const unsigned res_id = imageid + shader.ssbo_image_offset();

   bool read_result = !list_is_empty(&intr->def.uses);
   auto opcode = read_result ? get_rat_opcode(nir_intrinsic_atomic_op(intr))
                             : get_rat_opcode_wo(nir_intrinsic_atomic_op(intr));

   auto coord_orig = vf.src(intr->src[1], 0);
   auto coord = vf.temp_register(0);

   auto data_vec4 = vf.temp_vec4(pin_chgr, {0, 1, 2, 3});

   shader.emit_instruction(
      new AluInstr(op2_lshr_int, coord, coord_orig, vf.literal(2), AluInstr::write));

   shader.emit_instruction(
      new AluInstr(op1_mov, data_vec4[1], shader.rat_return_address(), AluInstr::write));

   if (intr->intrinsic == nir_intrinsic_ssbo_atomic_swap) {
      shader.emit_instruction(
         new AluInstr(op1_mov, data_vec4[0], vf.src(intr->src[3], 0), AluInstr::write));
      shader.emit_instruction(
         new AluInstr(op1_mov,
                      data_vec4[shader.chip_class() == ISA_CC_CAYMAN ? 2 : 3],
                      vf.src(intr->src[2], 0),
                      AluInstr::write));
   } else {
      shader.emit_instruction(
         new AluInstr(op1_mov, data_vec4[0], vf.src(intr->src[2], 0), AluInstr::write));
   }
   bool const cmpxchg_op = intr->intrinsic == nir_intrinsic_ssbo_atomic_swap;

   sfn_log << SfnLog::trans
           << "RAT_ATOMIC_EMIT path=ssbo_atomic"
           << " intrinsic=" << nir_intrinsic_infos[intr->intrinsic].name
           << " rat_opcode=" << opcode
           << " rat_id=" << res_id
           << " returning=" << read_result
           << " cmpxchg=" << cmpxchg_op
           << " coord=" << coord
           << " data=" << data_vec4
           << " comp_mask=15"
           << " burst_count=1"
           << " elem_size=0"
           << " return_address=" << *shader.rat_return_address() << "\n";
   if (cmpxchg_op) {
      sfn_log << SfnLog::trans
              << "RAT_CMPXCHG_MAP path=ssbo_atomic replacement=src3.x->data.x"
              << " compare=src2.x->data."
              << (shader.chip_class() == ISA_CC_CAYMAN ? "z" : "w") << "\n";
   }

   RegisterVec4 out_vec(coord, coord, coord, coord, pin_chgr);

   auto atomic =
      new RatInstr(cf_mem_rat, opcode, data_vec4, out_vec, res_id, image_offset, 1, 0xf, 0);
   shader.emit_instruction(atomic);

   atomic->set_ack();
   if (read_result) {
      atomic->set_instr_flag(ack_rat_return_write);
      auto dest = vf.dest_vec4(intr->def, pin_group);

      auto wait = new ControlFlowInstr(ControlFlowInstr::cf_wait_ack);
      wait->add_required_instr(atomic);
      shader.emit_instruction(wait);

      auto fetch = new FetchInstr(vc_fetch,
                                  dest,
                                  {0, 1, 2, 3},
                                  shader.rat_return_address(),
                                  0,
                                  no_index_offset,
                                  fmt_32,
                                  vtx_nf_int,
                                  vtx_es_none,
                                  R600_IMAGE_IMMED_RESOURCE_OFFSET + res_id,
                                  image_offset);
      fetch->set_mfc(15);
      fetch->set_fetch_flag(FetchInstr::srf_mode);
      fetch->set_fetch_flag(FetchInstr::use_tc);
      fetch->set_fetch_flag(FetchInstr::vpm);
      sfn_log << SfnLog::trans
              << "RAT_RETURN_FETCH path=ssbo_atomic"
              << " rat_opcode=" << opcode
              << " rat_id=" << res_id
              << " fetch_resource=" << (R600_IMAGE_IMMED_RESOURCE_OFFSET + res_id)
              << " fetch_format=fmt_32"
              << " fetch_mfc=15"
              << " wait_ack=1"
              << " ack=1"
              << " ack_rat_return_write=1"
              << " return_address=" << *shader.rat_return_address()
              << " dest=" << dest << "\n";
      fetch->add_required_instr(wait);
      shader.chain_ssbo_read(fetch);
      shader.emit_instruction(fetch);
   }

   return true;
}

bool
RatInstr::emit_ssbo_size(nir_intrinsic_instr *intr, Shader& shader)
{
   auto& vf = shader.value_factory();
   auto dest = vf.dest_vec4(intr->def, pin_group);

   auto const_offset = nir_src_as_const_value(intr->src[0]);
   int res_id = R600_IMAGE_REAL_RESOURCE_OFFSET;
   if (const_offset)
      res_id += const_offset[0].u32;
   else
      assert(0 && "dynamic buffer offset not supported in buffer_size");

   shader.emit_instruction(new QueryBufferSizeInstr(dest, {0, 1, 2, 3}, res_id));
   return true;
}

bool
RatInstr::emit_image_store(nir_intrinsic_instr *intrin, Shader& shader)
{
   auto& vf = shader.value_factory();
   auto [imageid, image_offset] = shader.evaluate_resource_offset(intrin, 0);
   {
   }

   auto coord_load = vf.src_vec4(intrin->src[1], pin_chan);
   auto coord = vf.temp_vec4(pin_chgr);

   auto value_load = vf.src_vec4(intrin->src[3], pin_chan);
   auto value = vf.temp_vec4(pin_chgr);

   RegisterVec4::Swizzle swizzle = {0, 1, 2, 3};
   if (nir_intrinsic_image_dim(intrin) == GLSL_SAMPLER_DIM_1D &&
       nir_intrinsic_image_array(intrin))
      swizzle = {0, 2, 1, 3};

   for (int i = 0; i < 4; ++i) {
      auto flags = i != 3 ? AluInstr::write : AluInstr::write;
      shader.emit_instruction(
         new AluInstr(op1_mov, coord[swizzle[i]], coord_load[i], flags));
   }
   for (int i = 0; i < 4; ++i) {
      auto flags = i != 3 ? AluInstr::write : AluInstr::write;
      shader.emit_instruction(new AluInstr(op1_mov, value[i], value_load[i], flags));
   }

   auto op = cf_mem_rat; // nir_intrinsic_access(intrin) & ACCESS_COHERENT ?
                         // cf_mem_rat_cacheless : cf_mem_rat;

   /* COMP_MASK must reflect the image format's channel count, not be
    * hardcoded to 0xF.  Hardcoding caused MEM_RAT_STORE_TYPED on
    * multi-channel formats (r32g32_uint, r8g8b8a8_uint, etc.) to
    * overrun into adjacent texels because the hardware interprets
    * "write 4 channels" against a sub-vec4 texel as "stride forward
    * by texel-channels-bytes per channel and write all 4".  For
    * single-channel destinations the extra channels were quietly
    * dropped (matching descriptor DST_SEL), so r32_sint passed.
    *
    * Derive channel count from the format; fall back to 0xF for
    * formats with no usable channel count (PIPE_FORMAT_NONE in
    * older NIR pipelines that don't propagate format). */
   pipe_format const store_format = nir_intrinsic_format(intrin);
   unsigned comp_mask = 0xf;
   unsigned elem_size_minus_one = 0;
   if (store_format != PIPE_FORMAT_NONE) {
      /* Only narrow COMP_MASK when the texel is at least one full
       * dword per channel (32-bit formats like r32_*, r32g32_*,
       * r32g32b32a32_*).  Sub-dword formats (r8_*, r16_*, packed
       * RGBA8 etc.) need COMP_MASK=0xF because MEM_RAT_STORE_TYPED
       * with a narrow mask on a sub-dword texel mis-handles the
       * RMW the descriptor's NUM_FORMAT/COMP_SWAP performs to pack
       * the value into the destination byte/word lanes.
       *
       * Empirical evidence (Wrestler HD 6310, 2026-04-20):
       *   r32g32_uint with comp_mask=0xF -> max diff (N,N) per slice
       *   r32g32_uint with comp_mask=0x3 -> PASS
       *   r8_uint     with comp_mask=0xF -> PASS
       *   r8_uint     with comp_mask=0x1 -> max diff 63 (all wrong)
       *
       * In the same multi-dword case we ALSO need ELEM_SIZE to encode
       * the actual dword count (Evergreen_ISA.pdf §10.18: "Number of
       * doublewords per array element, minus one. ... value in [1,2,4]
       * (3 is not supported)").  Hardcoded ELEM_SIZE=0 (= 1 dword)
       * caused MEM_RAT_STORE_TYPED on r32g32_uint to drop the FIRST
       * dword (R channel) in cold-context silicon state -- silicon
       * strictly honored "1 dword per element" while COMP_MASK said
       * "all 4 channels", and only the second dword (G) landed.
       * In warm-context, silicon's optional fallback cache wrote
       * both dwords, producing the previously-mysterious
       * non-monotonic latch behavior.
       *
       * Empirical evidence (Wrestler HD 6310, 2026-04-22):
       *   r32g32_uint truly-solo with elem_size=0 -> R_actual = 0
       *     universally across 4032/4096 pixels (max diff 63 in R)
       *   PNG-decoded readback proves silicon writes only G dword
       *
       * 3-channel formats (r32g32b32_*) are NOT supported by the
       * silicon's ELEM_SIZE encoding (bit pattern 0b10 has no defined
       * value; spec says "3 is not supported").  Per terakan's format
       * advertisement, those formats should not be reported as
       * STORAGE_IMAGE-capable; this code conservatively leaves
       * elem_size=0 for them.
       *
       * See steinmarder finding
       * 2026-04-22-tranche16-silicon-drops-r-dword-cold-context.md
       * and CLAIMS C-2026-04-22-43. */
      unsigned const channels = util_format_get_nr_components(store_format);
      unsigned const texel_bytes = util_format_get_blocksize(store_format);
      if (channels > 0 && channels <= 4 && texel_bytes >= 4 * channels) {
         comp_mask = (1u << channels) - 1u;
         /* ELEM_SIZE encodes value-minus-one in {0,1,3} for {1,2,4}
          * dwords.  Channels==3 (3-dword) has no valid encoding and
          * is silently kept at elem_size=0 (degenerate; format should
          * not have been advertised as storage-image-supported). */
         if (channels == 1)
            elem_size_minus_one = 0;
         else if (channels == 2)
            elem_size_minus_one = 1;
         else if (channels == 4)
            elem_size_minus_one = 3;
         /* channels == 3 falls through with elem_size_minus_one = 0 */
      }
   }

   auto store = new RatInstr(
      op, RatInstr::STORE_TYPED, value, coord, imageid, image_offset, 1, comp_mask,
      elem_size_minus_one);

   store->set_ack();
   if (nir_intrinsic_access(intrin) & ACCESS_INCLUDE_HELPERS)
      store->set_instr_flag(Instr::helper);

   shader.emit_instruction(store);
   return true;
}

bool
RatInstr::emit_image_load_or_atomic(nir_intrinsic_instr *intrin, Shader& shader)
{
   auto& vf = shader.value_factory();
   auto [imageid, image_offset] = shader.evaluate_resource_offset(intrin, 0);
   {
   }

   bool read_result = !list_is_empty(&intrin->def.uses);
   bool image_load = (intrin->intrinsic == nir_intrinsic_image_load);
   auto opcode = image_load  ? RatInstr::NOP_RTN :
                 read_result ? get_rat_opcode(nir_intrinsic_atomic_op(intrin))
                             : get_rat_opcode_wo(nir_intrinsic_atomic_op(intrin));

   auto coord_orig = vf.src_vec4(intrin->src[1], pin_chan);
   auto coord = vf.temp_vec4(pin_chgr);

   auto data_vec4 = vf.temp_vec4(pin_chgr, {0, 1, 2, 3});

   RegisterVec4::Swizzle swizzle = {0, 1, 2, 3};
   if (nir_intrinsic_image_dim(intrin) == GLSL_SAMPLER_DIM_1D &&
       nir_intrinsic_image_array(intrin))
      swizzle = {0, 2, 1, 3};

   for (int i = 0; i < 4; ++i) {
      auto flags = i != 3 ? AluInstr::write : AluInstr::write;
      shader.emit_instruction(
         new AluInstr(op1_mov, coord[swizzle[i]], coord_orig[i], flags));
   }

   shader.emit_instruction(
      new AluInstr(op1_mov, data_vec4[1], shader.rat_return_address(), AluInstr::write));

   if (intrin->intrinsic == nir_intrinsic_image_atomic_swap) {
      shader.emit_instruction(
         new AluInstr(op1_mov, data_vec4[0], vf.src(intrin->src[4], 0), AluInstr::write));
      shader.emit_instruction(
         new AluInstr(op1_mov,
                      data_vec4[shader.chip_class() == ISA_CC_CAYMAN ? 2 : 3],
                      vf.src(intrin->src[3], 0),
                      AluInstr::write));
   } else {
      shader.emit_instruction(
         new AluInstr(op1_mov, data_vec4[0], vf.src(intrin->src[3], 0), AluInstr::write));
      shader.emit_instruction(
         new AluInstr(op1_mov, data_vec4[2], vf.zero(), AluInstr::write));
   }

   auto atomic =
      new RatInstr(cf_mem_rat, opcode, data_vec4, coord, imageid, image_offset, 1, 0xf, 0);
   shader.emit_instruction(atomic);
   atomic->set_ack();

   if (read_result) {
      atomic->set_instr_flag(ack_rat_return_write);
      auto dest = vf.dest_vec4(intrin->def, pin_group);

      auto wait = new ControlFlowInstr(ControlFlowInstr::cf_wait_ack);

      shader.chain_ssbo_read(wait);
      shader.emit_instruction(wait);

      pipe_format format = nir_intrinsic_format(intrin);
      unsigned fmt = fmt_32;
      unsigned num_format = 0;
      unsigned format_comp = 0;
      unsigned endian = 0;
      r600_vertex_data_type(format, &fmt, &num_format, &format_comp, &endian);

      auto fetch = new FetchInstr(vc_fetch,
                                  dest,
                                  {0, 1, 2, 3},
                                  shader.rat_return_address(),
                                  0,
                                  no_index_offset,
                                  (EVTXDataFormat)fmt,
                                  (EVFetchNumFormat)num_format,
                                  (EVFetchEndianSwap)endian,
                                  R600_IMAGE_IMMED_RESOURCE_OFFSET + imageid,
                                  image_offset);
      fetch->set_mfc(3);
      fetch->set_fetch_flag(FetchInstr::use_tc);
      fetch->set_fetch_flag(FetchInstr::vpm);
      fetch->add_required_instr(wait);
      if (format_comp)
         fetch->set_fetch_flag(FetchInstr::format_comp_signed);

      shader.emit_instruction(fetch);
      shader.chain_ssbo_read(fetch);
   }

   return true;
}

#define R600_SHADER_BUFFER_INFO_SEL (512 + R600_BUFFER_INFO_OFFSET / 16)

bool
RatInstr::emit_image_size(nir_intrinsic_instr *intrin, Shader& shader)
{
   auto& vf = shader.value_factory();

   auto src = RegisterVec4(0, true, {4, 4, 4, 4});

   assert(nir_src_as_uint(intrin->src[1]) == 0);

   auto const_offset = nir_src_as_const_value(intrin->src[0]);
   PRegister dyn_offset = nullptr;

   int res_id = R600_IMAGE_REAL_RESOURCE_OFFSET + nir_intrinsic_range_base(intrin);
   if (const_offset)
      res_id += const_offset[0].u32;
   else
      dyn_offset = shader.emit_load_to_register(vf.src(intrin->src[0], 0));

   if (nir_intrinsic_image_dim(intrin) == GLSL_SAMPLER_DIM_BUF) {
      auto dest = vf.dest_vec4(intrin->def, pin_group);
      shader.emit_instruction(new QueryBufferSizeInstr(dest, {0, 1, 2, 3}, res_id));
      return true;
   } else {

      if (nir_intrinsic_image_dim(intrin) == GLSL_SAMPLER_DIM_CUBE &&
          nir_intrinsic_image_array(intrin) &&
          intrin->def.num_components > 2) {
         /* Need to load the layers from a const buffer */

         auto dest = vf.dest_vec4(intrin->def, pin_group);
         shader.emit_instruction(new TexInstr(TexInstr::get_resinfo,
                                              dest,
                                              {0, 1, 7, 3},
                                              src,
                                              res_id,
                                              dyn_offset));

         shader.set_flag(Shader::sh_txs_cube_array_comp);

         if (const_offset) {
            unsigned lookup_resid = (res_id - R600_IMAGE_REAL_RESOURCE_OFFSET) +
                                    shader.image_size_const_offset();
            shader.emit_instruction(
               new AluInstr(op1_mov,
                            dest[2],
                            vf.uniform(lookup_resid / 4 + R600_SHADER_BUFFER_INFO_SEL,
                                       lookup_resid % 4,
                                       R600_BUFFER_INFO_CONST_BUFFER),
                            AluInstr::write));
         } else {
            /* If the addressing is indirect we have to get the z-value by
             * using a binary search */
            auto addr = vf.temp_register();
            auto comp1 = vf.temp_register();
            auto comp2 = vf.temp_register();
            auto low_bit = vf.temp_register();
            auto high_bit = vf.temp_register();

            auto trgt = vf.temp_vec4(pin_group);

            shader.emit_instruction(new AluInstr(op2_lshr_int,
                                                 addr,
                                                 vf.src(intrin->src[0], 0),
                                                 vf.literal(2),
                                                 AluInstr::write));
            shader.emit_instruction(new AluInstr(op2_and_int,
                                                 low_bit,
                                                 vf.src(intrin->src[0], 0),
                                                 vf.one_i(),
                                                 AluInstr::write));
            shader.emit_instruction(new AluInstr(op2_and_int,
                                                 high_bit,
                                                 vf.src(intrin->src[0], 0),
                                                 vf.literal(2),
                                                 AluInstr::write));

            shader.emit_instruction(new LoadFromBuffer(trgt,
                                                       {0, 1, 2, 3},
                                                       addr,
                                                       R600_SHADER_BUFFER_INFO_SEL,
                                                       R600_BUFFER_INFO_CONST_BUFFER,
                                                       nullptr,
                                                       fmt_32_32_32_32_float));

            // this may be wrong
            shader.emit_instruction(new AluInstr(
               op3_cnde_int, comp1, high_bit, trgt[0], trgt[2], AluInstr::write));
            shader.emit_instruction(new AluInstr(op3_cnde_int,
                                                 comp2,
                                                 high_bit,
                                                 trgt[1],
                                                 trgt[3],
                                                 AluInstr::write));
            shader.emit_instruction(new AluInstr(op3_cnde_int,
                                                 dest[2],
                                                 low_bit,
                                                 comp1,
                                                 comp2,
                                                 AluInstr::write));
         }
      } else {
         auto dest = vf.dest_vec4(intrin->def, pin_group);
         shader.emit_instruction(new TexInstr(TexInstr::get_resinfo,
                                              dest,
                                              {0, 1, 2, 3},
                                              src,
                                              res_id,
                                              dyn_offset));
      }
   }
   return true;
}

bool
RatInstr::emit_image_samples(nir_intrinsic_instr *intrin, Shader& shader)
{
   auto& vf = shader.value_factory();

   auto src = RegisterVec4(0, true, {4, 4, 4, 4});

   auto tmp =  shader.value_factory().temp_vec4(pin_group);
   auto dest =  shader.value_factory().dest(intrin->def, 0, pin_free);

   auto const_offset = nir_src_as_const_value(intrin->src[0]);
   PRegister dyn_offset = nullptr;

   int res_id = R600_IMAGE_REAL_RESOURCE_OFFSET + nir_intrinsic_range_base(intrin);
   if (const_offset)
      res_id += const_offset[0].u32;
   else
      dyn_offset = shader.emit_load_to_register(vf.src(intrin->src[0], 0));

   shader.emit_instruction(new TexInstr(TexInstr::get_resinfo,
                                        tmp,
                                        {3, 7, 7, 7},
                                        src,
                                        res_id,
                                        dyn_offset));

   shader.emit_instruction(new AluInstr(op1_mov, dest, tmp[0], AluInstr::write));
   return true;
}

} // namespace r600

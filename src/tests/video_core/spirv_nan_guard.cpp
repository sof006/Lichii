// SPDX-License-Identifier: GPL-3.0-or-later

#include <span>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#define SPV_ENABLE_UTILITY_CODE
#include <spirv/unified1/spirv.hpp11>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"

namespace {
using namespace Shader;

std::vector<u32> Emit(Stage stage, bool nan_guard) {
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::Program program;
    program.stage = stage;
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                           {{}, IR::AbstractSyntaxNode::Type::Return}};
    IR::IREmitter ir{block};
    const IR::F32 value{ir.Imm32(0.5f)};
    if (stage == Stage::Fragment) {
        program.info.stores_frag_color[0] = true;
        for (u32 component = 0; component < 4; ++component) {
            ir.SetFragColor(0, component, value);
        }
    } else {
        for (const IR::Attribute attribute : {IR::Attribute::PositionX, IR::Attribute::PositionY,
                                              IR::Attribute::PositionZ, IR::Attribute::PositionW}) {
            program.info.stores.Set(attribute);
            ir.SetAttribute(attribute, value, ir.Imm32(0U));
        }
    }
    ir.Epilogue();
    Profile profile;
    profile.supported_spirv = 0x00010500;
    RuntimeInfo runtime_info{};
    runtime_info.color_output_types[0] = AttributeType::Float;
    runtime_info.nan_guard = nan_guard;
    Backend::Bindings bindings;
    return Backend::SPIRV::EmitSPIRV(profile, runtime_info, program, bindings);
}

size_t CountOps(std::span<const u32> code, spv::Op wanted) {
    size_t count{};
    for (size_t offset = 5; offset < code.size();) {
        const u32 size = code[offset] >> 16;
        REQUIRE(size != 0);
        if (static_cast<spv::Op>(code[offset] & 0xffffU) == wanted) {
            ++count;
        }
        offset += size;
    }
    return count;
}
} // Anonymous namespace

TEST_CASE("NaN guard checks fragment colors and positions only when asked",
          "[video_core][spirv][nan_guard]") {
    for (const Stage stage : {Stage::Fragment, Stage::VertexB}) {
        CAPTURE(static_cast<u32>(stage));
        const std::vector<u32> plain = Emit(stage, false);
        const std::vector<u32> guarded = Emit(stage, true);
        CHECK(CountOps(plain, spv::Op::OpIsNan) == 0);
        CHECK(CountOps(guarded, spv::Op::OpIsNan) == 4);
        CHECK(CountOps(guarded, spv::Op::OpSelectionMerge) ==
              CountOps(plain, spv::Op::OpSelectionMerge) + 1);
    }
}

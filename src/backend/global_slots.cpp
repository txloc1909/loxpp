#include "backend/global_slots.h"

#include "backend/chunk_decoder.h"
#include "exec_objects.h"
#include "object.h"
#include "value.h"

#include <algorithm>
#include <stdexcept>

namespace {

void collectInto(const DecodedFunction& node, std::vector<std::string>& out) {
    for (const DecodedInstruction& ins : node.instructions) {
        if (ins.op != Op::DEFINE_GLOBAL && ins.op != Op::GET_GLOBAL &&
            ins.op != Op::SET_GLOBAL) {
            continue;
        }
        Value v = node.function->chunk.getConstant(
            static_cast<uint16_t>(ins.constantIndex));
        if (!isString(v)) {
            throw std::runtime_error(
                "collectGlobalNames: global-variable name constant is not a "
                "string — decoder/compiler drift");
        }
        const ObjString* name = asObjString(v);
        out.emplace_back(name->chars.data(), name->chars.size());
    }
    for (const DecodedFunction& child : node.nested) {
        collectInto(child, out);
    }
}

} // namespace

namespace qbe {

std::vector<std::string> collectGlobalNames(const DecodedFunction& root) {
    std::vector<std::string> names;
    collectInto(root, names);
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

} // namespace qbe

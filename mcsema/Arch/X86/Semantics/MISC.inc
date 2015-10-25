
namespace {

template <typename D, typename S>
INSTR_ATTRIBS
static void LEA(State *, D dst, S src) {
  W(dst) = reinterpret_cast<uintptr_t>(src);
}

}  // namespace

DEF_INSTR(LEA_GPRv_AGEN_32) = LEA<R32W, M8>;
DEF_INSTR(LEA_GPRv_AGEN_64) = LEA<R64W, M8>;
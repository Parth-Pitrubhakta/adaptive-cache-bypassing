#include <cstdio>
#include <cstdint>
struct input_instr {
    uint64_t ip; uint8_t is_branch, branch_taken, dst_reg[2], src_reg[4];
    uint64_t dst_mem[2], src_mem[4];
};
static void emit(uint64_t ip, uint64_t addr) {
    input_instr ins{};
    ins.ip = ip; ins.src_reg[0] = 1; ins.dst_reg[0] = 2;
    if (addr) ins.src_mem[0] = addr;
    fwrite(&ins, sizeof(ins), 1, stdout);
}
int main() {
    for (uint64_t o = 0; o < 16ULL*1024*1024; o += 64) { emit(0x400100+(o&0xFFF), 0x10000000+o); emit(0x400104, 0); emit(0x400108, 0); }
    for (int r = 0; r < 4; r++) for (uint64_t o = 0; o < 3ULL*1024*1024; o += 64) { emit(0x401000+(o&0xFFF), 0x20000000+o); emit(0x401004, 0); }
    for (int r = 0; r < 20; r++) for (uint64_t o = 0; o < 128ULL*1024; o += 64) { emit(0x402000+(o&0xFFF), 0x30000000+o); emit(0x402004, 0); }
}

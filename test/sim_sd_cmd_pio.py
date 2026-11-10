"""Cycle-level simulation of src/sd_cmd.pio against a model SD host.

Covers both programs: sd_cmd (the CMD line) and sd_dat (DAT0 blocks for
CMD6 / ACMD51 / ACMD13).

Runs the assembled program (taken from build/sd_cmd.pio.h) on a small PIO
interpreter with the 2-cycle input synchroniser, clocks it with a host that
sends real command frames, and has a model CPU answer through the FIFOs with
a configurable latency. Checks that

  * every host frame reaches the RX FIFO intact (also back to back: CMD0
    followed by the next command after only NCC = 8 clocks),
  * responses come back bit-exact, MSB first, for 48- and 136-bit frames,
  * NCR (clocks between the command's end bit and the response start bit)
    stays within 2..64,
  * the card releases CMD after its end bit,
  * DAT0 blocks of different lengths, queued back to back, come out
    bit-exact after the programmed delay, with DAT0 released in between,
  * all of the above in each of the four launch phases (falling / rising
    edge, with and without the CLK input synchroniser), built the way
    sd_responder.c builds them: WAIT polarities flipped from the program's
    `launch` label on, plus INPUT_SYNC_BYPASS on CLK.

For each phase it also reports where, relative to the CLK falling edge,
the card's bits change (2-cycle synchroniser and PAD_NS of output delay
included). That is a report, not a check: which window a host accepts
depends on where it really samples. The Switch 2 rejected CMD responses
launched from the rising edge although a host sampling exactly on its
rising edge should have preferred them.

  python test/sim_sd_cmd_pio.py build/sd_cmd.pio.h
"""
import re
import sys


PAD_NS = 3.0    # PIO output register to pin


class Timing:
    """Collects CLK falling edges and the card's line transitions (in system
    clock cycles); reports where in the clock period the card changes."""

    def __init__(self, cycle_ns, period_cycles):
        self.cycle_ns = cycle_ns
        self.period_cycles = period_cycles
        self.falls, self.changes = [], []

    def report(self):
        offs = []
        for c in self.changes:
            prev = max((f for f in self.falls if f <= c), default=None)
            if prev is not None:
                offs.append((c - prev) * self.cycle_ns + PAD_NS)
        if not offs:
            return "no transitions"
        period = self.period_cycles * self.cycle_ns
        return (f"bits change {min(offs):.0f}-{max(offs):.0f} ns after the "
                f"falling edge (period {period:.0f} ns)")


def crc7(data):
    crc = 0
    for byte in data:
        for i in range(7, -1, -1):
            bit = (byte >> i) & 1
            top = (crc >> 6) & 1
            crc = (crc << 1) & 0x7F
            if bit ^ top:
                crc ^= 0x09
    return crc


def host_frame(index, arg):
    b = [0x40 | index, arg >> 24 & 0xFF, arg >> 16 & 0xFF, arg >> 8 & 0xFF,
         arg & 0xFF]
    b.append(crc7(b) << 1 | 1)
    return b


def bits_of(data, nbits):
    out = []
    for byte in data:
        for i in range(7, -1, -1):
            out.append((byte >> i) & 1)
    return out[:nbits]


def response_for(index, arg):
    """The model CPU: a few fixed answers, enough to exercise the PIO."""
    if index == 0:
        return None
    if index == 8:
        b = [0x08, arg >> 24 & 0xFF, arg >> 16 & 0xFF, arg >> 8 & 0xFF,
             arg & 0xFF]
        return b + [crc7(b) << 1 | 1], 48
    if index == 55:
        b = [55, 0, 0, 0x01, 0x20]
        return b + [crc7(b) << 1 | 1], 48
    if index == 2:
        reg = [0xA5, 0x5A, 0x01, 0x80, 0xFF, 0x00, 0x13, 0x37,
               0xDE, 0xAD, 0xBE, 0xEF, 0x12, 0x34, 0x56]
        reg.append(crc7(reg) << 1 | 1)
        return [0x3F] + reg, 136
    raise ValueError(index)


class Pio:
    """Just the instructions sd_cmd.pio uses. Side-set: 1 bit, optional,
    driving the pin direction. IN/OUT shift left with autopush/autopull at
    32. `background_pull` models the OSR being refilled while the SM is busy
    elsewhere; the datasheet leaves room for either, so both are tested."""

    def __init__(self, prog, wrap_target, wrap, in_base, background_pull,
                 bypass_mask=0):
        self.background_pull = background_pull
        self.bypass_mask = bypass_mask  # INPUT_SYNC_BYPASS
        self.prog, self.wrap_target, self.wrap = prog, wrap_target, wrap
        self.in_base = in_base
        self.pc = wrap_target
        self.x = self.y = 0
        self.isr = self.isr_count = 0
        self.osr, self.osr_count = 0, 32
        self.rx, self.tx = [], []
        self.out_val, self.out_dir = 1, 0
        self.sync = [0xFFFFFFFF, 0xFFFFFFFF]  # 2-stage input synchroniser

    def step(self, gpio_in):
        pins = (self.sync[0] & ~self.bypass_mask) | (gpio_in & self.bypass_mask)
        self.sync = [self.sync[1], gpio_in]
        ins = self.prog[self.pc]
        op = ins >> 13
        if ins & 0x1000:  # side-set enable (opt), value in bit 11
            self.out_dir = (ins >> 11) & 1
        assert (ins >> 8) & 7 == 0, "no delays expected"
        if self.background_pull and self.osr_count >= 32 and self.tx:
            self.osr, self.osr_count = self.tx.pop(0), 0
        nxt = self.wrap_target if self.pc == self.wrap else self.pc + 1
        stall = False

        def inpin(i):
            return (pins >> (self.in_base + i)) & 1

        if op == 0:  # JMP
            cond, addr = (ins >> 5) & 7, ins & 0x1F
            take = {
                0: True,
                1: self.x == 0,
                2: self.x != 0,
                3: self.y == 0,
                4: self.y != 0,
                6: inpin(0) == 1,
                7: self.osr_count < 32,
            }[cond]
            if cond == 2:
                self.x = (self.x - 1) & 0xFFFFFFFF
            if cond == 4:
                self.y = (self.y - 1) & 0xFFFFFFFF
            if take:
                nxt = addr
        elif op == 1:  # WAIT
            pol, src, idx = (ins >> 7) & 1, (ins >> 5) & 3, ins & 0x1F
            assert src == 1
            stall = inpin(idx) != pol
        elif op == 2:  # IN
            src, n = (ins >> 5) & 7, (ins & 0x1F) or 32
            assert src == 0 and n == 1
            if self.isr_count >= 32:  # autopush pending (FIFO was full)
                stall = True
            else:
                self.isr = ((self.isr << 1) | inpin(0)) & 0xFFFFFFFF
                self.isr_count += 1
                if self.isr_count == 32:
                    if len(self.rx) < 4:
                        self.rx.append(self.isr)
                        self.isr, self.isr_count = 0, 0
                    else:
                        raise RuntimeError("RX FIFO overflow (autopush)")
        elif op == 3 and self.osr_count >= 32 and not self.tx:
            stall = True  # autopull: wait for data
        elif op == 3:  # OUT
            if self.osr_count >= 32:  # autopull
                self.osr, self.osr_count = self.tx.pop(0), 0
            dst, n = (ins >> 5) & 7, (ins & 0x1F) or 32
            data = self.osr >> (32 - n)
            self.osr = (self.osr << n) & 0xFFFFFFFF if n < 32 else 0
            self.osr_count += n
            if dst == 0:
                self.out_val = data & 1
            elif dst == 1:
                self.x = data
            elif dst == 2:
                self.y = data
            else:
                raise ValueError(dst)
        elif op == 4:  # PUSH / PULL
            if ins & 0x80:  # PULL block; a no-op on a full OSR with autopull
                if self.osr_count == 0:
                    pass
                elif self.tx:
                    self.osr, self.osr_count = self.tx.pop(0), 0
                else:
                    stall = True
            else:  # PUSH block
                if len(self.rx) < 4:
                    self.rx.append(self.isr)
                    self.isr, self.isr_count = 0, 0
                else:
                    stall = True
        elif op == 7:  # SET
            dst, data = (ins >> 5) & 7, ins & 0x1F
            if dst == 1:
                self.x = data
            elif dst == 4:
                self.out_dir = data & 1
            else:
                raise ValueError(dst)
        else:
            raise ValueError(hex(ins))
        if not stall:
            self.pc = nxt


def apply_phase(prog, launch, phase):
    """WAITs from `launch` on flipped for a rising-edge launch."""
    if not phase & 2:
        return list(prog)
    return [ins ^ 0x80 if i >= launch and (ins & 0xE000) == 0x2000 else ins
            for i, ins in enumerate(prog)]


def load_program(path, name="sd_cmd"):
    text = open(path).read()
    body = re.search(name + r"_program_instructions\[\] = \{(.*?)\};", text,
                     re.S).group(1)
    prog = [int(h, 16) for h in re.findall(r"0x([0-9a-f]{4}),", body)]
    wt = int(re.search(rf"#define {name}_wrap_target (\d+)", text).group(1))
    w = int(re.search(rf"#define {name}_wrap (\d+)", text).group(1))
    launch = int(re.search(rf"#define {name}_offset_launch (\d+)u", text).group(1))
    return prog, wt, w, launch


def run(path, half_period, cpu_latency, background_pull, cycle_ns, phase,
        ncc=8):
    prog, wt, w, launch = load_program(path)
    pin_cmd, pin_clk = 1, 2
    pio = Pio(apply_phase(prog, launch, phase), wt, w, pin_cmd, background_pull,
              (1 << pin_clk) if phase & 1 else 0)
    timing = Timing(cycle_ns, 2 * half_period)

    # Host script, one entry per SD clock: ("drive", bit) / ("idle",) /
    # ("expect", nbits, index, arg). Responses are collected by the host.
    script = [("idle",)] * 80
    commands = [(0, 0), (8, 0x000011AA), (55, 0), (2, 0), (8, 0x000031AA)]
    for idx, arg in commands:
        script += [("drive", b) for b in bits_of(host_frame(idx, arg), 48)]
        if response_for(idx, arg) is None:
            script += [("idle",)] * ncc
        else:
            script += [("expect", idx, arg)]
            script += [("idle",)] * 8

    cpu_queue, cpu_ready_at = [], None
    early_sent = False
    received = []
    errors = []
    cyc = 0
    host_out = 1
    clk = 0

    def bus():
        return pio.out_val if pio.out_dir else host_out

    def tick(n):
        nonlocal cyc, cpu_ready_at, cpu_queue
        for _ in range(n):
            gpio = 0xFFFFFFFF
            gpio &= ~(1 << pin_cmd) if bus() == 0 else 0xFFFFFFFF
            gpio &= ~(1 << pin_clk) if clk == 0 else 0xFFFFFFFF
            before = bus()
            pio.step(gpio & 0xFFFFFFFF)
            if pio.out_dir and bus() != before:
                timing.changes.append(cyc)
            # Model CPU, as in sd_responder.c: for CMD0 the "no response"
            # header goes out right after the first word (fixed small delay);
            # otherwise it answers cpu_latency cycles after the second word.
            nonlocal early_sent
            if (not early_sent and pio.rx and (pio.rx[0] >> 24) & 0x3F == 0
                    and not cpu_queue):
                early_sent = True
                cpu_queue = [0]
            if cpu_ready_at is None and len(pio.rx) >= 2:
                cpu_ready_at = cyc + cpu_latency
            if cpu_ready_at is not None and cyc >= cpu_ready_at and not cpu_queue:
                w0, w1 = pio.rx.pop(0), pio.rx.pop(0)
                idx, arg = (w0 >> 24) & 0x3F, ((w0 << 8) & 0xFFFFFFFF) | (w1 >> 8 & 0xFF)
                received.append((w0, w1))
                r = response_for(idx, arg)
                if r is None:
                    if not early_sent:
                        cpu_queue = [0]
                    early_sent = False
                else:
                    data, nbits = r
                    data = data + [0] * (-len(data) % 4)
                    words = [int.from_bytes(bytes(data[i:i + 4]), "big")
                             for i in range(0, len(data), 4)]
                    cpu_queue = [nbits - 1] + words[:(nbits + 31) // 32]
                cpu_ready_at = None
            while cpu_queue and len(pio.tx) < 4:
                pio.tx.append(cpu_queue.pop(0))
            cyc += 1

    def sd_clock(drive=None):
        """One SD clock: falling edge (host drives), low, rising edge
        (host samples), high. Returns the bus value sampled at the rise."""
        nonlocal clk, host_out
        clk = 0
        timing.falls.append(cyc)
        if drive is not None:
            host_out = drive
        tick(half_period)
        clk = 1
        sample = bus()
        tick(half_period)
        return sample

    i = 0
    while i < len(script):
        item = script[i]
        if item[0] == "drive":
            sd_clock(item[1])
        elif item[0] == "idle":
            if pio.out_dir and bus() == 0:
                pass
            sd_clock(1)
        else:
            _, idx, arg = item
            data, nbits = response_for(idx, arg)
            want = bits_of(data, nbits)
            ncr = 0
            host_out = 1
            while True:
                s = sd_clock(1)
                if s == 0:
                    break
                ncr += 1
                if ncr > 64:
                    errors.append(f"CMD{idx}: no response within 64 clocks")
                    break
            got = [0]
            for _ in range(nbits - 1):
                got.append(sd_clock(1))
            if got != want:
                errors.append(f"CMD{idx}: response bits differ\n  got  {got}\n  want {want}")
            if ncr < 2:
                errors.append(f"CMD{idx}: NCR {ncr} < 2")
            # Next clock: the card must have released CMD by the following
            # rising edge (bus pulled up / host idle).
            sd_clock(1)
            if pio.out_dir:
                errors.append(f"CMD{idx}: CMD still driven after the end bit")
            print(f"    CMD{idx:<2} -> {nbits}-bit response, NCR={ncr}")
        i += 1

    want_frames = []
    for idx, arg in commands:
        b = host_frame(idx, arg)
        want_frames.append((int.from_bytes(bytes(b[:4]), "big"),
                            b[4] << 8 | b[5]))
    if received != want_frames:
        errors.append(f"RX frames differ:\n  got  {[(hex(a), hex(b)) for a, b in received]}\n"
                      f"  want {[(hex(a), hex(b)) for a, b in want_frames]}")
    print(f"    CMD: {timing.report()}")
    return errors


def crc16(data):
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else crc << 1
            crc &= 0xFFFF
    return crc


DAT_DELAY = 80  # SD_DAT_DELAY_CLOCKS in sd_proto.h


def dat_words(payload):
    """What sd_build_dat() produces: two header words, then the frame."""
    c = crc16(payload)
    bits = [0] + bits_of(payload + [c >> 8, c & 0xFF], len(payload) * 8 + 16) + [1]
    padded = bits + [1] * (-len(bits) % 32)
    frame = [int("".join(map(str, padded[i:i + 32])), 2)
             for i in range(0, len(padded), 32)]
    return [DAT_DELAY - 1, len(bits) - 1] + frame, bits


def run_dat(path, half_period, background_pull, cycle_ns, phase):
    prog, wt, w, launch = load_program(path, "sd_dat")
    pin_clk, pin_dat0 = 2, 7
    pio = Pio(apply_phase(prog, launch, phase), wt, w, pin_clk, background_pull,
              (1 << pin_clk) if phase & 1 else 0)
    timing = Timing(cycle_ns, 2 * half_period)
    errors = []
    payloads = [list(range(8)),                                  # SCR size
                [(i * 37 + 5) & 0xFF for i in range(64)],        # CMD6 size
                [0xFF] * 8]
    # The model DMA queues all blocks at once, right at the start: the worst
    # case for the leftover-bit handling between frames.
    dma = []
    for p in payloads:
        dma += dat_words(p)[0]
    clk = 0
    cyc = 0

    def tick(n):
        nonlocal cyc
        for _ in range(n):
            gpio = 0xFFFFFFFF & ~((0 if clk else 1) << pin_clk)
            before = pio.out_val if pio.out_dir else 1
            pio.step(gpio)
            if pio.out_dir and pio.out_val != before:
                timing.changes.append(cyc)
            while dma and len(pio.tx) < 4:
                pio.tx.append(dma.pop(0))
            cyc += 1

    def sd_clock():
        nonlocal clk
        clk = 0
        timing.falls.append(cyc)
        tick(half_period)
        clk = 1
        sample = pio.out_val if pio.out_dir else 1
        tick(half_period)
        return sample, pio.out_dir

    clock = 0
    last_end = 0
    for n, p in enumerate(payloads):
        want = dat_words(p)[1]
        # Wait for the start bit.
        start = None
        for _ in range(DAT_DELAY + 200):
            s, _ = sd_clock()
            clock += 1
            if s == 0:
                start = clock
                break
        if start is None:
            errors.append(f"block {n}: no start bit")
            return errors
        got = [0] + [sd_clock()[0] for _ in range(len(want) - 1)]
        clock += len(want) - 1
        if got != want:
            errors.append(f"block {n}: bits differ")
        gap = start - last_end - 1
        if gap < DAT_DELAY:
            errors.append(f"block {n}: started {gap} clocks after the previous, "
                          f"expected >= {DAT_DELAY}")
        # DAT0 must be released by the next rising edge.
        _, driven = sd_clock()
        clock += 1
        if driven:
            errors.append(f"block {n}: DAT0 still driven after the end bit")
        print(f"    block {n}: {len(p)} bytes, {len(want)} bits, "
              f"start {gap} clocks after the previous")
        last_end = start + len(want) - 1
    print(f"    DAT0: {timing.report()}")
    return errors


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "build/sd_cmd.pio.h"
    cases = [
        # (description, sys cycles per SD half-period, CPU latency in
        #  cycles, ns per sys cycle)
        ("400 kHz on the ~11 MHz boot oscillator, fast CPU", 14, 50, 91.0),
        ("400 kHz on the ~11 MHz boot oscillator, slow CPU", 14, 1500, 91.0),
        ("400 kHz on a ~6 MHz boot oscillator, slow CPU", 8, 900, 167.0),
        ("400 kHz at 150 MHz", 188, 300, 6.67),
        ("25 MHz at 150 MHz", 3, 100, 6.67),
    ]
    phases = ["fall", "fall+fast", "rise", "rise+fast"]
    failed = False
    for desc, half, lat, cns in cases:
        for phase, pname in enumerate(phases):
            for bg in (False, True):
                print(f"{desc}, phase {phase} ({pname}), "
                      f"{'background' if bg else 'on-demand'} autopull:")
                errs = run(path, half, lat, bg, cns, phase)
                for e in errs:
                    print("  FAIL", e)
                failed |= bool(errs)
    for desc, half, cns in [("DAT0 at 400 kHz on the boot oscillator", 14, 91.0),
                            ("DAT0 at 25 MHz at 150 MHz", 3, 6.67)]:
        for phase, pname in enumerate(phases):
            for bg in (False, True):
                print(f"{desc}, phase {phase} ({pname}), "
                      f"{'background' if bg else 'on-demand'} autopull:")
                errs = run_dat(path, half, bg, cns, phase)
                for e in errs:
                    print("  FAIL", e)
                failed |= bool(errs)
    if failed:
        sys.exit(1)
    print("all PIO simulations passed")


if __name__ == "__main__":
    main()

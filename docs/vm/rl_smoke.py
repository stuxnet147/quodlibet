"""RL-path smoke: keep all GPUs busy while the scorer runs in the same process.

This is a coexistence check, not a benchmark: a background thread drives a
matmul loop on every visible GPU while check_batch runs on the CPU. Success is
that both finish, the GPU loop keeps all devices active, and no verdict
degrades. Deliberately short; GOAL G7 forbids long training or evaluation.
"""
import threading
import time

import torch

import quodlibet

BITWUZLA = "/opt/quodlibet/quodlibet/third_party/bitwuzla-linux-x86_64/bin/bitwuzla"
GPU_SECONDS = 45
CHECKS = 32
WORKERS = 8

device_count = torch.cuda.device_count()
print("gpus       %d" % device_count)
stop = threading.Event()
iterations = [0] * device_count


def gpu_loop(index):
    torch.cuda.set_device(index)
    a = torch.randn(4096, 4096, device="cuda")
    b = torch.randn(4096, 4096, device="cuda")
    while not stop.is_set():
        a = torch.tanh(a @ b)
        iterations[index] += 1
    torch.cuda.synchronize()


threads = [threading.Thread(target=gpu_loop, args=(i,)) for i in range(device_count)]
for t in threads:
    t.start()

time.sleep(3)  # let the GPU loops reach steady state


def spec(i):
    return dict(
        left_source="int f(int a){return (a^%d)+a;}" % i, left_function="f",
        right_source="int g(int a){return a+(a^%d);}" % i, right_function="g",
        relation="equivalence", ub_policy="must-match",
        trust_smt_backend=True, solver_executable=BITWUZLA,
    )


t0 = time.perf_counter()
ok = 0
for base in range(0, CHECKS, WORKERS):
    results = quodlibet.check_batch(
        [spec(i) for i in range(base, base + WORKERS)], workers=WORKERS)
    ok += sum(1 for r in results
              if getattr(r, "verdict", None) == "proved-equivalent")
scorer_wall = time.perf_counter() - t0

time.sleep(max(0.0, GPU_SECONDS - scorer_wall - 3))
stop.set()
for t in threads:
    t.join()

print("checks     %d/%d proved under full GPU load in %.1fs" % (ok, CHECKS, scorer_wall))
print("gpu iters  min %d  max %d  (all devices active: %s)" % (
    min(iterations), max(iterations), all(n > 0 for n in iterations)))
